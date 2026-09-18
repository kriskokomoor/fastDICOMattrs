# fastDICOMattrs — C++ API Design

This describes the public C++ surface under `include/fastdicomattrs/`. It is a design
document with real signatures, kept in sync with the headers; if they drift, the headers win and
this file should be corrected.

## Design principles

1. **Minimal API.** Every public method must map to one of the eleven "common operations" listed
   in the charter (parse, inspect top-level elements, get a top-level tag, traverse nested
   sequences, name a nested element uniquely, read typed/raw value, replace a value, remove an
   element, inspect pixel data, serialize). Nothing is added "for completeness."
2. **No clinical convenience.** No `patient_name()`, `study_uid()`, etc. Tags are addressed by
   `Tag{group, element}`, never by name.
3. **Diagnostics over booleans.** Parsing and writing return structured results, not `bool`.
4. **Value access is layered.** Raw bytes are always available (`Value::bytes()`); typed decoding
   is opt-in and separate, so a caller who only wants to move/copy data never pays for or risks a
   failed type interpretation.

## Entry points

```cpp
namespace fds {

ParseResult parse_file(const std::filesystem::path& path, const ParseOptions& options = {});
ParseResult parse_buffer(std::span<const std::byte> bytes, const ParseOptions& options = {});
ParseResult parse_stream(std::istream& stream, const ParseOptions& options = {}); // reserved; returns Unsupported today

} // namespace fds
```

`parse_buffer` does not take ownership of `bytes`; the returned `DICOMStructure` holds a
`MemorySource` view over it, so **the caller must keep `bytes` alive** for as long as the
resulting `DICOMStructure` (or any `Value`/`Element` derived from it) is used. Callers who cannot
guarantee that should use `parse_file`, which always owns what it maps/reads, or retain their own
owned byte buffer for the structure's lifetime. The C ABI's `fds_parse_buffer` has a different,
binding-safe contract: it copies the input into storage owned by its returned handle.

## `ParseOptions` / `Fidelity`

```cpp
enum class Fidelity : std::uint8_t { Fast, Standard, Lossless };

struct ParseOptions {
  Fidelity fidelity = Fidelity::Standard;
  std::size_t max_element_count = 1'000'000;   // malformed-input guard
  std::size_t max_sequence_depth = 64;         // malformed-input guard
  bool stop_before_pixel_data = false;         // reserved FAST-mode hint; currently a no-op
};
```

## `ParseResult` / `ParseDiagnostic`

```cpp
enum class DiagnosticSeverity { Info, Warning, RecoverableError, FatalError, Unsupported, IOError };

struct ParseDiagnostic {
  DiagnosticSeverity severity;
  std::string message;
  std::uint64_t offset = 0;
  std::optional<Tag> tag;
};

enum class ParseStatus { Success, SuccessWithWarnings, Failed };

struct ParseResult {
  ParseStatus status = ParseStatus::Failed;
  std::unique_ptr<DICOMStructure> structure; // non-null iff status != Failed
  std::vector<ParseDiagnostic> diagnostics;
  bool ok() const noexcept { return status != ParseStatus::Failed; }
};
```

## `DICOMStructure`

```cpp
class DICOMStructure {
 public:
  bool has_file_preamble() const noexcept;
  std::span<const std::byte, 128> file_preamble() const;   // precondition: has_file_preamble()
  const TransferSyntax& transfer_syntax() const noexcept;
  Fidelity fidelity() const noexcept;

  // 2. inspect all top-level elements
  std::size_t element_count() const noexcept;
  const std::vector<Element>& elements() const noexcept;

  // 3. retrieve a top-level tag
  const Element* find(Tag tag) const noexcept;
  bool contains(Tag tag) const noexcept;

  // 4/5. traverse nested sequences / uniquely name a nested element
  const Element* find(const ElementPath& path) const noexcept;

  // 4. recursive traversal
  using Visitor = std::function<void(const Element&, const ElementPath&)>;
  void visit(const Visitor& visitor) const;

  // 7. replace an element value (existing element, VR preserved)
  bool set_value(const ElementPath& path, Value new_value);
  // upsert a top-level scalar element (concrete non-SQ VR required for insertion)
  bool set(Tag tag, VR vr, Value value);

  // 8. remove an element (top-level or nested)
  bool erase(const ElementPath& path);
  bool erase(Tag tag);

  // 9. inspect pixel data location/encoding
  const PixelDataReference* pixel_data() const noexcept;

  // 10. serialize (see roundtrip-contract.md "Two write contracts" for exactly what's supported
  // at each Fidelity, modified or not)
  WriteResult write(std::ostream& out) const;
  WriteResult write_file(const std::filesystem::path& path) const;
};
```

Notes:

* `elements()` returns top-level elements only, in original order — "simple iteration" (op. 2).
  Nested traversal is exclusively via `visit()` or explicit `ElementPath` navigation, never
  implicit recursion inside `elements()`, so the flat/nested distinction is always visible at the
  call site.
* `find(Tag)` and `find(ElementPath)` are overloads rather than one polymorphic signature so the
  common top-level case (`find(Tag{0x0010,0x0020})`) doesn't need to construct a one-element path.
* `visit()` always recurses into every `Sequence`/`Item` it encounters; there is no
  early-termination signal in this increment (no `SkipChildren`/`Stop` return value) — flagged in
  the architecture doc as a likely near-term addition once a real caller needs it, not added
  speculatively now.
