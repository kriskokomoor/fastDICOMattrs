#pragma once

// Data-shape declarations for the generated dictionary table
// (dictionary_data.generated.cpp, produced by tools/generate_dictionary.py).
// This header is hand-written and stable; only the .cpp defining the actual
// table contents is generated. Internal to the dictionary subsystem -- not
// part of the public API (see include/fastdicomattrs/dictionary.hpp for
// that).

#include <cstddef>
#include <cstdint>

#include "fastdicomattrs/dictionary.hpp"  // for VRAmbiguity
#include "fastdicomattrs/vr.hpp"

namespace fds::dictionary {
namespace detail {

// One exact-tag entry. `keyword_offset`/`keyword_length` index into
// kKeywordPool rather than owning a string, so the entire table (and pool)
// can be plain compiled-in static data with zero runtime construction cost.
struct RawEntry {
  std::uint32_t tag;  // (group << 16) | element
  VR vr;              // VR::Unknown when ambiguity != None
  VRAmbiguity ambiguity;
  bool retired;
  std::uint32_t keyword_offset;
  std::uint16_t keyword_length;
};

// One repeating-group rule: matches any tag whose group is even and within
// [group_first, group_last], and whose element equals `element` exactly.
// See PROVENANCE.md for how the three real patterns' (50xx/60xx/7Fxx) group
// ranges were determined.
struct RepeatingRule {
  std::uint16_t group_first;
  std::uint16_t group_last;  // inclusive
  std::uint16_t element;
  VR vr;
  VRAmbiguity ambiguity;
  bool retired;
  std::uint32_t keyword_offset;
  std::uint16_t keyword_length;
};

extern const char kKeywordPool[];
extern const RawEntry kExactEntries[];       // sorted ascending by `tag`
extern const RepeatingRule kRepeatingRules[];
extern const std::size_t kExactEntryCount;
extern const std::size_t kRepeatingRuleCount;

}  // namespace detail
}  // namespace fds::dictionary
