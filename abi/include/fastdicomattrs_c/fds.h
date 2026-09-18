/* fastDICOMattrs C ABI -- see docs/abi-design.md.
 *
 * This is the only header a non-C++ binding should need. Every exported
 * symbol is prefixed fds_. Opaque handles only: fds_structure_t and
 * fds_element_t are never defined here and must never be dereferenced by a
 * caller. No exception ever crosses this boundary; every fallible function
 * returns an fds_status_t.
 */
#ifndef FASTDICOMATTRS_C_FDS_H
#define FASTDICOMATTRS_C_FDS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define FDS_API __declspec(dllexport)
#else
#define FDS_API __attribute__((visibility("default")))
#endif

/* ---------------------------------------------------------------------- */
/* Opaque handles                                                          */
/* ---------------------------------------------------------------------- */

typedef struct fds_structure fds_structure_t;
typedef struct fds_element fds_element_t;

/* ---------------------------------------------------------------------- */
/* Plain value types                                                      */
/* ---------------------------------------------------------------------- */

typedef struct {
  uint16_t group;
  uint16_t element;
} fds_tag_t;

/* A1.7: one step of a path -- see docs/abi-design.md and the *_path
 * functions below. `has_item_index` == 0 means "this step names a leaf
 * element itself" (only ever legal as the *last* step of an existing-
 * element locator array, exactly mirroring the C++ ElementPath contract --
 * see element_path.hpp); `has_item_index` != 0 means "descend into Item
 * `item_index` of the Sequence named by `tag`." An array of these is used
 * in two distinct shapes, matching the C++ layer exactly:
 *   - an *existing-element locator* (fds_structure_find_path,
 *     _set_value_path, _erase_path, _decode_text_path, _set_text_path):
 *     every step but the last has_item_index != 0; the last has
 *     has_item_index == 0 and names the element itself.
 *   - a *container locator* (the `parent_steps`/`parent_step_count`
 *     arguments of fds_structure_insert_path/_insert_text_path): every
 *     step, with no exception for the last, has has_item_index != 0; an
 *     empty array (step_count == 0) means the root dataset. Never pass an
 *     existing-element-locator-shaped array (trailing has_item_index == 0)
 *     as a container locator, or vice versa -- see each function's own
 *     doc comment. */
typedef struct {
  fds_tag_t tag;
  int has_item_index;
  size_t item_index;
} fds_path_step_t;

typedef enum {
  FDS_STATUS_OK = 0,
  FDS_STATUS_NOT_FOUND = 1,
  FDS_STATUS_INVALID_ARGUMENT = 2,
  FDS_STATUS_IO_ERROR = 3,
  FDS_STATUS_PARSE_FAILED = 4,
  FDS_STATUS_UNSUPPORTED = 5,
  FDS_STATUS_INTERNAL_ERROR = 6, /* a C++ exception was caught at the ABI boundary: a library bug */

  /* A1.7: additive (see docs/abi-design.md's versioning rule) -- returned
   * only by the *_path insertion functions below (fds_structure_insert_path,
   * fds_structure_insert_text_path). Every pre-A1.7 function's status
   * vocabulary is unchanged; these four exist because a caller (concretely,
   * the Python binding) benefits from distinguishing them from a generic
   * FDS_STATUS_INVALID_ARGUMENT -- every other new failure reason these two
   * functions can have (bad container path, duplicate tag aside, VR/charset/
   * length problems not listed here) still collapses to
   * FDS_STATUS_INVALID_ARGUMENT, deliberately not one new code per reason. */
  FDS_STATUS_ALREADY_EXISTS = 7,             /* the target tag already exists in that container */
  FDS_STATUS_VR_REQUIRED = 8,                /* VR inference (FDS_VR_UNKNOWN sentinel) was ambiguous/unavailable */
  FDS_STATUS_UNREPRESENTABLE_CHARACTER = 9,  /* insert_text_path only: no declared repertoire can encode a character */
  FDS_STATUS_INVALID_UNICODE_INPUT = 10,     /* insert_text_path only: input is not well-formed UTF-8 */
} fds_status_t;

typedef enum {
  FDS_FIDELITY_FAST = 0,
  FDS_FIDELITY_STANDARD = 1,
  FDS_FIDELITY_LOSSLESS = 2,
} fds_fidelity_t;

