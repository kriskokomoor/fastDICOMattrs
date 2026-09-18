#include "fastdicomattrs/parse.hpp"

#include <istream>

#include "fastdicomattrs/dicom_structure.hpp"
#include "parser/explicit_vr_le_parser.hpp"

namespace fds {

ParseResult parse_file(const std::filesystem::path& path, const ParseOptions& options) {
  std::string error;
  auto source = FileSource::open(path, &error);
  if (!source) {
    ParseResult result;
    result.status = ParseStatus::Failed;
    result.diagnostics.push_back({DiagnosticSeverity::IOError, error, 0, std::nullopt});
    return result;
  }
  return parser::parse_explicit_vr(std::move(source), options);
}

ParseResult parse_buffer(std::span<const std::byte> bytes, const ParseOptions& options) {
  auto source = MemorySource::view(bytes.data(), bytes.size(), "<buffer>");
  return parser::parse_explicit_vr(std::move(source), options);
}

ParseResult parse_stream(std::istream&, const ParseOptions&) {
  // Reserved for a future increment -- see docs/architecture.md section 11.
  ParseResult result;
  result.status = ParseStatus::Failed;
  result.diagnostics.push_back({DiagnosticSeverity::Unsupported,
                                 "parse_stream is not implemented in this increment; use "
                                 "parse_file or parse_buffer",
                                 0, std::nullopt});
  return result;
}

}  // namespace fds
