#include "parser/explicit_vr_le_parser.hpp"

#include <array>
#include <cstring>
#include <optional>

#include "fastdicomattrs/dicom_structure.hpp"
#include "fastdicomattrs/element.hpp"
#include "fastdicomattrs/item.hpp"
#include "fastdicomattrs/sequence.hpp"
#include "parser/byte_reader.hpp"
#include "parser/implicit_vr_le_parser.hpp"

namespace fds::parser {
namespace {

struct Context {
  const Source& source;
  Fidelity fidelity;
  std::size_t max_sequence_depth;
  std::vector<ParseDiagnostic>& diagnostics;
  // Parse-wide element budget, enforced in parse_one() so it covers every
  // nesting depth, not just the top level -- see ParseOptions::
  // max_element_count. element_count starts wherever the caller left off
  // (nonzero when Implicit VR dataset parsing continues counting from
  // where Explicit VR File Meta parsing stopped).
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

// Non-consuming lookahead at the next tag.
bool peek_tag(ByteReader& reader, Tag* out) {
  std::uint64_t saved = reader.position();
  bool ok = read_tag(reader, out);
  reader.seek(saved);
  return ok;
}

Outcome parse_one(ByteReader& reader, Context& ctx, std::size_t depth, std::optional<Element>* out);

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

// Parses one element header + body starting at reader's current position.
// Handles Item/Sequence Delimitation tags (reporting them as such rather
// than as errors) since parse_item relies on that to find the end of an
// undefined-length item's element list.
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

  SourceSpan vr_span;
  if (!reader.read_span(2, &vr_span)) {
    ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "truncated element header (missing VR)", reader.position(), tag});
    return Outcome::Error;
  }
  const std::byte* vr_ptr = nullptr;
  if (!ctx.source.try_get(vr_span, &vr_ptr)) return Outcome::Error;  // unreachable: read_span already validated bounds
  std::string vr_text(reinterpret_cast<const char*>(vr_ptr), 2);
  std::optional<VR> vr_opt = vr_from_string(vr_text);
  const bool long_form = vr_opt.has_value() && is_long_form(*vr_opt);
  if (!vr_opt.has_value()) {
    ctx.diagnostics.push_back({DiagnosticSeverity::Warning,
                                "unrecognized VR '" + vr_text + "'; assuming short-form length field",
                                reader.position(), tag});
  }
  VR vr = vr_opt.value_or(VR::Unknown);

  LengthForm length_form;
  bool undefined_length = false;
  std::uint32_t length_value = 0;
  std::array<std::byte, 2> reserved{};

  if (long_form) {
    length_form = LengthForm::Long32;
    SourceSpan reserved_span;
    if (!reader.read_span(2, &reserved_span)) {
      ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                  "truncated element header (missing reserved bytes)",
                                  reader.position(), tag});
      return Outcome::Error;
    }
    const std::byte* rp = nullptr;
    if (!ctx.source.try_get(reserved_span, &rp)) return Outcome::Error;  // unreachable
    reserved = {rp[0], rp[1]};
    std::uint32_t len = 0;
    if (!reader.read_u32le(&len)) {
      ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                  "truncated element header (missing 4-byte length)",
                                  reader.position(), tag});
      return Outcome::Error;
    }
    if (len == ValueLength::kUndefinedMarker) {
      undefined_length = true;
    } else {
      length_value = len;
    }
  } else {
    length_form = LengthForm::Short16;
    std::uint16_t len16 = 0;
    if (!reader.read_u16le(&len16)) {
      ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                  "truncated element header (missing 2-byte length)",
                                  reader.position(), tag});
      return Outcome::Error;
    }
    length_value = len16;
  }

  if (vr == VR::SQ) {
    if (depth + 1 > ctx.max_sequence_depth) {
      ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                  "maximum sequence nesting depth exceeded", reader.position(), tag});
      return Outcome::Error;
    }
    auto sequence = parse_sequence(reader, ctx, depth + 1, undefined_length, length_value);
    if (!sequence) return Outcome::Error;
    *out = Element(tag, vr_opt.has_value() ? VRProvenance::Explicit : VRProvenance::Unknown,
                   length_form, undefined_length, reserved, std::move(sequence));
    return Outcome::Ok;
  }

  if (undefined_length) {
    ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "non-sequence element with undefined length", reader.position(), tag});
    return Outcome::Error;
  }

  SourceSpan value_span;
  if (!reader.read_span(length_value, &value_span)) {
    ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "truncated element value (declared length runs past end of source)",
                                reader.position(), tag});
    return Outcome::Error;
  }
  Value value = Value::from_source(ctx.source, value_span);
  *out = Element(tag, vr, vr_opt.has_value() ? VRProvenance::Explicit : VRProvenance::Unknown,
                 length_form, false, reserved, std::move(value));
  return Outcome::Ok;
}