typedef enum {
  FDS_VR_AE = 0, FDS_VR_AS, FDS_VR_AT, FDS_VR_CS, FDS_VR_DA, FDS_VR_DS, FDS_VR_DT, FDS_VR_FL,
  FDS_VR_FD, FDS_VR_IS, FDS_VR_LO, FDS_VR_LT, FDS_VR_OB, FDS_VR_OD, FDS_VR_OF, FDS_VR_OL,
  FDS_VR_OV, FDS_VR_OW, FDS_VR_PN, FDS_VR_SH, FDS_VR_SL, FDS_VR_SQ, FDS_VR_SS, FDS_VR_ST,
  FDS_VR_SV, FDS_VR_TM, FDS_VR_UC, FDS_VR_UI, FDS_VR_UL, FDS_VR_UN, FDS_VR_UR, FDS_VR_US,
  FDS_VR_UT, FDS_VR_UV,
  FDS_VR_UNKNOWN,
} fds_vr_t;

typedef enum {
  FDS_DIAGNOSTIC_INFO = 0,
  FDS_DIAGNOSTIC_WARNING = 1,
  FDS_DIAGNOSTIC_RECOVERABLE_ERROR = 2,
  FDS_DIAGNOSTIC_FATAL_ERROR = 3,
  FDS_DIAGNOSTIC_UNSUPPORTED = 4,
  FDS_DIAGNOSTIC_IO_ERROR = 5,
} fds_diagnostic_severity_t;

typedef struct {
  fds_fidelity_t fidelity;
  uint64_t max_element_count;
  uint64_t max_sequence_depth;
} fds_parse_options_t;

typedef struct {
  fds_diagnostic_severity_t severity;
  /* Owned by the fds_structure_t this diagnostic came from; valid until
   * that structure is freed. Never NULL. */
  const char* message;
  uint64_t offset;
  /* has_tag == 0 means `tag` is not meaningful for this diagnostic. */
  int has_tag;
  fds_tag_t tag;
} fds_diagnostic_t;

/* ---------------------------------------------------------------------- */
/* Version                                                                 */
/* ---------------------------------------------------------------------- */

/* Packed as (major << 16) | (minor << 8) | patch. See docs/abi-design.md
 * for the compatibility rules this encodes. */
FDS_API uint32_t fds_abi_version(void);

/* A static, human-readable description of `status`. Owned by the library
 * for the process lifetime; never free it. */
FDS_API const char* fds_status_message(fds_status_t status);

FDS_API void fds_parse_options_init_defaults(fds_parse_options_t* options);

/* ---------------------------------------------------------------------- */
/* Parsing                                                                 */
/* ---------------------------------------------------------------------- */

FDS_API fds_status_t fds_parse_file(const char* path, const fds_parse_options_t* options,
                                     fds_structure_t** out_structure);

/* Copies `data` into storage owned by the returned structure. The caller may
 * release or reuse `data` as soon as this function returns. */
FDS_API fds_status_t fds_parse_buffer(const uint8_t* data, size_t length,
                                       const fds_parse_options_t* options,
                                       fds_structure_t** out_structure);

/* Invalidates every fds_element_t* obtained from `structure`. Safe to call
 * with NULL (no-op). */
FDS_API void fds_structure_free(fds_structure_t* structure);

/* ---------------------------------------------------------------------- */
/* Structure inspection                                                    */
/* ---------------------------------------------------------------------- */

FDS_API size_t fds_structure_element_count(const fds_structure_t* structure);

FDS_API fds_status_t fds_structure_element_at(const fds_structure_t* structure, size_t index,
                                               const fds_element_t** out_element);

FDS_API fds_status_t fds_structure_find(const fds_structure_t* structure, fds_tag_t tag,
                                         const fds_element_t** out_element);

FDS_API int fds_structure_contains(const fds_structure_t* structure, fds_tag_t tag);

FDS_API size_t fds_structure_diagnostic_count(const fds_structure_t* structure);

FDS_API fds_status_t fds_structure_diagnostic_at(const fds_structure_t* structure, size_t index,
                                                  fds_diagnostic_t* out_diagnostic);

/* Structure-level metadata. The returned UID is owned by `structure` and
 * remains valid until fds_structure_free(). */
FDS_API const char* fds_structure_transfer_syntax_uid(const fds_structure_t* structure);
FDS_API int fds_structure_transfer_syntax_is_explicit_vr(const fds_structure_t* structure);
FDS_API int fds_structure_transfer_syntax_is_little_endian(const fds_structure_t* structure);

/* Returns 0 when Pixel Data is absent, 1 for native Pixel Data, and 2 for
 * encapsulated Pixel Data. */
FDS_API int fds_structure_pixel_data_kind(const fds_structure_t* structure);

