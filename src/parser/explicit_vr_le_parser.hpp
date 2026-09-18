#pragma once

#include <memory>

#include "fastdicomattrs/parse_options.hpp"
#include "fastdicomattrs/parse_result.hpp"
#include "fastdicomattrs/source.hpp"

namespace fds::parser {

// Parses a DICOM object from `source`: an optional 128-byte preamble +
// "DICM" magic, then File Meta Information (always Explicit VR Little
// Endian per the standard), then a main dataset. Once the dataset's
// Transfer Syntax is resolved from File Meta, this either continues parsing
// it directly (Explicit VR Little Endian, and any recognized compressed
// Transfer Syntax -- see TransferSyntax::supported_for_dataset_parsing())
// or hands off to parser::parse_dataset_implicit_vr for Implicit VR Little
// Endian. Anything else (e.g. Explicit VR Big Endian) is rejected with
// Unsupported (see docs/roundtrip-contract.md "Known gaps"). If there is no
// preamble, the content is treated as a bare dataset assumed to be Explicit
// VR Little Endian.
fds::ParseResult parse_explicit_vr(std::shared_ptr<const fds::Source> source,
                                    const fds::ParseOptions& options);

}  // namespace fds::parser
