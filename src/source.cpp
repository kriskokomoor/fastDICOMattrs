#include "fastdicomattrs/source.hpp"

#include <cstdio>
#include <fstream>

#if defined(__unix__) || defined(__APPLE__)
#define FDS_HAVE_MMAP 1
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#define FDS_HAVE_MMAP 0
#endif

namespace fds {

// ---------------------------------------------------------------------------
// MemorySource
// ---------------------------------------------------------------------------

MemorySource::MemorySource(const std::byte* data, std::uint64_t size, std::vector<std::byte> owned,
                            std::string description)
    : data_(data), size_(size), owned_(std::move(owned)), description_(std::move(description)) {}

std::shared_ptr<MemorySource> MemorySource::view(const std::byte* data, std::uint64_t size,
                                                   std::string description) {
  return std::shared_ptr<MemorySource>(new MemorySource(data, size, {}, std::move(description)));
}

std::shared_ptr<MemorySource> MemorySource::own(std::vector<std::byte> bytes, std::string description) {
  // `size` must be captured before `bytes` is passed to std::move() below --
  // C++ does not guarantee left-to-right evaluation of sibling call
  // arguments, so `bytes.size()` and `std::move(bytes)` in the same call
  // could evaluate in either order. If the compiler evaluates
  // std::move(bytes) first, it move-constructs the constructor's by-value
  // `owned` parameter (emptying `bytes`) before `bytes.size()` is read,
  // silently producing size 0 instead of the real size.
  const std::uint64_t size = bytes.size();
  auto* src = new MemorySource(nullptr, size, std::move(bytes), std::move(description));
  src->data_ = src->owned_.data();
  return std::shared_ptr<MemorySource>(src);
}

bool MemorySource::try_get(SourceSpan span, const std::byte** out) const noexcept {
  if (span.end() > size_ || span.end() < span.offset()) return false;
  *out = data_ + span.offset();
  return true;
}

// ---------------------------------------------------------------------------
// FileSource
// ---------------------------------------------------------------------------

std::shared_ptr<FileSource> FileSource::open(const std::filesystem::path& path, std::string* error) {
  auto result = std::shared_ptr<FileSource>(new FileSource());
  result->description_ = path.string();

#if FDS_HAVE_MMAP
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd >= 0) {
    struct stat st{};
    if (::fstat(fd, &st) == 0 && st.st_size >= 0) {
      const auto file_size = static_cast<std::uint64_t>(st.st_size);
      if (file_size == 0) {
        // mmap() of a zero-length file is not portable; treat as an empty
        // in-memory source instead of mapping.
        ::close(fd);
        result->size_ = 0;
        return result;
      }
      void* mapped = ::mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
      ::close(fd);
      if (mapped != MAP_FAILED) {
        result->mapped_ = mapped;
        result->size_ = file_size;
        return result;
      }
      // fall through to the read()-based fallback below
    } else {
      ::close(fd);
    }
  }
#endif

  // Fallback: read the whole file into an owned buffer. Also the only path
  // taken on platforms without mmap.
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    if (error) *error = "failed to open file: " + path.string();
    return nullptr;
  }
  const std::streamoff length = file.tellg();
  if (length < 0) {
    if (error) *error = "failed to determine file size: " + path.string();
    return nullptr;
  }
  file.seekg(0);
  result->fallback_buffer_.resize(static_cast<std::size_t>(length));
  if (length > 0 &&
      !file.read(reinterpret_cast<char*>(result->fallback_buffer_.data()), length)) {
    if (error) *error = "failed to read file: " + path.string();
    return nullptr;
  }
  result->size_ = static_cast<std::uint64_t>(length);
  return result;
}

FileSource::~FileSource() {
#if FDS_HAVE_MMAP
  if (mapped_ != nullptr) {
    ::munmap(mapped_, size_);
  }
#endif
}

bool FileSource::try_get(SourceSpan span, const std::byte** out) const noexcept {
  if (span.end() > size_ || span.end() < span.offset()) return false;
  const std::byte* base =
      mapped_ != nullptr ? static_cast<const std::byte*>(mapped_) : fallback_buffer_.data();
  *out = base + span.offset();
  return true;
}

}  // namespace fds
