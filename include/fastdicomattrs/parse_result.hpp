#pragma once

#include <memory>
#include <vector>

#include "fastdicomattrs/dicom_structure.hpp"
#include "fastdicomattrs/parse_diagnostic.hpp"

namespace fds {

enum class ParseStatus { Success, SuccessWithWarnings, Failed };

struct ParseResult {
  ParseStatus status = ParseStatus::Failed;
  std::unique_ptr<DICOMStructure> structure;  // non-null iff status != Failed
  std::vector<ParseDiagnostic> diagnostics;

  bool ok() const noexcept { return status != ParseStatus::Failed; }
};

}  // namespace fds
