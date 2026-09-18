#include "fastdicomattrs_c/fds.h"

#include <cstring>
#include <exception>
#include <sstream>
#include <vector>

#include "fastdicomattrs/fastdicomattrs.hpp"

namespace {

// The only place fds_structure_t is given a real definition -- opaque to
// every C caller, defined here so this translation unit can bundle a
// parsed DICOMStructure together with its diagnostics (which otherwise
// would only live inside the now-discarded ParseResult). See
// docs/abi-design.md "Implementation technique".
struct StructureHandle {
  // fds_parse_buffer owns its input for the handle's lifetime. This keeps
  // the C ABI's ownership rule simple even though the lower-level C++
  // parse_buffer API deliberately accepts a borrowed span.
  std::vector<std::byte> owned_input;
  std::unique_ptr<fds::DICOMStructure> structure;
  std::vector<fds::ParseDiagnostic> diagnostics;
  // Backing storage for fds_structure_write_buffer(_with_stats)'s returned
  // pointer -- mutable because writing is logically a const/read operation
  // on the DICOM content (matching fds_structure_write_file's const
  // signature) even though it must cache its output somewhere for the
  // pointer to outlive the call. Overwritten by the next write_buffer call
  // on this handle; see fds.h's doc comment for the full invalidation rule.
  mutable std::vector<std::byte> write_buffer;
  // A1.7: backing storage for fds_structure_decode_text_path's returned
  // pointer, under the identical mutable/overwrite-on-next-call rule as
  // write_buffer above.
  mutable std::string decoded_text_buffer;
};

fds_structure_t* to_c(StructureHandle* h) { return reinterpret_cast<fds_structure_t*>(h); }
StructureHandle* from_c(fds_structure_t* h) { return reinterpret_cast<StructureHandle*>(h); }
const StructureHandle* from_c(const fds_structure_t* h) {
  return reinterpret_cast<const StructureHandle*>(h);
}

// fds_element_t is never given a definition anywhere, even here: a
// fds::Element* is reinterpret_cast directly to/from fds_element_t*. See
// docs/abi-design.md "Implementation technique" for why this is safe.
const fds_element_t* to_c(const fds::Element* e) {
  return reinterpret_cast<const fds_element_t*>(e);
}
const fds::Element* from_c(const fds_element_t* e) {
  return reinterpret_cast<const fds::Element*>(e);
}

fds_vr_t to_c(fds::VR vr) {
  using fds::VR;
  switch (vr) {
    case VR::AE: return FDS_VR_AE;   case VR::AS: return FDS_VR_AS;
    case VR::AT: return FDS_VR_AT;   case VR::CS: return FDS_VR_CS;
    case VR::DA: return FDS_VR_DA;   case VR::DS: return FDS_VR_DS;
    case VR::DT: return FDS_VR_DT;   case VR::FL: return FDS_VR_FL;
    case VR::FD: return FDS_VR_FD;   case VR::IS: return FDS_VR_IS;
    case VR::LO: return FDS_VR_LO;   case VR::LT: return FDS_VR_LT;
    case VR::OB: return FDS_VR_OB;   case VR::OD: return FDS_VR_OD;
    case VR::OF: return FDS_VR_OF;   case VR::OL: return FDS_VR_OL;
    case VR::OV: return FDS_VR_OV;   case VR::OW: return FDS_VR_OW;
    case VR::PN: return FDS_VR_PN;   case VR::SH: return FDS_VR_SH;
    case VR::SL: return FDS_VR_SL;   case VR::SQ: return FDS_VR_SQ;
    case VR::SS: return FDS_VR_SS;   case VR::ST: return FDS_VR_ST;
    case VR::SV: return FDS_VR_SV;   case VR::TM: return FDS_VR_TM;
    case VR::UC: return FDS_VR_UC;   case VR::UI: return FDS_VR_UI;
    case VR::UL: return FDS_VR_UL;   case VR::UN: return FDS_VR_UN;
    case VR::UR: return FDS_VR_UR;   case VR::US: return FDS_VR_US;
    case VR::UT: return FDS_VR_UT;   case VR::UV: return FDS_VR_UV;
    case VR::Unknown: default: return FDS_VR_UNKNOWN;
  }
}

fds::VR from_c(fds_vr_t vr) {
  using fds::VR;
  switch (vr) {
    case FDS_VR_AE: return VR::AE;   case FDS_VR_AS: return VR::AS;
    case FDS_VR_AT: return VR::AT;   case FDS_VR_CS: return VR::CS;
    case FDS_VR_DA: return VR::DA;   case FDS_VR_DS: return VR::DS;
    case FDS_VR_DT: return VR::DT;   case FDS_VR_FL: return VR::FL;
    case FDS_VR_FD: return VR::FD;   case FDS_VR_IS: return VR::IS;
    case FDS_VR_LO: return VR::LO;   case FDS_VR_LT: return VR::LT;
    case FDS_VR_OB: return VR::OB;   case FDS_VR_OD: return VR::OD;
    case FDS_VR_OF: return VR::OF;   case FDS_VR_OL: return VR::OL;
    case FDS_VR_OV: return VR::OV;   case FDS_VR_OW: return VR::OW;
    case FDS_VR_PN: return VR::PN;   case FDS_VR_SH: return VR::SH;
    case FDS_VR_SL: return VR::SL;   case FDS_VR_SQ: return VR::SQ;
    case FDS_VR_SS: return VR::SS;   case FDS_VR_ST: return VR::ST;
    case FDS_VR_SV: return VR::SV;   case FDS_VR_TM: return VR::TM;
    case FDS_VR_UC: return VR::UC;   case FDS_VR_UI: return VR::UI;
    case FDS_VR_UL: return VR::UL;   case FDS_VR_UN: return VR::UN;
    case FDS_VR_UR: return VR::UR;   case FDS_VR_US: return VR::US;
    case FDS_VR_UT: return VR::UT;   case FDS_VR_UV: return VR::UV;
    case FDS_VR_UNKNOWN: default: return VR::Unknown;
  }
}

bool is_valid_insert_vr(fds_vr_t vr) {
  return vr >= FDS_VR_AE && vr <= FDS_VR_UV && vr != FDS_VR_SQ;
}

// Copies a caller-supplied buffer into an owned Value -- the ABI boundary
// never lets a Value reference caller memory of unknown lifetime.
fds::Value to_owned_value(const uint8_t* value, size_t value_length) {
  std::vector<std::byte> bytes(value_length);
  if (value_length > 0) {
    std::memcpy(bytes.data(), value, value_length);
  }
  return fds::Value::from_owned(std::move(bytes));
}

fds_diagnostic_severity_t to_c(fds::DiagnosticSeverity s) {
  using fds::DiagnosticSeverity;
  switch (s) {
    case DiagnosticSeverity::Info: return FDS_DIAGNOSTIC_INFO;
    case DiagnosticSeverity::Warning: return FDS_DIAGNOSTIC_WARNING;
    case DiagnosticSeverity::RecoverableError: return FDS_DIAGNOSTIC_RECOVERABLE_ERROR;
    case DiagnosticSeverity::FatalError: return FDS_DIAGNOSTIC_FATAL_ERROR;
    case DiagnosticSeverity::Unsupported: return FDS_DIAGNOSTIC_UNSUPPORTED;
    case DiagnosticSeverity::IOError: return FDS_DIAGNOSTIC_IO_ERROR;
  }
  return FDS_DIAGNOSTIC_INFO;
}

fds::Tag to_cpp(fds_tag_t t) { return fds::Tag(t.group, t.element); }

// A1.7: builds an ElementPath from a caller-supplied fds_path_step_t array
// verbatim -- one converter for both shapes an array can take (existing-
// element locator vs. container locator; see fds_path_step_t's doc
// comment in fds.h). Which shape is valid is enforced downstream by
// whichever C++ function consumes the resulting ElementPath (find/
// set_value/erase for an element locator; DICOMStructure::insert for a
// container locator), exactly as it already is for a direct C++ caller.
fds::ElementPath to_cpp_path(const fds_path_step_t* steps, size_t step_count) {
  fds::ElementPath path;
  for (size_t i = 0; i < step_count; ++i) {
    const fds_path_step_t& step = steps[i];
    if (step.has_item_index) {
      path.push(to_cpp(step.tag), static_cast<std::size_t>(step.item_index));
    } else {
      path.push(to_cpp(step.tag));
    }
  }
  return path;
}

fds_status_t to_c(fds::mutation::InsertStatus status) {
  using fds::mutation::InsertStatus;
  switch (status) {
    case InsertStatus::Success: return FDS_STATUS_OK;
    case InsertStatus::ContainerNotFound: return FDS_STATUS_NOT_FOUND;
    case InsertStatus::NotASequence: return FDS_STATUS_NOT_FOUND;
    case InsertStatus::ItemIndexOutOfRange: return FDS_STATUS_NOT_FOUND;
    case InsertStatus::AlreadyExists: return FDS_STATUS_ALREADY_EXISTS;
    case InsertStatus::VRRequired: return FDS_STATUS_VR_REQUIRED;
    case InsertStatus::InvalidVR: return FDS_STATUS_INVALID_ARGUMENT;
    case InsertStatus::ValueTooLong: return FDS_STATUS_INVALID_ARGUMENT;
  }
  return FDS_STATUS_INTERNAL_ERROR;  // unreachable
}

fds_status_t to_c(fds::charset::SetTextStatus status) {
  using fds::charset::SetTextStatus;
  switch (status) {
    case SetTextStatus::Success: return FDS_STATUS_OK;
    case SetTextStatus::NotATextVR: return FDS_STATUS_UNSUPPORTED;
    case SetTextStatus::UnsupportedCharset: return FDS_STATUS_UNSUPPORTED;
    case SetTextStatus::MalformedCharsetDeclaration: return FDS_STATUS_UNSUPPORTED;
    case SetTextStatus::InvalidUnicodeInput: return FDS_STATUS_INVALID_UNICODE_INPUT;
    case SetTextStatus::UnrepresentableCharacter: return FDS_STATUS_UNREPRESENTABLE_CHARACTER;
    case SetTextStatus::ValueTooLong: return FDS_STATUS_INVALID_ARGUMENT;
    case SetTextStatus::PathNotFound: return FDS_STATUS_NOT_FOUND;
    case SetTextStatus::IsSequenceElement: return FDS_STATUS_INVALID_ARGUMENT;
    case SetTextStatus::ContainerNotFound: return FDS_STATUS_NOT_FOUND;
    case SetTextStatus::NotASequence: return FDS_STATUS_NOT_FOUND;
    case SetTextStatus::ItemIndexOutOfRange: return FDS_STATUS_NOT_FOUND;
    case SetTextStatus::AlreadyExists: return FDS_STATUS_ALREADY_EXISTS;
    case SetTextStatus::VRRequired: return FDS_STATUS_VR_REQUIRED;
  }
  return FDS_STATUS_INTERNAL_ERROR;  // unreachable
}

fds::ParseOptions to_cpp(const fds_parse_options_t* options) {
  fds::ParseOptions cpp_options;
  if (options != nullptr) {
    switch (options->fidelity) {
      case FDS_FIDELITY_FAST: cpp_options.fidelity = fds::Fidelity::Fast; break;
      case FDS_FIDELITY_STANDARD: cpp_options.fidelity = fds::Fidelity::Standard; break;
      case FDS_FIDELITY_LOSSLESS: cpp_options.fidelity = fds::Fidelity::Lossless; break;
    }
    cpp_options.max_element_count = static_cast<std::size_t>(options->max_element_count);
    cpp_options.max_sequence_depth = static_cast<std::size_t>(options->max_sequence_depth);
  }
  return cpp_options;
}

fds_status_t status_from_parse_result(const fds::ParseResult& result) {
  if (result.status == fds::ParseStatus::Failed) {
    for (const auto& d : result.diagnostics) {
      if (d.severity == fds::DiagnosticSeverity::Unsupported) return FDS_STATUS_UNSUPPORTED;
      if (d.severity == fds::DiagnosticSeverity::IOError) return FDS_STATUS_IO_ERROR;
    }
    return FDS_STATUS_PARSE_FAILED;
  }
  return FDS_STATUS_OK;
}

fds_status_t make_structure_handle(fds::ParseResult result, fds_structure_t** out) {
  fds_status_t status = status_from_parse_result(result);
  auto* handle = new StructureHandle();
  handle->structure = std::move(result.structure);
  handle->diagnostics = std::move(result.diagnostics);
  *out = to_c(handle);
  return status;
}

const fds::Sequence* sequence_of(const fds_element_t* element) {
  const fds::Element* e = from_c(element);
  return e->is_sequence() ? &e->sequence() : nullptr;
}

}  // namespace

