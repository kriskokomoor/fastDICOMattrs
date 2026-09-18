#pragma once

#include <vector>

#include "fastdicomattrs/element.hpp"
#include "fastdicomattrs/tag.hpp"

namespace fds {

// One Item of a Sequence: an ordered list of Elements, plus this item's own
// defined/undefined-length bookkeeping (independent of the parent
// Sequence's -- see docs/roundtrip-contract.md).
class Item {
 public:
  Item(std::vector<Element> elements, bool undefined_length)
      : elements_(std::move(elements)), undefined_length_(undefined_length) {}

  bool has_undefined_length() const noexcept { return undefined_length_; }
  const std::vector<Element>& elements() const noexcept { return elements_; }
  std::vector<Element>& elements() noexcept { return elements_; }

  // Direct child only (does not descend into nested sequences).
  const Element* find(Tag tag) const noexcept {
    for (const auto& e : elements_) {
      if (e.tag() == tag) return &e;
    }
    return nullptr;
  }

 private:
  std::vector<Element> elements_;
  bool undefined_length_;
};

}  // namespace fds
