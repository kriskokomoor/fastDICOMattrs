#include "fastdicom/tags.hpp"

#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/dcmdata/dcuid.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

}  // namespace

int main() {
  fastdicom::Tag parsed;
  check(fastdicom::parseTag("0010,0010", parsed), "parse plain tag");
  check(parsed == fastdicom::Tag{0x0010, 0x0010}, "parsed values");
  check(fastdicom::parseTag("(0008,0060)", parsed), "parse parenthesized tag");
  check(!fastdicom::parseTag("0010:0010", parsed), "reject invalid separator");
  check(!fastdicom::parseTag("xyz0,0010", parsed), "reject non-hex tag");
  check(fastdicom::formatTag({0x10, 0x20}) == "(0010,0020)", "format tag");

  DcmFileFormat in_memory;
  auto* dataset = in_memory.getDataset();
  check(dataset->putAndInsertString(DCM_PatientName, "Example^Patient").good(),
        "insert patient name");
  check(dataset->putAndInsertString(DCM_SOPClassUID,
                                    UID_SecondaryCaptureImageStorage).good(),
        "insert SOP class UID");
  check(dataset->putAndInsertString(DCM_SOPInstanceUID,
                                    "1.2.826.0.1.3680043.10.543.1").good(),
        "insert SOP instance UID");

  const auto found = fastdicom::getTag(in_memory, {0x0010, 0x0010});
  check(found.status == fastdicom::TagStatus::found, "found status");
  check(found.value == "Example^Patient", "found value");

  const auto missing = fastdicom::getTag(in_memory, {0x0010, 0x0020});
  check(missing.status == fastdicom::TagStatus::missing, "missing status");
  check(!missing.has_value(), "missing has no value");

  const auto many = fastdicom::getTags(
      in_memory, {{0x0010, 0x0010}, {0x0010, 0x0020}, {0x0010, 0x0010}});
  check(many.size() == 3, "one result per requested tag");
  check(many[0].has_value() && !many[1].has_value() && many[2].has_value(),
        "multiple lookup statuses and order");

  const auto temp_file =
      std::filesystem::temp_directory_path() / "fastdicom-tags-test.dcm";
  check(in_memory.saveFile(temp_file.string().c_str(), EXS_LittleEndianExplicit)
            .good(),
        "write temporary DICOM");
  const auto from_path = fastdicom::getTag(temp_file, {0x0010, 0x0010});
  check(from_path.has_value() && from_path.value == "Example^Patient",
        "filename lookup");
  std::error_code remove_error;
  std::filesystem::remove(temp_file, remove_error);

  const auto unreadable =
      fastdicom::getTag(temp_file, {0x0010, 0x0010});
  check(unreadable.status == fastdicom::TagStatus::error,
        "read error differs from missing tag");
  check(!unreadable.message.empty(), "read error contains diagnostic");

  if (failures == 0) {
    std::cout << "All tests passed\n";
  }
  return failures == 0 ? 0 : 1;
}

