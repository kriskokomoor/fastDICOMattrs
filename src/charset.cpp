#include "fastdicomattrs/charset.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include "fastdicomattrs/sequence.hpp"
#include "fastdicomattrs/value.hpp"
#include "fastdicomattrs/value_length.hpp"
#include "fastdicomattrs/vr.hpp"
#include "charset_tables.generated.hpp"
#include "internal/container_locate.hpp"
#include "internal/vr_inference.hpp"

namespace fds::charset {
namespace {

// --- DICOM-defined-term <-> internal repertoire identity -------------------

enum class RepertoireId : std::uint8_t {
  Ascii,
  Latin1,
  Latin2,
  Latin3,
  Latin4,
  Cyrillic,
  Arabic,
  Greek,
  Hebrew,
  Latin5,
  Thai,
  Utf8,
};

const char32_t* high_table_for(RepertoireId id) {
  using namespace detail;
  switch (id) {
    case RepertoireId::Latin1: return kLatin1High;
    case RepertoireId::Latin2: return kLatin2High;
    case RepertoireId::Latin3: return kLatin3High;
    case RepertoireId::Latin4: return kLatin4High;
    case RepertoireId::Cyrillic: return kCyrillicHigh;
    case RepertoireId::Arabic: return kArabicHigh;
    case RepertoireId::Greek: return kGreekHigh;
    case RepertoireId::Hebrew: return kHebrewHigh;
    case RepertoireId::Latin5: return kLatin5High;
    case RepertoireId::Thai: return kThaiHigh;
    default: return nullptr;
  }
}

// Strips the leading "ISO_IR " or "ISO 2022 IR " prefix and returns the
// bare number, or empty if `term` doesn't match either shape. Both prefix
// forms of the same number name the same repertoire (see charset.hpp) --
// this library is deliberately lenient about which prefix form appears
// where, matching real-world tolerant behavior other implementations
// (e.g. pydicom) already exhibit, rather than rejecting a technically-
// slightly-off-conformant declaration outright.
std::string_view strip_ir_prefix(std::string_view term) {
  constexpr std::string_view kA = "ISO_IR ";
  constexpr std::string_view kB = "ISO 2022 IR ";
  if (term.substr(0, kA.size()) == kA) return term.substr(kA.size());
  if (term.substr(0, kB.size()) == kB) return term.substr(kB.size());
  return {};
}

std::optional<RepertoireId> single_byte_id_for_number(std::string_view number) {
  static const std::unordered_map<std::string_view, RepertoireId> kMap = {
      {"100", RepertoireId::Latin1}, {"101", RepertoireId::Latin2},
      {"109", RepertoireId::Latin3}, {"110", RepertoireId::Latin4},
      {"144", RepertoireId::Cyrillic}, {"127", RepertoireId::Arabic},
      {"126", RepertoireId::Greek}, {"138", RepertoireId::Hebrew},
      {"148", RepertoireId::Latin5}, {"166", RepertoireId::Thai},
  };
  auto it = kMap.find(number);
  return it != kMap.end() ? std::optional<RepertoireId>(it->second) : std::nullopt;
}

// DICOM defined terms this library recognizes but deliberately does not
// decode in V1 (multi-byte code-extension or otherwise out of scope) --
// see the report's "deferred/unsupported charset table". Recognizing them
// (rather than treating them as Unrecognized) matters for correct
// diagnostics: a declared-but-deferred charset is a different, more
// specific outcome than a typo or a genuinely unknown term.
bool is_deferred_term(std::string_view term) {
  static const std::unordered_set<std::string_view> kDeferred = {
      "ISO_IR 13", "ISO 2022 IR 13", "ISO 2022 IR 14", "ISO 2022 IR 87",
      "ISO 2022 IR 159", "ISO 2022 IR 149", "ISO 2022 IR 58", "ISO_IR 58",
      "GB18030", "GBK", "ISO 2022 GBK", "ISO 2022 58",
  };
  return kDeferred.count(term) != 0;
}

// Terms that PS3.5 Table C.12-5 forbids combining with anything else in a
// multi-valued (0008,0005) declaration (code extension techniques).
bool is_stand_alone_term(std::string_view term) {
  return term == "ISO_IR 192" || term == "GB18030" || term == "GBK";
}

RepertoireTerm classify_term(std::string_view raw_term) {
  RepertoireTerm result;
  result.defined_term = std::string(raw_term);

  if (raw_term.empty()) {
    result.support = RepertoireSupport::DefaultRepertoire;
    return result;
  }
  if (raw_term == "ISO_IR 6" || raw_term == "ISO 2022 IR 6") {
    result.support = RepertoireSupport::DefaultRepertoire;
    return result;
  }
  if (raw_term == "ISO_IR 192") {
    result.support = RepertoireSupport::Utf8Supported;
    return result;
  }
  std::string_view number = strip_ir_prefix(raw_term);
  if (!number.empty() && single_byte_id_for_number(number).has_value()) {
    result.support = RepertoireSupport::SingleByteSupported;
    return result;
  }
  if (is_deferred_term(raw_term)) {
    result.support = RepertoireSupport::Deferred;
    return result;
  }
  result.support = RepertoireSupport::Unrecognized;
  return result;
}

// The internal RepertoireId a term resolves to for decode dispatch.
// Precondition: term's support is DefaultRepertoire, SingleByteSupported,
// or Utf8Supported (never called for Deferred/Unrecognized).
RepertoireId dispatch_id_for(const RepertoireTerm& term) {
  if (term.support == RepertoireSupport::Utf8Supported) return RepertoireId::Utf8;
  if (term.support == RepertoireSupport::DefaultRepertoire) return RepertoireId::Ascii;
  return *single_byte_id_for_number(strip_ir_prefix(term.defined_term));
}

// --- ElementPath-scoped (0008,0005) resolution ------------------------------

const Element* find_charset_element(const std::vector<Element>& scope) {
  for (const auto& e : scope) {
    if (e.tag() == kSpecificCharacterSetTag && !e.is_sequence()) return &e;
  }
  return nullptr;
}

struct CharsetSearchResult {
  const Element* charset_element = nullptr;
  bool declared_locally = false;
};

// A1.7: the single inheritance-walk implementation shared by every caller
// that needs "the nearest enclosing (0008,0005) declaration in scope" --
// both the original A1.5 element-locator form (resolve_character_set_
// context, below) and A1.7's new container-locator form (insert_text's
// resolve_character_set_context_for_container, further below). See
// docs/architecture/A1_7_MUTATION_ERGONOMICS_AND_PUBLIC_API_REPORT.md
// section 8: two callers, one algorithm, not two.
//
// `descent_steps` must be *purely* descent steps (every one has
// item_index set) -- callers are responsible for stripping any trailing
// bare leaf step first (see find_effective_charset_element below). Walks
// from `root`, checking every scope crossed (root, then each Item
// boundary named by a step) -- unlike A1.2's private-creator resolution, a
// declaration found here is remembered and carried forward (cascades)
// rather than being scoped strictly to one container. Stops (keeping
// whatever was found so far) the moment a step doesn't resolve -- not an
// error case for this function: an unresolvable *deeper* step simply means
// nothing more specific than what was already found governs from there
// down; callers that need to know a container doesn't exist at all (for
// insertion) check that separately via fds::internal::locate_container.
CharsetSearchResult find_effective_charset_declaration(
    const std::vector<Element>& root, std::span<const ElementPath::Step> descent_steps) {
  CharsetSearchResult result;
  result.charset_element = find_charset_element(root);

  const std::vector<Element>* container = &root;
  for (const auto& step : descent_steps) {
    if (!step.item_index.has_value()) break;  // malformed descent step -- stop here

    const Element* found = nullptr;
    for (const auto& e : *container) {
      if (e.tag() == step.tag) {
        found = &e;
        break;
      }
    }
    if (found == nullptr || !found->is_sequence()) break;

    const auto& items = found->sequence().items();
    if (*step.item_index >= items.size()) break;
    container = &items[*step.item_index].elements();

    const Element* local = find_charset_element(*container);
    if (local != nullptr) {
      result.charset_element = local;
      result.declared_locally = true;
    }
  }
  return result;
}

// Element-locator form (A1.5, unchanged behavior): `path`'s last step
// names the leaf itself and is never a descent -- only the steps before it
// cross a container boundary whose declarations matter here.
CharsetSearchResult find_effective_charset_element(const std::vector<Element>& root,
                                                     const ElementPath& path) {
  const auto& steps = path.steps();
  std::span<const ElementPath::Step> descent_steps(steps.data(),
                                                      steps.empty() ? 0 : steps.size() - 1);
  return find_effective_charset_declaration(root, descent_steps);
}

// Container-locator form (A1.7): `parent`'s every step, with no exception
// for the last, is itself a descent step -- exactly DICOMStructure::
// insert()'s own `parent` contract (see dicom_structure.hpp).
CharsetSearchResult find_effective_charset_for_container(const std::vector<Element>& root,
                                                           const ElementPath& parent) {
  return find_effective_charset_declaration(root, parent.steps());
}

// --- codec engine ------------------------------------------------------------

// Strict UTF-8 validation (RFC 3629): rejects overlong encodings, surrogate
// code points, out-of-range code points, and truncated/invalid sequences.
// Never silently replaces. Deliberately does NOT share an implementation
// with decode_utf8_to_codepoints() below despite the near-identical state
// machine: this is the hot path both decode_text() and encode_text() run
// over *every* input unconditionally, and building/discarding a
// std::vector<char32_t> just to answer a yes/no question measurably
// regressed both (a real ~15x throughput drop, caught by A1.6's own
// benchmarking -- see the A1.6 report's performance section) -- a case
// where the "don't repeat yourself" instinct actively worked against this
// project's stated performance discipline (A1.5's own "avoid gratuitous
// decoding" principle), so the two are kept as separate, allocation-free
// vs. allocating variants of the same algorithm on purpose.
bool is_valid_strict_utf8(std::span<const std::byte> bytes) {
  std::size_t i = 0;
  while (i < bytes.size()) {
    auto b0 = static_cast<unsigned char>(bytes[i]);
    std::size_t len;
    char32_t min_cp;
    char32_t cp;
    if (b0 < 0x80) {
      ++i;
      continue;
    } else if ((b0 & 0xE0) == 0xC0) {
      len = 2;
      min_cp = 0x80;
      cp = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
      len = 3;
      min_cp = 0x800;
      cp = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
      len = 4;
      min_cp = 0x10000;
      cp = b0 & 0x07;
    } else {
      return false;  // stray continuation byte, or 0xF8-0xFF (never valid)
    }
    if (i + len > bytes.size()) return false;  // truncated
    for (std::size_t k = 1; k < len; ++k) {
      auto bk = static_cast<unsigned char>(bytes[i + k]);
      if ((bk & 0xC0) != 0x80) return false;  // not a continuation byte
      cp = (cp << 6) | (bk & 0x3F);
    }
    if (cp < min_cp) return false;                     // overlong encoding
    if (cp > 0x10FFFFu) return false;                   // out of Unicode range
    if (cp >= 0xD800u && cp <= 0xDFFFu) return false;   // surrogate, illegal in UTF-8
    i += len;
  }
  return true;
}

// A1.6: the same state machine as is_valid_strict_utf8 above, but
// returning the actual decoded code points -- needed only by the encoder,
// which must inspect (and look up, per repertoire) each one individually;
// decode_text()'s UTF-8 path never needs this, since valid UTF-8 input
// bytes are simply passed through unchanged (UTF-8 in, UTF-8 out).
std::optional<std::vector<char32_t>> decode_utf8_to_codepoints(std::span<const std::byte> bytes) {
  std::vector<char32_t> out;
  std::size_t i = 0;
  while (i < bytes.size()) {
    auto b0 = static_cast<unsigned char>(bytes[i]);
    std::size_t len;
    char32_t min_cp;
    char32_t cp;
    if (b0 < 0x80) {
      out.push_back(b0);
      ++i;
      continue;
    } else if ((b0 & 0xE0) == 0xC0) {
      len = 2;
      min_cp = 0x80;
      cp = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
      len = 3;
      min_cp = 0x800;
      cp = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
      len = 4;
      min_cp = 0x10000;
      cp = b0 & 0x07;
    } else {
      return std::nullopt;  // stray continuation byte, or 0xF8-0xFF (never valid)
    }
    if (i + len > bytes.size()) return std::nullopt;  // truncated
    for (std::size_t k = 1; k < len; ++k) {
      auto bk = static_cast<unsigned char>(bytes[i + k]);
      if ((bk & 0xC0) != 0x80) return std::nullopt;  // not a continuation byte
      cp = (cp << 6) | (bk & 0x3F);
    }
    if (cp < min_cp) return std::nullopt;                    // overlong encoding
    if (cp > 0x10FFFFu) return std::nullopt;                  // out of Unicode range
    if (cp >= 0xD800u && cp <= 0xDFFFu) return std::nullopt;  // surrogate, illegal in UTF-8
    out.push_back(cp);
    i += len;
  }
  return out;
}

// Appends the UTF-8 encoding of `cp` to `out`.
void append_utf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Decodes `bytes` (entirely, no escape/delimiter awareness) under a single
// repertoire. Returns nullopt on any byte this repertoire cannot
// represent -- never a guess, never lossy replacement.
std::optional<std::string> decode_single_repertoire(RepertoireId id,
                                                      std::span<const std::byte> bytes) {
  if (id == RepertoireId::Utf8) {
    if (!is_valid_strict_utf8(bytes)) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  std::string out;
  out.reserve(bytes.size());
  if (id == RepertoireId::Ascii) {
    for (std::byte b : bytes) {
      auto v = static_cast<unsigned char>(b);
      if (v >= 0x80) return std::nullopt;  // strict: default repertoire is 7-bit ASCII only
      out.push_back(static_cast<char>(v));
    }
    return out;
  }
  const char32_t* high = high_table_for(id);
  for (std::byte b : bytes) {
    auto v = static_cast<unsigned char>(b);
    if (v < 0x80) {
      out.push_back(static_cast<char>(v));
      continue;
    }
    char32_t cp = high[v - 0x80];
    if (cp == 0) return std::nullopt;  // undefined position in this repertoire
    append_utf8(out, cp);
  }
  return out;
}

// One 3- or 4-byte ISO 2022 escape sequence this library recognizes,
// within the V1 single-byte scope, per PS3.5 Table C.12-3 (byte values
// cross-checked against pydicom's own CODES_TO_ENCODINGS table as an
// independent reference during development -- see the freeze report).
struct EscapeEntry {
  std::array<std::byte, 3> bytes;
  RepertoireId id;
};
constexpr std::byte kEsc{0x1B};
const std::array<EscapeEntry, 11> kEscapeTable = {{
    {{kEsc, std::byte{'('}, std::byte{'B'}}, RepertoireId::Ascii},
    {{kEsc, std::byte{'-'}, std::byte{'A'}}, RepertoireId::Latin1},
    {{kEsc, std::byte{'-'}, std::byte{'B'}}, RepertoireId::Latin2},
    {{kEsc, std::byte{'-'}, std::byte{'C'}}, RepertoireId::Latin3},
    {{kEsc, std::byte{'-'}, std::byte{'D'}}, RepertoireId::Latin4},
    {{kEsc, std::byte{'-'}, std::byte{'L'}}, RepertoireId::Cyrillic},
    {{kEsc, std::byte{'-'}, std::byte{'G'}}, RepertoireId::Arabic},
    {{kEsc, std::byte{'-'}, std::byte{'F'}}, RepertoireId::Greek},
    {{kEsc, std::byte{'-'}, std::byte{'H'}}, RepertoireId::Hebrew},
    {{kEsc, std::byte{'-'}, std::byte{'M'}}, RepertoireId::Latin5},
    {{kEsc, std::byte{'-'}, std::byte{'T'}}, RepertoireId::Thai},
}};

std::optional<RepertoireId> match_escape(std::span<const std::byte> fragment,
                                          std::size_t* consumed) {
  if (fragment.size() < 3 || fragment[0] != kEsc) return std::nullopt;
  for (const auto& entry : kEscapeTable) {
    if (fragment[1] == entry.bytes[1] && fragment[2] == entry.bytes[2]) {
      *consumed = 3;
      return entry.id;
    }
  }
  return std::nullopt;
}

// Decodes one VM component (or one PN component-group) with ISO 2022
// single-byte code-extension awareness: splits at ESC boundaries, resets
// to `encodings[0]` at any byte in `delimiters`, and requires every
// escape-designated repertoire actually be one of `encodings` (or Ascii,
// always an implicit reset target) -- see charset.hpp / the freeze
// report's "ISO 2022" section for the full algorithm derivation.
// Returns nullopt on any malformed escape, undeclared repertoire switch,
// or byte this library cannot represent -- never a guess.
std::optional<std::string> decode_with_delimiter_reset(std::span<const std::byte> bytes,
                                                         const std::vector<RepertoireId>& encodings,
                                                         std::span<const std::byte> delimiters) {
  auto is_delimiter = [&](std::byte b) {
    for (std::byte d : delimiters) {
      if (b == d) return true;
    }
    return false;
  };
  auto contains = [&](RepertoireId id) {
    for (auto e : encodings) {
      if (e == id) return true;
    }
    return false;
  };

  std::string out;
  std::size_t i = 0;
  RepertoireId active = encodings.empty() ? RepertoireId::Ascii : encodings[0];

  // No escape anywhere: the whole thing decodes under encodings[0] --
  // the common case, and the correct one for a single, non-code-extension
  // declaration (or the default repertoire) with zero special handling.
  bool has_escape = false;
  for (std::byte b : bytes) {
    if (b == kEsc) {
      has_escape = true;
      break;
    }
  }
  if (!has_escape) {
    return decode_single_repertoire(active, bytes);
  }

  while (i < bytes.size()) {
    if (bytes[i] == kEsc) {
      std::size_t consumed = 0;
      auto id = match_escape(bytes.subspan(i), &consumed);
      if (!id.has_value()) return std::nullopt;  // unrecognized/malformed escape
      if (*id != RepertoireId::Ascii && !contains(*id)) {
        return std::nullopt;  // switches to a repertoire not declared in this context
      }
      active = *id;
      i += consumed;
      continue;
    }
    // Accumulate a run up to the next ESC or delimiter, then decode it as
    // one unit under `active` -- delimiter bytes themselves are included
    // in the run before the reset takes effect for what follows, matching
    // PS3.5's "value after the delimiter reverts to the default" rule
    // (delimiter bytes are always in the 0x00-0x7F range, so which side of
    // the reset decodes them is immaterial: every repertoire agrees on
    // their meaning).
    std::size_t start = i;
    while (i < bytes.size() && bytes[i] != kEsc && !is_delimiter(bytes[i])) ++i;
    bool hit_delimiter = (i < bytes.size() && is_delimiter(bytes[i]));
    if (hit_delimiter) ++i;  // include the delimiter byte itself in this run

    auto decoded = decode_single_repertoire(active, bytes.subspan(start, i - start));
    if (!decoded.has_value()) return std::nullopt;
    out += *decoded;

    if (hit_delimiter) active = encodings.empty() ? RepertoireId::Ascii : encodings[0];
  }
  return out;
}

std::vector<RepertoireId> build_encodings(const CharacterSetContext& context) {
  std::vector<RepertoireId> encodings;
  if (context.terms.empty()) {
    encodings.push_back(RepertoireId::Ascii);
    return encodings;
  }
  for (std::size_t i = 0; i < context.terms.size(); ++i) {
    const auto& term = context.terms[i];
    // Per PS3.5 C.12.1.1.2: an empty first term in a multi-valued
    // declaration means ISO 2022 IR 6 (ASCII).
    if (i == 0 && term.support == RepertoireSupport::DefaultRepertoire) {
      encodings.push_back(RepertoireId::Ascii);
      continue;
    }
    encodings.push_back(dispatch_id_for(term));
  }
  return encodings;
}

bool is_text_vr(VR vr) {
  switch (vr) {
    case VR::LO:
    case VR::LT:
    case VR::PN:
    case VR::SH:
    case VR::ST:
    case VR::UC:
    case VR::UT:
      return true;
    default:
      return false;
  }
}

std::span<const std::byte> as_bytes(const std::string& s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

constexpr std::array<std::byte, 4> kTextDelims = {std::byte{0x0D}, std::byte{0x0A}, std::byte{0x09},
                                                    std::byte{0x0C}};
constexpr std::array<std::byte, 1> kPnDelims = {std::byte{'^'}};

std::optional<std::string> decode_pn_component(std::span<const std::byte> bytes,
                                                const std::vector<RepertoireId>& encodings) {
  // Split on '=' (component-group separator) -- each group is decoded
  // independently, starting fresh at encodings[0], then rejoined with a
  // literal '=' -- matching PS3.5's per-component-group charset reset,
  // distinct from '^''s in-group reset.
  std::vector<std::string> groups;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= bytes.size(); ++i) {
    if (i == bytes.size() || bytes[i] == std::byte{'='}) {
      auto decoded = decode_with_delimiter_reset(bytes.subspan(start, i - start), encodings,
                                                   {kPnDelims.data(), kPnDelims.size()});
      if (!decoded.has_value()) return std::nullopt;
      groups.push_back(*decoded);
      start = i + 1;
    }
  }
  std::string out;
  for (std::size_t i = 0; i < groups.size(); ++i) {
    if (i > 0) out.push_back('=');
    out += groups[i];
  }
  return out;
}

// --- A1.6: the inverse (encode) direction -----------------------------------

// Forward lookup in the same kEscapeTable decode uses -- the encoder and
// decoder always agree on which escape sequence designates which
// repertoire, by construction (one table, both directions), not by two
// independently-maintained ones that could drift apart.
std::optional<std::array<std::byte, 3>> escape_bytes_for(RepertoireId id) {
  for (const auto& entry : kEscapeTable) {
    if (entry.id == id) return entry.bytes;
  }
  return std::nullopt;  // Utf8 has no ISO 2022 escape -- it is stand-alone only
}

const detail::ReverseEntry* reverse_table_for(RepertoireId id, std::size_t* count) {
  using namespace detail;
  switch (id) {
    case RepertoireId::Latin1: *count = kLatin1ReverseCount; return kLatin1Reverse;
    case RepertoireId::Latin2: *count = kLatin2ReverseCount; return kLatin2Reverse;
    case RepertoireId::Latin3: *count = kLatin3ReverseCount; return kLatin3Reverse;
    case RepertoireId::Latin4: *count = kLatin4ReverseCount; return kLatin4Reverse;
    case RepertoireId::Cyrillic: *count = kCyrillicReverseCount; return kCyrillicReverse;
    case RepertoireId::Arabic: *count = kArabicReverseCount; return kArabicReverse;
    case RepertoireId::Greek: *count = kGreekReverseCount; return kGreekReverse;
    case RepertoireId::Hebrew: *count = kHebrewReverseCount; return kHebrewReverse;
    case RepertoireId::Latin5: *count = kLatin5ReverseCount; return kLatin5Reverse;
    case RepertoireId::Thai: *count = kThaiReverseCount; return kThaiReverse;
    default: *count = 0; return nullptr;
  }
}

// The byte representing `cp` under repertoire `id`, or nullopt if `id`
// cannot represent it. 0x00-0x7F is representable identically under every
// repertoire (plain ASCII pass-through, mirroring decode_single_
// repertoire's own low-range handling); Ascii represents nothing else
// (strict, matching decode's default-repertoire strictness); Utf8 is
// handled separately by its caller (stand-alone, no per-code-point table
// lookup applies).
std::optional<std::uint8_t> encode_in_repertoire(RepertoireId id, char32_t cp) {
  if (cp < 0x80) return static_cast<std::uint8_t>(cp);
  if (id == RepertoireId::Ascii || id == RepertoireId::Utf8) return std::nullopt;
  std::size_t count = 0;
  const detail::ReverseEntry* table = reverse_table_for(id, &count);
  const detail::ReverseEntry* end = table + count;
  auto it = std::lower_bound(table, end, cp, [](const detail::ReverseEntry& e, char32_t c) {
    return e.codepoint < c;
  });
  if (it != end && it->codepoint == cp) return it->byte;
  return std::nullopt;
}

constexpr std::array<char32_t, 4> kTextDelimsCp = {0x0D, 0x0A, 0x09, 0x0C};
constexpr std::array<char32_t, 1> kPnDelimsCp = {U'^'};

// Encodes one VM component's (or one PN component-group's) code points
// with ISO 2022 single-byte code-extension awareness -- the exact mirror
// of decode_with_delimiter_reset: resets to `encodings[0]` at any code
// point in `delimiters`, and, per charset.hpp's documented deterministic
// selection policy, prefers staying in the currently-active repertoire
// (avoiding a gratuitous escape) before scanning `encodings` in declared
// order for the first one that can represent a given code point. Returns
// nullopt (UnrepresentableCharacter, diagnosed by the caller) the moment
// no declared repertoire can represent a code point -- never drops or
// substitutes it.
std::optional<std::vector<std::byte>> encode_with_delimiter_reset(
    const std::vector<char32_t>& codepoints, const std::vector<RepertoireId>& encodings,
    std::span<const char32_t> delimiters) {
  auto is_delimiter = [&](char32_t cp) {
    for (char32_t d : delimiters) {
      if (cp == d) return true;
    }
    return false;
  };

  const RepertoireId default_state = encodings.empty() ? RepertoireId::Ascii : encodings[0];
  RepertoireId current = default_state;
  std::vector<std::byte> out;

  for (char32_t cp : codepoints) {
    if (is_delimiter(cp)) {
      // Delimiters are always in the 0x00-0x7F range -- representable
      // identically under any repertoire, so no repertoire-selection logic
      // applies to them; only the *reset* they cause matters here.
      out.push_back(static_cast<std::byte>(cp));
      current = default_state;
      continue;
    }

    std::optional<std::uint8_t> byte = encode_in_repertoire(current, cp);
    RepertoireId chosen = current;
    if (!byte.has_value()) {
      for (RepertoireId candidate : encodings) {
        byte = encode_in_repertoire(candidate, cp);
        if (byte.has_value()) {
          chosen = candidate;
          break;
        }
      }
    }
    if (!byte.has_value()) return std::nullopt;  // no declared repertoire can represent cp

    if (chosen != current) {
      auto escape = escape_bytes_for(chosen);
      if (!escape.has_value()) return std::nullopt;  // unreachable for a declared single-byte id
      out.insert(out.end(), escape->begin(), escape->end());
      current = chosen;
    }
    out.push_back(static_cast<std::byte>(*byte));
  }
  return out;
}

// Mirrors decode_pn_component: splits on '=' (component-group separator)
// -- each group encoded independently, starting fresh at encodings[0] --
// then rejoins with a literal '=' byte. '^' is handled as an in-group
// reset point by encode_with_delimiter_reset itself.
std::optional<std::vector<std::byte>> encode_pn_component(
    const std::string& text, const std::vector<RepertoireId>& encodings) {
  auto codepoints = decode_utf8_to_codepoints(as_bytes(text));
  if (!codepoints.has_value()) return std::nullopt;

  std::vector<std::byte> out;
  std::size_t start = 0;
  bool first_group = true;
  for (std::size_t i = 0; i <= codepoints->size(); ++i) {
    if (i == codepoints->size() || (*codepoints)[i] == U'=') {
      std::vector<char32_t> group(codepoints->begin() + static_cast<std::ptrdiff_t>(start),
                                   codepoints->begin() + static_cast<std::ptrdiff_t>(i));
      auto encoded = encode_with_delimiter_reset(
          group, encodings, {kPnDelimsCp.data(), kPnDelimsCp.size()});
      if (!encoded.has_value()) return std::nullopt;
      if (!first_group) out.push_back(std::byte{'='});
      out.insert(out.end(), encoded->begin(), encoded->end());
      first_group = false;
      start = i + 1;
    }
  }
  return out;
}

}  // namespace

namespace {
// Shared by resolve_character_set_context() and (A1.7) the internal
// container-locator variant below -- one classification implementation,
// fed by either of find_effective_charset_element/_for_container's
// results. See this file's `find_effective_charset_declaration` doc
// comment for why the inheritance *walk* is likewise shared, not
// duplicated.
CharacterSetContext build_context_from_search(const CharsetSearchResult& search) {
  CharacterSetContext context;
  if (search.charset_element == nullptr) {
    context.mode = CharacterSetMode::Default;
    return context;
  }
  context.declared_locally = search.declared_locally;

  std::vector<std::string> raw_terms;
  try {
    raw_terms = search.charset_element->value().as_string_list();
  } catch (const ValueTypeError&) {
    context.mode = CharacterSetMode::Default;
    return context;
  }

  // A single, empty value (no (0008,0005) content at all) means default.
  if (raw_terms.size() == 1 && raw_terms[0].empty()) {
    context.mode = CharacterSetMode::Default;
    return context;
  }

  for (const auto& raw : raw_terms) context.terms.push_back(classify_term(raw));

  if (context.terms.size() == 1) {
    const auto& only = context.terms[0];
    if (only.support == RepertoireSupport::DefaultRepertoire) {
      context.mode = CharacterSetMode::Default;
    } else if (only.support == RepertoireSupport::SingleByteSupported ||
               only.support == RepertoireSupport::Utf8Supported) {
      context.mode = CharacterSetMode::SingleValue;
    } else {
      context.mode = CharacterSetMode::Unsupported;
    }
    return context;
  }

  // Multi-valued: PS3.5 Table C.12-5 forbids combining a stand-alone term
  // (UTF-8/GB18030/GBK) with anything else -- checked first since it's a
  // more specific diagnosis than a generic "unsupported" term.
  for (const auto& term : context.terms) {
    if (is_stand_alone_term(term.defined_term)) {
      context.mode = CharacterSetMode::Malformed;
      return context;
    }
  }
  for (const auto& term : context.terms) {
    if (term.support == RepertoireSupport::Deferred ||
        term.support == RepertoireSupport::Unrecognized) {
      context.mode = CharacterSetMode::Unsupported;
      return context;
    }
  }
  context.mode = CharacterSetMode::MultiValue;
  return context;
}

// A1.7: the container-locator counterpart to resolve_character_set_context
// (public, below) -- resolves the effective context for the container a
// new element would be inserted into, rather than for an existing leaf
// element. Internal only (not in charset.hpp): callers outside this file
// reach it exclusively through insert_text()/insert_text_inferred(), which
// need no lower-level access to a bare CharacterSetContext for a
// not-yet-existing element. See section 8 of the freeze report for the
// proof that resolve_character_set_context() itself -- built for an
// element-locator path whose last step is bare -- cannot simply be handed
// a container-locator path instead: its `is_last` short-circuit would skip
// descending into the very Item the new element is about to live in,
// silently missing that Item's own local (0008,0005) override.
CharacterSetContext resolve_character_set_context_for_container(const DICOMStructure& structure,
                                                                   const ElementPath& parent) {
  return build_context_from_search(find_effective_charset_for_container(structure.elements(), parent));
}
}  // namespace

CharacterSetContext resolve_character_set_context(const DICOMStructure& structure,
                                                    const ElementPath& path) {
  return build_context_from_search(find_effective_charset_element(structure.elements(), path));
}

DecodedText decode_text(const Element& element, const CharacterSetContext& context) {
  DecodedText result;
  if (!is_text_vr(element.vr())) {
    result.status = DecodeStatus::NotATextVR;
    return result;
  }
  if (context.mode == CharacterSetMode::Malformed) {
    result.status = DecodeStatus::MalformedCharsetDeclaration;
    return result;
  }
  if (context.mode == CharacterSetMode::Unsupported) {
    result.status = DecodeStatus::UnsupportedCharset;
    return result;
  }

  // ST, LT, and UT are inherently single-valued (PS3.5 6.2 -- VM is
  // always 1 for these three, unlike SH/LO/PN/UC): a backslash byte
  // within their raw content is literal text (e.g. a Windows-style file
  // path embedded in free text), never a value-multiplicity delimiter.
  // Splitting on it would silently truncate real content -- confirmed by
  // real-world corpus data (an embedded-XML LT value containing several
  // literal backslashes), not merely a hypothetical.
  bool is_single_valued = (element.vr() == VR::ST || element.vr() == VR::LT ||
                            element.vr() == VR::UT);
  std::vector<std::string> raw_components;
  try {
    if (is_single_valued) {
      raw_components.push_back(element.value().as_string());
    } else {
      raw_components = element.value().as_string_list();
    }
  } catch (const ValueTypeError&) {
    result.status = DecodeStatus::InvalidEncodedBytes;
    return result;
  }

  auto encodings = build_encodings(context);
  bool is_pn = (element.vr() == VR::PN);

  std::vector<std::string> decoded_values;
  decoded_values.reserve(raw_components.size());
  for (const auto& raw : raw_components) {
    auto bytes = as_bytes(raw);
    std::optional<std::string> decoded =
        is_pn ? decode_pn_component(bytes, encodings)
              : decode_with_delimiter_reset(bytes, encodings,
                                             {kTextDelims.data(), kTextDelims.size()});
    if (!decoded.has_value()) {
      result.status = DecodeStatus::InvalidEncodedBytes;
      result.values.clear();
      return result;
    }
    decoded_values.push_back(std::move(*decoded));
  }

  result.status = DecodeStatus::Success;
  result.values = std::move(decoded_values);
  return result;
}

EncodedText encode_text(const std::vector<std::string>& values, VR vr,
                         const CharacterSetContext& context) {
  EncodedText result;
  if (!is_text_vr(vr)) {
    result.status = EncodeStatus::NotATextVR;
    return result;
  }
  if (context.mode == CharacterSetMode::Malformed) {
    result.status = EncodeStatus::MalformedCharsetDeclaration;
    return result;
  }
  if (context.mode == CharacterSetMode::Unsupported) {
    result.status = EncodeStatus::UnsupportedCharset;
    return result;
  }

  // Validate every input string is well-formed UTF-8 up front, before any
  // encoding work begins -- part of this function's atomicity guarantee
  // (see charset.hpp): a caller-supplied malformed string must never
  // produce a partially-encoded result.
  for (const auto& v : values) {
    if (!is_valid_strict_utf8(as_bytes(v))) {
      result.status = EncodeStatus::InvalidUnicodeInput;
      return result;
    }
  }

  // ST/LT/UT are inherently VM=1 (see decode_text's own ST/LT/UT handling,
  // A1.5) -- more than one component here is a caller usage error, not
  // encodable content.
  bool is_single_valued = (vr == VR::ST || vr == VR::LT || vr == VR::UT);
  if (is_single_valued && values.size() != 1) {
    result.status = EncodeStatus::InvalidUnicodeInput;
    return result;
  }

  auto encodings = build_encodings(context);
  bool is_pn = (vr == VR::PN);
  bool is_utf8_only = (encodings.size() == 1 && encodings[0] == RepertoireId::Utf8);

  std::vector<std::byte> joined;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) joined.push_back(std::byte{'\\'});

