#include "writer/lossless_writer.hpp"

#include <sstream>

#include "byte_order.hpp"
#include "fastdicomattrs/item.hpp"
#include "fastdicomattrs/sequence.hpp"

namespace fds {
namespace {

// Accumulates bytes into an ostream (which may be the real output, or a
// temporary buffer used to measure a defined-length Sequence/Item's
// content before its length field can be written). See
// docs/roundtrip-contract.md "Reconstruction, not verbatim copy".
struct Writer {
  std::ostream& out;
  std::uint64_t bytes = 0;
  // Value-payload byte provenance -- see WriteResult's field comments for
  // exactly what these do and don't count.
  std::uint64_t source_backed_value_bytes = 0;
  std::uint64_t regenerated_value_bytes = 0;
  bool ok = true;

  void raw(const void* data, std::size_t n) {
    if (n == 0) return;
    if (ok) {
      out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
      if (!out) ok = false;
    }
    bytes += n;
  }
  void u16(std::uint16_t v) {
    std::byte b[2];
    internal::store_u16le(v, b);
    raw(b, 2);
  }
  void u32(std::uint32_t v) {
    std::byte b[4];
    internal::store_u32le(v, b);
    raw(b, 4);
  }
  void write_tag(Tag t) {
    u16(t.group);
    u16(t.element);
  }
};

void write_element(Writer& w, const Element& e);

void write_item(Writer& w, const Item& item) {
  w.write_tag(kItemTag);
  if (item.has_undefined_length()) {
    w.u32(ValueLength::kUndefinedMarker);
    for (const auto& e : item.elements()) write_element(w, e);
    w.write_tag(kItemDelimitationTag);
    w.u32(0);
  } else {
    std::ostringstream tmp;
    Writer inner{tmp};
    for (const auto& e : item.elements()) write_element(inner, e);
    w.u32(static_cast<std::uint32_t>(inner.bytes));
    std::string content = tmp.str();
    w.raw(content.data(), content.size());
    if (!inner.ok) w.ok = false;
    w.source_backed_value_bytes += inner.source_backed_value_bytes;
    w.regenerated_value_bytes += inner.regenerated_value_bytes;
  }
}

// Counts `bytes` as source-backed (untouched, verbatim from the original
// Source) or regenerated (a caller-supplied replacement), for the
// byte-preservation accounting in WriteResult.
void count_value_bytes(Writer& w, const Value& value, std::size_t byte_count) {
  if (value.is_source_backed()) {
    w.source_backed_value_bytes += byte_count;
  } else {
    w.regenerated_value_bytes += byte_count;
  }
}

void write_element(Writer& w, const Element& e) {
  w.write_tag(e.tag());
  std::string_view vr_text = to_string(e.vr());
  w.raw(vr_text.data(), 2);

  if (e.length_form() == LengthForm::Short16) {
    w.u16(static_cast<std::uint16_t>(e.value().size()));
    auto bytes = e.value().bytes();
    w.raw(bytes.data(), bytes.size());
    count_value_bytes(w, e.value(), bytes.size());
    return;
  }

  auto reserved = e.reserved_bytes();
  w.raw(reserved.data(), reserved.size());

  if (e.is_sequence()) {
    const Sequence& seq = e.sequence();
    if (seq.has_undefined_length()) {
      w.u32(ValueLength::kUndefinedMarker);
      for (const auto& item : seq.items()) write_item(w, item);
      w.write_tag(kSequenceDelimitationTag);
      w.u32(0);
    } else {
      std::ostringstream tmp;
      Writer inner{tmp};
      for (const auto& item : seq.items()) write_item(inner, item);
      w.u32(static_cast<std::uint32_t>(inner.bytes));
      std::string content = tmp.str();
      w.raw(content.data(), content.size());
      if (!inner.ok) w.ok = false;
      w.source_backed_value_bytes += inner.source_backed_value_bytes;
      w.regenerated_value_bytes += inner.regenerated_value_bytes;
    }
    return;
  }

  w.u32(static_cast<std::uint32_t>(e.value().size()));
  auto bytes = e.value().bytes();
  w.raw(bytes.data(), bytes.size());
  count_value_bytes(w, e.value(), bytes.size());
}

// Recomputes File Meta Information Group Length (0002,0000)'s value from
// the actual encoded byte length of the rest of the File Meta group (every
// other group-0002 element), for the modified-write path. An unmodified
// write reproduces the original group length byte-for-byte, which is
// correct since nothing changed; but a modified write can add, remove, or
// resize File Meta elements (directly, or indirectly via the
// TransferSyntaxUID rewrite above), and PS3.10 requires the group length
// to reflect the group's actual encoded size -- a stale value is a real
// conformance defect, not just an unproven claim (see
// docs/roundtrip-contract.md "Two write contracts"). Returns nullopt if
// the structure has no (0002,0000) element to begin with (some
// legitimately don't); nothing to recompute in that case.
std::optional<std::uint32_t> compute_file_meta_group_length(const std::vector<Element>& elements,
                                                              bool rewrite_transfer_syntax_uid) {
  bool has_group_length = false;
  std::ostringstream tmp;
  Writer inner{tmp};
  for (const auto& element : elements) {
    if (element.tag() == Tag(0x0002, 0x0000)) {
      has_group_length = true;
      continue;
    }
    if (element.tag().group != 0x0002) continue;
    if (rewrite_transfer_syntax_uid && element.tag() == Tag(0x0002, 0x0010)) {
      // Must match exactly what the main write loop below substitutes, or
      // the recomputed length wouldn't reflect what's actually written.
      Element rewritten(element.tag(), element.vr(), element.vr_provenance(),
                         element.length_form(), element.has_undefined_length(),
                         element.reserved_bytes(), Value::from_string("1.2.840.10008.1.2.1"));
      write_element(inner, rewritten);
      continue;
    }
    write_element(inner, element);
  }
  if (!has_group_length) return std::nullopt;
  return static_cast<std::uint32_t>(inner.bytes);
}

void write_pixel_data(Writer& w, const PixelDataReference& ref, const Source& source) {
  w.write_tag(ref.tag());
  std::string_view vr_text = to_string(ref.vr());
  w.raw(vr_text.data(), 2);
  auto reserved = ref.reserved_bytes();
  w.raw(reserved.data(), reserved.size());

  // Pixel Data is never mutated by this library (there is no pixel-data
  // mutation API), so every byte written here is always source-backed --
  // usually the overwhelming majority of a file's size, and exactly the
  // case the README's own "passthru (7FE0,0010)" policy line is about.
  if (!ref.is_encapsulated()) {
    SourceSpan span = ref.native_span();
    w.u32(static_cast<std::uint32_t>(span.length()));
    w.raw(source.data(span), span.length());
    w.source_backed_value_bytes += span.length();
    return;
  }

  w.u32(ValueLength::kUndefinedMarker);

  w.write_tag(kItemTag);
  const auto& bot = ref.basic_offset_table();
  std::uint64_t bot_len = bot.has_value() ? bot->length() : 0;
  w.u32(static_cast<std::uint32_t>(bot_len));
  if (bot.has_value() && bot_len > 0) {
    w.raw(source.data(*bot), bot_len);
    w.source_backed_value_bytes += bot_len;
  }

  for (const auto& fragment : ref.fragments()) {
    w.write_tag(kItemTag);
    w.u32(static_cast<std::uint32_t>(fragment.span.length()));
    w.raw(source.data(fragment.span), fragment.span.length());
    w.source_backed_value_bytes += fragment.span.length();
  }

  w.write_tag(kSequenceDelimitationTag);
  w.u32(0);
}

}  // namespace

WriteResult write_lossless(const DICOMStructure& structure, std::ostream& out) {
  WriteResult result;

  // Two distinct contracts, gated on whether the structure was mutated --
  // see docs/roundtrip-contract.md "Two write contracts".
  //
  // Unmodified: byte-identical reproduction, guaranteed only at LOSSLESS
  // (FAST/STANDARD don't retain the header-encoding detail LOSSLESS does).
  //
  // Modified: a valid, semantically-correct reconstruction (not a
  // byte-identical guarantee) at LOSSLESS or STANDARD -- both retain every
  // field the writer needs (Tag, VR, LengthForm, undefined-length flag,
  // value bytes) to reconstruct headers deterministically. FAST is reserved
  // for future short-circuiting that may not populate those fields, so it
  // stays excluded here even though today's parser happens to populate them
  // identically at every fidelity.
  if (structure.is_modified()) {
    if (structure.fidelity() != Fidelity::Lossless && structure.fidelity() != Fidelity::Standard) {
      result.status = WriteStatus::Unsupported;
      result.diagnostics.push_back(
          {DiagnosticSeverity::Unsupported,
           "write() of a modified structure requires Fidelity::Standard or Fidelity::Lossless "
           "(see docs/roundtrip-contract.md)",
           0, std::nullopt});
      return result;
    }
  } else if (structure.fidelity() != Fidelity::Lossless) {
    result.status = WriteStatus::Unsupported;
    result.diagnostics.push_back(
        {DiagnosticSeverity::Unsupported,
         "write() of an unmodified structure is only supported for Fidelity::Lossless "
         "(see docs/roundtrip-contract.md)",
         0, std::nullopt});
    return result;
  } else if (!structure.transfer_syntax().explicit_vr()) {
    // This writer always emits Explicit VR output (every element header it
    // writes includes VR text -- see write_element below), regardless of
    // what the source was encoded with. An unmodified Implicit VR Little
    // Endian structure therefore can never round-trip byte-for-byte: the
    // output would categorically differ from the input, not just risk
    // differing. The LOSSLESS guarantee only ever applied to Explicit VR
    // input -- see docs/roundtrip-contract.md "Implicit VR Little Endian".
    result.status = WriteStatus::Unsupported;
    result.diagnostics.push_back(
        {DiagnosticSeverity::Unsupported,
         "write() cannot reproduce an unmodified Implicit VR Little Endian structure "
         "byte-for-byte; this writer only emits Explicit VR output "
         "(see docs/roundtrip-contract.md)",
         0, std::nullopt});
    return result;
  }
  if (!structure.source()) {
    result.status = WriteStatus::Failed;
    result.diagnostics.push_back(
        {DiagnosticSeverity::FatalError, "structure has no backing Source", 0, std::nullopt});
    return result;
  }

  Writer w{out};

  if (structure.has_file_preamble()) {
    const auto& preamble = structure.file_preamble();
    w.raw(preamble.data(), preamble.size());
    w.raw("DICM", 4);
  }

  // This writer always emits Explicit VR headers (write_element writes VR
  // text unconditionally), regardless of how the structure was parsed. A
  // structure parsed from Implicit VR Little Endian input therefore always
  // comes out re-encoded as Explicit VR -- which is only reachable here via
  // the modified-structure write path (see the guard above); the
  // unmodified byte-identical path already refuses this case. If that
  // happens, (0002,0010) TransferSyntaxUID must be rewritten to match what
  // the writer actually produces -- otherwise the output declares Implicit
  // VR while being encoded as Explicit VR, which no DICOM reader (including
  // this library's own parser) can correctly interpret. See
  // docs/roundtrip-contract.md "Implicit VR Little Endian".
  const bool rewrite_transfer_syntax_uid = !structure.transfer_syntax().explicit_vr();

  // Group length only needs recomputing on the modified-write path -- an
  // unmodified write's File Meta is byte-identical to the source, group
  // length included, which is already correct. See
  // compute_file_meta_group_length's comment.
  std::optional<std::uint32_t> file_meta_group_length;
  if (structure.is_modified()) {
    file_meta_group_length =
        compute_file_meta_group_length(structure.elements(), rewrite_transfer_syntax_uid);
  }

  // Where Pixel Data belongs among the ordinary top-level elements --
  // expressed as how many of them are written before it. Two distinct
  // rules, matching this writer's two write contracts (see
  // docs/architecture/A1_3_PIXEL_DATA_ORDERING_REPORT.md "Mutation
  // interaction" for the full reasoning):
  //
  //  * Unmodified (byte-identical) path: honor the position recorded at
  //    parse time exactly -- that position can never go stale, because
  //    nothing has changed since it was recorded. Unknown (a
  //    programmatically-built structure that didn't supply one) means
  //    "after every ordinary element."
  //  * Modified (semantic-reconstruction) path: never consult the recorded
  //    position -- insertion/removal may have invalidated it as a plain
  //    index. Instead, place Pixel Data exactly where its own tag
  //    (7FE0,0010) belongs in the current, already-ascending-tag-ordered
  //    elements_ list (DICOMStructure::set() maintains that ordering for
  //    every top-level insertion), the same ascending-tag-order rule every
  //    other newly-inserted element already follows on this path.
  const auto* pixel_data = structure.pixel_data();
  std::size_t pixel_data_index = structure.element_count();
  if (pixel_data != nullptr) {
    if (structure.is_modified()) {
      pixel_data_index = 0;
      for (const auto& element : structure.elements()) {
        if (!(element.tag() < kPixelDataTag)) break;
        ++pixel_data_index;
      }
    } else {
      pixel_data_index = structure.pixel_data_position().value_or(structure.element_count());
    }
  }

  bool pixel_data_written = false;
  auto write_pixel_data_now = [&] {
    if (pixel_data != nullptr && !pixel_data_written) {
      write_pixel_data(w, *pixel_data, *structure.source());
      pixel_data_written = true;
    }
  };

  std::size_t index = 0;
  for (const auto& element : structure.elements()) {
    if (index == pixel_data_index) write_pixel_data_now();
    if (rewrite_transfer_syntax_uid && element.tag() == Tag(0x0002, 0x0010)) {
      Element rewritten(element.tag(), element.vr(), element.vr_provenance(),
                         element.length_form(), element.has_undefined_length(),
                         element.reserved_bytes(), Value::from_string("1.2.840.10008.1.2.1"));
      write_element(w, rewritten);
    } else if (file_meta_group_length.has_value() && element.tag() == Tag(0x0002, 0x0000)) {
      std::array<std::byte, 4> value_bytes{};
      internal::store_u32le(*file_meta_group_length, value_bytes.data());
      Element rewritten(element.tag(), element.vr(), element.vr_provenance(),
                         element.length_form(), element.has_undefined_length(),
                         element.reserved_bytes(),
                         Value::from_owned(
                             std::vector<std::byte>(value_bytes.begin(), value_bytes.end())));
      write_element(w, rewritten);
    } else {
      write_element(w, element);
    }
    ++index;
  }
  write_pixel_data_now();

  if (!w.ok) {
    result.status = WriteStatus::IOError;
    result.diagnostics.push_back(
        {DiagnosticSeverity::IOError, "I/O error while writing output", w.bytes, std::nullopt});
    return result;
  }

  result.status = WriteStatus::Success;
  result.bytes_written = w.bytes;
  result.source_backed_value_bytes = w.source_backed_value_bytes;
  result.regenerated_value_bytes = w.regenerated_value_bytes;
  return result;
}

}  // namespace fds