std::optional<PixelDataReference> parse_pixel_data(ByteReader& reader, Context& ctx,
                                                     const TransferSyntax& transfer_syntax) {
  Tag tag;
  if (!read_tag(reader, &tag)) {
    // Unreachable in practice (the caller already peeked this exact tag),
    // but every failure path here must diagnose -- see the comment on the
    // main parse loop's Pixel Data branch for why an undiagnosed nullopt is
    // dangerous: it is silently indistinguishable from "no Pixel Data in
    // this file" once the diagnostics list is otherwise empty.
    ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "truncated Pixel Data (could not re-read its own tag)",
                                reader.position(), std::nullopt});
    return std::nullopt;
  }

  SourceSpan vr_span;
  if (!reader.read_span(2, &vr_span)) {
    ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "truncated Pixel Data header (missing VR)", reader.position(), tag});
    return std::nullopt;
  }
  const std::byte* vr_ptr = nullptr;
  if (!ctx.source.try_get(vr_span, &vr_ptr)) return std::nullopt;  // unreachable: read_span already validated bounds
  std::optional<VR> vr_opt = vr_from_string(std::string(reinterpret_cast<const char*>(vr_ptr), 2));
  VR vr = vr_opt.value_or(VR::OB);

  SourceSpan reserved_span;
  if (!reader.read_span(2, &reserved_span)) {
    ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "truncated Pixel Data header (missing reserved bytes)",
                                reader.position(), tag});
    return std::nullopt;
  }
  const std::byte* reserved_ptr = nullptr;
  if (!ctx.source.try_get(reserved_span, &reserved_ptr)) return std::nullopt;  // unreachable
  std::array<std::byte, 2> reserved{reserved_ptr[0], reserved_ptr[1]};
  std::uint32_t length = 0;
  if (!reader.read_u32le(&length)) {
    ctx.diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                                "truncated Pixel Data header (missing 4-byte length)",
                                reader.position(), tag});
    return std::nullopt;
  }

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
  // Sequence Delimitation Item. See docs/architecture.md section 8.
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

}  // namespace

