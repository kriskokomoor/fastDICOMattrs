#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

class DcmFileFormat;

namespace fastdicom {

struct Tag {
  std::uint16_t group{};
  std::uint16_t element{};

  friend constexpr bool operator==(Tag lhs, Tag rhs) noexcept {
    return lhs.group == rhs.group && lhs.element == rhs.element;
  }
};

enum class TagStatus {
  found,
  missing,
  error,
};

struct TagResult {
  Tag tag{};
  TagStatus status{TagStatus::missing};
  std::string value;
  std::string message;

  [[nodiscard]] bool has_value() const noexcept {
    return status == TagStatus::found;
  }
};

// Loads only values up to max_value_bytes. Large values (most importantly Pixel
// Data) remain on disk, avoiding unnecessary allocation while metadata is read.
struct ReadOptions {
  std::uint32_t max_value_bytes{4096};
};

[[nodiscard]] TagResult getTag(const std::filesystem::path& filename,
                               Tag tag,
                               ReadOptions options = {});

// The file is parsed once, no matter how many tags are requested. Results are
// returned in input order, including duplicates.
[[nodiscard]] std::vector<TagResult> getTags(
    const std::filesystem::path& filename,
    const std::vector<Tag>& tags,
    ReadOptions options = {});

// Handle-based overloads for callers that reuse an already-loaded DICOM file.
// The caller retains ownership and must keep the object alive for the call.
[[nodiscard]] TagResult getTag(DcmFileFormat& file, Tag tag);
[[nodiscard]] std::vector<TagResult> getTags(DcmFileFormat& file,
                                             const std::vector<Tag>& tags);

// Parses "GGGG,EEEE" or "(GGGG,EEEE)" as hexadecimal DICOM tag numbers.
[[nodiscard]] bool parseTag(std::string_view text, Tag& output) noexcept;
[[nodiscard]] std::string formatTag(Tag tag);

}  // namespace fastdicom
