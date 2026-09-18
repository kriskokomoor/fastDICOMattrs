#pragma once

#include <ostream>

#include "fastdicomattrs/dicom_structure.hpp"
#include "fastdicomattrs/write_result.hpp"

namespace fds {

// Serializes `structure` to `out`. See docs/roundtrip-contract.md: this is
// only proven (and only attempted) for an unmodified structure parsed at
// Fidelity::Lossless; anything else returns WriteStatus::Unsupported.
WriteResult write_lossless(const DICOMStructure& structure, std::ostream& out);

}  // namespace fds
