#pragma once

// A1.5/A1.6 -- Specific Character Set (0008,0005) context resolution,
// standards-aware text decoding, and (A1.6) the inverse: charset-aware
// text encoding/mutation. See docs/architecture/
// A1_5_CHARACTER_SET_CONTEXT_AND_DECODING_REPORT.md and
// A1_6_CHARACTER_SET_REENCODING_REPORT.md for the full design and
// provenance.
//
// This is a dedicated, additive codec subsystem, architecturally separate
// from Value and Element: neither's meaning changes. Value::bytes() and
// Value::as_string() remain exactly what they were before A1.5 -- the
// raw/default-repertoire view. decode_text()/encode_text() below are an
// additional semantic read/write pair built on top of that raw data, never
// a replacement for it: raw-byte mutation (DICOMStructure::set_value with
// caller-supplied bytes) still means exactly what it always meant --
// "caller supplied raw encoded DICOM bytes" -- and is completely
// unaffected by this file. set_text() (A1.6) is the new, explicit,
// Unicode-aware entry point; it is never reachable by accident through the
// raw mutation API.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fastdicomattrs/dicom_structure.hpp"
#include "fastdicomattrs/element.hpp"
#include "fastdicomattrs/element_path.hpp"
#include "fastdicomattrs/tag.hpp"
#include "fastdicomattrs/vr.hpp"

