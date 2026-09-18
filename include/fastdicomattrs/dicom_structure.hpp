#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fastdicomattrs/element.hpp"
#include "fastdicomattrs/element_path.hpp"
#include "fastdicomattrs/parse_options.hpp"
#include "fastdicomattrs/pixel_data_reference.hpp"
#include "fastdicomattrs/sequence.hpp"
#include "fastdicomattrs/source.hpp"
#include "fastdicomattrs/tag.hpp"
#include "fastdicomattrs/transfer_syntax.hpp"
#include "fastdicomattrs/write_result.hpp"

namespace fds {

// The canonical, order-preserving, structurally-complete in-memory
// representation of one DICOM object. See docs/architecture.md section 5.
class DICOMStructure {
 public:
  // `pixel_data_position` is the number of ordinary top-level elements
  // (i.e. an index into `elements`) that precede Pixel Data in the source
  // -- see pixel_data_position() below. Ignored when `pixel_data` is
  // nullopt. Defaults to nullopt ("position unknown"), which the writer
  // treats as "after every ordinary element" -- the conventional layout,
  // and this library's own pre-A1.3 behavior -- so callers constructing a
  // DICOMStructure programmatically (not via a parser) need not supply it.
  DICOMStructure(std::shared_ptr<const Source> source, TransferSyntax transfer_syntax,
                 Fidelity fidelity, std::vector<Element> elements,
                 std::optional<std::array<std::byte, 128>> file_preamble,
                 std::optional<PixelDataReference> pixel_data,
                 std::optional<std::size_t> pixel_data_position = std::nullopt);

  bool has_file_preamble() const noexcept { return file_preamble_.has_value(); }
  // Precondition: has_file_preamble().
  const std::array<std::byte, 128>& file_preamble() const noexcept { return *file_preamble_; }

  const TransferSyntax& transfer_syntax() const noexcept { return transfer_syntax_; }
  Fidelity fidelity() const noexcept { return fidelity_; }
  const std::shared_ptr<const Source>& source() const noexcept { return source_; }

  std::size_t element_count() const noexcept { return elements_.size(); }
  const std::vector<Element>& elements() const noexcept { return elements_; }

  // Pointer/reference invalidation (A1.7 section 16): every element,
  // top-level or nested, lives inside a std::vector<Element> at some level
  // of this tree (elements_ itself, or one Item's own elements()). Any
  // mutation that structurally changes a container -- insert() or erase()
  // at that level, including one nested arbitrarily deep -- may reallocate
  // or shift that specific std::vector<Element>, invalidating every
  // Element*/iterator/reference into it, exactly per std::vector's own
  // ordinary invalidation rules; set_value()/set_text() only guarantee the
  // mutated element's own Value stays valid, not that sibling elements'
  // addresses are stable. This library adds no additional runtime
  // protection at the C++ layer -- a stale Element* is a plain dangling
  // pointer, and dereferencing one is undefined behavior, same as any raw
  // C++ pointer after container reallocation. Callers that need to keep
  // touching elements across mutations must re-fetch via find()/
  // find_mutable() after each one rather than reusing a pointer obtained
  // beforehand -- exactly the discipline this codebase's own test suite
  // follows throughout. (The C ABI documents the identical coarse rule --
  // see docs/abi-design.md "Pointer validity" -- and the Python binding is
  // the one layer that enforces it at runtime, via a generation counter
  // raising StaleElementError instead of touching invalidated memory.)
  const Element* find(Tag tag) const noexcept;
  bool contains(Tag tag) const noexcept { return find(tag) != nullptr; }

  const Element* find(const ElementPath& path) const noexcept;
  Element* find_mutable(const ElementPath& path) noexcept;

  using Visitor = std::function<void(const Element&, const ElementPath&)>;
  void visit(const Visitor& visitor) const;

  // Replace an existing element's value (top-level or nested). VR is
  // preserved. Returns false if no element exists at `path`, if it is a
  // sequence element (use erase + rebuild for structural changes), or if
  // `new_value` cannot be encoded as a conformant DICOM value (odd length,
  // or too large for the element's length form -- see Element::set_value).
  bool set_value(const ElementPath& path, Value new_value);

  // Upsert a top-level element. Creating a new element requires an explicit
  // VR -- there is no dictionary to infer one from. See
  // docs/architecture.md section 9. Returns false without applying any
  // change if `value` cannot be encoded as a conformant DICOM value (odd
  // length, too large for the VR's length form, or SQ/Unknown used for a
  // scalar insertion).
  bool set(Tag tag, VR vr, Value value);

