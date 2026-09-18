#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "fastdicomattrs/element.hpp"
#include "fastdicomattrs/parse_diagnostic.hpp"
#include "fastdicomattrs/parse_options.hpp"
#include "fastdicomattrs/pixel_data_reference.hpp"
#include "fastdicomattrs/source.hpp"
#include "fastdicomattrs/transfer_syntax.hpp"
#include "parser/byte_reader.hpp"

namespace fds::parser {

// Parses dataset elements under Implicit VR Little Endian (1.2.840.10008.1.2)
// starting at `reader`'s current position (already positioned just past File
// Meta -- File Meta is always Explicit VR regardless of dataset Transfer
// Syntax, and is parsed by parse_explicit_vr before handing off here) through
// `size`. Appends parsed top-level elements to `elements` and sets
// `pixel_data` (plus `pixel_data_position`, the count of `elements` already
// appended at the moment Pixel Data was found -- see
// docs/architecture/A1_3_PIXEL_DATA_ORDERING_REPORT.md) if a Pixel Data
// element is found. Diagnostics are pushed to
// `diagnostics`; this never "fails" outright the way parse_explicit_vr's
// caller can -- a recoverable error just stops the loop early, matching
// parse_explicit_vr's own top-level loop.
//
// No data dictionary is available (see docs/architecture.md section 9), so
// VR cannot be read from the wire under this Transfer Syntax; see
// docs/roundtrip-contract.md "Implicit VR Little Endian" for the structural
// (undefined-length-implies-Sequence) heuristic this uses instead, and its
// documented limitation.
//
// `initial_element_count` continues ParseOptions::max_element_count's
// budget from wherever Explicit VR File Meta parsing left off, so the
// limit stays parse-wide across the File-Meta/dataset handoff rather than
// resetting to zero here.
void parse_dataset_implicit_vr(ByteReader& reader, const Source& source,
                                const ParseOptions& options, std::uint64_t size,
                                const TransferSyntax& transfer_syntax,
                                std::vector<ParseDiagnostic>& diagnostics,
                                std::vector<Element>& elements,
                                std::optional<PixelDataReference>& pixel_data,
                                std::optional<std::size_t>& pixel_data_position,
                                std::size_t initial_element_count);

}  // namespace fds::parser
