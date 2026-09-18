#pragma once

// Internal helper: portable little-endian byte assembly, independent of
// host endianness. DICOM Transfer Syntaxes parsed by this library are all
// little endian on the wire (see docs/roundtrip-contract.md "Known gaps"),
// but the host running this code is not guaranteed to be.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace fds::internal {

inline std::uint16_t load_u16le(const std::byte* p) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint8_t>(p[0])) |
         (static_cast<std::uint16_t>(static_cast<std::uint8_t>(p[1])) << 8);
}

inline std::uint32_t load_u32le(const std::byte* p) noexcept {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[0])) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[3])) << 24);
}

inline float load_f32le(const std::byte* p) noexcept {
  std::uint32_t bits = load_u32le(p);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline double load_f64le(const std::byte* p) noexcept {
  std::uint64_t bits = static_cast<std::uint64_t>(load_u32le(p)) |
                        (static_cast<std::uint64_t>(load_u32le(p + 4)) << 32);
  double value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline void store_u16le(std::uint16_t v, std::byte* out) noexcept {
  out[0] = static_cast<std::byte>(v & 0xFF);
  out[1] = static_cast<std::byte>((v >> 8) & 0xFF);
}

inline void store_u32le(std::uint32_t v, std::byte* out) noexcept {
  out[0] = static_cast<std::byte>(v & 0xFF);
  out[1] = static_cast<std::byte>((v >> 8) & 0xFF);
  out[2] = static_cast<std::byte>((v >> 16) & 0xFF);
  out[3] = static_cast<std::byte>((v >> 24) & 0xFF);
}

}  // namespace fds::internal