  bool erase(const ElementPath& path);
  bool erase(Tag tag) { return erase(ElementPath(tag)); }

  // A1.7: inserts a brand-new element, identified as `tag` inside the
  // container named by `parent` -- the structural capability A1.6
  // documented as missing. `parent` is an ElementPath used in a second,
  // narrower shape than find()/set_value()/erase() above use it: a
  // *container locator*, not an element locator. Every step of `parent`,
  // with no exception for the last, must resolve to an existing Sequence
  // Item to descend into; an empty `parent` names the root dataset itself.
  // This is deliberately a different shape from the element-locator
  // ElementPath those other methods take (where the *last* step is bare
  // and names the leaf) -- never pass a `parent` whose last step is bare,
  // and never pass an element-locator path (with a trailing bare step)
  // here expecting it to name a container one level up; there is no
  // implicit "drop the last step" behavior. See element_path.hpp and
  // docs/architecture/A1_7_MUTATION_ERGONOMICS_AND_PUBLIC_API_REPORT.md
  // section 1 for the full rationale.
  //
  // `tag` must not already exist in that container. Maintains ascending
  // tag order within that container (the same invariant set()'s existing
  // top-level insertion already maintains -- this is its nested
  // generalization, not a new ordering rule). Requires an explicit VR --
  // there is still no dictionary coupling at this structural layer (see
  // docs/architecture.md section 9 and fds::mutation::insert_inferred for
  // the dictionary-aware convenience built on top of this). Returns false,
  // applying no change, if:
  //   * any step of `parent` doesn't resolve to an existing Sequence Item
  //     (including a `parent` whose last step is malformed -- bare, or
  //     otherwise not a valid descent step),
  //   * an element with `tag` already exists in that container,
  //   * vr is SQ or Unknown (a scalar insertion needs a real scalar VR),
  //   * `value` cannot be encoded as a conformant DICOM value (odd length,
  //     or too large for vr's length form).
  //
  // There is deliberately no path-based upsert (insert-or-replace) in this
  // API: a caller-supplied VR would be silently ignored on the replace
  // branch (the existing element's VR always wins, exactly like set()'s
  // already-frozen top-level upsert does) -- an ambiguity not worth
  // enshrining in a second, nested primitive. Compose it explicitly
  // instead: `if (structure.find(path)) structure.set_value(path, value);
  // else structure.insert(parent, tag, vr, value);`.
  bool insert(const ElementPath& parent, Tag tag, VR vr, Value value);

  // Removes every Element, at any nesting depth, for which `predicate`
  // returns true, in a single traversal-safe pass -- see
  // docs/architecture.md section 9 for why this exists instead of a
  // visit()-then-erase-by-path loop (unsafe: erasure shifts indices out
  // from under a path collected before the loop started). Returns the
  // number of elements removed.
  std::size_t erase_if(const std::function<bool(const Element&)>& predicate);

  // Convenience for the bulk-removal policy this library's README calls
  // out by name: removing every private (odd group number) element, at any
  // nesting depth.
  std::size_t erase_private_elements() {
    return erase_if([](const Element& e) { return e.tag().is_private(); });
  }

  // Removes every element whose tag equals `tag`, at any nesting depth --
  // the recursive counterpart to erase(Tag), which is deliberately
  // top-level-only (a caller that needs an exact single element removed by
  // full path, top-level or nested, already has erase(const ElementPath&)
  // above; this is for "every occurrence of this tag, wherever it occurs").
  // Returns the number of elements removed.
  std::size_t erase_recursive(Tag tag) {
    return erase_if([tag](const Element& e) { return e.tag() == tag; });
  }

  // Replaces the value of every non-sequence element whose tag equals
  // `tag`, at any nesting depth, with a copy of `new_value`. set_value(const
  // ElementPath&) above only ever addresses one exact element (top-level,
  // or one specific nested occurrence named by a full path); this is the
  // "every occurrence of this tag, wherever it occurs" counterpart, the
  // same relationship erase_recursive(Tag) has to erase(const
  // ElementPath&). Same per-occurrence rules as set_value: an occurrence
  // that is itself a sequence element is left unchanged and not counted
  // (see Element::set_value). Returns the number of elements changed.
  std::size_t set_value_recursive(Tag tag, Value new_value);

  const PixelDataReference* pixel_data() const noexcept {
    return pixel_data_ ? &*pixel_data_ : nullptr;
  }

