#pragma once

// A1.7 -- the dictionary-aware convenience layer on top of DICOMStructure's
// dictionary-agnostic structural primitives (find/set_value/insert/erase).
// See docs/architecture/A1_7_MUTATION_ERGONOMICS_AND_PUBLIC_API_REPORT.md
// section 3 for the layering rationale: DICOMStructure itself never depends
// on fds::dictionary (exactly as it never has, even before A1.7 -- see
// dicom_structure.hpp's set() doc comment, "there is no dictionary to infer
// one from"); this namespace is where that dependency is allowed to exist.

#include "fastdicomattrs/dicom_structure.hpp"
#include "fastdicomattrs/element_path.hpp"
#include "fastdicomattrs/tag.hpp"
#include "fastdicomattrs/value.hpp"

namespace fds::mutation {

enum class InsertStatus : std::uint8_t {
  Success,
  ContainerNotFound,    // some step of `parent` doesn't resolve to an existing Sequence Item
  NotASequence,          // some step of `parent` names a tag that exists but isn't a Sequence
  ItemIndexOutOfRange,   // some step of `parent` names an Item index with no corresponding Item
  AlreadyExists,          // `tag` already exists in the target container
  VRRequired,             // `tag`'s VR cannot be safely inferred -- see infer_vr's policy
  InvalidVR,              // caller-unreachable in insert_inferred() (never emits SQ/Unknown itself)
  ValueTooLong,           // `value` exceeds the VR's wire length-form ceiling (after auto-padding -- see below)
};

// A1.7 section 19 (auto-padding audit): unlike DICOMStructure::insert()/
// set()/set_value() (the raw structural primitives, whose contract this
// does not change -- an odd-length `value` there is still the caller's
// responsibility, exactly as before A1.7), both functions below auto-pad
// an odd-length `value` with PS3.5 6.4's correct trailing pad byte for the
// resolved VR (SPACE for text-natured VRs, NUL for VR::UI and every binary
// VR) before insertion -- the same thing charset::encode_text already does
// for its seven text VRs. This is safe and unambiguous specifically
// *because* this convenience layer always knows the final VR by the time
// padding would matter (caller-supplied for insert(), inferred for
// insert_inferred()); the raw structural primitives below it deliberately
// keep their pre-A1.7 no-auto-pad contract unchanged -- see this header's
// top-of-file note on that layering boundary.


// The explicit-VR counterpart to insert_inferred() below: identical
// container resolution, duplicate, and value-encodability checks (the one
// centralized implementation both share -- see
// docs/architecture/A1_7_MUTATION_ERGONOMICS_AND_PUBLIC_API_REPORT.md
// section 4), but `vr` is caller-supplied and authoritative -- never
// consulted against the dictionary, exactly like DICOMStructure::insert()
// itself (which this function wraps). This exists purely to give a status-
// needing caller (concretely, the C ABI's fds_structure_insert_path, which
// needs to distinguish FDS_STATUS_ALREADY_EXISTS from other failures for
// both its explicit- and inferred-VR modes) the same diagnostic richness
// for the explicit-VR case that insert_inferred() already has for the
// inferred case -- it is not a hybrid "explicit VR with dictionary
// fallback" operation, and never inspects the dictionary at all.
InsertStatus insert(DICOMStructure& structure, const ElementPath& parent, Tag tag, VR vr,
                     Value value);

// Inserts a brand-new element at `tag` inside the container named by
// `parent` (a container-locator ElementPath -- see
// DICOMStructure::insert()'s doc comment for its exact contract: every step
// is a descent step, empty means root), inferring `tag`'s VR from the
// standard PS3.6 dictionary rather than requiring the caller to supply one.
//
// VR-inference policy (see fds::internal::infer_vr, the single shared
// implementation of this rule -- also used by
// fds::charset::insert_text_inferred, so there is exactly one inference
// rule in this library, not two):
//   * a standard tag with one unambiguous dictionary VR -> inferred, used.
//   * a standard tag with an ambiguous VR (e.g. "US or SS") -> VRRequired.
//     A1.4's sibling-context resolution (e.g. reading Pixel Representation)
//     is deliberately NOT reused during insertion -- see the freeze
//     report's VR-inference-policy section for why guessing here would be
//     the wrong tradeoff. A caller who wants an ambiguous VR must call
//     DICOMStructure::insert() directly with an explicit VR.
//   * a tag absent from the dictionary -> VRRequired.
//   * a private (odd-group) data element -> VRRequired always (the
//     dictionary never has an entry for private data, by design).
//   * a Private Creator declaration (odd group, element 0x0010-0x00FF) ->
//     inferred as VR::LO -- a normative structural fact from PS3.5 7.8.1,
//     not a dictionary guess (see A1.2's PrivateElementKind::Creator).
//
// This function never silently substitutes a VR a caller explicitly
// requested -- there is no explicit-VR parameter here at all; a caller who
// wants to pin the VR themselves calls DICOMStructure::insert() directly.
// The two are separate, non-hybrid operations by design.
//
// Atomic: every check (container resolution, duplicate check, VR
// inference, value encodability) completes before `structure` is touched;
// any non-Success outcome leaves it completely unchanged.
InsertStatus insert_inferred(DICOMStructure& structure, const ElementPath& parent, Tag tag,
                              Value value);

}  // namespace fds::mutation