    std::optional<std::vector<std::byte>> encoded;
    if (is_utf8_only) {
      // UTF-8 is stand-alone (PS3.5 Table C.12-5): no escape/delimiter
      // state machine and no repertoire selection apply -- already-
      // validated input bytes pass through unchanged.
      auto b = as_bytes(values[i]);
      encoded = std::vector<std::byte>(b.begin(), b.end());
    } else if (is_pn) {
      encoded = encode_pn_component(values[i], encodings);
    } else {
      auto codepoints = decode_utf8_to_codepoints(as_bytes(values[i]));
      encoded = codepoints.has_value()
                    ? encode_with_delimiter_reset(
                          *codepoints, encodings, {kTextDelimsCp.data(), kTextDelimsCp.size()})
                    : std::nullopt;
    }
    if (!encoded.has_value()) {
      result.status = EncodeStatus::UnrepresentableCharacter;
      return result;
    }
    joined.insert(joined.end(), encoded->begin(), encoded->end());
  }

  // PS3.5 6.4: pad to even length. Every VR this function handles is a
  // text VR -- the pad character is SPACE (0x20); NUL padding is specific
  // to VR::UI, which is never Specific-Character-Set-governed in the
  // first place (is_text_vr() already excluded it above).
  if (joined.size() % 2 != 0) joined.push_back(std::byte{0x20});

