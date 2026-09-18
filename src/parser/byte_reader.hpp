#pragma once

#include <cstdint>

#include "fastdicomattrs/source.hpp"
#include "fastdicomattrs/source_span.hpp"

namespace fds::parser {

// A bounds-checked forward cursor over a byte range of a Source. Never
// throws; every read reports success/failure so the parser can turn a
// truncated file into a RecoverableError diagnostic instead of a crash.
class ByteReader {
 public:
  ByteReader(const Source& source, std::uint64_t start, std::uint64_t end) noexcept
      : source_(source), pos_(start), end_(end) {}

  std::uint64_t position() const noexcept { return pos_; }
  // Rewinds/advances the cursor to an already-validated position (used for
  // tag lookahead: save position, read, restore). Callers must only pass a
  // value previously returned by position() on this reader.
  void seek(std::uint64_t pos) noexcept { pos_ = pos; }
  std::uint64_t end() const noexcept { return end_; }
  std::uint64_t remaining() const noexcept { return end_ > pos_ ? end_ - pos_ : 0; }
  bool at_end() const noexcept { return pos_ >= end_; }

  // Advances the cursor by `count` bytes and returns the span it covered.
  // Fails (returns false, cursor unchanged) if that would run past `end`.
  bool read_span(std::uint64_t count, SourceSpan* out) noexcept;

  bool read_u16le(std::uint16_t* out) noexcept;
  bool read_u32le(std::uint32_t* out) noexcept;

  // Peeks 4 bytes at an absolute offset without moving the cursor. Used for
  // the DICM magic / preamble probe.
  bool peek_u32le_at(std::uint64_t offset, std::uint32_t* out) const noexcept;

 private:
  const Source& source_;
  std::uint64_t pos_;
  std::uint64_t end_;
};

}  // namespace fds::parser
