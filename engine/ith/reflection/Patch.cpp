#include "ith/reflection/Patch.h"

#include "ith/foundation/Assert.h"

#include <cstddef>
#include <ranges>
#include <string>
#include <utility>

namespace ith {
namespace {

Unexpected<Error> entryError(std::size_t entry, std::string code, const std::string& message) {
    return makeError(std::move(code), "entry " + std::to_string(entry) + ": " + message);
}

Unexpected<Error> entryError(std::size_t entry, Error error) {
    return entryError(entry, std::move(error.code), error.message);
}

// Checks that `value` holds a value of `expected`.
Expected<void> checkType(std::size_t entry, const OwnedValue& value, const TypeInfo& expected,
                         const PropertyPath& path) {
    if (value.hasValue() && value.type()->id == expected.id) {
        return {};
    }
    return entryError(entry, "reflection.patch.wrong_type",
                      "path '" + path.toString() + "' holds " + std::string(expected.name) + ", the value is " +
                          (value.hasValue() ? std::string(value.type()->name) : std::string("empty")));
}

// Applies one entry to `root` and appends its inverse to `inverse`.
Expected<void> applyEntry(std::size_t entry, const PatchEntry& patchEntry, void* root,
                          std::vector<PatchEntry>& inverse) {
    const PropertyPath& path = patchEntry.path;
    if (patchEntry.op == PatchOp::Set) {
        const TypeInfo& type = path.leafType();
        if (Expected<void> checked = checkType(entry, patchEntry.value, type, path); !checked) {
            return checked;
        }
        Expected<void*> target = resolve(path, root);
        if (!target) {
            return entryError(entry, std::move(target).error());
        }
        inverse.push_back(PatchEntry{.op = PatchOp::Set, .path = path, .value = OwnedValue::copyOf(type, *target)});
        type.destroy(*target);
        type.copy(*target, patchEntry.value.get());
        return {};
    }

    if (path.empty() || path.steps().back().kind != PathStep::Kind::Index ||
        path.withoutLastStep().leafType().kind != TypeKind::DynamicArray) {
        return entryError(entry, "reflection.patch.not_resizable",
                          "path '" + path.toString() + "' is not an element of a std::vector");
    }
    const PropertyPath vectorPath = path.withoutLastStep();
    const TypeInfo& vectorType = vectorPath.leafType();
    const TypeInfo& element = vectorType.element();
    const ContainerOps& ops = *vectorType.container;
    if (patchEntry.op == PatchOp::Insert) {
        if (Expected<void> checked = checkType(entry, patchEntry.value, element, path); !checked) {
            return checked;
        }
    }
    Expected<void*> found = resolve(vectorPath, root);
    if (!found) {
        return entryError(entry, std::move(found).error());
    }
    void* vector = *found;
    const std::size_t size = ops.size(vector);
    const std::size_t index = path.steps().back().index;
    const std::size_t limit = patchEntry.op == PatchOp::Insert ? size + 1 : size;
    if (index >= limit) {
        return entryError(entry, "reflection.path.index_out_of_range",
                          "path '" + path.toString() + "': index " + std::to_string(index) + " is beyond the " +
                              std::to_string(size) + " elements of the std::vector");
    }

    if (patchEntry.op == PatchOp::Insert) {
        ops.resize(vector, size + 1);
        for (std::size_t slot = size; slot > index; --slot) {
            void* destination = ops.at(vector, slot);
            element.destroy(destination);
            element.move(destination, ops.at(vector, slot - 1));
        }
        void* inserted = ops.at(vector, index);
        element.destroy(inserted);
        element.copy(inserted, patchEntry.value.get());
        inverse.push_back(PatchEntry{.op = PatchOp::Erase, .path = path, .value = {}});
        return {};
    }

    inverse.push_back(
        PatchEntry{.op = PatchOp::Insert, .path = path, .value = OwnedValue::copyOf(element, ops.at(vector, index))});
    for (std::size_t slot = index; slot + 1 < size; ++slot) {
        void* destination = ops.at(vector, slot);
        element.destroy(destination);
        element.move(destination, ops.at(vector, slot + 1));
    }
    ops.resize(vector, size - 1);
    return {};
}

} // namespace

void Patch::set(PropertyPath path, OwnedValue value) {
    append(PatchOp::Set, std::move(path), std::move(value));
}

void Patch::insert(PropertyPath path, OwnedValue value) {
    append(PatchOp::Insert, std::move(path), std::move(value));
}

void Patch::erase(PropertyPath path) {
    append(PatchOp::Erase, std::move(path), OwnedValue());
}

void Patch::append(PatchOp op, PropertyPath path, OwnedValue value) {
    const bool sameRoot = m_root != nullptr && path.rootType().id == m_root->id;
    ITH_ASSERT_MSG(sameRoot, "Patch: the path starts from another type than the patch");
    if (sameRoot) {
        m_entries.push_back(PatchEntry{.op = op, .path = std::move(path), .value = std::move(value)});
    }
}

Expected<PreparedPatch> preparePatch(const Patch& patch, const void* root) {
    const TypeInfo* rootType = patch.rootType();
    ITH_ASSERT_MSG(rootType != nullptr, "preparePatch: the patch has no root type");
    if (rootType == nullptr) {
        return makeError("reflection.patch.wrong_type", "the patch has no root type");
    }
    OwnedValue value = OwnedValue::copyOf(*rootType, root);
    std::vector<PatchEntry> inverse;
    inverse.reserve(patch.entries().size());
    for (std::size_t entry = 0; entry < patch.entries().size(); ++entry) {
        if (Expected<void> applied = applyEntry(entry, patch.entries()[entry], value.get(), inverse); !applied) {
            return makeUnexpected(std::move(applied).error());
        }
    }
    PreparedPatch prepared{.value = std::move(value), .inverse = Patch(*rootType)};
    for (PatchEntry& entry : std::views::reverse(inverse)) {
        switch (entry.op) {
            case PatchOp::Set:
                prepared.inverse.set(std::move(entry.path), std::move(entry.value));
                break;
            case PatchOp::Insert:
                prepared.inverse.insert(std::move(entry.path), std::move(entry.value));
                break;
            case PatchOp::Erase:
                prepared.inverse.erase(std::move(entry.path));
                break;
        }
    }
    return prepared;
}

Expected<Patch> applyPatch(const Patch& patch, void* root) {
    Expected<PreparedPatch> prepared = preparePatch(patch, root);
    if (!prepared) {
        return makeUnexpected(std::move(prepared).error());
    }
    prepared->value.moveInto(root);
    return std::move(prepared->inverse);
}

Expected<OwnedValue> readValue(const PropertyPath& path, const void* root) {
    Expected<const void*> value = resolve(path, root);
    if (!value) {
        return makeUnexpected(std::move(value).error());
    }
    return OwnedValue::copyOf(path.leafType(), *value);
}

void remapEntities(Patch& patch, const EntityMap& map) {
    for (PatchEntry& entry : patch.entries()) {
        if (entry.value.hasValue()) {
            remapEntities(*entry.value.type(), entry.value.get(), map);
        }
    }
}

} // namespace ith
