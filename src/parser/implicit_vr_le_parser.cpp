#include "parser/implicit_vr_le_parser.hpp"

#include <array>
#include <optional>

#include "fastdicomattrs/dictionary.hpp"
#include "fastdicomattrs/dicom_structure.hpp"
#include "fastdicomattrs/item.hpp"
#include "fastdicomattrs/sequence.hpp"
#include "fastdicomattrs/value_length.hpp"

// Deliberately parallel in structure to explicit_vr_le_parser.cpp rather
// than sharing code with it (e.g. via a templated parse_item/parse_sequence):
// Item/Sequence-Delimiter walking is encoding-independent and would be easy
// to share, but this is the highest-uncertainty phase of the project (no
// data dictionary, heuristic-based VR inference -- see the file header
// comment below), and duplicating this self-contained, spec-fixed algorithm
// keeps the well-tested Explicit VR path completely untouched while this one
// is free to change independently if the heuristic needs revisiting.

namespace fds::parser {
namespace {

struct Context {
  const Source& source;
  Fidelity fidelity;
  std::size_t max_sequence_depth;
  std::vector<ParseDiagnostic>& diagnostics;
  // Parse-wide element budget, enforced in parse_one() so it covers every
  // nesting depth, not just the top level -- see ParseOptions::
  // max_element_count. element_count starts nonzero here, continuing the
  // count from where Explicit VR File Meta parsing left off.
  std::size_t max_element_count;
  std::size_t element_count = 0;
};

enum class Outcome { Ok, ItemDelimiter, SequenceDelimiter, Error };

bool read_tag(ByteReader& reader, Tag* out) {
  std::uint16_t group = 0, element = 0;
  if (!reader.read_u16le(&group)) return false;
  if (!reader.read_u16le(&element)) return false;
  *out = Tag(group, element);
  return true;
}

bool peek_tag(ByteReader& reader, Tag* out) {
  std::uint64_t saved = reader.position();
  bool ok = read_tag(reader, out);
  reader.seek(saved);
  return ok;
}

Outcome parse_one(ByteReader& reader, Context& ctx, std::size_t depth, std::optional<Element>* out);

// Item/Sequence-Delimiter handling is byte-identical to the Explicit VR
// parser's parse_item -- Item and delimiter pseudo-tags never carry a VR
// field in either Transfer Syntax.
std::optional<Item> parse_item(ByteReader& reader, Context& ctx, std::size_t depth) {
  Tag tag;
  if (!read_tag(reader, &tag) || tag != kItemTag) {
    ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "expected Item tag (FFFE,E000) in sequence item stream",
                                reader.position(), tag});
    return std::nullopt;
  }
  std::uint32_t length = 0;
  if (!reader.read_u32le(&length)) {
    ctx.diagnostics.push_back(
        {DiagnosticSeverity::RecoverableError, "truncated Item header", reader.position(), tag});
    return std::nullopt;
  }

  std::vector<Element> elements;
  const bool undefined_length = (length == ValueLength::kUndefinedMarker);

  if (undefined_length) {
    while (true) {
      if (reader.at_end()) {
        ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                    "truncated undefined-length Item (no Item Delimitation Item found)",
                                    reader.position(), tag});
        return std::nullopt;
      }
      std::optional<Element> element;
      Outcome outcome = parse_one(reader, ctx, depth, &element);
      if (outcome == Outcome::ItemDelimiter) break;
      if (outcome != Outcome::Ok) return std::nullopt;
      elements.push_back(std::move(*element));
    }
  } else {
    const std::uint64_t limit = reader.position() + length;
    while (reader.position() < limit) {
      std::optional<Element> element;
      Outcome outcome = parse_one(reader, ctx, depth, &element);
      if (outcome != Outcome::Ok) return std::nullopt;
      elements.push_back(std::move(*element));
    }
    // The loop only exits once position() >= limit, so != here means an
    // element's declared length ran past this Item's own declared end --
    // it consumed bytes that belonged to whatever follows the Item in the
    // parent's byte stream. Silently accepting that would misassign
    // trailing bytes to the wrong structural container.
    if (reader.position() != limit) {
      ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                  "an element overran its containing Item's declared length",
                                  reader.position(), tag});
      return std::nullopt;
    }
  }
  return Item(std::move(elements), undefined_length);
}