/* Writes `structure` to `path`. An unmodified structure requires
 * FDS_FIDELITY_LOSSLESS (byte-identical reproduction). A modified structure
 * requires FDS_FIDELITY_LOSSLESS or FDS_FIDELITY_STANDARD (a valid,
 * semantically-correct reconstruction -- not a byte-identical guarantee).
 * Returns FDS_STATUS_UNSUPPORTED otherwise. See docs/roundtrip-contract.md
 * "Two write contracts". */
FDS_API fds_status_t fds_structure_write_file(const fds_structure_t* structure, const char* path,
                                               uint64_t* out_bytes_written);

/* Same as fds_structure_write_file, but also reports byte-level
 * preservation of untouched content: *out_source_backed_value_bytes is
 * value-payload bytes (plus Pixel Data, which this library never mutates)
 * written verbatim from the original source; *out_regenerated_value_bytes
 * is value-payload bytes written from a caller-supplied replacement
 * (set/set_value). Neither counts structural framing (tag/VR/length
 * fields, Item/Sequence delimiters), which this writer always reconstructs
 * regardless of modification -- see docs/roundtrip-contract.md
 * "Reconstruction, not verbatim copy". All three out-params may be NULL if
 * the caller doesn't need that particular count. */
FDS_API fds_status_t fds_structure_write_file_with_stats(
    const fds_structure_t* structure, const char* path, uint64_t* out_bytes_written,
    uint64_t* out_source_backed_value_bytes, uint64_t* out_regenerated_value_bytes);

/* Same as fds_structure_write_file, but serializes into a buffer owned by
 * `structure` instead of writing to a path. `*out_data`/`*out_length`
 * describe the serialized bytes on success; that pointer follows the same
 * structure-owned-pointer rule as fds_element_value_bytes (see
 * docs/abi-design.md "Pointer validity") plus one more trigger: it stays
 * valid until whichever happens first -- fds_structure_free, any mutation
 * call, or the *next* fds_structure_write_buffer/_with_stats call on this
 * same structure (which overwrites the same owned storage). Uses the same
 * write contract (fidelity / modified-state rules) as
 * fds_structure_write_file -- see its doc comment above. */
FDS_API fds_status_t fds_structure_write_buffer(const fds_structure_t* structure,
                                                 const uint8_t** out_data, size_t* out_length);

/* Same as fds_structure_write_buffer, but also reports byte-level
 * preservation of untouched content -- see
 * fds_structure_write_file_with_stats for exactly what the two counters
 * mean. Either may be NULL if the caller doesn't need that particular
 * count. */
FDS_API fds_status_t fds_structure_write_buffer_with_stats(
    const fds_structure_t* structure, const uint8_t** out_data, size_t* out_length,
    uint64_t* out_source_backed_value_bytes, uint64_t* out_regenerated_value_bytes);

/* ---------------------------------------------------------------------- */
/* Structure mutation                                                      */
/* ---------------------------------------------------------------------- */

/* Every mutation call below invalidates every fds_element_t* and value
 * data pointer previously obtained from `structure` -- the backing element
 * list can be reallocated or shifted by insertion/erasure. Re-fetch via
 * fds_structure_find / fds_structure_element_at after calling any of these.
 * See docs/abi-design.md "Pointer validity". Top-level tags only, except
 * fds_structure_erase_private, fds_structure_erase_recursive, and
 * fds_structure_set_value_recursive below, which operate at any nesting
 * depth by design; general per-path nested mutation remains C++-only in
 * this increment. */

FDS_API int fds_structure_is_modified(const fds_structure_t* structure);

/* Replaces the value of an existing top-level element; its VR is preserved.
 * FDS_STATUS_NOT_FOUND if `tag` is absent. FDS_STATUS_INVALID_ARGUMENT if
 * `tag` names a sequence element, or `value` cannot be encoded in the
 * element's length form (e.g. grows a short-form VR's value past 65534
 * bytes) -- the element is left unchanged in either failure case.
 *
 * This library does not auto-pad `value`. An odd-length value is rejected
 * with FDS_STATUS_INVALID_ARGUMENT and must be padded by the caller first
 * (trailing 0x00, or 0x20 for text VRs other than UI). */
FDS_API fds_status_t fds_structure_set_value(fds_structure_t* structure, fds_tag_t tag,
                                              const uint8_t* value, size_t value_length);

/* Upserts a top-level element: if `tag` already exists, replaces its value
 * (VR preserved, the `vr` argument ignored); otherwise inserts a brand-new
 * element with the given `vr` at its sorted tag position (DICOM data sets
 * require ascending tag order). FDS_STATUS_INVALID_ARGUMENT if `tag` names
 * an existing sequence element, or `value` cannot be encoded for the
 * relevant length form. New scalar elements cannot use FDS_VR_SQ or
 * FDS_VR_UNKNOWN; invalid enum values are also rejected. */
