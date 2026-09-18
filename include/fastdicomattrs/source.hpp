#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "fastdicomattrs/source_span.hpp"

namespace fds {

// Abstract byte-provider backing zero-copy Values. See docs/architecture.md
// section 4 for the full lifetime contract: a DICOMStructure holds a
// shared_ptr<const Source>, and every SourceSpan-backed Value inside it is
// only valid as long as that shared_ptr (or a copy) is kept alive.
class Source {
 public:
  virtual ~Source() = default;

  virtual std::uint64_t size() const noexcept = 0;

  // Bounds-checked, non-throwing resolution of a span to a raw pointer.
  // Returns false (and leaves *out untouched) if the span is out of range.
  virtual bool try_get(SourceSpan span, const std::byte** out) const noexcept = 0;

  // Convenience for callers that have already validated the span (e.g. the
  // writer, re-reading a span it just wrote via the parser's own bounds).
  // Precondition: try_get() for this span would succeed.
  const std::byte* data(SourceSpan span) const noexcept {
    const std::byte* ptr = nullptr;
    [[maybe_unused]] bool ok = try_get(span, &ptr);
    return ptr;
  }

  // Human-readable origin, for diagnostics only (e.g. a file path).
  virtual std::string_view description() const noexcept = 0;
};

// A Source backed entirely by memory: either a non-owning view over a
// caller-supplied buffer, or a buffer this object owns.
class MemorySource final : public Source {
 public:
  static std::shared_ptr<MemorySource> view(const std::byte* data, std::uint64_t size,
                                             std::string description = {});
  static std::shared_ptr<MemorySource> own(std::vector<std::byte> bytes,
                                            std::string description = {});

  std::uint64_t size() const noexcept override { return size_; }
  bool try_get(SourceSpan span, const std::byte** out) const noexcept override;
  std::string_view description() const noexcept override { return description_; }

 private:
  MemorySource(const std::byte* data, std::uint64_t size, std::vector<std::byte> owned,
               std::string description);

  const std::byte* data_;
  std::uint64_t size_;
  std::vector<std::byte> owned_;  // empty if this is a non-owning view
  std::string description_;
};

// A Source backed by a file on disk. Memory-maps the file for zero-copy
// access where the platform supports it (POSIX mmap); falls back to
// reading the whole file into an owned buffer otherwise. Callers cannot
// observe which strategy was used other than through description().
class FileSource final : public Source {
 public:
  // Returns nullptr and sets *error on failure (file not found, permission
  // denied, etc.) -- this is an I/O boundary, so it reports rather than
  // throws, matching the "no exceptions across meaningful boundaries" spirit
  // even though FileSource itself is pure C++ (not ABI-facing).
  static std::shared_ptr<FileSource> open(const std::filesystem::path& path, std::string* error);

  ~FileSource() override;

  std::uint64_t size() const noexcept override { return size_; }
  bool try_get(SourceSpan span, const std::byte** out) const noexcept override;
  std::string_view description() const noexcept override { return description_; }

  bool is_memory_mapped() const noexcept { return mapped_ != nullptr; }

 private:
  FileSource() = default;

  void* mapped_ = nullptr;   // mmap base, or nullptr if using the fallback buffer
  std::uint64_t size_ = 0;
  std::vector<std::byte> fallback_buffer_;  // used iff mapped_ == nullptr
  std::string description_;
};

}  // namespace fds