// Every extern "C" function body is wrapped so a C++ exception can never
// cross the ABI boundary (see docs/abi-design.md). FDS_ABI_CATCH is placed
// at the bottom of a try block.
#define FDS_ABI_TRY try
#define FDS_ABI_CATCH(fallback)              \
  catch (const std::exception&) {            \
    return (fallback);                       \
  } catch (...) {                            \
    return (fallback);                       \
  }

extern "C" {

uint32_t fds_abi_version(void) { return (0u << 16) | (6u << 8) | 0u; }

const char* fds_status_message(fds_status_t status) {
  switch (status) {
    case FDS_STATUS_OK: return "ok";
    case FDS_STATUS_NOT_FOUND: return "not found";
    case FDS_STATUS_INVALID_ARGUMENT: return "invalid argument";
    case FDS_STATUS_ALREADY_EXISTS: return "already exists";
    case FDS_STATUS_VR_REQUIRED: return "VR required (inference was ambiguous or unavailable)";
    case FDS_STATUS_UNREPRESENTABLE_CHARACTER: return "unrepresentable character";
    case FDS_STATUS_INVALID_UNICODE_INPUT: return "invalid Unicode input";
    case FDS_STATUS_IO_ERROR: return "I/O error";
    case FDS_STATUS_PARSE_FAILED: return "parse failed";
    case FDS_STATUS_UNSUPPORTED: return "unsupported";
    case FDS_STATUS_INTERNAL_ERROR: return "internal error (library bug)";
  }
  return "unknown status";
}

void fds_parse_options_init_defaults(fds_parse_options_t* options) {
  if (options == nullptr) return;
  fds::ParseOptions defaults;
  options->fidelity = FDS_FIDELITY_STANDARD;
  options->max_element_count = static_cast<uint64_t>(defaults.max_element_count);
  options->max_sequence_depth = static_cast<uint64_t>(defaults.max_sequence_depth);
}

fds_status_t fds_parse_file(const char* path, const fds_parse_options_t* options,
                             fds_structure_t** out_structure) {
  if (path == nullptr || out_structure == nullptr) return FDS_STATUS_INVALID_ARGUMENT;
  *out_structure = nullptr;
  FDS_ABI_TRY {
    auto result = fds::parse_file(path, to_cpp(options));
    return make_structure_handle(std::move(result), out_structure);
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
}

fds_status_t fds_parse_buffer(const uint8_t* data, size_t length, const fds_parse_options_t* options,
                               fds_structure_t** out_structure) {
  if (out_structure == nullptr || (data == nullptr && length != 0)) return FDS_STATUS_INVALID_ARGUMENT;
  *out_structure = nullptr;
  FDS_ABI_TRY {
    auto handle = std::make_unique<StructureHandle>();
    handle->owned_input.resize(length);
    if (length > 0) std::memcpy(handle->owned_input.data(), data, length);
    auto result = fds::parse_buffer(handle->owned_input, to_cpp(options));
    const fds_status_t status = status_from_parse_result(result);
    handle->structure = std::move(result.structure);
    handle->diagnostics = std::move(result.diagnostics);
    *out_structure = to_c(handle.release());
    return status;
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
}

void fds_structure_free(fds_structure_t* structure) {
  if (structure == nullptr) return;
  delete from_c(structure);
}

size_t fds_structure_element_count(const fds_structure_t* structure) {
  if (structure == nullptr) return 0;
  const auto* handle = from_c(structure);
  if (!handle->structure) return 0;
  return handle->structure->element_count();
}

fds_status_t fds_structure_element_at(const fds_structure_t* structure, size_t index,
                                       const fds_element_t** out_element) {
  if (out_element == nullptr) return FDS_STATUS_INVALID_ARGUMENT;
  *out_element = nullptr;
  if (structure == nullptr) return FDS_STATUS_INVALID_ARGUMENT;
  const auto* handle = from_c(structure);
  if (!handle->structure || index >= handle->structure->element_count()) return FDS_STATUS_NOT_FOUND;
  *out_element = to_c(&handle->structure->elements()[index]);
  return FDS_STATUS_OK;
}

fds_status_t fds_structure_find(const fds_structure_t* structure, fds_tag_t tag,
                                 const fds_element_t** out_element) {
  if (out_element == nullptr) return FDS_STATUS_INVALID_ARGUMENT;
  *out_element = nullptr;
  if (structure == nullptr) return FDS_STATUS_INVALID_ARGUMENT;
  const auto* handle = from_c(structure);
  if (!handle->structure) return FDS_STATUS_NOT_FOUND;
  const fds::Element* found = handle->structure->find(to_cpp(tag));
  if (found == nullptr) return FDS_STATUS_NOT_FOUND;
  *out_element = to_c(found);
  return FDS_STATUS_OK;
}

int fds_structure_contains(const fds_structure_t* structure, fds_tag_t tag) {
  if (structure == nullptr) return 0;
  const auto* handle = from_c(structure);
  if (!handle->structure) return 0;
  return handle->structure->contains(to_cpp(tag)) ? 1 : 0;
}

size_t fds_structure_diagnostic_count(const fds_structure_t* structure) {
  if (structure == nullptr) return 0;
  return from_c(structure)->diagnostics.size();
}

fds_status_t fds_structure_diagnostic_at(const fds_structure_t* structure, size_t index,
                                          fds_diagnostic_t* out_diagnostic) {
  if (out_diagnostic == nullptr) return FDS_STATUS_INVALID_ARGUMENT;
  if (structure == nullptr) return FDS_STATUS_INVALID_ARGUMENT;
  const auto* handle = from_c(structure);
  if (index >= handle->diagnostics.size()) return FDS_STATUS_NOT_FOUND;
  const auto& d = handle->diagnostics[index];
  out_diagnostic->severity = to_c(d.severity);
  out_diagnostic->message = d.message.c_str();  // owned by `handle`, valid until structure_free
  out_diagnostic->offset = d.offset;
  out_diagnostic->has_tag = d.tag.has_value() ? 1 : 0;
  if (d.tag.has_value()) {
    out_diagnostic->tag = fds_tag_t{d.tag->group, d.tag->element};
  } else {
    out_diagnostic->tag = fds_tag_t{0, 0};
  }
  return FDS_STATUS_OK;
}

const char* fds_structure_transfer_syntax_uid(const fds_structure_t* structure) {
  if (structure == nullptr) return nullptr;
  const auto* handle = from_c(structure);
  if (!handle->structure) return nullptr;
  return handle->structure->transfer_syntax().uid().c_str();
}

int fds_structure_transfer_syntax_is_explicit_vr(const fds_structure_t* structure) {
  if (structure == nullptr || !from_c(structure)->structure) return 0;
  return from_c(structure)->structure->transfer_syntax().explicit_vr() ? 1 : 0;
}

int fds_structure_transfer_syntax_is_little_endian(const fds_structure_t* structure) {
  if (structure == nullptr || !from_c(structure)->structure) return 0;
  return from_c(structure)->structure->transfer_syntax().little_endian() ? 1 : 0;
}

int fds_structure_pixel_data_kind(const fds_structure_t* structure) {
  if (structure == nullptr || !from_c(structure)->structure) return 0;
  const auto* pixel_data = from_c(structure)->structure->pixel_data();
  if (pixel_data == nullptr) return 0;
  return pixel_data->is_encapsulated() ? 2 : 1;
}

fds_status_t fds_structure_write_file(const fds_structure_t* structure, const char* path,
                                       uint64_t* out_bytes_written) {
  if (structure == nullptr || path == nullptr || out_bytes_written == nullptr) {
    return FDS_STATUS_INVALID_ARGUMENT;
  }
  *out_bytes_written = 0;
  const auto* handle = from_c(structure);
  if (!handle->structure) return FDS_STATUS_INVALID_ARGUMENT;
  FDS_ABI_TRY {
    auto result = handle->structure->write_file(path);
    *out_bytes_written = result.bytes_written;
    switch (result.status) {
      case fds::WriteStatus::Success: return FDS_STATUS_OK;
      case fds::WriteStatus::Unsupported: return FDS_STATUS_UNSUPPORTED;
      case fds::WriteStatus::IOError: return FDS_STATUS_IO_ERROR;
      case fds::WriteStatus::Failed: return FDS_STATUS_INTERNAL_ERROR;
    }
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
  return FDS_STATUS_INTERNAL_ERROR;
}

fds_status_t fds_structure_write_file_with_stats(const fds_structure_t* structure, const char* path,
                                                   uint64_t* out_bytes_written,
                                                   uint64_t* out_source_backed_value_bytes,
                                                   uint64_t* out_regenerated_value_bytes) {
  if (structure == nullptr || path == nullptr) return FDS_STATUS_INVALID_ARGUMENT;
  if (out_bytes_written != nullptr) *out_bytes_written = 0;
  if (out_source_backed_value_bytes != nullptr) *out_source_backed_value_bytes = 0;
  if (out_regenerated_value_bytes != nullptr) *out_regenerated_value_bytes = 0;
  const auto* handle = from_c(structure);
  if (!handle->structure) return FDS_STATUS_INVALID_ARGUMENT;
  FDS_ABI_TRY {
    auto result = handle->structure->write_file(path);
    if (out_bytes_written != nullptr) *out_bytes_written = result.bytes_written;
    if (out_source_backed_value_bytes != nullptr) {
      *out_source_backed_value_bytes = result.source_backed_value_bytes;
    }
    if (out_regenerated_value_bytes != nullptr) {
      *out_regenerated_value_bytes = result.regenerated_value_bytes;
    }
    switch (result.status) {
      case fds::WriteStatus::Success: return FDS_STATUS_OK;
      case fds::WriteStatus::Unsupported: return FDS_STATUS_UNSUPPORTED;
      case fds::WriteStatus::IOError: return FDS_STATUS_IO_ERROR;
      case fds::WriteStatus::Failed: return FDS_STATUS_INTERNAL_ERROR;
    }
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
  return FDS_STATUS_INTERNAL_ERROR;
}

namespace {

fds_status_t to_status(fds::WriteStatus status) {
  switch (status) {
    case fds::WriteStatus::Success: return FDS_STATUS_OK;
    case fds::WriteStatus::Unsupported: return FDS_STATUS_UNSUPPORTED;
    case fds::WriteStatus::IOError: return FDS_STATUS_IO_ERROR;
    case fds::WriteStatus::Failed: return FDS_STATUS_INTERNAL_ERROR;
  }
  return FDS_STATUS_INTERNAL_ERROR;
}

}  // namespace

fds_status_t fds_structure_write_buffer(const fds_structure_t* structure, const uint8_t** out_data,
                                         size_t* out_length) {
  if (structure == nullptr || out_data == nullptr || out_length == nullptr) {
    return FDS_STATUS_INVALID_ARGUMENT;
  }
  *out_data = nullptr;
  *out_length = 0;
  const auto* handle = from_c(structure);
  if (!handle->structure) return FDS_STATUS_INVALID_ARGUMENT;
  FDS_ABI_TRY {
    std::ostringstream out;
    auto result = handle->structure->write(out);
    if (result.status != fds::WriteStatus::Success) return to_status(result.status);
    std::string bytes = out.str();
    handle->write_buffer.assign(reinterpret_cast<const std::byte*>(bytes.data()),
                                 reinterpret_cast<const std::byte*>(bytes.data() + bytes.size()));
    *out_data = reinterpret_cast<const uint8_t*>(handle->write_buffer.data());
    *out_length = handle->write_buffer.size();
    return FDS_STATUS_OK;
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
  return FDS_STATUS_INTERNAL_ERROR;
}

fds_status_t fds_structure_write_buffer_with_stats(const fds_structure_t* structure,
                                                     const uint8_t** out_data, size_t* out_length,
                                                     uint64_t* out_source_backed_value_bytes,
                                                     uint64_t* out_regenerated_value_bytes) {
  if (structure == nullptr || out_data == nullptr || out_length == nullptr) {
    return FDS_STATUS_INVALID_ARGUMENT;
  }
  *out_data = nullptr;
  *out_length = 0;
  if (out_source_backed_value_bytes != nullptr) *out_source_backed_value_bytes = 0;
  if (out_regenerated_value_bytes != nullptr) *out_regenerated_value_bytes = 0;
  const auto* handle = from_c(structure);
  if (!handle->structure) return FDS_STATUS_INVALID_ARGUMENT;
  FDS_ABI_TRY {
    std::ostringstream out;
    auto result = handle->structure->write(out);
    if (result.status != fds::WriteStatus::Success) return to_status(result.status);
    std::string bytes = out.str();
    handle->write_buffer.assign(reinterpret_cast<const std::byte*>(bytes.data()),
                                 reinterpret_cast<const std::byte*>(bytes.data() + bytes.size()));
    *out_data = reinterpret_cast<const uint8_t*>(handle->write_buffer.data());
    *out_length = handle->write_buffer.size();
    if (out_source_backed_value_bytes != nullptr) {
      *out_source_backed_value_bytes = result.source_backed_value_bytes;
    }
    if (out_regenerated_value_bytes != nullptr) {
      *out_regenerated_value_bytes = result.regenerated_value_bytes;
    }
    return FDS_STATUS_OK;
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
  return FDS_STATUS_INTERNAL_ERROR;
}

int fds_structure_is_modified(const fds_structure_t* structure) {
  if (structure == nullptr || !from_c(structure)->structure) return 0;
  return from_c(structure)->structure->is_modified() ? 1 : 0;
}

fds_status_t fds_structure_set_value(fds_structure_t* structure, fds_tag_t tag,
                                      const uint8_t* value, size_t value_length) {
  if (structure == nullptr || (value == nullptr && value_length != 0)) {
    return FDS_STATUS_INVALID_ARGUMENT;
  }
  auto* handle = from_c(structure);
  if (!handle->structure) return FDS_STATUS_INVALID_ARGUMENT;
  FDS_ABI_TRY {
    fds::Tag cpp_tag = to_cpp(tag);
    if (!handle->structure->contains(cpp_tag)) return FDS_STATUS_NOT_FOUND;
    bool ok = handle->structure->set_value(fds::ElementPath(cpp_tag),
                                            to_owned_value(value, value_length));
    return ok ? FDS_STATUS_OK : FDS_STATUS_INVALID_ARGUMENT;
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
}

fds_status_t fds_structure_set(fds_structure_t* structure, fds_tag_t tag, fds_vr_t vr,
                                const uint8_t* value, size_t value_length) {
  if (structure == nullptr || (value == nullptr && value_length != 0)) {
    return FDS_STATUS_INVALID_ARGUMENT;
  }
  auto* handle = from_c(structure);
  if (!handle->structure) return FDS_STATUS_INVALID_ARGUMENT;
  FDS_ABI_TRY {
    // The VR is ignored when replacing an existing element, but must be a
    // valid scalar VR when it can govern insertion of a new element.
    if (!handle->structure->contains(to_cpp(tag)) && !is_valid_insert_vr(vr)) {
      return FDS_STATUS_INVALID_ARGUMENT;
    }
    bool ok = handle->structure->set(to_cpp(tag), from_c(vr), to_owned_value(value, value_length));
    return ok ? FDS_STATUS_OK : FDS_STATUS_INVALID_ARGUMENT;
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
}

fds_status_t fds_structure_erase(fds_structure_t* structure, fds_tag_t tag) {
  if (structure == nullptr) return FDS_STATUS_INVALID_ARGUMENT;
  auto* handle = from_c(structure);
  if (!handle->structure) return FDS_STATUS_INVALID_ARGUMENT;
  FDS_ABI_TRY {
    bool ok = handle->structure->erase(to_cpp(tag));
    return ok ? FDS_STATUS_OK : FDS_STATUS_NOT_FOUND;
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
}

fds_status_t fds_structure_erase_private(fds_structure_t* structure, size_t* out_count) {
  if (structure == nullptr || out_count == nullptr) return FDS_STATUS_INVALID_ARGUMENT;
  *out_count = 0;
  auto* handle = from_c(structure);
  if (!handle->structure) return FDS_STATUS_INVALID_ARGUMENT;
  FDS_ABI_TRY {
    *out_count = handle->structure->erase_private_elements();
    return FDS_STATUS_OK;
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
}

fds_status_t fds_structure_erase_recursive(fds_structure_t* structure, fds_tag_t tag,
                                            size_t* out_count) {
  if (structure == nullptr || out_count == nullptr) return FDS_STATUS_INVALID_ARGUMENT;
  *out_count = 0;
  auto* handle = from_c(structure);
  if (!handle->structure) return FDS_STATUS_INVALID_ARGUMENT;
  FDS_ABI_TRY {
    *out_count = handle->structure->erase_recursive(to_cpp(tag));
    return FDS_STATUS_OK;
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
}

fds_status_t fds_structure_set_value_recursive(fds_structure_t* structure, fds_tag_t tag,
                                                const uint8_t* value, size_t value_length,
                                                size_t* out_count) {
  if (structure == nullptr || out_count == nullptr || (value == nullptr && value_length != 0)) {
    return FDS_STATUS_INVALID_ARGUMENT;
  }
  *out_count = 0;
  auto* handle = from_c(structure);
  if (!handle->structure) return FDS_STATUS_INVALID_ARGUMENT;
  FDS_ABI_TRY {
    *out_count = handle->structure->set_value_recursive(to_cpp(tag),
                                                          to_owned_value(value, value_length));
    return FDS_STATUS_OK;
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
}

/* --- A1.7: path-based (nested) mutation -------------------------------- */

fds_status_t fds_structure_find_path(const fds_structure_t* structure,
                                      const fds_path_step_t* steps, size_t step_count,
                                      const fds_element_t** out_element) {
  if (out_element == nullptr) return FDS_STATUS_INVALID_ARGUMENT;
  *out_element = nullptr;
  if (structure == nullptr || (steps == nullptr && step_count != 0)) {
    return FDS_STATUS_INVALID_ARGUMENT;
  }
  const auto* handle = from_c(structure);
  if (!handle->structure) return FDS_STATUS_NOT_FOUND;
  FDS_ABI_TRY {
    const fds::Element* found = handle->structure->find(to_cpp_path(steps, step_count));
    if (found == nullptr) return FDS_STATUS_NOT_FOUND;
    *out_element = to_c(found);
    return FDS_STATUS_OK;
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
}

fds_status_t fds_structure_set_value_path(fds_structure_t* structure, const fds_path_step_t* steps,
                                           size_t step_count, const uint8_t* value,
                                           size_t value_length) {
  if (structure == nullptr || (steps == nullptr && step_count != 0) ||
      (value == nullptr && value_length != 0)) {
    return FDS_STATUS_INVALID_ARGUMENT;
  }
  auto* handle = from_c(structure);
  if (!handle->structure) return FDS_STATUS_INVALID_ARGUMENT;
  FDS_ABI_TRY {
    fds::ElementPath path = to_cpp_path(steps, step_count);
    if (handle->structure->find(path) == nullptr) return FDS_STATUS_NOT_FOUND;
    bool ok = handle->structure->set_value(path, to_owned_value(value, value_length));
    return ok ? FDS_STATUS_OK : FDS_STATUS_INVALID_ARGUMENT;
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
}

fds_status_t fds_structure_erase_path(fds_structure_t* structure, const fds_path_step_t* steps,
                                       size_t step_count) {
  if (structure == nullptr || (steps == nullptr && step_count != 0)) {
    return FDS_STATUS_INVALID_ARGUMENT;
  }
  auto* handle = from_c(structure);
  if (!handle->structure) return FDS_STATUS_INVALID_ARGUMENT;
  FDS_ABI_TRY {
    bool ok = handle->structure->erase(to_cpp_path(steps, step_count));
    return ok ? FDS_STATUS_OK : FDS_STATUS_NOT_FOUND;
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
}

fds_status_t fds_structure_insert_path(fds_structure_t* structure,
                                        const fds_path_step_t* parent_steps,
                                        size_t parent_step_count, fds_tag_t tag, fds_vr_t vr,
                                        const uint8_t* value, size_t value_length) {
  if (structure == nullptr || (parent_steps == nullptr && parent_step_count != 0) ||
      (value == nullptr && value_length != 0)) {
    return FDS_STATUS_INVALID_ARGUMENT;
  }
  auto* handle = from_c(structure);
  if (!handle->structure) return FDS_STATUS_INVALID_ARGUMENT;
  FDS_ABI_TRY {
    fds::ElementPath parent = to_cpp_path(parent_steps, parent_step_count);
    fds::Value cpp_value = to_owned_value(value, value_length);
    if (vr == FDS_VR_UNKNOWN) {
      return to_c(fds::mutation::insert_inferred(*handle->structure, parent, to_cpp(tag),
                                                  std::move(cpp_value)));
    }
    if (!is_valid_insert_vr(vr)) return FDS_STATUS_INVALID_ARGUMENT;
    return to_c(fds::mutation::insert(*handle->structure, parent, to_cpp(tag), from_c(vr),
                                       std::move(cpp_value)));
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
}

fds_status_t fds_structure_decode_text_path(const fds_structure_t* structure,
                                             const fds_path_step_t* steps, size_t step_count,
                                             const char** out_utf8_joined) {
  if (out_utf8_joined == nullptr) return FDS_STATUS_INVALID_ARGUMENT;
  *out_utf8_joined = nullptr;
  if (structure == nullptr || (steps == nullptr && step_count != 0)) {
    return FDS_STATUS_INVALID_ARGUMENT;
  }
  const auto* handle = from_c(structure);
  if (!handle->structure) return FDS_STATUS_NOT_FOUND;
  FDS_ABI_TRY {
    fds::ElementPath path = to_cpp_path(steps, step_count);
    const fds::Element* element = handle->structure->find(path);
    if (element == nullptr) return FDS_STATUS_NOT_FOUND;
    auto context = fds::charset::resolve_character_set_context(*handle->structure, path);
    auto decoded = fds::charset::decode_text(*element, context);
    if (decoded.status != fds::charset::DecodeStatus::Success) return FDS_STATUS_UNSUPPORTED;

    std::string joined;
    for (std::size_t i = 0; i < decoded.values.size(); ++i) {
      if (i > 0) joined += '\\';
      joined += decoded.values[i];
    }
    handle->decoded_text_buffer = std::move(joined);
    *out_utf8_joined = handle->decoded_text_buffer.c_str();
    return FDS_STATUS_OK;
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
}

namespace {
// A1.7: splits a backslash-joined UTF-8 string into VM components -- the
// ABI-boundary inverse of fds_structure_decode_text_path's join, and of
// Python's own '\\'.join()/split('\\') convention on the other side of
// this same boundary.
std::vector<std::string> split_backslash(const char* joined) {
  std::vector<std::string> parts;
  std::string current;
  for (const char* p = joined; *p != '\0'; ++p) {
    if (*p == '\\') {
      parts.push_back(std::move(current));
      current.clear();
    } else {
      current.push_back(*p);
    }
  }
  parts.push_back(std::move(current));
  return parts;
}
}  // namespace

fds_status_t fds_structure_set_text_path(fds_structure_t* structure, const fds_path_step_t* steps,
                                          size_t step_count, const char* utf8_joined) {
  if (structure == nullptr || (steps == nullptr && step_count != 0) || utf8_joined == nullptr) {
    return FDS_STATUS_INVALID_ARGUMENT;
  }
  auto* handle = from_c(structure);
  if (!handle->structure) return FDS_STATUS_INVALID_ARGUMENT;
  FDS_ABI_TRY {
    auto status = fds::charset::set_text(*handle->structure, to_cpp_path(steps, step_count),
                                          split_backslash(utf8_joined));
    return to_c(status);
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
}

fds_status_t fds_structure_insert_text_path(fds_structure_t* structure,
                                             const fds_path_step_t* parent_steps,
                                             size_t parent_step_count, fds_tag_t tag, fds_vr_t vr,
                                             const char* utf8_joined) {
  if (structure == nullptr || (parent_steps == nullptr && parent_step_count != 0) ||
      utf8_joined == nullptr) {
    return FDS_STATUS_INVALID_ARGUMENT;
  }
  auto* handle = from_c(structure);
  if (!handle->structure) return FDS_STATUS_INVALID_ARGUMENT;
  FDS_ABI_TRY {
    fds::ElementPath parent = to_cpp_path(parent_steps, parent_step_count);
    auto values = split_backslash(utf8_joined);
    if (vr == FDS_VR_UNKNOWN) {
      return to_c(fds::charset::insert_text_inferred(*handle->structure, parent, to_cpp(tag),
                                                       values));
    }
    if (!is_valid_insert_vr(vr)) return FDS_STATUS_INVALID_ARGUMENT;
    return to_c(fds::charset::insert_text(*handle->structure, parent, to_cpp(tag), from_c(vr),
                                           values));
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
}

fds_tag_t fds_element_tag(const fds_element_t* element) {
  if (element == nullptr) return fds_tag_t{0, 0};
  fds::Tag t = from_c(element)->tag();
  return fds_tag_t{t.group, t.element};
}

fds_vr_t fds_element_vr(const fds_element_t* element) {
  if (element == nullptr) return FDS_VR_UNKNOWN;
  return to_c(from_c(element)->vr());
}

int fds_element_is_sequence(const fds_element_t* element) {
  if (element == nullptr) return 0;
  return from_c(element)->is_sequence() ? 1 : 0;
}

fds_status_t fds_element_value_bytes(const fds_element_t* element, const uint8_t** out_data,
                                      size_t* out_length) {
  if (element == nullptr || out_data == nullptr || out_length == nullptr) {
    return FDS_STATUS_INVALID_ARGUMENT;
  }
  *out_data = nullptr;
  *out_length = 0;
  const fds::Element* e = from_c(element);
  if (e->is_sequence()) return FDS_STATUS_INVALID_ARGUMENT;
  FDS_ABI_TRY {
    auto bytes = e->value().bytes();
    *out_data = reinterpret_cast<const uint8_t*>(bytes.data());
    *out_length = bytes.size();
    return FDS_STATUS_OK;
  }
  FDS_ABI_CATCH(FDS_STATUS_INTERNAL_ERROR)
}

fds_status_t fds_element_sequence_item_count(const fds_element_t* element, size_t* out_count) {
  if (element == nullptr || out_count == nullptr) return FDS_STATUS_INVALID_ARGUMENT;
  *out_count = 0;
  const fds::Sequence* seq = sequence_of(element);
  if (seq == nullptr) return FDS_STATUS_INVALID_ARGUMENT;
  *out_count = seq->items().size();
  return FDS_STATUS_OK;
}

fds_status_t fds_element_sequence_item_element_count(const fds_element_t* element,
                                                      size_t item_index, size_t* out_count) {
  if (element == nullptr || out_count == nullptr) return FDS_STATUS_INVALID_ARGUMENT;
  *out_count = 0;
  const fds::Sequence* seq = sequence_of(element);
  if (seq == nullptr || item_index >= seq->items().size()) return FDS_STATUS_INVALID_ARGUMENT;
  *out_count = seq->items()[item_index].elements().size();
  return FDS_STATUS_OK;
}

fds_status_t fds_element_sequence_item_element_at(const fds_element_t* element, size_t item_index,
                                                   size_t element_index,
                                                   const fds_element_t** out_element) {
  if (element == nullptr || out_element == nullptr) return FDS_STATUS_INVALID_ARGUMENT;
  *out_element = nullptr;
  const fds::Sequence* seq = sequence_of(element);
  if (seq == nullptr || item_index >= seq->items().size()) return FDS_STATUS_INVALID_ARGUMENT;
  const auto& elements = seq->items()[item_index].elements();
  if (element_index >= elements.size()) return FDS_STATUS_NOT_FOUND;
  *out_element = to_c(&elements[element_index]);
  return FDS_STATUS_OK;
}

}  // extern "C"
