#pragma once

#include <cstdint>
#include <vector>

#include "fastdicomattrs/parse_diagnostic.hpp"

namespace fds {

enum class WriteStatus { Success, Unsupported, IOError, Failed };

struct WriteResult {
  WriteStatus status = WriteStatus::Failed;
  std::uint64_t bytes_written = 0;

  // Byte-level preservation of untouched content (README "Success
  // criteria": "quantify byte-level preservation of untouched content").
  // Covers every element's *value* payload plus Pixel Data -- never
  // structural framing (tag/VR/length fields, Item/Sequence delimiters),
  // which this writer always reconstructs by design regardless of
  // modification (see docs/roundtrip-contract.md "Reconstruction, not
  // verbatim copy") and so isn't a meaningful "was this preserved?"
  // signal. source_backed_value_bytes + regenerated_value_bytes is the
  // total value-payload bytes *written* (a removed element contributes to
  // neither counter -- it simply does not appear in the output);
  // source_backed_value_bytes came verbatim from the original Source (an
  // untouched element, or Pixel Data, which this library never mutates);
  // regenerated_value_bytes came from a caller-supplied replacement (an
  // owned Value from set/set_value).
  std::uint64_t source_backed_value_bytes = 0;
  std::uint64_t regenerated_value_bytes = 0;

  std::vector<ParseDiagnostic> diagnostics;

  bool ok() const noexcept { return status == WriteStatus::Success; }
};

}  // namespace fds
