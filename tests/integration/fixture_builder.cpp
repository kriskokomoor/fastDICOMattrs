#include "fixture_builder.hpp"

#include <cstring>

namespace fds_test {
namespace {
std::vector<std::byte> to_bytes(const std::string& s) {
  std::vector<std::byte> out(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
  return out;
}
}  // namespace

FixtureBuilder& FixtureBuilder::raw_u16(std::uint16_t v) {
  bytes_.push_back(static_cast<std::byte>(v & 0xFF));
  bytes_.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
  return *this;
}

FixtureBuilder& FixtureBuilder::raw_u32(std::uint32_t v) {
  bytes_.push_back(static_cast<std::byte>(v & 0xFF));
  bytes_.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
  bytes_.push_back(static_cast<std::byte>((v >> 16) & 0xFF));
  bytes_.push_back(static_cast<std::byte>((v >> 24) & 0xFF));
  return *this;
}

FixtureBuilder& FixtureBuilder::raw_byte(std::uint8_t v) {
  bytes_.push_back(static_cast<std::byte>(v));
  return *this;
}

FixtureBuilder& FixtureBuilder::raw_bytes(const std::vector<std::byte>& bytes) {
  bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  return *this;
}

FixtureBuilder& FixtureBuilder::ascii(const std::string& text) {
  return raw_bytes(to_bytes(text));
}

FixtureBuilder& FixtureBuilder::tag(std::uint16_t group, std::uint16_t element) {
  raw_u16(group);
  raw_u16(element);
  return *this;
}

FixtureBuilder& FixtureBuilder::append(const FixtureBuilder& other) {
  bytes_.insert(bytes_.end(), other.bytes_.begin(), other.bytes_.end());
  return *this;
}

FixtureBuilder& FixtureBuilder::preamble() {
  bytes_.insert(bytes_.end(), 128, std::byte{0});
  ascii("DICM");
  return *this;
}

FixtureBuilder& FixtureBuilder::element_short(std::uint16_t group, std::uint16_t element,
                                               const std::string& vr, const std::string& ascii_value) {
  return element_short_bytes(group, element, vr, to_bytes(ascii_value));
}

FixtureBuilder& FixtureBuilder::element_short_bytes(std::uint16_t group, std::uint16_t element,
                                                     const std::string& vr,
                                                     const std::vector<std::byte>& value) {
  tag(group, element);
  ascii(vr);
  // DICOM values must have even length; auto-pad with a NUL byte rather
  // than requiring every call site to remember to. (A raw string literal
  // like "ID1\0" does NOT achieve this: the embedded NUL is swallowed by
  // the const char* -> std::string conversion before element_short() ever
  // sees it, silently leaving an odd-length value -- this padding is the
  // single point where evenness is actually guaranteed.)
  std::vector<std::byte> padded = value;
  if (padded.size() % 2 != 0) padded.push_back(std::byte{0});
  raw_u16(static_cast<std::uint16_t>(padded.size()));
  raw_bytes(padded);
  return *this;
}

FixtureBuilder& FixtureBuilder::element_long(std::uint16_t group, std::uint16_t element,
                                              const std::string& vr,
                                              const std::vector<std::byte>& value) {
  tag(group, element);
  ascii(vr);
  raw_u16(0);  // reserved
  raw_u32(static_cast<std::uint32_t>(value.size()));
  raw_bytes(value);
  return *this;
}

FixtureBuilder& FixtureBuilder::sequence_defined(std::uint16_t group, std::uint16_t element,
                                                  std::uint32_t content_length) {
  tag(group, element);
  ascii("SQ");
  raw_u16(0);
  raw_u32(content_length);
  return *this;
}

FixtureBuilder& FixtureBuilder::sequence_undefined(std::uint16_t group, std::uint16_t element) {
  tag(group, element);
  ascii("SQ");
  raw_u16(0);
  raw_u32(0xFFFFFFFFu);
  return *this;
}

FixtureBuilder& FixtureBuilder::sequence_delimiter() {
  tag(0xFFFE, 0xE0DD);
  raw_u32(0);
  return *this;
}

FixtureBuilder& FixtureBuilder::item_defined_header(std::uint32_t content_length) {
  tag(0xFFFE, 0xE000);
  raw_u32(content_length);
  return *this;
}

FixtureBuilder& FixtureBuilder::item_undefined_header() {
  tag(0xFFFE, 0xE000);
  raw_u32(0xFFFFFFFFu);
  return *this;
}

FixtureBuilder& FixtureBuilder::item_delimiter() {
  tag(0xFFFE, 0xE00D);
  raw_u32(0);
  return *this;
}

FixtureBuilder& FixtureBuilder::pixel_data_native(const std::string& vr,
                                                   const std::vector<std::byte>& value) {
  tag(0x7FE0, 0x0010);
  ascii(vr);
  raw_u16(0);
  raw_u32(static_cast<std::uint32_t>(value.size()));
  raw_bytes(value);
  return *this;
}

FixtureBuilder& FixtureBuilder::pixel_data_encapsulated_header(const std::string& vr) {
  tag(0x7FE0, 0x0010);
  ascii(vr);
  raw_u16(0);
  raw_u32(0xFFFFFFFFu);
  return *this;
}

FixtureBuilder& FixtureBuilder::element_implicit(std::uint16_t group, std::uint16_t element,
                                                  const std::vector<std::byte>& value) {
  std::vector<std::byte> padded = value;
  if (padded.size() % 2 != 0) padded.push_back(std::byte{0});
  element_implicit_header(group, element, static_cast<std::uint32_t>(padded.size()));
  raw_bytes(padded);
  return *this;
}

FixtureBuilder& FixtureBuilder::element_implicit_ascii(std::uint16_t group, std::uint16_t element,
                                                        const std::string& ascii_value) {
  return element_implicit(group, element, to_bytes(ascii_value));
}

FixtureBuilder& FixtureBuilder::element_implicit_header(std::uint16_t group, std::uint16_t element,
                                                         std::uint32_t content_length) {
  tag(group, element);
  raw_u32(content_length);
  return *this;
}

FixtureBuilder wrap_in_item(const FixtureBuilder& content) {
  FixtureBuilder out;
  out.item_defined_header(static_cast<std::uint32_t>(content.size()));
  out.append(content);
  return out;
}

FixtureBuilder wrap_in_undefined_item(const FixtureBuilder& content) {
  FixtureBuilder out;
  out.item_undefined_header();
  out.append(content);
  out.item_delimiter();
  return out;
}

FixtureBuilder make_file_meta(const std::string& transfer_syntax_uid) {
  std::string ts_uid = transfer_syntax_uid;
  if (ts_uid.size() % 2 != 0) ts_uid.push_back('\0');
  std::string sop_class_uid = "1.2.840.10008.5.1.4.1.1.7";  // Secondary Capture Image Storage
  if (sop_class_uid.size() % 2 != 0) sop_class_uid.push_back('\0');
  std::string sop_instance_uid = "1.2.3.4.5.6.7.8";
  if (sop_instance_uid.size() % 2 != 0) sop_instance_uid.push_back('\0');
  std::string impl_class_uid = "1.2.3.4.9999.1";
  if (impl_class_uid.size() % 2 != 0) impl_class_uid.push_back('\0');

  FixtureBuilder group_body;
  // (0002,0001) FileMetaInformationVersion is VR OB, a long-form VR: even
  // within File Meta Information, elements follow ordinary Explicit VR
  // Little Endian header rules (PS3.10 section 7.1) -- there is no
  // File-Meta-specific short-form exception.
  group_body.element_long(0x0002, 0x0001, "OB",
                           std::vector<std::byte>{std::byte{0}, std::byte{1}});
  group_body.element_short(0x0002, 0x0002, "UI", sop_class_uid);
  group_body.element_short(0x0002, 0x0003, "UI", sop_instance_uid);
  group_body.element_short(0x0002, 0x0010, "UI", ts_uid);
  group_body.element_short(0x0002, 0x0012, "UI", impl_class_uid);

  FixtureBuilder out;
  out.preamble();
  out.element_short_bytes(0x0002, 0x0000, "UL",
                           [&] {
                             std::uint32_t len = static_cast<std::uint32_t>(group_body.size());
                             std::vector<std::byte> v(4);
                             v[0] = static_cast<std::byte>(len & 0xFF);
                             v[1] = static_cast<std::byte>((len >> 8) & 0xFF);
                             v[2] = static_cast<std::byte>((len >> 16) & 0xFF);
                             v[3] = static_cast<std::byte>((len >> 24) & 0xFF);
                             return v;
                           }());
  out.append(group_body);
  return out;
}

}  // namespace fds_test
