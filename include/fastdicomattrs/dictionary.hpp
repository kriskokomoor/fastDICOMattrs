#pragma once

// The standard PS3.6 data dictionary: tag -> VR (or explicit ambiguity) and
// keyword. See docs/architecture/A1_1_DICTIONARY_SUBSTRATE_REPORT.md for
// full provenance, scope, and the reasoning behind every design choice
// below.
//
// This is A1.1's deliverable: a correct, provenance-tracked lookup surface.
// It is deliberately NOT wired into parsing, mutation, serialization, or
// any other existing behavior -- nothing in this codebase calls lookup()
// yet. That wiring (dictionary-backed VR resolution for Implicit VR,
// specifically) is A1.4's job, per docs/architecture/
// A1_ATTRS_V1_IMPLEMENTATION_PROGRESSION.md.
//
// Scope, precisely: this dictionary contains only what PS3.6's Table 6-1
// ("Registry of DICOM Data Elements") defines -- standard (including
// DICONDE/DICOS companion-standard) data elements, current and retired,
// including the three group-level repeating patterns (Overlay Data 60xx,
// retired Curve Data 50xx, retired Variable Pixel Data 7Fxx). It contains:
//   * no private/vendor-specific tag semantics of any kind, by design --
//     private (odd-group) tags always return nullopt from lookup();
//   * no DICOM Command elements (PS3.7) or File Meta elements (PS3.6 Table
//     7-1) -- a different registry from the one this dictionary reflects;
//   * no structural pseudo-tags (Item/Item-Delimitation/Sequence-
//     Delimitation) -- those are encoding framing, not data elements, and
//     are already handled structurally elsewhere (see Tag::
//     is_item_or_delimiter() in tag.hpp);
//   * no VM (Value Multiplicity) enforcement -- this dictionary does not
//     expose VM at all; nothing here validates a value's multiplicity
//     against the standard's declared VM, by design (see the report's
//     "why not VM" rationale).

#include <cstdint>
#include <optional>
#include <string_view>

#include "fastdicomattrs/tag.hpp"
#include "fastdicomattrs/vr.hpp"

namespace fds::dictionary {

// Some standard tags have a VR that PS3.6 itself declares ambiguous --
// resolvable only by inspecting other elements at read time (most commonly
// Pixel Representation (0028,0103) for the "US or SS" family). This
// dictionary represents that ambiguity explicitly rather than guessing one
// VR; the actual context-dependent resolution rule belongs to a later
// increment (A1.4), not here.
enum class VRAmbiguity : std::uint8_t {
  None,          // vr is definitive
  USorSS,
  OBorOW,
  USorOW,
  USorSSorOW,
};

struct DictionaryEntry {
  // VR::Unknown exactly when ambiguity != VRAmbiguity::None -- callers must
  // check `ambiguity` before trusting `vr` as a definitive answer.
  VR vr = VR::Unknown;
  VRAmbiguity ambiguity = VRAmbiguity::None;
  // Empty for the small number of standard entries with no recorded
  // keyword. Points into a static, program-lifetime string pool -- never
  // dangles, never needs freeing.
  std::string_view keyword;
  bool retired = false;
};

// Looks up `tag` in the standard PS3.6 data dictionary. Returns nullopt for
// any tag not covered by this dictionary's scope (see this header's
// top-of-file scope note) -- including every private (odd-group) tag,
// every Command/File-Meta-registry tag, and the small set of obscure
// element-level repeating patterns this dictionary deliberately does not
// support (see tools/generate_dictionary.py's EXCLUDED_ELEMENT_PATTERNS).
//
// O(log n) binary search over the exact-tag table (n ~5,190 as of the
// pinned edition), falling back to a short (~71-entry) linear scan of the
// group-level repeating-pattern rules only when the exact lookup misses.
[[nodiscard]] std::optional<DictionaryEntry> lookup(Tag tag) noexcept;

}  // namespace fds::dictionary
