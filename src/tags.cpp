#include "fastdicom/tags.hpp"

#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/dcmdata/dcmetinf.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dctk.h>

#include <charconv>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

namespace fastdicom {
namespace {

TagResult lookup(DcmFileFormat& file, Tag tag) {
  const DcmTagKey key(tag.group, tag.element);
  OFString value;

  // Group 0002 belongs to the File Meta Information, not the data set.
  DcmItem* item = tag.group == 0x0002
                      ? static_cast<DcmItem*>(file.getMetaInfo())
                      : static_cast<DcmItem*>(file.getDataset());
  const OFCondition condition = item->findAndGetOFStringArray(key, value, OFFalse);

  if (condition.good()) {
    return {tag, TagStatus::found, value.c_str(), {}};
  }
  if (condition == EC_TagNotFound) {
    return {tag, TagStatus::missing, {}, {}};
  }
  return {tag, TagStatus::error, {}, condition.text()};
}

std::vector<TagResult> loadError(const std::vector<Tag>& tags,
                                 const OFCondition& condition) {
  std::vector<TagResult> results;
  results.reserve(tags.size());
  for (const Tag tag : tags) {
    results.push_back({tag, TagStatus::error, {}, condition.text()});
  }
  return results;
}

}  // namespace

TagResult getTag(const std::filesystem::path& filename,
                 Tag tag,
                 ReadOptions options) {
  auto results = getTags(filename, std::vector<Tag>{tag}, options);
  return std::move(results.front());
}

std::vector<TagResult> getTags(const std::filesystem::path& filename,
                               const std::vector<Tag>& tags,
                               ReadOptions options) {
  if (tags.empty()) {
    return {};
  }

  DcmFileFormat file;
  const auto max_read_length = static_cast<Uint32>(options.max_value_bytes);
  const OFCondition condition = file.loadFile(
      filename.string().c_str(), EXS_Unknown, EGL_noChange, max_read_length,
      ERM_autoDetect);
  if (condition.bad()) {
    return loadError(tags, condition);
  }
  return getTags(file, tags);
}

TagResult getTag(DcmFileFormat& file, Tag tag) {
  return lookup(file, tag);
}

std::vector<TagResult> getTags(DcmFileFormat& file,
                               const std::vector<Tag>& tags) {
  std::vector<TagResult> results;
  results.reserve(tags.size());
  for (const Tag tag : tags) {
    results.push_back(lookup(file, tag));
  }
  return results;
}

bool parseTag(std::string_view text, Tag& output) noexcept {
  if (text.size() == 11 && text.front() == '(' && text.back() == ')') {
    text.remove_prefix(1);
    text.remove_suffix(1);
  }
  if (text.size() != 9 || text[4] != ',') {
    return false;
  }

  unsigned int group = 0;
  unsigned int element = 0;
  const auto group_result =
      std::from_chars(text.data(), text.data() + 4, group, 16);
  const auto element_result =
      std::from_chars(text.data() + 5, text.data() + 9, element, 16);
  if (group_result.ec != std::errc{} || group_result.ptr != text.data() + 4 ||
      element_result.ec != std::errc{} ||
      element_result.ptr != text.data() + 9 ||
      group > std::numeric_limits<std::uint16_t>::max() ||
      element > std::numeric_limits<std::uint16_t>::max()) {
    return false;
  }
  output = {static_cast<std::uint16_t>(group),
            static_cast<std::uint16_t>(element)};
  return true;
}

std::string formatTag(Tag tag) {
  std::ostringstream stream;
  stream << '(' << std::hex << std::setfill('0') << std::setw(4) << tag.group
         << ',' << std::setw(4) << tag.element << ')';
  return stream.str();
}

}  // namespace fastdicom
