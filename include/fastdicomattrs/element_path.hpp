#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "fastdicomattrs/tag.hpp"

namespace fds {

// Uniquely names an element that may be nested inside sequences/items,
// where Tag alone is ambiguous (the same tag can appear once per item,
// across many items, at many nesting depths). See docs/api-design.md.
//
// A path is a list of steps. Every step but the last must name a
// Sequence-VR element and specify which Item to descend into; the last
// step names the element itself.
class ElementPath {
 public:
  struct Step {
    Tag tag;
    std::optional<std::size_t> item_index;  // unset only for the final step
  };

  ElementPath() = default;
  explicit ElementPath(Tag top_level_tag) { steps_.push_back({top_level_tag, std::nullopt}); }

  // Descend: `tag` must be a Sequence element; `item_index` selects which
  // Item of it to continue into.
  ElementPath& push(Tag tag, std::size_t item_index) {
    steps_.push_back({tag, item_index});
    return *this;
  }
  // Final step: address the element itself (which may itself be a Sequence
  // element, if the caller wants to name the sequence as a whole).
  ElementPath& push(Tag tag) {
    steps_.push_back({tag, std::nullopt});
    return *this;
  }

  const std::vector<Step>& steps() const noexcept { return steps_; }
  bool empty() const noexcept { return steps_.empty(); }

  std::string to_string() const;

 private:
  std::vector<Step> steps_;
};

}  // namespace fds
