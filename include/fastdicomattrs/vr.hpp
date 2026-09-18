#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace fds {

// The 33 standard DICOM Value Representations (PS3.5 section 6.2), plus
// Unknown for VR text this library doesn't recognize (still preserved
// structurally -- see docs/architecture.md).
enum class VR : std::uint8_t {
  AE, AS, AT, CS, DA, DS, DT, FL, FD, IS, LO, LT, OB, OD, OF, OL, OV, OW, PN,
  SH, SL, SQ, SS, ST, SV, TM, UC, UI, UL, UN, UR, US, UT, UV,
  Unknown
};

// VRs whose header uses the "long form": VR (2 bytes) + 2 reserved bytes +
// 4-byte length. All other (short-form) VRs use VR (2 bytes) + 2-byte length.
// PS3.5 Table 7.1-1.
constexpr bool is_long_form(VR vr) noexcept {
  switch (vr) {
    case VR::OB: case VR::OD: case VR::OF: case VR::OL: case VR::OV:
    case VR::OW: case VR::SQ: case VR::SV: case VR::UC: case VR::UN:
    case VR::UR: case VR::UT: case VR::UV:
      return true;
    default:
      return false;
  }
}

// Parses a two-character VR code as it appears in an Explicit VR header,
// e.g. "PN" -> VR::PN. Returns nullopt for unrecognized text (the caller
// decides how to represent that -- see Element::has_explicit_vr_in_source).
std::optional<VR> vr_from_string(std::string_view text) noexcept;

// Inverse of vr_from_string. Returns "UN" for VR::Unknown (a safe DICOM
// fallback VR), matching what an Explicit VR writer would emit.
std::string_view to_string(VR vr) noexcept;

// How an Element's VR was determined (A1.4). Distinct from the VR value
// itself: two elements can both carry VR::US while having arrived at it by
// entirely different, differently-trustworthy routes. See
// docs/architecture/A1_4_IMPLICIT_VR_SEMANTIC_COMPLETENESS_REPORT.md "VR
// provenance design" for the full reasoning.
enum class VRProvenance : std::uint8_t {
  // The VR was read directly from the wire (an Explicit VR element whose
  // 2-character VR text was recognized), or was supplied explicitly by a
  // caller via the mutation API (DICOMStructure::set) -- in both cases, a
  // VR nothing had to infer.
  Explicit,
  // The VR came from the frozen PS3.6 dictionary (fds::dictionary::lookup),
  // for an entry with no recorded ambiguity -- or, for a Sequence
  // specifically, the dictionary independently *confirmed* what the wire's
  // undefined-length encoding already made structurally certain (see
  // Structural below).
  Dictionary,
  // VR::SQ, but not sourced from the dictionary: Implicit VR's own encoding
  // rule makes undefined length legal only for Sequences (PS3.5 7.5),
  // regardless of whether the tag has a dictionary entry at all -- the
  // common case being a private or otherwise-unrecognized undefined-length
  // Sequence, which by design (A1.1) never has a dictionary entry to
  // confirm it. The VR is nonetheless certain, not guessed.
  Structural,
  // The dictionary entry was one of PS3.6's documented ambiguous forms
  // (e.g. "US or SS"), and this library's own V1-scoped resolver determined
  // the effective VR by inspecting another element in the same dataset
  // (e.g. Pixel Representation).
  ContextResolved,
  // No dictionary entry exists for this tag, or it exists but is one of the
  // ambiguous forms V1 does not attempt to resolve, or is ambiguous and the
  // context required to resolve it (ContextResolved) was not present. Never
  // a guess -- raw bytes remain fully preserved regardless.
  Unknown,
};

}  // namespace fds