namespace fds::charset {

inline constexpr Tag kSpecificCharacterSetTag{0x0008, 0x0005};

// How one raw term from a (0008,0005) declaration classifies against the
// V1 charset envelope (see the report's supported/deferred tables).
enum class RepertoireSupport : std::uint8_t {
  DefaultRepertoire,    // blank term (or the attribute absent) -- ISO 2022 IR 6 / ASCII
  SingleByteSupported,  // one of PS3.5's ten single-byte repertoires (V1 scope)
  Utf8Supported,        // ISO_IR 192
  Deferred,             // a real DICOM defined term, but multi-byte / out of V1 scope
  Unrecognized,         // not a DICOM-defined term this library knows at all
};

struct RepertoireTerm {
  std::string defined_term;  // exact raw term text as declared (may be empty)
  RepertoireSupport support = RepertoireSupport::DefaultRepertoire;
};

enum class CharacterSetMode : std::uint8_t {
  Default,       // no declaration in scope (or an explicit but blank one) -- default repertoire
  SingleValue,   // exactly one supported term (a single-byte repertoire, or UTF-8)
  MultiValue,    // 2+ terms, every one recognized and supported (ISO 2022 single-byte switching)
  Unsupported,   // a real declaration, but containing a Deferred or Unrecognized term
  Malformed,     // a structurally illegal combination (e.g. a stand-alone term combined with others)
};

struct CharacterSetContext {
  CharacterSetMode mode = CharacterSetMode::Default;
  // The resolved (0008,0005) terms in effect, in declared order; empty
  // when mode == Default.
  std::vector<RepertoireTerm> terms;
  // True if this context came from a (0008,0005) found at some scope
  // closer than the top-level dataset (i.e. a nested Item's own
  // declaration is in effect for the target) -- informational only.
  bool declared_locally = false;
};

// Resolves the effective Specific Character Set context for the element
// named by `path`, relative to the dataset (top-level, or a specific
// nested Item) that actually contains it. Per PS3.5's own charset
// inheritance rule (distinct from A1.2's private-creator scoping, which
// never cascades): a (0008,0005) declaration governs its own scope *and*
// every nested scope inside it that does not declare its own override --
// the nearest enclosing declaration wins, found by walking `path` from the
// root and checking each scope crossed along the way, using only the
// public DICOMStructure/Sequence/Item accessors (no privileged access, no
// parent back-pointer -- the same architectural pattern A1.2/A1.4 already
// established).
CharacterSetContext resolve_character_set_context(const DICOMStructure& structure,
                                                    const ElementPath& path);

enum class DecodeStatus : std::uint8_t {
  Success,
  NotATextVR,                   // this element's VR is not one Specific Character Set governs
  UnsupportedCharset,            // context.mode == Unsupported
  MalformedCharsetDeclaration,   // context.mode == Malformed
  InvalidEncodedBytes,           // bytes could not be decoded under the resolved repertoire(s)
};

struct DecodedText {
  DecodeStatus status = DecodeStatus::NotATextVR;
  // One UTF-8 entry per VM component (backslash-separated); empty unless
  // status == Success. For VR::PN, the '^' component and '=' component-
  // group delimiters are preserved literally within each entry's text.
  std::vector<std::string> values;
};

// Decodes `element`'s raw bytes into Unicode text under `context` --
// obtained once via resolve_character_set_context() and reusable across
// every element in the same structural region, so a caller decoding many
// values does not repeat the path walk per element. Never modifies
// `element`, never touches Value::bytes()/as_string() -- this is a
// read-only, additional semantic view.
DecodedText decode_text(const Element& element, const CharacterSetContext& context);

// --- A1.6: the inverse direction -------------------------------------------

enum class EncodeStatus : std::uint8_t {
  Success,
  NotATextVR,                   // `vr` is not one Specific Character Set governs
  UnsupportedCharset,            // context.mode == Unsupported
  MalformedCharsetDeclaration,   // context.mode == Malformed
  InvalidUnicodeInput,           // an input string is not well-formed UTF-8
  UnrepresentableCharacter,      // a code point has no byte in any declared/default repertoire
  ValueTooLong,                  // encoded (padded) length exceeds the VR's wire length-form ceiling
};

struct EncodedText {
  EncodeStatus status = EncodeStatus::NotATextVR;
  // Present iff status == Success: fully encoded, padded-to-even-length
  // DICOM wire bytes, ready to write verbatim (e.g. via
  // DICOMStructure::set_value/set). VM components are joined with a
  // literal '\'; for VR::PN, '^' and '=' in the input are treated as the
  // structural delimiters decode_text() itself produces (see below), not
  // literal data.
  std::optional<std::vector<std::byte>> bytes;
};

// Encodes `values` (UTF-8; one entry per VM component -- matching
// decode_text()'s own output convention exactly, so `decode_text(...).
// values` round-trips through this function) into conformant DICOM wire
// bytes for `vr`, under `context`. A pure function: never touches a
// DICOMStructure, never allocates outside its own return value, and always
// either fully succeeds or leaves nothing behind to clean up.
//
// Deterministic repertoire selection for a multi-valued (ISO 2022)
// context: for each Unicode code point, the currently-active repertoire is
// preferred if it can represent the code point (avoiding a gratuitous
// escape sequence); otherwise the earliest repertoire in `context.terms`'
// declared order that can represent it is chosen; if none can, encoding
// fails with UnrepresentableCharacter rather than guessing or dropping the
// character. State resets to the default (the same one decode_text()
// resets to) at every point PS3.5 requires: between VM components, at
// CR/LF/TAB/FF within ordinary text, at '^' within one PN component group,
// and independently at '=' between PN component groups -- the exact
// mirror of decode_text()'s own reset points, so a standards-aware
// independent decoder (this library's own decode_text(), pydicom, DCMTK)
// recovers the original text exactly.
//
// Never broadens the V1 charset envelope: a `context` whose mode is
// Unsupported or Malformed is rejected outright, exactly as decode_text()
// rejects it for reading.
EncodedText encode_text(const std::vector<std::string>& values, VR vr,
                         const CharacterSetContext& context);

enum class SetTextStatus : std::uint8_t {
  Success,
  NotATextVR,
  UnsupportedCharset,
  MalformedCharsetDeclaration,
  InvalidUnicodeInput,
  UnrepresentableCharacter,
  ValueTooLong,
  PathNotFound,       // `path` does not resolve to an existing element (set_text only)
  IsSequenceElement,  // `path` resolves to a Sequence, which has no text to set (set_text only)