* `set(Tag, VR, Value)` preserves the existing element's VR when replacing a value. For a new
  scalar element, callers must supply a concrete non-`SQ` VR; `SQ` and `Unknown` are rejected
  because this overload cannot construct Sequence Items or infer an unknown header shape.

## `ElementPath`

```cpp
class ElementPath {
 public:
  struct Step { Tag tag; std::optional<std::size_t> item_index; };

  ElementPath() = default;                       // empty path
  explicit ElementPath(Tag top_level_tag);        // single-step convenience
  ElementPath& push(Tag tag, std::size_t item_index); // descend: tag is a Sequence, then pick item
  ElementPath& push(Tag tag);                     // final step: address the element itself

  std::span<const Step> steps() const noexcept;
  std::string to_string() const;                  // e.g. "(300A,00B0)[2]/(300A,00B6)[0]/(300A,0072)"
};
```

A path is a list of steps; every step but the last must name a `Sequence`-VR element and specify
which `Item` to descend into, and the last step names the element itself (which may itself be a
`Sequence`, if the caller wants to address the sequence as a whole rather than a leaf value).

## `Element`

```cpp
class Element {
 public:
  Tag tag() const noexcept;
  VR vr() const noexcept;
  bool has_explicit_vr_in_source() const noexcept;
  bool has_undefined_length() const noexcept;

  bool is_sequence() const noexcept;               // vr() == VR::SQ
  const Value& value() const;                      // precondition: !is_sequence()
  const Sequence& sequence() const;                 // precondition: is_sequence()

  bool is_modified() const noexcept;
};
```

## `Value`

```cpp
class Value {
 public:
  static Value from_source(const Source& source, SourceSpan span);
  static Value from_owned(std::vector<std::byte> bytes);
  static Value from_string(std::string_view text);  // convenience: UTF-8/ASCII text VRs

  bool is_source_backed() const noexcept;
  std::span<const std::byte> bytes() const;         // 6. raw value, always available
  std::size_t size() const noexcept;

  // 6. typed value (throws ValueTypeError on VR/size mismatch)
  std::string as_string() const;                    // trims trailing VR padding (space/NUL)
  std::vector<std::string> as_string_list() const;   // split on backslash, per multi-valued VRs
  std::uint16_t as_uint16() const;                   // US
  std::int16_t  as_int16() const;                    // SS
  std::uint32_t as_uint32() const;                   // UL
  std::int32_t  as_int32() const;                    // SL
  float  as_float32() const;                         // FL
  double as_float64() const;                         // FD
};
```

Typed accessors are a thin, separately-testable layer over `bytes()`; they never change what is
stored, only how it is read back. `set_value`/`set` always take a `Value`, so producing one via
`Value::from_string`/`from_owned` is how callers write.

## `Sequence` / `Item`

```cpp
class Item {
 public:
  bool has_undefined_length() const noexcept;
  const std::vector<Element>& elements() const noexcept;
  const Element* find(Tag tag) const noexcept;       // direct child only
};

class Sequence {
 public:
  bool has_undefined_length() const noexcept;
  const std::vector<Item>& items() const noexcept;
};
```

## `PixelDataReference`

```cpp
struct PixelDataFragment { SourceSpan span; };

class PixelDataReference {
 public:
  Tag tag() const noexcept;                          // always (7FE0,0010)
  VR vr() const noexcept;                             // OB or OW
  const TransferSyntax& transfer_syntax() const noexcept;
  bool is_encapsulated() const noexcept;

  SourceSpan native_span() const;                     // precondition: !is_encapsulated()
  const std::optional<SourceSpan>& basic_offset_table() const noexcept; // encapsulated only
  const std::vector<PixelDataFragment>& fragments() const noexcept;     // precondition: is_encapsulated()
};
```

## `WriteResult`

```cpp
enum class WriteStatus { Success, Unsupported, IOError, Failed };

struct WriteResult {
  WriteStatus status = WriteStatus::Failed;
  std::uint64_t bytes_written = 0;
  std::uint64_t source_backed_value_bytes = 0;  // untouched value/Pixel Data bytes, verbatim
  std::uint64_t regenerated_value_bytes = 0;    // caller-supplied replacement value bytes
  std::vector<ParseDiagnostic> diagnostics;
  bool ok() const noexcept { return status == WriteStatus::Success; }
};
```

`source_backed_value_bytes`/`regenerated_value_bytes` are the concrete numbers behind the README's
"quantify byte-level preservation of untouched content" success criterion — see
`write_result.hpp`'s field comments for exactly what is and isn't counted (value payloads and Pixel
Data only, never structural framing, which is always reconstructed regardless of modification).

`write`/`write_file` return `WriteStatus::Unsupported` (with an explanatory diagnostic) for an
unmodified structure that isn't `Fidelity::Lossless`, or a modified structure parsed at
`Fidelity::Fast` — see `docs/roundtrip-contract.md` "Two write contracts" for exactly what
"supported" means today at each `Fidelity`.

## Deliberately absent from this increment

* Nested `insert` (adding a brand-new element inside a specific `Item`) — only top-level `set`
  can create new elements; nested mutation is limited to replacing/erasing existing elements.
* `visit` early-exit signaling.
* Any dictionary-backed convenience (`vr_for_tag`, name lookup, clinical accessors).
