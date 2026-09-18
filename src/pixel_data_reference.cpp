#include "fastdicomattrs/pixel_data_reference.hpp"

namespace fds {

PixelDataReference PixelDataReference::native(VR vr, std::array<std::byte, 2> reserved,
                                               TransferSyntax transfer_syntax, SourceSpan span) {
  PixelDataReference ref;
  ref.vr_ = vr;
  ref.reserved_ = reserved;
  ref.transfer_syntax_ = std::move(transfer_syntax);
  ref.encapsulated_ = false;
  ref.native_span_ = span;
  return ref;
}

PixelDataReference PixelDataReference::encapsulated(VR vr, std::array<std::byte, 2> reserved,
                                                      TransferSyntax transfer_syntax,
                                                      std::optional<SourceSpan> basic_offset_table,
                                                      std::vector<PixelDataFragment> fragments) {
  PixelDataReference ref;
  ref.vr_ = vr;
  ref.reserved_ = reserved;
  ref.transfer_syntax_ = std::move(transfer_syntax);
  ref.encapsulated_ = true;
  ref.basic_offset_table_ = basic_offset_table;
  ref.fragments_ = std::move(fragments);
  return ref;
}

}  // namespace fds