ParseResult parse_explicit_vr(std::shared_ptr<const Source> source, const ParseOptions& options) {
  ParseResult result;
  std::vector<ParseDiagnostic> diagnostics;
  const std::uint64_t size = source->size();

  std::optional<std::array<std::byte, 128>> preamble;
  std::uint64_t dataset_start = 0;

  if (size >= 132) {
    const std::byte* magic_ptr = nullptr;
    if (source->try_get(SourceSpan(128, 4), &magic_ptr) &&
        std::memcmp(magic_ptr, "DICM", 4) == 0) {
      const std::byte* preamble_ptr = nullptr;
      if (source->try_get(SourceSpan(0, 128), &preamble_ptr)) {
        std::array<std::byte, 128> bytes{};
        std::memcpy(bytes.data(), preamble_ptr, 128);
        preamble = bytes;
        dataset_start = 132;
      }
    }
  }

  ByteReader reader(*source, dataset_start, size);
  Context ctx{*source, options.fidelity, options.max_sequence_depth, diagnostics,
              options.max_element_count};

  std::vector<Element> elements;
  std::optional<PixelDataReference> pixel_data;
  std::optional<std::size_t> pixel_data_position;
  std::optional<TransferSyntax> transfer_syntax;

  // File Meta Information (if any) has ended; resolve the Transfer Syntax
  // before parsing anything else. See docs/architecture.md section 6 and
  // docs/roundtrip-contract.md "Known gaps". Called either mid-loop (as
  // soon as a non-0002-group tag is seen) or once after the loop if the
  // source never contained anything past File Meta (or no File Meta at
  // all) -- both paths must apply the same supported-Transfer-Syntax
  // check, otherwise an unsupported Transfer Syntax declared in a
  // File-Meta-only (empty dataset) source would silently pass.
  auto resolve_transfer_syntax = [&]() {
    std::string uid;
    bool found = false;
    for (const auto& e : elements) {
      if (e.tag() == Tag(0x0002, 0x0010)) {
        uid = e.value().as_string();
        found = true;
        break;
      }
    }
    if (!found) {
      // No wire signal exists for a bare dataset's Transfer Syntax; the
      // caller's own bare_dataset_is_implicit_vr hint (A1.4) decides
      // between the two defaults below -- this is never auto-detected from
      // the byte content itself. See docs/architecture/
      // A1_4_IMPLICIT_VR_SEMANTIC_COMPLETENESS_REPORT.md "Bare datasets".
      if (options.bare_dataset_is_implicit_vr) {
        uid = "1.2.840.10008.1.2";  // Implicit VR Little Endian, per caller's hint.
        diagnostics.push_back(
            {DiagnosticSeverity::Warning,
             "no (0002,0010) Transfer Syntax UID found; assuming Implicit VR Little Endian "
             "per ParseOptions::bare_dataset_is_implicit_vr",
             reader.position(), std::nullopt});
      } else {
        uid = "1.2.840.10008.1.2.1";  // Explicit VR Little Endian, documented default.
        diagnostics.push_back({DiagnosticSeverity::Warning,
                                "no (0002,0010) Transfer Syntax UID found; assuming Explicit VR "
                                "Little Endian",
                                reader.position(), std::nullopt});
      }
    }
    transfer_syntax = TransferSyntax::from_uid(uid);
  };

  while (reader.position() < size) {
    // max_element_count is enforced inside parse_one() (see Context), so it
    // covers nested elements too, not just this top-level loop -- no
    // separate pre-check needed here; a Context-level trip surfaces as
    // parse_one() returning Outcome::Error below, same as any other parse
    // failure.

    Tag next_tag;
    if (!peek_tag(reader, &next_tag)) {
      diagnostics.push_back({DiagnosticSeverity::RecoverableError,
                              "truncated element header at end of source", reader.position(),
                              std::nullopt});
      break;
    }

    if (!transfer_syntax.has_value() && next_tag.group != 0x0002) {
      resolve_transfer_syntax();
      if (transfer_syntax->kind() == TransferSyntaxKind::ImplicitVRLittleEndian) {
        // File Meta (always Explicit VR, per the standard) is done; hand
        // the rest of the stream to the Implicit VR dataset parser and stop
        // this loop -- see docs/roundtrip-contract.md "Implicit VR Little
        // Endian". `reader` is left exactly where `next_tag` was peeked
        // from (unconsumed), which is where that parser needs to start.
        parser::parse_dataset_implicit_vr(reader, *source, options, size, *transfer_syntax,
                                           diagnostics, elements, pixel_data, pixel_data_position,
                                           ctx.element_count);
        break;
      }
      if (!transfer_syntax->supported_for_dataset_parsing()) {
        diagnostics.push_back(
            {DiagnosticSeverity::Unsupported,
             "Transfer Syntax '" + transfer_syntax->uid() + "' is not supported for dataset "
             "parsing in this increment (see docs/roundtrip-contract.md)",
             reader.position(), std::nullopt});
        result.status = ParseStatus::Failed;
        result.diagnostics = std::move(diagnostics);
        return result;
      }
    }

    if (transfer_syntax.has_value() && next_tag == kPixelDataTag) {
      auto ref = parse_pixel_data(reader, ctx, *transfer_syntax);
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

  if (!transfer_syntax.has_value()) {
    // Empty source, or a source that never left the group-0002 prefix
    // (e.g. a File-Meta-only source with no dataset elements at all).
    // Still must be validated -- an unsupported Transfer Syntax declared
    // in File Meta doesn't stop being unsupported just because the
    // dataset that follows it happens to be empty.
    resolve_transfer_syntax();
    // Implicit VR LE is fine here too (nothing left to parse either way --
    // this branch only runs when the loop above never found a dataset
    // element at all).
    if (!transfer_syntax->supported_for_dataset_parsing() &&
        transfer_syntax->kind() != TransferSyntaxKind::ImplicitVRLittleEndian) {
      diagnostics.push_back(
          {DiagnosticSeverity::Unsupported,
           "Transfer Syntax '" + transfer_syntax->uid() + "' is not supported for dataset "
           "parsing in this increment (see docs/roundtrip-contract.md)",
           reader.position(), std::nullopt});
      result.status = ParseStatus::Failed;
      result.diagnostics = std::move(diagnostics);
      return result;
    }
  }

  result.status = diagnostics.empty() ? ParseStatus::Success : ParseStatus::SuccessWithWarnings;
  result.structure = std::make_unique<DICOMStructure>(
      std::move(source), *transfer_syntax, options.fidelity, std::move(elements), preamble,
      std::move(pixel_data), pixel_data_position);
  result.diagnostics = std::move(diagnostics);
  return result;
}

}  // namespace fds::parser