  // Wire length-form ceiling: LO/LT/PN/SH/ST are short-form (2-byte length
  // field); UC/UT are long-form (4-byte field) and have no realistic
  // ceiling worth enforcing here -- matching Element::set_value's own
  // existing policy of enforcing only the wire length-form width, never a
  // VR-specific semantic maximum length.
  bool is_short_form =
      (vr == VR::LO || vr == VR::LT || vr == VR::PN || vr == VR::SH || vr == VR::ST);
  if (is_short_form && joined.size() > ValueLength::kMaxShortFormLength) {
    result.status = EncodeStatus::ValueTooLong;
    return result;
  }

  result.status = EncodeStatus::Success;
  result.bytes = std::move(joined);
  return result;
}

namespace {
SetTextStatus to_set_text_status(EncodeStatus status) {
  switch (status) {
    case EncodeStatus::Success: return SetTextStatus::Success;
    case EncodeStatus::NotATextVR: return SetTextStatus::NotATextVR;
    case EncodeStatus::UnsupportedCharset: return SetTextStatus::UnsupportedCharset;
    case EncodeStatus::MalformedCharsetDeclaration: return SetTextStatus::MalformedCharsetDeclaration;
    case EncodeStatus::InvalidUnicodeInput: return SetTextStatus::InvalidUnicodeInput;
    case EncodeStatus::UnrepresentableCharacter: return SetTextStatus::UnrepresentableCharacter;
    case EncodeStatus::ValueTooLong: return SetTextStatus::ValueTooLong;
  }
  return SetTextStatus::ValueTooLong;  // unreachable
}
}  // namespace