std::unique_ptr<Sequence> parse_sequence(ByteReader& reader, Context& ctx, std::size_t depth,
                                          bool undefined_length, std::uint32_t declared_length) {
  std::vector<Item> items;

  if (undefined_length) {
    while (true) {
      if (reader.at_end()) {
        ctx.diagnostics.push_back(
            {DiagnosticSeverity::RecoverableError,
             "truncated undefined-length Sequence (no Sequence Delimitation Item found)",
             reader.position(), std::nullopt});
        return nullptr;
      }
      Tag next;
      if (!peek_tag(reader, &next)) return nullptr;
      if (next == kSequenceDelimitationTag) {
        Tag consumed;
        std::uint32_t len = 0;
        read_tag(reader, &consumed);
        reader.read_u32le(&len);
        if (len != 0) {
          ctx.diagnostics.push_back({DiagnosticSeverity::Warning,
                                      "nonzero Sequence Delimitation Item length",
                                      reader.position() - 8, consumed});
        }
        break;
      }
      auto item = parse_item(reader, ctx, depth);
      if (!item.has_value()) return nullptr;
      items.push_back(std::move(*item));
    }
  } else {
    const std::uint64_t limit = reader.position() + declared_length;
    while (reader.position() < limit) {
      auto item = parse_item(reader, ctx, depth);
      if (!item.has_value()) return nullptr;
      items.push_back(std::move(*item));
    }
    // See the matching check in parse_item for why this must be exact
    // equality, not just "stopped once we reached or passed the limit".
    if (reader.position() != limit) {
      ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                  "an item overran its containing Sequence's declared length",
                                  reader.position(), std::nullopt});
      return nullptr;
    }
  }
  return std::make_unique<Sequence>(std::move(items), undefined_length);
}