  // A1.7: additive values used only by insert_text()/insert_text_inferred()
  // below -- set_text()'s own contract and every existing enumerator's
  // meaning for it are unchanged. See
  // docs/architecture/A1_7_MUTATION_ERGONOMICS_AND_PUBLIC_API_REPORT.md
  // section 7 ("charset-aware nested insertion") and section 4 ("centralize
  // container-location semantics") -- these mirror fds::mutation::
  // InsertStatus's own container-location/duplicate/VR outcomes exactly,
  // reusing the same names on purpose (one vocabulary for "why an insertion
  // failed," reused across the raw and text-aware layers).
  ContainerNotFound,    // some step of `parent` doesn't resolve to an existing Sequence Item
  NotASequence,         // some step of `parent` names a tag that exists but isn't a Sequence
  ItemIndexOutOfRange,  // some step of `parent` names an Item index with no corresponding Item
  AlreadyExists,        // `tag` already exists in the target container (insert_text only)
  VRRequired,           // insert_text_inferred only: `tag`'s VR cannot be safely inferred
};

// Convenience: resolves `path`'s existing element and its already-effective
// Specific Character Set context (the same one decode_text() would use),
// encodes `values` under that context and the element's existing VR, and
// applies the result via DICOMStructure::set_value() -- atomically: every
// validation step (path resolution, VR, context, Unicode well-formedness,
// representability, resulting byte length) happens before `structure` is
// touched at all, so any failure leaves it completely unchanged (raw
// bytes, modified flag, everything).
//
// Requires an *existing*, non-sequence, text-VR element at `path`.
// set_text() never inserts -- see insert_text()/insert_text_inferred()
// below for that (A1.7; this increment's structural insert() finally gives
// a nested container enough addressable context to support it, which is
// why A1.6 did not attempt it).
SetTextStatus set_text(DICOMStructure& structure, const ElementPath& path,
                        const std::vector<std::string>& values);

// A1.7: inserts a brand-new text element (root or nested -- `parent` is a
// container-locator ElementPath, exactly DICOMStructure::insert()'s own
// `parent` contract: every step a descent step, empty means root),
// requiring an explicit `vr`. Composition (see the freeze report section
// 7): locate the target container -> confirm `tag` absent -> resolve the
// effective Specific Character Set context AT THAT CONTAINER (the nearest
// enclosing (0008,0005) declaration crossed while walking `parent`,
// cascading exactly as resolve_character_set_context() already defines --
// reusing that same inheritance logic, not a second implementation of it)
// -> encode_text() -> only on total success, DICOMStructure::insert().
// `vr` must be one of the seven Specific-Character-Set-governed VRs (LO LT
// PN SH ST UC UT); anything else returns NotATextVR without touching
// `structure`. Atomic, exactly like set_text().
SetTextStatus insert_text(DICOMStructure& structure, const ElementPath& parent, Tag tag, VR vr,
                           const std::vector<std::string>& values);

// A1.7: insert_text(), but with `tag`'s VR inferred via the same policy
// fds::mutation::insert_inferred() uses (fds::internal::infer_vr -- one
// shared inference rule, not two) instead of caller-supplied. Returns
// VRRequired, touching nothing, when inference is ambiguous, unknown, or
// (for private data) never attempted. If inference succeeds but yields a
// non-text VR (a legitimate outcome -- most standard tags aren't
// Specific-Character-Set-governed), returns NotATextVR: this function
// never silently falls back to raw-byte insertion, since its entire
// purpose is Unicode-aware insertion.
SetTextStatus insert_text_inferred(DICOMStructure& structure, const ElementPath& parent, Tag tag,
                                    const std::vector<std::string>& values);

}  // namespace fds::charset
