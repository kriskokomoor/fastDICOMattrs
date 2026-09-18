#pragma once

#include <string>
#include <string_view>

namespace fds {

enum class TransferSyntaxKind {
  ImplicitVRLittleEndian,   // 1.2.840.10008.1.2
  ExplicitVRLittleEndian,   // 1.2.840.10008.1.2.1
  ExplicitVRBigEndian,      // 1.2.840.10008.1.2.2 (retired)
  DeflatedExplicitVRLittleEndian,  // 1.2.840.10008.1.2.1.99: the *entire
                            // dataset* (not just Pixel Data) is
                            // zlib-deflate-compressed after the preamble/
                            // File Meta. Detected and rejected, not parsed
                            // -- this library has no inflate step. Without
                            // this special case it would otherwise fall
                            // into ExplicitVREncapsulated below, since its
                            // UID also starts with the 1.2.840.10008.1.2
                            // prefix, and get silently misread as plain
                            // (non-deflated) bytes.
  ExplicitVREncapsulated,   // any other recognized 1.2.840.10008.1.2.* UID:
                            // still Explicit VR Little Endian dataset encoding,
                            // but Pixel Data is encapsulated/compressed.
  Unrecognized,             // UID not recognized at all
};

// Classifies a Transfer Syntax UID into the encoding rules needed to parse
// the dataset that follows it. Does not know how to decode compressed pixel
// data -- only whether Pixel Data will be encapsulated. See
// docs/architecture.md section 8 and docs/roundtrip-contract.md "Known gaps".
class TransferSyntax {
 public:
  static TransferSyntax from_uid(std::string_view uid);

  const std::string& uid() const noexcept { return uid_; }
  TransferSyntaxKind kind() const noexcept { return kind_; }

  bool explicit_vr() const noexcept { return kind_ != TransferSyntaxKind::ImplicitVRLittleEndian; }
  bool little_endian() const noexcept { return kind_ != TransferSyntaxKind::ExplicitVRBigEndian; }
  bool encapsulated_pixel_data() const noexcept {
    return kind_ == TransferSyntaxKind::ExplicitVREncapsulated;
  }

  // True for any Transfer Syntax whose *dataset* (as opposed to pixel data)
  // this increment's parser can read: everything except Implicit VR Little
  // Endian and the retired Explicit VR Big Endian, and except UIDs this
  // library doesn't recognize at all.
  bool supported_for_dataset_parsing() const noexcept {
    return kind_ == TransferSyntaxKind::ExplicitVRLittleEndian ||
           kind_ == TransferSyntaxKind::ExplicitVREncapsulated;
  }

 private:
  TransferSyntax(std::string uid, TransferSyntaxKind kind) : uid_(std::move(uid)), kind_(kind) {}

  std::string uid_;
  TransferSyntaxKind kind_;
};

}  // namespace fds