// Implicit VR Little Endian element header: tag(4) + length(4), no VR, no
// reserved bytes -- unlike Explicit VR, nothing on the wire says what an
// element's VR is. A1.4 recovers it from the frozen PS3.6 dictionary
// (fds::dictionary::lookup) wherever the dictionary has an unambiguous
// answer; see docs/architecture/A1_4_IMPLICIT_VR_SEMANTIC_COMPLETENESS_
// REPORT.md for the full design. Two structural facts remain true
// regardless of the dictionary and are decided first, exactly as before:
//   * undefined length is only ever legal for SQ (and encapsulated Pixel
//     Data, handled separately) in Implicit VR (PS3.5 7.5) -- an
//     undefined-length element is always parsed as a Sequence, dictionary
//     or not (VRProvenance::Structural when the dictionary doesn't confirm
//     it, e.g. a private Sequence);
//   * a *defined-length* element whose dictionary entry says VR::SQ is now
//     also recursively parsed as a Sequence (A1.4's central new
//     capability) -- reusing the exact same parse_sequence() machinery as
//     the undefined-length case, just bounded by the declared length
//     instead of a delimiter.
// Every other defined-length element gets: the dictionary's VR if the entry
// is unambiguous (VRProvenance::Dictionary); VR::Unknown, deferred to a
// later context-resolution pass, if the entry is one of PS3.6's documented
// ambiguous forms (VRProvenance::Unknown for now -- see
// resolve_ambiguous_vrs below); or VR::Unknown if no dictionary entry
// exists at all (private/unrecognized tags, by design -- A1.1). In every
// "Unknown" case the raw bytes are preserved exactly as before -- nothing
// about a tag this library cannot semantically resolve is ever dropped or
// guessed at.
Outcome parse_one(ByteReader& reader, Context& ctx, std::size_t depth, std::optional<Element>* out) {
  Tag tag;
  const std::uint64_t element_start = reader.position();
  if (!read_tag(reader, &tag)) {
    ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "truncated element header (fewer than 4 bytes remaining for tag)",
                                element_start, std::nullopt});
    return Outcome::Error;
  }

  if (tag == kItemDelimitationTag) {
    std::uint32_t len = 0;
    if (!reader.read_u32le(&len)) return Outcome::Error;
    if (len != 0) {
      ctx.diagnostics.push_back({DiagnosticSeverity::Warning,
                                  "nonzero Item Delimitation Item length", element_start, tag});
    }
    return Outcome::ItemDelimiter;
  }
  if (tag == kSequenceDelimitationTag) {
    std::uint32_t len = 0;
    if (!reader.read_u32le(&len)) return Outcome::Error;
    if (len != 0) {
      ctx.diagnostics.push_back({DiagnosticSeverity::Warning,
                                  "nonzero Sequence Delimitation Item length", element_start, tag});
    }
    return Outcome::SequenceDelimiter;
  }
  if (tag == kItemTag) {
    ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "unexpected Item tag (FFFE,E000) outside a Sequence",
                                reader.position(), tag});
    return Outcome::Error;
  }

  // Counted here, not just at the top level: this is the one place every
  // real element (top-level or nested at any depth) passes through, so
  // this is a genuinely parse-wide guard against resource exhaustion from
  // deeply-nested Items/Sequences, not just a large flat element list.
  if (ctx.element_count >= ctx.max_element_count) {
    ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "maximum element count exceeded; stopping parse", reader.position(),
                                std::nullopt});
    return Outcome::Error;
  }
  ++ctx.element_count;

  std::uint32_t length = 0;
  if (!reader.read_u32le(&length)) {
    ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "truncated element header (missing 4-byte length)",
                                reader.position(), tag});
    return Outcome::Error;
  }
  const bool undefined_length = (length == ValueLength::kUndefinedMarker);
  const std::array<std::byte, 2> reserved{};  // no reserved-bytes field on the wire; synthesized.

  // Dictionary-backed VR resolution (A1.4). A lookup miss (nullopt) is
  // expected and routine -- every private tag, by design (A1.1) -- not a
  // parse problem.
  std::optional<dictionary::DictionaryEntry> entry = dictionary::lookup(tag);
  const bool dictionary_says_sq =
      entry.has_value() && entry->ambiguity == dictionary::VRAmbiguity::None && entry->vr == VR::SQ;

  if (undefined_length || dictionary_says_sq) {
    // Structural rule (undefined length) takes precedence over the
    // dictionary if the two ever disagree: FFFFFFFF cannot be interpreted
    // as anything but "parse as Sequence" under Implicit VR, regardless of
    // what any dictionary entry claims. A dictionary entry that positively
    // confirms SQ (the defined-length case, or an undefined-length entry
    // the dictionary also recognizes) gets VRProvenance::Dictionary;
    // otherwise (undefined length with no dictionary confirmation, e.g. a
    // private Sequence, or the rare malformed case of a dictionary-non-SQ
    // tag encoded with undefined length) gets VRProvenance::Structural.
    if (depth + 1 > ctx.max_sequence_depth) {
      ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                  "maximum sequence nesting depth exceeded", reader.position(), tag});
      return Outcome::Error;
    }
    if (undefined_length && entry.has_value() && entry->ambiguity == dictionary::VRAmbiguity::None &&
        entry->vr != VR::SQ) {
      ctx.diagnostics.push_back(
          {DiagnosticSeverity::Info,
           "tag has undefined length (legal only for Sequence, PS3.5 7.5) but the standard "
           "dictionary records a non-Sequence VR for it; parsing as a Sequence per the wire "
           "encoding, which takes precedence",
           reader.position(), tag});
    }
    auto sequence = parse_sequence(reader, ctx, depth + 1, undefined_length, undefined_length ? 0 : length);
    if (!sequence) return Outcome::Error;
    const VRProvenance sq_provenance =
        dictionary_says_sq || (undefined_length && entry.has_value() && entry->vr == VR::SQ)
            ? VRProvenance::Dictionary
            : VRProvenance::Structural;
    *out = Element(tag, sq_provenance, LengthForm::Long32, undefined_length, reserved,
                   std::move(sequence));
    return Outcome::Ok;
  }

  SourceSpan value_span;
  if (!reader.read_span(length, &value_span)) {
    ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "truncated element value (declared length runs past end of source)",
                                reader.position(), tag});
    return Outcome::Error;
  }
  Value value = Value::from_source(ctx.source, value_span);

  VR resolved_vr = VR::Unknown;
  VRProvenance provenance = VRProvenance::Unknown;
  // Implicit VR's own wire header is always tag(4)+length(4) regardless of
  // VR -- there is no concept of "short form" on the wire here. This field
  // instead records what LengthForm this element's *resolved* VR would
  // require if re-encoded as Explicit VR (the only way an Implicit-VR-
  // sourced element can ever be written -- see write_lossless's Transfer
  // Syntax rewrite), matching the exact invariant DICOMStructure::set()
  // already establishes for caller-inserted elements: LengthForm must track
  // is_long_form(vr), never be assumed. VR::Unknown (unresolved) keeps
  // Long32, matching to_string(VR::Unknown) == "UN", itself a long-form VR.
  LengthForm scalar_length_form = LengthForm::Long32;
  if (entry.has_value() && entry->ambiguity == dictionary::VRAmbiguity::None) {
    // A short-form VR's Explicit-VR-re-encoded length field is only 2
    // bytes wide, but this value's actual length was read from Implicit
    // VR's always-4-byte length field, so it could in principle exceed
    // what a short form can encode (PS3.5's own short-form VRs all have
    // small maximum lengths, e.g. LO's 64-character max, but a malformed
    // or adversarial file could still claim more). Never resolve to a VR
    // this library could not correctly re-encode -- leave it Unknown
    // instead, exactly the same "reject rather than silently truncate"
    // discipline Element::set_value already applies to mutation.
    const bool fits =
        is_long_form(entry->vr) || value.size() <= ValueLength::kMaxShortFormLength;
    if (fits) {
      resolved_vr = entry->vr;
      provenance = VRProvenance::Dictionary;
      scalar_length_form = is_long_form(resolved_vr) ? LengthForm::Long32 : LengthForm::Short16;
    } else {
      ctx.diagnostics.push_back(
          {DiagnosticSeverity::Info,
           "the standard dictionary's VR for this tag cannot encode a value this long in "
           "Explicit VR's short form; leaving VR unresolved rather than risk truncating it",
           reader.position(), tag});
    }
  }
  // else: either no dictionary entry (private/unrecognized -- routine), or
  // an ambiguous entry. USorSS ambiguity is resolved in a dedicated
  // post-parse pass once the whole top-level dataset is available (see
  // resolve_ambiguous_vrs) -- it cannot be resolved here because the
  // disambiguating attribute (Pixel Representation) may not have been
  // parsed yet. The other three ambiguous forms are not resolved in V1 --
  // see the A1.4 report's ambiguous-VR inventory. Both cases leave
  // VR::Unknown/VRProvenance::Unknown for now; raw bytes are unaffected
  // either way.
  *out = Element(tag, resolved_vr, provenance, scalar_length_form,
                 /*undefined_length=*/false, reserved, std::move(value));
  return Outcome::Ok;
}

