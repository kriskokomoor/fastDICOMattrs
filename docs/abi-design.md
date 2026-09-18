# fastDICOMattrs — C ABI Design

`abi/include/fastdicomattrs_c/fds.h` is the only header a non-C++ binding should ever need.
It exists so Python (today) and Rust/C#/Java/etc. (future) can link against one narrow, stable,
C-linkage surface instead of the C++ API, which is neither ABI-stable across compilers nor
representable in most FFI systems.

## Rules

* **Prefix:** every exported symbol and type starts with `fds_`.
* **Opaque handles only.** `fds_structure_t` and `fds_element_t` are incomplete types in the
  public header. Callers may only hold and pass pointers to them; they can never be constructed,
  copied, or inspected as structs by a caller.
* **Fixed-width integers only.** `uint16_t`, `uint32_t`, `uint64_t`, `size_t` — never `int`,
  `long`, or a C++ type.
* **Explicit status codes, no exceptions.** Every function that can fail returns `fds_status_t`;
  no `fds_*` function throws, and no C++ exception is allowed to propagate across the ABI
  boundary. Every `extern "C"` function body in `abi/src/` is wrapped so that any C++ exception is
  caught internally and converted to `FDS_STATUS_INTERNAL_ERROR` — the one status code that means
  "something threw that shouldn't have; this is a library bug."
* **Library-owned memory.** All memory reachable through the ABI (structures, elements, byte
  buffers returned by value accessors, diagnostic strings) is owned by the `fds_structure_t` it
  came from. There is exactly one thing a caller ever frees directly: `fds_structure_free`.
  Freeing a structure invalidates every `fds_element_t*`/pointer obtained from it — using one
  afterward is undefined behavior, exactly like a dangling C++ pointer.
  `fds_parse_buffer` copies its caller-supplied bytes into the structure handle, so the caller may
  release or reuse the input buffer immediately after the function returns.
* **No STL, no C++ classes, no templates, no references in signatures.** The ABI layer
  (`abi/src/fds_abi.cpp`) is the only code allowed to translate between `fds::*` C++ types and
  `fds_*` C types.
* **ABI version query.** `fds_abi_version()` returns a `uint32_t` packed as
  `(major << 16) | (minor << 8) | patch`, bumped whenever the ABI changes. Additive changes
  (new function, new enum value appended) bump `minor`; anything that changes an existing
  function's signature or a struct's layout bumps `major` and is a breaking change no existing
  binary can tolerate. This increment ships as `0.4.0` — no stability promise yet, by design,
  since the surface is still small and evolving; the version query mechanism itself is the
  stable part. This increment (M1.1 of the fastDICOM family) adds
  `fds_structure_write_buffer`/`_with_stats`, an additive change, bumping the ABI to `0.5.0`.
  A1.7 adds `fds_path_step_t` and seven `*_path` nested-mutation functions plus four new
  `fds_status_t` values, another additive change, bumping the ABI to `0.6.0`.

## Pointer validity (the ownership rule, precisely)

> Pointers returned by element/value accessors are owned by the containing structure and remain
> valid only as documented by the API.

Precisely: an `fds_element_t*` (or a `const uint8_t*` from `fds_element_value_bytes`) is valid
from the moment it is returned until whichever happens first:

1. `fds_structure_free` is called on the owning `fds_structure_t`, or
2. **any** mutation call (`fds_structure_set_value`, `fds_structure_set`, `fds_structure_erase`,
   `fds_structure_erase_private`, `fds_structure_erase_recursive`,
   `fds_structure_set_value_recursive`, or, as of A1.7, any of the seven `*_path` functions --
   `fds_structure_set_value_path`, `_erase_path`, `_insert_path`, `_set_text_path`,
   `_insert_text_path`; `_find_path` and `_decode_text_path` are read-only and invalidate
   nothing) is made against that structure — even one that, in principle, didn't touch the
   specific element a caller's pointer refers to, and even one that ultimately returned a
   non-OK status other than a pure no-op (see below).

Rule 2 is deliberately coarse rather than "only pointers downstream of the change, or only
siblings of an erased/inserted element": `set`/`erase`/`insert` (root or nested) can shift or
reallocate the backing `std::vector<Element>` at any level of the tree (insertion may relocate
every element in that vector; erasure shifts every element after the removed one), and
`set_value`/`set_text` only guarantee the *mutated* element's data pointer changes, not that
every other element's address is stable across a future increment's implementation. Stating the
narrowest-currently-true rule would be a promise this ABI cannot keep as the C++ implementation
evolves; stating the coarse rule is simple, safe, and matches how `fds_structure_free` already
documents its own invalidation. **After any mutation call, re-fetch every `fds_element_t*` you
still need** via `fds_structure_find`/`fds_structure_find_path`/`fds_structure_element_at`/
`fds_element_sequence_item_element_at`, rather than reusing one obtained beforehand.