SetTextStatus set_text(DICOMStructure& structure, const ElementPath& path,
                        const std::vector<std::string>& values) {
  const Element* existing = structure.find(path);
  if (existing == nullptr) return SetTextStatus::PathNotFound;
  if (existing->is_sequence()) return SetTextStatus::IsSequenceElement;

  auto context = resolve_character_set_context(structure, path);
  auto encoded = encode_text(values, existing->vr(), context);
  if (encoded.status != EncodeStatus::Success) return to_set_text_status(encoded.status);

  // encode_text() already validated everything DICOMStructure::set_value()
  // itself checks (even length, short-form wire ceiling), so this call is
  // expected to always succeed; the fallback below is defensive -- never
  // silently reports Success for a mutation that did not actually happen.
  bool applied = structure.set_value(path, Value::from_owned(std::move(*encoded.bytes)));
  return applied ? SetTextStatus::Success : SetTextStatus::ValueTooLong;
}

namespace {
SetTextStatus to_set_text_status(internal::ContainerLocateStatus status) {
  switch (status) {
    case internal::ContainerLocateStatus::Success: return SetTextStatus::Success;
    case internal::ContainerLocateStatus::ContainerNotFound: return SetTextStatus::ContainerNotFound;
    case internal::ContainerLocateStatus::NotASequence: return SetTextStatus::NotASequence;
    case internal::ContainerLocateStatus::ItemIndexOutOfRange:
      return SetTextStatus::ItemIndexOutOfRange;
  }
  return SetTextStatus::ContainerNotFound;  // unreachable
}
}  // namespace

