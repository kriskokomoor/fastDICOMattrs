#include "fastdicomattrs/dictionary.hpp"

#include <algorithm>

#include "dictionary_data.generated.hpp"

namespace fds::dictionary {

namespace {

DictionaryEntry to_entry(const detail::RawEntry& raw) {
  return DictionaryEntry{
      raw.vr,
      raw.ambiguity,
      std::string_view(detail::kKeywordPool + raw.keyword_offset, raw.keyword_length),
      raw.retired,
  };
}

DictionaryEntry to_entry(const detail::RepeatingRule& rule) {
  return DictionaryEntry{
      rule.vr,
      rule.ambiguity,
      std::string_view(detail::kKeywordPool + rule.keyword_offset, rule.keyword_length),
      rule.retired,
  };
}

std::optional<DictionaryEntry> lookup_exact(std::uint32_t tag_u32) noexcept {
  const detail::RawEntry* begin = detail::kExactEntries;
  const detail::RawEntry* end = detail::kExactEntries + detail::kExactEntryCount;
  const auto it = std::lower_bound(
      begin, end, tag_u32,
      [](const detail::RawEntry& entry, std::uint32_t value) { return entry.tag < value; });
  if (it != end && it->tag == tag_u32) {
    return to_entry(*it);
  }
  return std::nullopt;
}

std::optional<DictionaryEntry> lookup_repeating(Tag tag) noexcept {
  if ((tag.group & 0x1u) != 0) {
    return std::nullopt;  // every repeating-group pattern this dictionary supports is even-only
  }
  for (std::size_t i = 0; i < detail::kRepeatingRuleCount; ++i) {
    const detail::RepeatingRule& rule = detail::kRepeatingRules[i];
    if (tag.group >= rule.group_first && tag.group <= rule.group_last &&
        tag.element == rule.element) {
      return to_entry(rule);
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<DictionaryEntry> lookup(Tag tag) noexcept {
  if (auto exact = lookup_exact(tag.to_uint32())) {
    return exact;
  }
  return lookup_repeating(tag);
}

}  // namespace fds::dictionary
