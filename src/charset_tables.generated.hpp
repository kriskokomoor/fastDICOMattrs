#pragma once

// Data-shape declaration for the generated single-byte character-repertoire
// tables (charset_tables.generated.cpp, produced by
// tools/generate_charset_tables.py). This header is hand-written and
// stable; only the .cpp defining the actual table contents is generated.
// Internal to the charset subsystem -- not part of the public API.

#include <cstddef>
#include <cstdint>

namespace fds::charset::detail {

// Each table holds the Unicode code point for bytes 0x80-0xFF (index 0 =
// byte 0x80); bytes 0x00-0x7F are always identical to plain ASCII for
// every one of these repertoires (verified by the generator). A value of
// 0 means "undefined in this repertoire" -- none of the ten repertoires
// legitimately maps any byte to U+0000.
extern const char32_t kLatin1High[128];    // ISO_IR 100 / ISO 2022 IR 100
extern const char32_t kLatin2High[128];    // ISO_IR 101 / ISO 2022 IR 101
extern const char32_t kLatin3High[128];    // ISO_IR 109 / ISO 2022 IR 109
extern const char32_t kLatin4High[128];    // ISO_IR 110 / ISO 2022 IR 110
extern const char32_t kCyrillicHigh[128];  // ISO_IR 144 / ISO 2022 IR 144
extern const char32_t kArabicHigh[128];    // ISO_IR 127 / ISO 2022 IR 127
extern const char32_t kGreekHigh[128];     // ISO_IR 126 / ISO 2022 IR 126
extern const char32_t kHebrewHigh[128];    // ISO_IR 138 / ISO 2022 IR 138
extern const char32_t kLatin5High[128];    // ISO_IR 148 / ISO 2022 IR 148
extern const char32_t kThaiHigh[128];      // ISO_IR 166 / ISO 2022 IR 166

// A1.6: the deterministic inversion of one *High table above -- (code
// point, byte) pairs for every defined high-half byte, sorted ascending
// by code point for binary-search encode lookup. Generated in the same
// run as the decode tables (never hand-authored), so it can never drift
// out of sync with them -- see tools/generate_charset_tables.py.
struct ReverseEntry {
  char32_t codepoint;
  std::uint8_t byte;
};
extern const std::size_t kLatin1ReverseCount;
extern const ReverseEntry kLatin1Reverse[];
extern const std::size_t kLatin2ReverseCount;
extern const ReverseEntry kLatin2Reverse[];
extern const std::size_t kLatin3ReverseCount;
extern const ReverseEntry kLatin3Reverse[];
extern const std::size_t kLatin4ReverseCount;
extern const ReverseEntry kLatin4Reverse[];
extern const std::size_t kCyrillicReverseCount;
extern const ReverseEntry kCyrillicReverse[];
extern const std::size_t kArabicReverseCount;
extern const ReverseEntry kArabicReverse[];
extern const std::size_t kGreekReverseCount;
extern const ReverseEntry kGreekReverse[];
extern const std::size_t kHebrewReverseCount;
extern const ReverseEntry kHebrewReverse[];
extern const std::size_t kLatin5ReverseCount;
extern const ReverseEntry kLatin5Reverse[];
extern const std::size_t kThaiReverseCount;
extern const ReverseEntry kThaiReverse[];

}  // namespace fds::charset::detail