std::optional<PixelDataReference> parse_pixel_data(ByteReader& reader, Context& ctx,
                                                     const TransferSyntax& transfer_syntax) {
  Tag tag;
  if (!read_tag(reader, &tag)) {
    // Unreachable in practice (the caller already peeked this exact tag),
    // but every failure path here must diagnose -- an undiagnosed nullopt
    // is silently indistinguishable from "no Pixel Data in this file" once
    // the diagnostics list is otherwise empty.
    ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "truncated Pixel Data (could not re-read its own tag)",
                                reader.position(), std::nullopt});
    return std::nullopt;
  }

  std::uint32_t length = 0;
  if (!reader.read_u32le(&length)) {
    ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "truncated Pixel Data header (missing 4-byte length)",
                                reader.position(), tag});
    return std::nullopt;
  }
  const std::array<std::byte, 2> reserved{};
  // No VR on the wire; OB is the same safe generic fallback the Explicit VR
  // parser uses for an unrecognized VR text (see parse_pixel_data there).
  const VR vr = VR::OB;

  if (length != ValueLength::kUndefinedMarker) {
    SourceSpan span;
    if (!reader.read_span(length, &span)) {
      ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                  "truncated native Pixel Data value", reader.position(), tag});
      return std::nullopt;
    }
    return PixelDataReference::native(vr, reserved, transfer_syntax, span);
  }

  // Encapsulated: Basic Offset Table item, then fragment items, then
  // Sequence Delimitation Item -- structurally identical to Explicit VR's
  // encapsulated Pixel Data (Item tags are encoding-independent), even
  // though this shape essentially never occurs in practice under Implicit
  // VR LE (compressed Transfer Syntaxes always declare Explicit VR LE
  // dataset encoding per the standard). Handled anyway since nothing
  // forbids it structurally.
  Tag bot_tag;
  if (!read_tag(reader, &bot_tag) || bot_tag != kItemTag) {
    ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "expected Basic Offset Table Item at start of encapsulated Pixel Data",
                                reader.position(), tag});
    return std::nullopt;
  }
  std::uint32_t bot_length = 0;
  if (!reader.read_u32le(&bot_length)) {
    ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "truncated encapsulated Pixel Data (missing Basic Offset Table length)",
                                reader.position(), tag});
    return std::nullopt;
  }
  SourceSpan bot_span;
  if (!reader.read_span(bot_length, &bot_span)) {
    ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "truncated encapsulated Pixel Data (Basic Offset Table runs past "
                                "end of source)",
                                reader.position(), tag});
    return std::nullopt;
  }

  std::vector<PixelDataFragment> fragments;
  while (true) {
    if (reader.at_end()) {
      ctx.diagnostics.push_back(
          {DiagnosticSeverity::RecoverableError,
           "truncated encapsulated Pixel Data (no Sequence Delimitation Item found)",
           reader.position(), tag});
      return std::nullopt;
    }
    Tag next;
    if (!peek_tag(reader, &next)) {
      ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                  "truncated encapsulated Pixel Data (could not read next fragment "
                                  "tag)",
                                  reader.position(), tag});
      return std::nullopt;
    }
    if (next == kSequenceDelimitationTag) {
      Tag consumed;
      std::uint32_t len = 0;
      read_tag(reader, &consumed);
      reader.read_u32le(&len);
      if (len != 0) {
        ctx.diagnostics.push_back({DiagnosticSeverity::Warning,
                                    "nonzero Sequence Delimitation Item length",
                                    reader.position() - 8, consumed});
      }
      break;
    }
    Tag frag_tag;
    if (!read_tag(reader, &frag_tag) || frag_tag != kItemTag) {
      ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                  "expected fragment Item in encapsulated Pixel Data",
                                  reader.position(), tag});
      return std::nullopt;
    }
    std::uint32_t frag_length = 0;
    if (!reader.read_u32le(&frag_length)) {
      ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                  "truncated encapsulated Pixel Data (missing fragment length)",
                                  reader.position(), tag});
      return std::nullopt;
    }
    SourceSpan frag_span;
    if (!reader.read_span(frag_length, &frag_span)) {
      ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                  "truncated encapsulated Pixel Data (fragment runs past end of "
                                  "source)",
                                  reader.position(), tag});
      return std::nullopt;
    }
    fragments.push_back({frag_span});
  }

  return PixelDataReference::encapsulated(vr, reserved, transfer_syntax, bot_span, std::move(fragments));
}