A call that returns a non-OK status without actually applying any change (e.g.
`fds_structure_insert_path` returning `FDS_STATUS_ALREADY_EXISTS`, or `fds_structure_erase_path`
returning `FDS_STATUS_NOT_FOUND`) does *not* invalidate anything -- `fds_structure_is_modified`
staying false after such a call is the caller-observable confirmation of that. The C ABI itself
has no runtime mechanism to detect or reject use of an already-invalidated pointer (dereferencing
one is undefined behavior, exactly like a dangling C++ pointer -- see docs/architecture's note on
why this layer deliberately does not attempt one). The Python binding is the one layer in this
codebase that *does* enforce this at runtime, via a generation counter stamped onto every
`Element`/`Item` at creation and checked before use, raising `StaleElementError` deterministically
instead of touching invalidated memory -- see `python/fastdicomattrs/__init__.py`.

`fds_structure_write_buffer`/`_with_stats` returns a pointer under this same structure-owned rule,
plus one addition specific to it: calling either of those two functions again on the same
structure also invalidates the pointer they previously returned (the new call overwrites the same
owned storage) -- this is in addition to, not instead of, invalidation by `fds_structure_free` or
any mutation call.

## Implementation technique (not part of the contract)

`fds_element_t` is never defined, even in the `.cpp`. `abi/src/fds_abi.cpp` obtains a
`const fds::Element*` from the C++ layer and hands it back to the caller via
`reinterpret_cast<const fds_element_t*>(ptr)`, casting back the same way on every call that takes
one in. This avoids allocating a wrapper object per element while keeping the C header fully
opaque. This is an implementation detail of `fds_abi.cpp`; nothing about the public contract
depends on it, and it could change to a wrapper-struct scheme without being an ABI break.

`fds_structure_t` **is** given a definition, but only inside `fds_abi.cpp`: a small struct that
bundles the parsed `std::unique_ptr<fds::DICOMStructure>` together with the `ParseResult`'s
diagnostics (so `fds_structure_diagnostic_at` has something to read after the `ParseResult` itself
has gone out of scope).

## Surface shipped in this increment

