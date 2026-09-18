#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "fastdicomattrs/tag.hpp"

namespace fds {

// Structured severity, never collapsed to a bare bool. See
// docs/architecture.md section 7.
enum class DiagnosticSeverity {
  Info,               // purely informational
  Warning,            // parser noticed something odd but is confident in the result
  RecoverableError,   // parser had to stop early but what it has so far is usable
  FatalError,         // parser could not produce a usable structure at all
  Unsupported,        // recognized but unimplemented construct (e.g. Explicit VR Big Endian)
  IOError,            // failure reading the underlying source
};

// One diagnostic from parsing or (later) writing. Reused for both -- see
// docs/api-design.md.
struct ParseDiagnostic {
  DiagnosticSeverity severity;
  std::string message;
  std::uint64_t offset = 0;
  std::optional<Tag> tag;
};

}  // namespace fds
