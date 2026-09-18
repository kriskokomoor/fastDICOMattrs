#include "parser/byte_reader.hpp"

#include "byte_order.hpp"

namespace fds::parser {

bool ByteReader::read_span(std::uint64_t count, SourceSpan* out) noexcept {
  if (pos_ + count > end_ || pos_ + count < pos_) return false;  // also guards overflow
  *out = SourceSpan(pos_, count);
  pos_ += count;
  return true;
}

bool ByteReader::read_u16le(std::uint16_t* out) noexcept {
  SourceSpan span;
  if (!read_span(2, &span)) return false;
  const std::byte* ptr = nullptr;
  if (!source_.try_get(span, &ptr)) return false;
  *out = internal::load_u16le(ptr);
  return true;
}

bool ByteReader::read_u32le(std::uint32_t* out) noexcept {
  SourceSpan span;
  if (!read_span(4, &span)) return false;
  const std::byte* ptr = nullptr;
  if (!source_.try_get(span, &ptr)) return false;
  *out = internal::load_u32le(ptr);
  return true;
}

bool ByteReader::peek_u32le_at(std::uint64_t offset, std::uint32_t* out) const noexcept {
  const std::byte* ptr = nullptr;
  if (!source_.try_get(SourceSpan(offset, 4), &ptr)) return false;
  *out = internal::load_u32le(ptr);
  return true;
}

}  // namespace fds::parser
