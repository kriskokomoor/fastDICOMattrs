#pragma once

#include <vector>

#include "fastdicomattrs/item.hpp"

namespace fds {

// The complete value of one VR::SQ Element: an ordered list of Items, plus
// this sequence's own defined/undefined-length bookkeeping.
class Sequence {
 public:
  Sequence(std::vector<Item> items, bool undefined_length)
      : items_(std::move(items)), undefined_length_(undefined_length) {}

  bool has_undefined_length() const noexcept { return undefined_length_; }
  const std::vector<Item>& items() const noexcept { return items_; }
  std::vector<Item>& items() noexcept { return items_; }

 private:
  std::vector<Item> items_;
  bool undefined_length_;
};

}  // namespace fds