```c
uint32_t fds_abi_version(void);
const char* fds_status_message(fds_status_t status);

void fds_parse_options_init_defaults(fds_parse_options_t* options);

fds_status_t fds_parse_file(const char* path, const fds_parse_options_t* options, fds_structure_t** out);
fds_status_t fds_parse_buffer(const uint8_t* data, size_t length, const fds_parse_options_t* options, fds_structure_t** out);
void fds_structure_free(fds_structure_t* structure);

size_t fds_structure_element_count(const fds_structure_t* structure);
fds_status_t fds_structure_element_at(const fds_structure_t* structure, size_t index, const fds_element_t** out);
fds_status_t fds_structure_find(const fds_structure_t* structure, fds_tag_t tag, const fds_element_t** out);
int fds_structure_contains(const fds_structure_t* structure, fds_tag_t tag);

size_t fds_structure_diagnostic_count(const fds_structure_t* structure);
fds_status_t fds_structure_diagnostic_at(const fds_structure_t* structure, size_t index, fds_diagnostic_t* out);

fds_status_t fds_structure_write_file(const fds_structure_t* structure, const char* path, uint64_t* out_bytes_written);
fds_status_t fds_structure_write_file_with_stats(const fds_structure_t* structure, const char* path, uint64_t* out_bytes_written, uint64_t* out_source_backed_value_bytes, uint64_t* out_regenerated_value_bytes);
fds_status_t fds_structure_write_buffer(const fds_structure_t* structure, const uint8_t** out_data, size_t* out_length);
fds_status_t fds_structure_write_buffer_with_stats(const fds_structure_t* structure, const uint8_t** out_data, size_t* out_length, uint64_t* out_source_backed_value_bytes, uint64_t* out_regenerated_value_bytes);

int fds_structure_is_modified(const fds_structure_t* structure);
fds_status_t fds_structure_set_value(fds_structure_t* structure, fds_tag_t tag, const uint8_t* value, size_t value_length);
fds_status_t fds_structure_set(fds_structure_t* structure, fds_tag_t tag, fds_vr_t vr, const uint8_t* value, size_t value_length);
fds_status_t fds_structure_erase(fds_structure_t* structure, fds_tag_t tag);
fds_status_t fds_structure_erase_private(fds_structure_t* structure, size_t* out_count);
fds_status_t fds_structure_erase_recursive(fds_structure_t* structure, fds_tag_t tag, size_t* out_count);
fds_status_t fds_structure_set_value_recursive(fds_structure_t* structure, fds_tag_t tag, const uint8_t* value, size_t value_length, size_t* out_count);

/* A1.7 -- path-based (nested) mutation, at any nesting depth. See
 * fds_path_step_t's own doc comment in fds.h for the existing-element-locator vs.
 * container-locator step-array shapes these take. */
fds_status_t fds_structure_find_path(const fds_structure_t* structure, const fds_path_step_t* steps, size_t step_count, const fds_element_t** out);
fds_status_t fds_structure_set_value_path(fds_structure_t* structure, const fds_path_step_t* steps, size_t step_count, const uint8_t* value, size_t value_length);
fds_status_t fds_structure_erase_path(fds_structure_t* structure, const fds_path_step_t* steps, size_t step_count);
fds_status_t fds_structure_insert_path(fds_structure_t* structure, const fds_path_step_t* parent_steps, size_t parent_step_count, fds_tag_t tag, fds_vr_t vr, const uint8_t* value, size_t value_length);
fds_status_t fds_structure_decode_text_path(const fds_structure_t* structure, const fds_path_step_t* steps, size_t step_count, const char** out_utf8_joined);
fds_status_t fds_structure_set_text_path(fds_structure_t* structure, const fds_path_step_t* steps, size_t step_count, const char* utf8_joined);
fds_status_t fds_structure_insert_text_path(fds_structure_t* structure, const fds_path_step_t* parent_steps, size_t parent_step_count, fds_tag_t tag, fds_vr_t vr, const char* utf8_joined);

fds_tag_t fds_element_tag(const fds_element_t* element);
fds_vr_t fds_element_vr(const fds_element_t* element);
int fds_element_is_sequence(const fds_element_t* element);
fds_status_t fds_element_value_bytes(const fds_element_t* element, const uint8_t** out_data, size_t* out_length);

fds_status_t fds_element_sequence_item_count(const fds_element_t* element, size_t* out_count);
fds_status_t fds_element_sequence_item_element_count(const fds_element_t* element, size_t item_index, size_t* out_count);
fds_status_t fds_element_sequence_item_element_at(const fds_element_t* element, size_t item_index, size_t element_index, const fds_element_t** out);
```

`fds_diagnostic_t` and `fds_status_t`/`fds_vr_t`/`fds_fidelity_t`/`fds_tag_t`/`fds_parse_options_t`
are plain C structs/enums defined in full in the public header (they carry no pointers into
library-owned memory except the `message` field of `fds_diagnostic_t`, which follows the same
structure-owned-pointer rule as element accessors).

## Deliberately absent from this ABI in this increment

* **Path-based upsert.** There is no `fds_structure_upsert_path`/`_insert_or_replace` — an
  experimental version was cut before A1.7 froze (ambiguous insert/replace semantics, a
  caller-supplied VR silently discarded on the replace branch). The documented two-call idiom
  (`fds_structure_find_path` then `_set_value_path` or `_insert_path`) is the supported
  replacement. `fds_structure_set` (root-only, pre-A1.7) keeps its own pre-existing upsert
  behavior unchanged for compatibility, but is not the pattern to follow for new nested code.
* **A recursive-traversal ABI call.** No `fds_structure_visit_path` or similar exists; a caller
  needing full recursive traversal composes it from `fds_structure_element_at`/
  `fds_element_is_sequence`/`fds_element_sequence_item_element_at`, exactly as the Python
  binding's `iter_elements(recursive=True)` does entirely in Python, with no ABI gap.
* **Keyword-string lookup/insertion** (e.g. inserting by `"PatientID"` instead of `(0x0010,
  0x0020)`) is deferred; numeric tags are the only supported addressing form in V1.
* `fds_parse_stream` — no stream ABI shape (buffered? callback-based reader?) has been decided;
  the C++ `parse_stream` itself is a stub, so there is nothing to wrap yet.
* Detailed Pixel Data accessors (`fds_structure_pixel_data`) — the ABI exposes the native versus
  encapsulated classification needed by corpus inspection, but not fragment or source-span
  traversal yet.

## Threading

No `fds_*` function is thread-safe against concurrent calls that share the same
`fds_structure_t*`, including read-only accessors racing a `fds_structure_free` on another
thread. Concurrent read-only access from multiple threads to the same already-parsed,
never-mutated structure is safe (no shared mutable state is touched by the read path). This
matches the underlying C++ `DICOMStructure`, which provides the same guarantee and no more.
