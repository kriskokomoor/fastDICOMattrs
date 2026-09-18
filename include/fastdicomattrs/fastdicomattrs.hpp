#pragma once

// Umbrella header for consumers who want the whole public API. Internal
// implementation files should include only what they need directly.

#include "fastdicomattrs/charset.hpp"
#include "fastdicomattrs/dicom_structure.hpp"
#include "fastdicomattrs/element.hpp"
#include "fastdicomattrs/element_path.hpp"
#include "fastdicomattrs/item.hpp"
#include "fastdicomattrs/mutation.hpp"
#include "fastdicomattrs/parse.hpp"
#include "fastdicomattrs/parse_diagnostic.hpp"
#include "fastdicomattrs/parse_options.hpp"
#include "fastdicomattrs/parse_result.hpp"
#include "fastdicomattrs/pixel_data_reference.hpp"
#include "fastdicomattrs/sequence.hpp"
#include "fastdicomattrs/source.hpp"
#include "fastdicomattrs/source_span.hpp"
#include "fastdicomattrs/tag.hpp"
#include "fastdicomattrs/transfer_syntax.hpp"
#include "fastdicomattrs/value.hpp"
#include "fastdicomattrs/value_length.hpp"
#include "fastdicomattrs/vr.hpp"
#include "fastdicomattrs/write_result.hpp"