SetTextStatus insert_text(DICOMStructure& structure, const ElementPath& parent, Tag tag, VR vr,
                           const std::vector<std::string>& values) {
  if (!is_text_vr(vr)) return SetTextStatus::NotATextVR;

  auto loc = internal::locate_container_const(structure.elements(), parent);
  if (loc.status != internal::ContainerLocateStatus::Success) return to_set_text_status(loc.status);

  for (const auto& e : *loc.container) {
    if (e.tag() == tag) return SetTextStatus::AlreadyExists;
  }

  auto context = resolve_character_set_context_for_container(structure, parent);
  auto encoded = encode_text(values, vr, context);
  if (encoded.status != EncodeStatus::Success) return to_set_text_status(encoded.status);

  // Every precondition DICOMStructure::insert() itself checks (container
  // resolution, duplicate, VR, value encodability) has already been
  // verified above by this point (encode_text() guarantees conformant
  // bytes -- even length, wire-ceiling-checked), so this call is expected
  // to always succeed; the fallback is defensive, matching set_text()'s
  // and insert_inferred()'s own convention -- never silently report
  // Success for a mutation that did not actually happen.
  bool applied = structure.insert(parent, tag, vr, Value::from_owned(std::move(*encoded.bytes)));
  return applied ? SetTextStatus::Success : SetTextStatus::ValueTooLong;
}

SetTextStatus insert_text_inferred(DICOMStructure& structure, const ElementPath& parent, Tag tag,
                                    const std::vector<std::string>& values) {
  auto inference = internal::infer_vr(tag);
  if (inference.outcome != internal::VRInferenceOutcome::Inferred) return SetTextStatus::VRRequired;
  return insert_text(structure, parent, tag, inference.vr, values);
}

}  // namespace fds::charset
