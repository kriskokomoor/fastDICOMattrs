#include "fastdicomattrs/value.hpp"

#include <cstring>
#include <sstream>

#include "byte_order.hpp"

namespace fds {
namespace {

// Numeric VRs in every Transfer Syntax this increment parses are little
// endian on the wire (Explicit VR Big Endian is rejected before parsing --
// see docs/roundtrip-contract.md "Known gaps"); internal::load_*le assemble
// them correctly regardless of host endianness.
void check_size(std::span<const std::byte> bytes, std::size_t expected, std::string_view vr_name) {
  if (bytes.size() != expected) {
    std::ostringstream oss;
    oss << "value size " << bytes.size() << " does not match " << vr_name << " (expected "
        << expected << " bytes)";
    throw ValueTypeError(oss.str());
  }
}

}  // namespace

Value Value::from_string(std::string_view text) {
  std::vector<std::byte> bytes(text.size());
  std::memcpy(bytes.data(), text.data(), text.size());
  return Value::from_owned(std::move(bytes));
}

std::span<const std::byte> Value::bytes() const {
  if (is_source_backed()) {
    const std::byte* ptr = nullptr;
    if (!source_->try_get(span_, &ptr)) {
      throw ValueTypeError("source span out of range (source truncated or Value outlived Source)");
    }
    return {ptr, static_cast<std::size_t>(span_.length())};
  }
  return {owned_.data(), owned_.size()};
}

std::string Value::as_string() const {
  auto b = bytes();
  std::size_t len = b.size();
  if (len > 0) {
    auto last = static_cast<char>(b[len - 1]);
    if (last == ' ' || last == '\0') len -= 1;
  }
  return std::string(reinterpret_cast<const char*>(b.data()), len);
}

std::vector<std::string> Value::as_string_list() const {
  std::vector<std::string> out;
  std::string whole = as_string();
  std::size_t start = 0;
  while (true) {
    std::size_t pos = whole.find('\\', start);
    if (pos == std::string::npos) {
      out.push_back(whole.substr(start));
      break;
    }
    out.push_back(whole.substr(start, pos - start));
    start = pos + 1;
  }
  return out;
}

std::uint16_t Value::as_uint16() const {
  auto b = bytes();
  check_size(b, 2, "US");
  return internal::load_u16le(b.data());
}
std::int16_t Value::as_int16() const {
  auto b = bytes();
  check_size(b, 2, "SS");
  return static_cast<std::int16_t>(internal::load_u16le(b.data()));
}
std::uint32_t Value::as_uint32() const {
  auto b = bytes();
  check_size(b, 4, "UL");
  return internal::load_u32le(b.data());
}
std::int32_t Value::as_int32() const {
  auto b = bytes();
  check_size(b, 4, "SL");
  return static_cast<std::int32_t>(internal::load_u32le(b.data()));
}
float Value::as_float32() const {
  auto b = bytes();
  check_size(b, 4, "FL");
  return internal::load_f32le(b.data());
}
double Value::as_float64() const {
  auto b = bytes();
  check_size(b, 8, "FD");
  return internal::load_f64le(b.data());
}

}  // namespace fds
