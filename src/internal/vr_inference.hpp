#pragma once

// A1.7's VR-inference policy, in one place, shared by
// fds::mutation::insert_inferred() and fds::charset::insert_text_inferred()
// -- see docs/architecture/A1_7_MUTATION_ERGONOMICS_AND_PUBLIC_API_REPORT.md
// section 5/6 for the full policy derivation and its justification. An
// internal, unexported helper (src/internal/, not include/) -- not a public
// API.

#include <cstdint>

#include "fastdicomattrs/tag.hpp"
#include "fastdicomattrs/vr.hpp"

namespace fds::internal {

enum class VRInferenceOutcome : std::uint8_t {
  Inferred,  // `vr` is authoritative -- safe to use without caller confirmation
  Required,  // ambiguous, unknown to the dictionary, or private data -- caller must supply a VR
};

struct VRInferenceResult {
  VRInferenceOutcome outcome = VRInferenceOutcome::Required;
  VR vr = VR::Unknown;  // meaningful iff outcome == Inferred
};

// Policy, exactly:
//   * a Private Creator declaration (odd group, element 0x0010-0x00FF) ->
//     Inferred(LO) -- not a dictionary guess: PS3.5 7.8.1 normatively
//     requires every Private Creator value to be LO, and A1.2's
//     PrivateElementKind::Creator already classifies this structural
//     position independently of any dictionary lookup.
//   * any other private (odd-group) tag -> Required. The standard
//     dictionary never has an entry for private data by design (see
//     dictionary.hpp) -- there is nothing to infer from.
//   * a standard tag absent from the dictionary -> Required.
//   * a standard tag with an ambiguous VR (dictionary::VRAmbiguity != None,
//     e.g. "US or SS") -> Required. A1.4's context-resolution rules
//     (reading a sibling element such as Pixel Representation) are
//     deliberately NOT reused here -- that reads context that may not
//     exist yet at insertion time, and reusing it would reintroduce
//     exactly the "guess instead of ask" pattern V1 avoids elsewhere.
//   * a standard tag with one unambiguous VR -> Inferred(that VR).
VRInferenceResult infer_vr(Tag tag);

}  // namespace fds::internal
