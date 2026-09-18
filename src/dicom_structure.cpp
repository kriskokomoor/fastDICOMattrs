#include "fastdicomattrs/dicom_structure.hpp"

#include <algorithm>
#include <fstream>
#include <optional>

#include "fastdicomattrs/sequence.hpp"
#include "fastdicomattrs/value.hpp"
#include "fastdicomattrs/value_length.hpp"
#include "internal/container_locate.hpp"
#include "writer/lossless_writer.hpp"

namespace fds {
namespace {

struct Location {
  std::vector<Element>* container;
  std::size_t index;
};

// Walks `path` starting from `top`, returning the containing vector and
// index of the final-step element, or nullopt if the path doesn't resolve.
// Shared by find_mutable/set_value/erase; find() has its own const-only
// walk to avoid const_cast in the common read-only path.
std::optional<Location> locate(std::vector<Element>& top, const ElementPath& path) {
  if (path.empty()) return std::nullopt;
  std::vector<Element>* current_list = &top;
  const auto& steps = path.steps();

  for (std::size_t i = 0; i < steps.size(); ++i) {
    const auto& step = steps[i];
    bool is_last = (i + 1 == steps.size());

    std::size_t found_index = current_list->size();
    for (std::size_t j = 0; j < current_list->size(); ++j) {
      if ((*current_list)[j].tag() == step.tag) {
        found_index = j;
        break;
      }
    }
    if (found_index == current_list->size()) return std::nullopt;

    if (step.item_index.has_value()) {
      if (is_last) return std::nullopt;  // malformed: descent step can't be final
      Element& e = (*current_list)[found_index];
      if (!e.is_sequence()) return std::nullopt;
      auto& items = e.sequence().items();
      if (*step.item_index >= items.size()) return std::nullopt;
      current_list = &items[*step.item_index].elements();
    } else {
      if (!is_last) return std::nullopt;  // malformed: only the final step may omit item_index
      return Location{current_list, found_index};
    }
  }
  return std::nullopt;
}

std::size_t erase_if_impl(std::vector<Element>& list,
                           const std::function<bool(const Element&)>& predicate) {
  std::size_t removed = 0;
  for (auto it = list.begin(); it != list.end();) {
    if (predicate(*it)) {
      it = list.erase(it);
      ++removed;
      continue;
    }
    if (it->is_sequence()) {
      for (auto& item : it->sequence().items()) {
        removed += erase_if_impl(item.elements(), predicate);
      }
    }
    ++it;
  }
  return removed;
}

// Same traversal shape as erase_if_impl (recurse into every Sequence's
// Items regardless of whether the current element matched), but replaces
// a matching non-sequence element's value in place instead of removing it
// -- nothing here is erased, so there is no iterator-invalidation concern
// erase_if_impl has to guard against.
std::size_t set_value_recursive_impl(std::vector<Element>& list, Tag tag, const Value& new_value) {
  std::size_t changed = 0;
  for (auto& e : list) {
    if (e.tag() == tag && !e.is_sequence()) {
      if (e.set_value(new_value)) ++changed;
    }
    if (e.is_sequence()) {
      for (auto& item : e.sequence().items()) {
        changed += set_value_recursive_impl(item.elements(), tag, new_value);
      }
    }
  }
  return changed;
}

// Const-only counterpart to locate() above, used only by
// resolve_private_creator(). Walks `path` from `root` using exclusively the
// public Sequence::items()/Item::elements() accessors, so it needs no
// access to DICOMStructure's private elements_ and no const_cast. On
// success, returns the vector<Element> that directly contains the final
// step's element (its "dataset scope" -- top-level, or one specific Item)
// and sets `*out_target` to that element; returns nullptr (leaving
// `*out_target` untouched) if `path` does not resolve.
const std::vector<Element>* locate_container_const(const std::vector<Element>& root,
                                                     const ElementPath& path,
                                                     const Element** out_target) {
  if (path.empty()) return nullptr;
  const std::vector<Element>* container = &root;
  const auto& steps = path.steps();

  for (std::size_t i = 0; i < steps.size(); ++i) {
    const auto& step = steps[i];
    bool is_last = (i + 1 == steps.size());

    const Element* found = nullptr;
    for (const auto& e : *container) {
      if (e.tag() == step.tag) {
        found = &e;
        break;
      }
    }
    if (found == nullptr) return nullptr;

    if (is_last) {
      if (step.item_index.has_value()) return nullptr;  // malformed: descent step can't be final
      *out_target = found;
      return container;
    }

    if (!step.item_index.has_value() || !found->is_sequence()) return nullptr;
    const auto& items = found->sequence().items();
    if (*step.item_index >= items.size()) return nullptr;
    container = &items[*step.item_index].elements();
  }
  return nullptr;
}

}  // namespace

DICOMStructure::DICOMStructure(std::shared_ptr<const Source> source, TransferSyntax transfer_syntax,
                                Fidelity fidelity, std::vector<Element> elements,
                                std::optional<std::array<std::byte, 128>> file_preamble,
                                std::optional<PixelDataReference> pixel_data,
                                std::optional<std::size_t> pixel_data_position)
    : source_(std::move(source)),
      transfer_syntax_(std::move(transfer_syntax)),
      fidelity_(fidelity),
      elements_(std::move(elements)),
      file_preamble_(file_preamble),
      pixel_data_(std::move(pixel_data)),
      pixel_data_position_(pixel_data_position) {}

const Element* DICOMStructure::find(Tag tag) const noexcept {
  for (const auto& e : elements_) {
    if (e.tag() == tag) return &e;
  }
  return nullptr;
}

const Element* DICOMStructure::find(const ElementPath& path) const noexcept {
  return const_cast<DICOMStructure*>(this)->find_mutable(path);
}

Element* DICOMStructure::find_mutable(const ElementPath& path) noexcept {
  auto loc = locate(elements_, path);
  if (!loc.has_value()) return nullptr;
  return &(*loc->container)[loc->index];
}

void DICOMStructure::visit(const Visitor& visitor) const {
  std::function<void(const std::vector<Element>&, ElementPath)> walk =
      [&](const std::vector<Element>& list, ElementPath prefix) {
        for (const auto& e : list) {
          ElementPath here = prefix;
          here.push(e.tag());
          visitor(e, here);
          if (e.is_sequence()) {
            const auto& items = e.sequence().items();
            for (std::size_t i = 0; i < items.size(); ++i) {
              ElementPath into = prefix;
              into.push(e.tag(), i);
              walk(items[i].elements(), into);
            }
          }
        }
      };
  walk(elements_, ElementPath());
}

bool DICOMStructure::set_value(const ElementPath& path, Value new_value) {
  Element* e = find_mutable(path);
  if (e == nullptr || e->is_sequence()) return false;
  if (!e->set_value(std::move(new_value))) return false;
  modified_ = true;
  return true;
}

bool DICOMStructure::set(Tag tag, VR vr, Value value) {
  for (auto& e : elements_) {
    if (e.tag() == tag) {
      if (e.is_sequence()) return false;  // replacing a sequence wholesale isn't supported yet
      if (!e.set_value(std::move(value))) return false;
      modified_ = true;
      return true;
    }
  }

  // This overload inserts scalar values only. SQ requires an Item/Sequence
  // object, and Unknown would be emitted as UN by the Explicit-VR writer but
  // does not itself define the required long-form header shape.
  if (vr == VR::SQ || vr == VR::Unknown) return false;

  // DICOM values must have even length (PS3.5 6.4); see Element::set_value
  // for why this is unconditional, not just a short-form-VR concern.
  if (value.size() % 2 != 0) return false;

  LengthForm form = is_long_form(vr) ? LengthForm::Long32 : LengthForm::Short16;
  if (form == LengthForm::Short16 && value.size() > ValueLength::kMaxShortFormLength) return false;

  // DICOM data sets require ascending tag order (PS3.5 section 7.1); a
  // brand-new top-level element must be inserted in order, not appended.
  auto insert_pos = std::lower_bound(
      elements_.begin(), elements_.end(), tag,
      [](const Element& existing, Tag t) { return existing.tag() < t; });
  elements_.emplace(insert_pos, tag, vr, VRProvenance::Explicit, form,
                     /*undefined_length=*/false, std::array<std::byte, 2>{}, std::move(value));
  modified_ = true;
  return true;
}

bool DICOMStructure::insert(const ElementPath& parent, Tag tag, VR vr, Value value) {
  auto loc = internal::locate_container(elements_, parent);
  if (loc.status != internal::ContainerLocateStatus::Success) return false;

  for (const auto& e : *loc.container) {
    if (e.tag() == tag) return false;  // already exists -- insert() never overwrites
  }

  // Same scalar-only, even-length, length-form constraints as set()'s
  // top-level insertion above -- this is its nested generalization, not a
  // new rule.
  if (vr == VR::SQ || vr == VR::Unknown) return false;
  if (value.size() % 2 != 0) return false;

  LengthForm form = is_long_form(vr) ? LengthForm::Long32 : LengthForm::Short16;
  if (form == LengthForm::Short16 && value.size() > ValueLength::kMaxShortFormLength) return false;

  auto insert_pos = std::lower_bound(
      loc.container->begin(), loc.container->end(), tag,
      [](const Element& existing, Tag t) { return existing.tag() < t; });
  loc.container->emplace(insert_pos, tag, vr, VRProvenance::Explicit, form,
                          /*undefined_length=*/false, std::array<std::byte, 2>{}, std::move(value));
  modified_ = true;
  return true;
}

bool DICOMStructure::erase(const ElementPath& path) {
  auto loc = locate(elements_, path);
  if (!loc.has_value()) return false;
  loc->container->erase(loc->container->begin() + static_cast<std::ptrdiff_t>(loc->index));
  modified_ = true;
  return true;
}

std::size_t DICOMStructure::erase_if(const std::function<bool(const Element&)>& predicate) {
  std::size_t removed = erase_if_impl(elements_, predicate);
  if (removed > 0) modified_ = true;
  return removed;
}

std::size_t DICOMStructure::set_value_recursive(Tag tag, Value new_value) {
  std::size_t changed = set_value_recursive_impl(elements_, tag, new_value);
  if (changed > 0) modified_ = true;
  return changed;
}

WriteResult DICOMStructure::write(std::ostream& out) const {
  return write_lossless(*this, out);
}

PrivateCreatorResolution resolve_private_creator(const DICOMStructure& structure,
                                                  const ElementPath& path) {
  PrivateCreatorResolution result;

  const Element* target = nullptr;
  const std::vector<Element>* container =
      locate_container_const(structure.elements(), path, &target);
  if (container == nullptr || target == nullptr) {
    // `path` does not resolve to any element -- nothing to classify.
    return result;  // kind = NotPrivate, status = NotApplicable
  }

  Tag tag = target->tag();
  if (!tag.is_private()) return result;  // NotPrivate

  if (tag.element == 0x0000) {
    result.kind = PrivateElementKind::GroupLength;
    return result;
  }

  if (tag.element >= 0x0010 && tag.element <= 0x00FF) {
    // This element IS a Private Creator declaration (PS3.5 7.8.1), not a
    // data element owned by one. "Who is this creator's creator" is not a
    // meaningful question, so `creator` is left unset deliberately -- only
    // the block number it reserves is reported, a plain structural fact.
    result.kind = PrivateElementKind::Creator;
    result.block = static_cast<std::uint8_t>(tag.element & 0xFF);
    return result;
  }

  if (tag.element < 0x1000) {
    // (gggg,0001-000F) and (gggg,0100-0FFF): outside the [0x10,0xFF] block
    // range PS3.5 7.8.1 defines for Private Creators. No creator scheme
    // applies here -- this is either an unused/reserved address or legacy
    // pre-1993 private data that predates the creator-identification
    // convention. Never guess a creator for it.
    result.kind = PrivateElementKind::Reserved;
    return result;
  }

  // (gggg,1000-FFFF): an ordinary private data element. Its block number is
  // the high byte of its element number; the creator that reserved that
  // block, if any, is declared at (gggg,00cc) in the *same* dataset scope
  // (top-level, or the same Item) as this element -- PS3.5 7.8.1.
  result.kind = PrivateElementKind::Data;
  auto block = static_cast<std::uint8_t>((tag.element >> 8) & 0xFF);
  result.block = block;

  Tag creator_tag(tag.group, block);
  const Element* creator_element = nullptr;
  for (const auto& e : *container) {
    if (e.tag() == creator_tag) {
      creator_element = &e;
      break;
    }
  }

  if (creator_element == nullptr) {
    result.status = PrivateCreatorStatus::NoCreator;
    return result;
  }

  if (creator_element->is_sequence()) {
    // A Private Creator element cannot legitimately be a Sequence -- PS3.5
    // 7.8.1 requires it to carry an identification value. Report this
    // structurally rather than crash or fabricate a creator string.
    result.status = PrivateCreatorStatus::Malformed;
    return result;
  }

  try {
    result.creator = creator_element->value().as_string();
    result.status = PrivateCreatorStatus::Resolved;
  } catch (const ValueTypeError&) {
    // Source-backed value bytes unreadable (e.g. a truncated source) --
    // never fabricate a creator string from a read that failed.
    result.status = PrivateCreatorStatus::Malformed;
  }
  return result;
}

WriteResult DICOMStructure::write_file(const std::filesystem::path& path) const {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    WriteResult result;
    result.status = WriteStatus::IOError;
    result.diagnostics.push_back(
        {DiagnosticSeverity::IOError, "failed to open output file: " + path.string(), 0, std::nullopt});
    return result;
  }
  return write(out);
}

}  // namespace fds
