#pragma once

// A minimal, purpose-built byte-stream builder for constructing synthetic
// Explicit VR Little Endian DICOM fixtures in tests. Not part of the
// public library. See tests/fixtures/README.md for why tests use
// programmatically-built fixtures instead of vendored sample files.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fds_test {

class FixtureBuilder {
 public:
  FixtureBuilder& raw_u16(std::uint16_t v);
  FixtureBuilder& raw_u32(std::uint32_t v);
  FixtureBuilder& raw_byte(std::uint8_t v);
  FixtureBuilder& raw_bytes(const std::vector<std::byte>& bytes);
  FixtureBuilder& ascii(const std::string& text);  // appended verbatim, no padding
  FixtureBuilder& tag(std::uint16_t group, std::uint16_t element);
  FixtureBuilder& append(const FixtureBuilder& other);

  // 128-byte zero-filled preamble + "DICM" magic.
  FixtureBuilder& preamble();

  // Short-form explicit VR element (VR must not be a long-form VR).
  FixtureBuilder& element_short(std::uint16_t group, std::uint16_t element, const std::string& vr,
                                const std::string& ascii_value);
  FixtureBuilder& element_short_bytes(std::uint16_t group, std::uint16_t element,
                                      const std::string& vr, const std::vector<std::byte>& value);

  // Long-form explicit VR element with a defined length.
  FixtureBuilder& element_long(std::uint16_t group, std::uint16_t element, const std::string& vr,
                               const std::vector<std::byte>& value);

  // Sequence element headers; nested content is appended separately (see
  // wrap_in_item/wrap_in_undefined_item below for building item content).
  FixtureBuilder& sequence_defined(std::uint16_t group, std::uint16_t element,
                                   std::uint32_t content_length);
  FixtureBuilder& sequence_undefined(std::uint16_t group, std::uint16_t element);
  FixtureBuilder& sequence_delimiter();

  FixtureBuilder& item_defined_header(std::uint32_t content_length);
  FixtureBuilder& item_undefined_header();
  FixtureBuilder& item_delimiter();

  // Native (non-encapsulated) Pixel Data.
  FixtureBuilder& pixel_data_native(const std::string& vr, const std::vector<std::byte>& value);
  // Encapsulated Pixel Data header only (0xFFFFFFFF length); caller appends
  // the Basic Offset Table item and fragment items via item_defined_header
  // + raw_bytes, then sequence_delimiter().
  FixtureBuilder& pixel_data_encapsulated_header(const std::string& vr);

  // Implicit VR Little Endian element: tag + 4-byte length + value, no VR
  // field on the wire at all. Auto-pads odd-length values, same convention
  // as element_short.
  FixtureBuilder& element_implicit(std::uint16_t group, std::uint16_t element,
                                    const std::vector<std::byte>& value);
  FixtureBuilder& element_implicit_ascii(std::uint16_t group, std::uint16_t element,
                                          const std::string& ascii_value);
  // Implicit VR header only (tag + length), no value bytes: `content_length`
  // may be ValueLength::kUndefinedMarker for an undefined-length Sequence or
  // encapsulated Pixel Data, whose content the caller appends separately via
  // item_defined_header/wrap_in_item/sequence_delimiter (Item and delimiter
  // tags are encoding-independent, so those helpers are reused as-is).
  // Otherwise `content_length` is the byte length of the content the caller
  // appends next (e.g. one wrap_in_item()'s worth of bytes, for a
  // defined-length Sequence).
  FixtureBuilder& element_implicit_header(std::uint16_t group, std::uint16_t element,
                                           std::uint32_t content_length);

  const std::vector<std::byte>& bytes() const { return bytes_; }
  std::size_t size() const { return bytes_.size(); }

 private:
  std::vector<std::byte> bytes_;
};

// Wraps `content`'s bytes as one defined-length Item (FFFE,E000).
FixtureBuilder wrap_in_item(const FixtureBuilder& content);
// Wraps `content`'s bytes as one undefined-length Item, closed with an
// Item Delimitation Item.
FixtureBuilder wrap_in_undefined_item(const FixtureBuilder& content);

// A representative File Meta Information group: (0002,0000) group length,
// (0002,0001) version, (0002,0002)/(0002,0003) UIDs, (0002,0010) Transfer
// Syntax UID, (0002,0012) implementation UID. Includes preamble + "DICM".
FixtureBuilder make_file_meta(const std::string& transfer_syntax_uid);

}  // namespace fds_test