  // The number of ordinary top-level elements that preceded Pixel Data in
  // the source -- e.g. 0 means Pixel Data was first; element_count() means
  // it was last (nothing at the top level followed it). nullopt means
  // either there is no Pixel Data at all, or its position was never
  // recorded (a programmatically-constructed structure that didn't supply
  // one) -- see the constructor's doc comment for how the writer treats
  // that case. This value reflects the *source's* order only: it is never
  // consulted, and never kept in sync, once the structure is modified --
  // see docs/architecture/A1_3_PIXEL_DATA_ORDERING_REPORT.md "Mutation
  // interaction" for why that is the correct, coherent invariant rather
  // than a gap.
  std::optional<std::size_t> pixel_data_position() const noexcept { return pixel_data_position_; }

  bool is_modified() const noexcept { return modified_; }

  // Supported today only for an unmodified Fidelity::Lossless structure --
  // see docs/roundtrip-contract.md "Scope of the guarantee".
  WriteResult write(std::ostream& out) const;
  WriteResult write_file(const std::filesystem::path& path) const;

 private:
  std::shared_ptr<const Source> source_;
  TransferSyntax transfer_syntax_;
  Fidelity fidelity_;
  std::vector<Element> elements_;
  std::optional<std::array<std::byte, 128>> file_preamble_;
  std::optional<PixelDataReference> pixel_data_;
  std::optional<std::size_t> pixel_data_position_;
  bool modified_ = false;
};

// Structural classification of a private (odd-group) element, per DICOM
// PS3.5 section 7.8.1. See
// docs/architecture/A1_2_PRIVATE_CREATOR_IDENTITY_REPORT.md for the full
// rule and its provenance.
enum class PrivateElementKind {
  NotPrivate,   // even group number, or `path` did not resolve to an element
  GroupLength,  // (gggg,0000) -- group length; outside the creator scheme
  Creator,      // (gggg,0010-00FF) -- this element IS a creator declaration,
                // not a data element owned by one
  Reserved,     // (gggg,0001-000F) or (gggg,0100-0FFF) -- no valid creator
                // block number (0x10-0xFF) is derivable for this address;
                // legacy pre-1993 private data or an unused address, never
                // guessed at
  Data,         // (gggg,1000-FFFF) -- an ordinary private data element,
                // owned by whichever creator (if any) reserved its block
};

enum class PrivateCreatorStatus {
  NotApplicable,  // `kind` is not Data -- creator resolution does not apply
  Resolved,       // the block's creator element was found and read (its
                  // value may legitimately be an empty string -- PS3.5 does
                  // not forbid a zero-length LO)
  NoCreator,      // no (gggg,00cc) element exists for this block in the same
                  // dataset scope as the data element -- never guessed
  Malformed,      // a (gggg,00cc) element exists but cannot be read as a
                  // creator identifier (e.g. it is itself a Sequence, or its
                  // source-backed bytes could not be read)
};

struct PrivateCreatorResolution {
  PrivateElementKind kind = PrivateElementKind::NotPrivate;
  PrivateCreatorStatus status = PrivateCreatorStatus::NotApplicable;

  // Set whenever `kind` is Creator or Data: the block number (0x10-0xFF)
  // this element either declares (Creator) or belongs to (Data).
  std::optional<std::uint8_t> block;

  // Set only when `status == Resolved`: the creator element's value,
  // decoded with Value::as_string() (default/raw repertoire, one trailing
  // pad byte trimmed -- no Specific-Character-Set-aware decoding; see
  // docs/architecture/A1_2_PRIVATE_CREATOR_IDENTITY_REPORT.md "Charset").
  std::optional<std::string> creator;
};

// Resolves which registered Private Creator, if any, owns the private
// element named by `path`, relative to the dataset (the top-level
// structure, or the one specific sequence Item) that actually contains it.
// A creator registered at the root, in a sibling Item, or in an unrelated
// nested dataset is never visible to a private element outside that same
// scope -- PS3.5 7.8.1 scopes a Private Creator's registration to the
// dataset it appears in, and this function enforces that scope structurally
// rather than by convention.
//
// This is a pure, caller-invoked, read-only query: it performs no
// interpretation of what a resolved creator's private data means, requires
// no dictionary, and is never called from any parse or write path.
PrivateCreatorResolution resolve_private_creator(const DICOMStructure& structure,
                                                  const ElementPath& path);

}  // namespace fds
