#include "internal/container_locate.hpp"

#include <algorithm>

#include "fastdicomattrs/sequence.hpp"

namespace fds::internal {
namespace {

// Shared shape between the mutable and const walks below -- identical
// traversal, differing only in constness (std::find_if's return type
// naturally tracks whether `current` is const-qualified, so no manual
// const/non-const duplication is needed). Written once so there is exactly
// one place that encodes "how to walk a container-locator path," matching
// this file's whole reason for existing.
template <typename ElementsVec>
ContainerLocateStatus walk(ElementsVec*& current, const ElementPath& parent) {
  for (const auto& step : parent.steps()) {
    // A container-locator path's contract (see container_locate.hpp):
    // every step must be a descent step. A bare final step (the
    // element-locator shape find()/set_value()/erase() use) is a caller
    // error, not a legitimate "container not found" -- but since
    // ElementPath itself cannot express "which shape a given instance is,"
    // a missing item_index here is reported the same way any other
    // unresolvable step is: the step doesn't identify a container.
    if (!step.item_index.has_value()) return ContainerLocateStatus::ContainerNotFound;

    auto it = std::find_if(current->begin(), current->end(),
                            [&](const auto& e) { return e.tag() == step.tag; });
    if (it == current->end()) return ContainerLocateStatus::ContainerNotFound;
    if (!it->is_sequence()) return ContainerLocateStatus::NotASequence;

    auto& items = it->sequence().items();
    if (*step.item_index >= items.size()) return ContainerLocateStatus::ItemIndexOutOfRange;
    current = &items[*step.item_index].elements();
  }
  return ContainerLocateStatus::Success;
}

}  // namespace

ContainerLocateResult locate_container(std::vector<Element>& top, const ElementPath& parent) {
  ContainerLocateResult result;
  std::vector<Element>* current = &top;
  result.status = walk(current, parent);
  if (result.status == ContainerLocateStatus::Success) result.container = current;
  return result;
}

ConstContainerLocateResult locate_container_const(const std::vector<Element>& top,
                                                    const ElementPath& parent) {
  ConstContainerLocateResult result;
  const std::vector<Element>* current = &top;
  result.status = walk(current, parent);
  if (result.status == ContainerLocateStatus::Success) result.container = current;
  return result;
}

}  // namespace fds::internal
