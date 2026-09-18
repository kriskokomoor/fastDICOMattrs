#pragma once

// A1.7: the single authoritative implementation of "walk a container-locator
// path and return the target container to insert into." Every insertion
// layer (DICOMStructure::insert(), fds::mutation::insert_inferred(),
// fds::charset::insert_text()/insert_text_inferred()) goes through this one
// traversal -- see docs/architecture/A1_7_MUTATION_ERGONOMICS_AND_PUBLIC_API_REPORT.md
// section 4 ("centralize container-location semantics").
//
// This is an internal, unexported helper (src/internal/, not include/) --
// ContainerLocateStatus is a private implementation detail, not a public
// API. Public layers translate it into whatever status model they already
// expose (bool for DICOMStructure, InsertStatus for fds::mutation,
// SetTextStatus for fds::charset) rather than exposing it directly.
//
// Contract for `parent`: an ElementPath used in its second, narrower shape
// (see element_path.hpp and dicom_structure.hpp's insert() doc comment) --
// *every* step, with no exception for the last, must be a descent step
// (Sequence tag + Item index). An empty `parent` names the root dataset.
// This is deliberately distinct from ElementPath's other, more common use
// as an existing-element locator (find/set_value/erase), where the last
// step is bare (no item_index) and names the leaf itself -- do not pass an
// element-locator path here, and do not pass a container-locator path to
// find()/set_value()/erase().

#include <vector>

#include "fastdicomattrs/element.hpp"
#include "fastdicomattrs/element_path.hpp"

namespace fds::internal {

enum class ContainerLocateStatus : std::uint8_t {
  Success,
  ContainerNotFound,    // some step's tag doesn't exist in its enclosing container
  NotASequence,         // some step's tag exists but isn't a Sequence element
  ItemIndexOutOfRange,  // some step's item_index has no corresponding Item
};

struct ContainerLocateResult {
  ContainerLocateStatus status = ContainerLocateStatus::ContainerNotFound;
  // Set iff status == Success.
  std::vector<Element>* container = nullptr;
};

ContainerLocateResult locate_container(std::vector<Element>& top, const ElementPath& parent);

// Read-only counterpart, for callers (fds::charset) that only need to walk
// the container chain without mutating anything.
struct ConstContainerLocateResult {
  ContainerLocateStatus status = ContainerLocateStatus::ContainerNotFound;
  const std::vector<Element>* container = nullptr;
};

ConstContainerLocateResult locate_container_const(const std::vector<Element>& top,
                                                    const ElementPath& parent);

}  // namespace fds::internal
