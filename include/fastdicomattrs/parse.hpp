#pragma once

#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <span>

#include "fastdicomattrs/parse_options.hpp"
#include "fastdicomattrs/parse_result.hpp"

namespace fds {

ParseResult parse_file(const std::filesystem::path& path, const ParseOptions& options = {});

// Does not take ownership of `bytes`: the caller must keep it alive for as
// long as the returned DICOMStructure (or anything derived from it) is
// used. See docs/api-design.md.
ParseResult parse_buffer(std::span<const std::byte> bytes, const ParseOptions& options = {});

// Reserved for a future increment (see docs/architecture.md section 11).
// Always returns ParseStatus::Failed with a single Unsupported diagnostic.
ParseResult parse_stream(std::istream& stream, const ParseOptions& options = {});

}  // namespace fds
