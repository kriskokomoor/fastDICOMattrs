#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "fastdicomattrs/source_span.hpp"
#include "fastdicomattrs/tag.hpp"
#include "fastdicomattrs/transfer_syntax.hpp"
#include "fastdicomattrs/vr.hpp"

namespace fds {

struct PixelDataFragment {
  SourceSpan span;
};

// Location/extent/encoding of the dataset's Pixel Data element, without
// decoding it. See docs/architecture.md section 8. Pixel bytes are never
// materialized into an owned buffer by this library.
class PixelDataReference {
 public:
  static PixelDataReference native(VR vr, std::array<std::byte, 2> reserved,
                                    TransferSyntax transfer_syntax, SourceSpan span);
  static PixelDataReference encapsulated(VR vr, std::array<std::byte, 2> reserved,
                                          TransferSyntax transfer_syntax,
                                          std::optional<SourceSpan> basic_offset_table,
                                          std::vector<PixelDataFragment> fragments);

  Tag tag() const noexcept { return kPixelDataTag; }
  VR vr() const noexcept { return vr_; }
  // The 2 reserved header bytes after the VR (should be 0x0000 per the
  // standard, but retained verbatim for LOSSLESS fidelity regardless).
  std::array<std::byte, 2> reserved_bytes() const noexcept { return reserved_; }
  const TransferSyntax& transfer_syntax() const noexcept { return transfer_syntax_; }
  bool is_encapsulated() const noexcept { return encapsulated_; }

  // Precondition: !is_encapsulated().
  SourceSpan native_span() const noexcept { return native_span_; }

  // Both precondition: is_encapsulated().
  const std::optional<SourceSpan>& basic_offset_table() const noexcept { return basic_offset_table_; }
  const std::vector<PixelDataFragment>& fragments() const noexcept { return fragments_; }

 private:
  PixelDataReference() = default;

  VR vr_ = VR::OB;
  std::array<std::byte, 2> reserved_{};
  TransferSyntax transfer_syntax_ = TransferSyntax::from_uid("");
  bool encapsulated_ = false;
  SourceSpan native_span_{};
  std::optional<SourceSpan> basic_offset_table_;
  std::vector<PixelDataFragment> fragments_;
};

}  // namespace fds
