#pragma once

#include <cstddef>
#include <cstdint>

namespace fds {

// Controls how much encoding detail the parser retains -- not which tags it
// visits (all three levels walk the entire structure). See
// docs/architecture.md section 6 and docs/roundtrip-contract.md.
enum class Fidelity : std::uint8_t { Fast, Standard, Lossless };

struct ParseOptions {
  Fidelity fidelity = Fidelity::Standard;

  // Malformed-input guards, not structural limits: a hostile or corrupt
  // file with a bogus length can otherwise induce unbounded allocation or
  // unbounded recursion before the parser notices anything is wrong.
  // max_element_count is parse-wide -- it counts elements at every nesting
  // depth (top-level and nested inside Items/Sequences alike), not just
  // the top-level element list.
  std::size_t max_element_count = 1'000'000;
  std::size_t max_sequence_depth = 64;

  // Reserved FAST-mode hint; a no-op in this increment. See
  // docs/architecture.md section 6 ("FAST").
  bool stop_before_pixel_data = false;

  // A bare dataset (no 128-byte preamble, no File Meta Information group
  // 0002 -- so no (0002,0010) Transfer Syntax UID to read) has no wire
  // signal at all for which Transfer Syntax it uses. Without this hint set,
  // such a source is assumed to be Explicit VR Little Endian (unchanged
  // default behavior, with its own Warning diagnostic). Setting this to
  // true tells the parser to assume Implicit VR Little Endian instead, for
  // this bare-dataset case only -- it never overrides an actual
  // (0002,0010) value when File Meta is present, and the parser never
  // tries to auto-detect the Transfer Syntax from the byte content itself;
  // this is a caller-supplied fact about a specific input, not a guess (A1.4
  // -- see docs/architecture/A1_4_IMPLICIT_VR_SEMANTIC_COMPLETENESS_
  // REPORT.md "Bare datasets").
  bool bare_dataset_is_implicit_vr = false;
};

}  // namespace fds