// Finds (0028,0103) Pixel Representation among `top_level` (the top-level
// element list only -- File Meta plus dataset). PS3.5 treats Pixel
// Representation as a whole-object attribute of the primary Image Pixel
// Module, not something re-declared per Sequence Item (unlike e.g.
// Specific Character Set, which the standard explicitly allows to vary
// per-Item) -- so unlike A1.2's private-creator resolution, this
// deliberately does *not* search the ambiguous element's own immediate
// container; it always looks at the top level, regardless of how deeply
// nested the ambiguous element itself is.
std::optional<std::uint16_t> find_pixel_representation(const std::vector<Element>& top_level) {
  for (const auto& e : top_level) {
    if (e.tag() != Tag(0x0028, 0x0103) || e.is_sequence()) continue;
    try {
      return e.value().as_uint16();
    } catch (const ValueTypeError&) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

void resolve_ambiguous_vrs_recursive(std::vector<Element>& elements,
                                      const std::optional<std::uint16_t>& pixel_representation) {
  for (auto& e : elements) {
    if (e.is_sequence()) {
      for (auto& item : e.sequence().items()) {
        resolve_ambiguous_vrs_recursive(item.elements(), pixel_representation);
      }
      continue;
    }
    // Only a still-Unknown, Implicit-VR-inferred element can be a pending
    // ambiguous entry -- an Explicit-VR (File Meta) element is never
    // touched, per A1.4's "Explicit VR behavior must remain unchanged"
    // requirement.
    if (e.vr() != VR::Unknown || e.has_explicit_vr_in_source()) continue;
    auto entry = dictionary::lookup(e.tag());
    if (!entry.has_value() || entry->ambiguity != dictionary::VRAmbiguity::USorSS) continue;
    if (!pixel_representation.has_value()) continue;  // no context available -- never guess
    VR resolved;
    if (*pixel_representation == 0) {
      resolved = VR::US;
    } else if (*pixel_representation == 1) {
      resolved = VR::SS;
    } else {
      continue;  // out-of-spec Pixel Representation value (PS3.5 only defines 0/1) -- never guess
    }
    // US and SS are both short-form VRs (2-byte Explicit-VR length field);
    // same "never resolve to something this library could not correctly
    // re-encode" discipline as the dictionary path in parse_one.
    if (e.value().size() > ValueLength::kMaxShortFormLength) continue;
    Value value_copy = e.value();
    e = Element(e.tag(), resolved, VRProvenance::ContextResolved, LengthForm::Short16,
                e.has_undefined_length(), e.reserved_bytes(), std::move(value_copy));
  }
}

// Ambiguous-VR context resolution (A1.4), run once per parse over the
// fully-built element tree -- never incrementally during parsing. This is
// deliberate, not an implementation convenience: the disambiguating
// attribute (Pixel Representation) may legally appear on either side of an
// ambiguous element on the wire (nothing in PS3.5 orders them relative to
// each other), so any resolution attempted while still parsing could only
// ever be correct for one of the two orderings. Running this pass after
// the entire top-level list (and, transitively, every nested Item -- all
// already fully constructed in memory by the time parsing finishes, since
// parse_one() only ever returns a completed Element) sidesteps the
// ordering question entirely: by the time this runs, every element that
// will ever exist already does, regardless of wire order. See the A1.4
// report's "context ordering" section.
void resolve_ambiguous_vrs(std::vector<Element>& top_level_elements) {
  std::optional<std::uint16_t> pixel_representation = find_pixel_representation(top_level_elements);
  resolve_ambiguous_vrs_recursive(top_level_elements, pixel_representation);
}

}  // namespace

void parse_dataset_implicit_vr(ByteReader& reader, const Source& source, const ParseOptions& options,
                                std::uint64_t size, const TransferSyntax& transfer_syntax,
                                std::vector<ParseDiagnostic>& diagnostics,
                                std::vector<Element>& elements,
                                std::optional<PixelDataReference>& pixel_data,
                                std::optional<std::size_t>& pixel_data_position,
                                std::size_t initial_element_count) {
  Context ctx{source, options.fidelity, options.max_sequence_depth, diagnostics,
              options.max_element_count, initial_element_count};

  while (reader.position() < size) {
    // max_element_count is enforced inside parse_one() (see Context), so it
    // covers nested elements too, not just this loop -- no separate
    // pre-check needed here.

    Tag next_tag;
    if (!peek_tag(reader, &next_tag)) {
      diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                              "truncated element header at end of source", reader.position(),
                              std::nullopt});
      break;
    }

    if (next_tag == kPixelDataTag) {
      auto ref = parse_pixel_data(reader, ctx, transfer_syntax);
      if (!ref.has_value()) break;
      pixel_data = std::move(ref);
      pixel_data_position = elements.size();
      continue;
    }

    std::optional<Element> element;
    Outcome outcome = parse_one(reader, ctx, /*depth=*/0, &element);
    if (outcome != Outcome::Ok) {
      if (outcome == Outcome::ItemDelimiter || outcome == Outcome::SequenceDelimiter) {
        diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "unexpected delimiter tag at dataset top level", reader.position(),
                                std::nullopt});
      }
      break;
    }
    elements.push_back(std::move(*element));
  }

  // See resolve_ambiguous_vrs's own comment for why this runs once, here,
  // after the entire top-level list (File Meta included) and every nested
  // Item are fully built, rather than incrementally during the loop above.
  resolve_ambiguous_vrs(elements);
}

}  // namespace fds::parser
