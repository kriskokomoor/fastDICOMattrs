#include "fastdicomattrs/transfer_syntax.hpp"

#include <string>

namespace fds {
namespace {

constexpr std::string_view kImplicitVRLittleEndian = "1.2.840.10008.1.2";
constexpr std::string_view kExplicitVRLittleEndian = "1.2.840.10008.1.2.1";
constexpr std::string_view kExplicitVRBigEndian = "1.2.840.10008.1.2.2";
constexpr std::string_view kDeflatedExplicitVRLittleEndian = "1.2.840.10008.1.2.1.99";
// Common prefix for the whole "1.2.840.10008.1.2.*" DICOM Transfer Syntax
// family. Anything under this prefix other than the three UIDs above uses
// Explicit VR Little Endian dataset encoding with encapsulated Pixel Data
// (JPEG family, JPEG 2000, RLE Lossless, etc.) -- see docs/architecture.md
// section 8 and docs/roundtrip-contract.md "Known gaps".
constexpr std::string_view kDicomTransferSyntaxPrefix = "1.2.840.10008.1.2";

// Strips a single trailing NUL, which UI-VR values are padded with.
std::string_view trim_uid(std::string_view uid) {
  while (!uid.empty() && (uid.back() == '\0' || uid.back() == ' ')) uid.remove_suffix(1);
  return uid;
}

}  // namespace

TransferSyntax TransferSyntax::from_uid(std::string_view uid) {
  std::string_view trimmed = trim_uid(uid);

  if (trimmed == kImplicitVRLittleEndian) {
    return TransferSyntax(std::string(trimmed), TransferSyntaxKind::ImplicitVRLittleEndian);
  }
  if (trimmed == kExplicitVRLittleEndian) {
    return TransferSyntax(std::string(trimmed), TransferSyntaxKind::ExplicitVRLittleEndian);
  }
  if (trimmed == kExplicitVRBigEndian) {
    return TransferSyntax(std::string(trimmed), TransferSyntaxKind::ExplicitVRBigEndian);
  }
  if (trimmed == kDeflatedExplicitVRLittleEndian) {
    return TransferSyntax(std::string(trimmed), TransferSyntaxKind::DeflatedExplicitVRLittleEndian);
  }
  if (trimmed.size() > kDicomTransferSyntaxPrefix.size() &&
      trimmed.substr(0, kDicomTransferSyntaxPrefix.size()) == kDicomTransferSyntaxPrefix) {
    return TransferSyntax(std::string(trimmed), TransferSyntaxKind::ExplicitVREncapsulated);
  }
  return TransferSyntax(std::string(trimmed), TransferSyntaxKind::Unrecognized);
}

}  // namespace fds