FDS_API fds_status_t fds_structure_set(fds_structure_t* structure, fds_tag_t tag, fds_vr_t vr,
                                        const uint8_t* value, size_t value_length);

/* Removes a top-level element. FDS_STATUS_NOT_FOUND if `tag` is absent. */
FDS_API fds_status_t fds_structure_erase(fds_structure_t* structure, fds_tag_t tag);

/* Removes every element with an odd group number (DICOM's private-tag
 * convention), at any nesting depth (not just top-level, unlike the other
 * mutation calls above). *out_count receives how many were removed; 0 is
 * not an error. */
FDS_API fds_status_t fds_structure_erase_private(fds_structure_t* structure, size_t* out_count);

/* Removes every element whose tag equals `tag`, at any nesting depth (not
 * just top-level) -- the recursive counterpart to fds_structure_erase().
 * *out_count receives how many were removed; 0 is not an error. */
FDS_API fds_status_t fds_structure_erase_recursive(fds_structure_t* structure, fds_tag_t tag,
                                                    size_t* out_count);

/* Replaces the value of every non-sequence element whose tag equals `tag`,
 * at any nesting depth (not just top-level) -- the recursive counterpart to
 * fds_structure_set_value(). Each occurrence follows the same encoding
 * rules as fds_structure_set_value() (VR preserved, no auto-padding); an
 * occurrence that is itself a sequence element is left unchanged and not
 * counted. *out_count receives how many were changed; 0 is not an error. */
FDS_API fds_status_t fds_structure_set_value_recursive(fds_structure_t* structure, fds_tag_t tag,
                                                        const uint8_t* value, size_t value_length,
                                                        size_t* out_count);

/* ---------------------------------------------------------------------- */
/* Path-based (nested) mutation -- A1.7                                    */
/* ---------------------------------------------------------------------- */

/* Every function below takes an fds_path_step_t array -- see its doc
 * comment above for the two distinct shapes (existing-element locator vs.
 * container locator) an array can take, and which shape each parameter
 * below requires. All the same pointer-invalidation rules as the
 * tag-based mutation functions above apply: any successful call through
 * this section invalidates every fds_element_t* (and value) pointer
 * previously obtained from `structure`. */

/* Existing-element locator lookup, at any nesting depth -- the path-aware
 * generalization of fds_structure_find(). */
FDS_API fds_status_t fds_structure_find_path(const fds_structure_t* structure,
                                              const fds_path_step_t* steps, size_t step_count,
                                              const fds_element_t** out_element);

/* Replaces an existing element's value at any nesting depth -- the
 * path-aware generalization of fds_structure_set_value(). Same encoding
 * rules (VR preserved, no auto-padding, even length required). */
FDS_API fds_status_t fds_structure_set_value_path(fds_structure_t* structure,
                                                   const fds_path_step_t* steps, size_t step_count,
                                                   const uint8_t* value, size_t value_length);

/* Removes an existing element at any nesting depth -- the path-aware
 * generalization of fds_structure_erase(). If `steps` names a Sequence
 * element, its entire subtree (every Item and everything in it) is
 * removed with it. */
FDS_API fds_status_t fds_structure_erase_path(fds_structure_t* structure,
                                               const fds_path_step_t* steps, size_t step_count);

/* Inserts a brand-new element -- `tag` -- into the container named by
 * `parent_steps`/`parent_step_count` (a *container locator*; see
 * fds_path_step_t's doc comment -- an empty array means the root dataset).
 * `tag` must not already exist there (FDS_STATUS_ALREADY_EXISTS).
 *
 * `vr` is normally the caller-supplied, authoritative VR for the new
 * element (never consulting the dictionary, exactly like
 * fds_structure_set()'s insertion branch) -- FDS_VR_SQ is never valid here
 * (a scalar insertion needs a real scalar VR).
 *
 * FDS_VR_UNKNOWN is a special sentinel MEANING "INFER THE VR FROM THE
 * STANDARD DICTIONARY" -- ONLY IN THIS FUNCTION AND
 * fds_structure_insert_text_path BELOW. It retains its ordinary "no VR"
 * meaning everywhere else in this ABI (e.g. as returned by
 * fds_element_vr()). Inference follows fastDICOMattrs' V1 policy exactly:
 * an unambiguous standard tag is inferred; an ambiguous, dictionary-unknown,
 * or private-data tag returns FDS_STATUS_VR_REQUIRED (the caller must
 * supply an explicit `vr` and retry) -- except a Private Creator
 * declaration (odd group, element 0x0010-0x00FF), which is always inferred
 * as LO (a normative PS3.5 7.8.1 fact, not a dictionary guess). Inference
 * never silently overrides a caller-supplied `vr`: pass an explicit VR to
 * skip inference entirely.
 *
 * Same value-encoding rules as fds_structure_set(). Atomic: every check
 * (container resolution, duplicate, VR, value encodability) completes
 * before `structure` is touched; any non-OK status leaves it unchanged. */
FDS_API fds_status_t fds_structure_insert_path(fds_structure_t* structure,
                                                const fds_path_step_t* parent_steps,
                                                size_t parent_step_count, fds_tag_t tag, fds_vr_t vr,
                                                const uint8_t* value, size_t value_length);

/* Decodes an existing text-VR element's value into Unicode text, using its
 * already-effective Specific Character Set context (A1.5/A1.6). On
 * success, *out_utf8_joined points to a NUL-terminated UTF-8 string, its
 * VM components joined with a literal '\' (mirroring the wire encoding),
 * owned by `structure` under the same rule as fds_structure_write_buffer's
 * pointer (valid until the next call to this function on the same
 * structure, any mutation call, or fds_structure_free). Returns
 * FDS_STATUS_UNSUPPORTED for an element whose VR isn't
 * Specific-Character-Set-governed, or whose declared charset is outside
 * this library's V1 envelope. */
FDS_API fds_status_t fds_structure_decode_text_path(const fds_structure_t* structure,
                                                     const fds_path_step_t* steps, size_t step_count,
                                                     const char** out_utf8_joined);

/* Replaces an existing text-VR element's value from Unicode text (UTF-8,
 * VM components '\'-joined, exactly decode_text_path's own output
 * convention) -- encodes under the element's already-effective Specific
 * Character Set context and its existing VR, atomically. Returns
 * FDS_STATUS_INVALID_UNICODE_INPUT for malformed UTF-8,
 * FDS_STATUS_UNREPRESENTABLE_CHARACTER if no declared repertoire can
 * encode some character, FDS_STATUS_UNSUPPORTED for a non-text VR or an
 * out-of-V1-envelope charset declaration. */
FDS_API fds_status_t fds_structure_set_text_path(fds_structure_t* structure,
                                                  const fds_path_step_t* steps, size_t step_count,
                                                  const char* utf8_joined);

/* Inserts a brand-new text element -- the charset-aware counterpart to
 * fds_structure_insert_path(), with the identical container-locator
 * (`parent_steps`) and FDS_VR_UNKNOWN-means-infer conventions. Resolves
 * the effective Specific Character Set context for the *target container*
 * (not merely its parent's parent -- see
 * docs/architecture/A1_7_MUTATION_ERGONOMICS_AND_PUBLIC_API_REPORT.md
 * section 8), encodes `utf8_joined` under it and `vr`, and inserts --
 * atomically, exactly like fds_structure_insert_path. If VR inference
 * succeeds but yields a non-text VR, returns FDS_STATUS_UNSUPPORTED (this
 * function never falls back to raw-byte insertion). */
FDS_API fds_status_t fds_structure_insert_text_path(fds_structure_t* structure,
                                                     const fds_path_step_t* parent_steps,
                                                     size_t parent_step_count, fds_tag_t tag,
                                                     fds_vr_t vr, const char* utf8_joined);

/* ---------------------------------------------------------------------- */
/* Element inspection                                                      */
/* ---------------------------------------------------------------------- */

FDS_API fds_tag_t fds_element_tag(const fds_element_t* element);

FDS_API fds_vr_t fds_element_vr(const fds_element_t* element);

FDS_API int fds_element_is_sequence(const fds_element_t* element);

/* Precondition: !fds_element_is_sequence(element). *out_data is owned by
 * the containing fds_structure_t and valid on the same terms as any other
 * structure-owned pointer (see docs/abi-design.md). */
FDS_API fds_status_t fds_element_value_bytes(const fds_element_t* element, const uint8_t** out_data,
                                              size_t* out_length);

/* Precondition: fds_element_is_sequence(element). */
FDS_API fds_status_t fds_element_sequence_item_count(const fds_element_t* element,
                                                      size_t* out_count);

FDS_API fds_status_t fds_element_sequence_item_element_count(const fds_element_t* element,
                                                              size_t item_index,
                                                              size_t* out_count);

FDS_API fds_status_t fds_element_sequence_item_element_at(const fds_element_t* element,
                                                           size_t item_index, size_t element_index,
                                                           const fds_element_t** out_element);

#ifdef __cplusplus
}
#endif

#endif /* FASTDICOMATTRS_C_FDS_H */
