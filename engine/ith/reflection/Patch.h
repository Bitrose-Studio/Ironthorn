#pragma once

// Patches: edits of a reflected value, validated in full before anything changes (ADR-0033).
//
//   const ith::TypeInfo& squadType = ith::typeOf<game::Squad>();
//   ith::Patch patch(squadType);
//   patch.set(*ith::PropertyPath::parse(squadType, "name"), ith::OwnedValue::of(std::string("Red")));
//   patch.insert(*ith::PropertyPath::parse(squadType, "members[0]"), ith::OwnedValue::of(game::Member{}));
//
//   ith::Expected<ith::Patch> undo = ith::applyPatch(patch, &squad); // squad is unchanged on failure
//   ...
//   static_cast<void>(ith::applyPatch(*undo, &squad));              // back to what it was
//
// A patch is an ordered list of entries on one root type. Each entry applies to the result of the
// entries before it, as in JSON Patch (RFC 6902):
// - set(path, value): replaces the value at the path; the empty path replaces the whole root;
// - insert(path, value): the path ends in "[i]" on a std::vector; the value is inserted before
//   element i, and i may be the size, which appends;
// - erase(path): the path ends in "[i]" on a std::vector; element i is removed.
// std::array and std::optional take set only; set on a whole std::vector covers resizing it.
//
// preparePatch applies the entries to a copy of the root and builds the inverse patch; nothing is
// written to the root, so a failure changes nothing. Committing moves the copy into place, which
// can't fail. applyPatch does both. Patches don't check Min, Max or ReadOnly: the editor's
// command layer does (PropertyPath::chainFlags).
//
// Entity values in a patch are handles of the world the patch was built against; remapEntities
// translates them for another world.
//
// Errors (ith::Error; the message starts with the entry's position, "entry 2: "):
// - the PropertyPath resolve errors: "reflection.path.index_out_of_range" (also an insert beyond
//   the size or an erase at or beyond it) and "reflection.path.empty_optional";
// - "reflection.patch.wrong_type": the value is empty or of another type than the path's leaf
//   (for insert, the element type);
// - "reflection.patch.not_resizable": insert or erase on a path that doesn't end in an index of a
//   std::vector.
//
// Patches are plain values and not thread-safe; distinct patches may be used from different
// threads.

#include "ith/foundation/Expected.h"
#include "ith/reflection/Entities.h"
#include "ith/reflection/OwnedValue.h"
#include "ith/reflection/PropertyPath.h"
#include "ith/reflection/TypeInfo.h"

#include <cstdint>
#include <span>
#include <vector>

namespace ith {

// What one patch entry does.
enum class PatchOp : std::uint8_t {
    Set,
    Insert,
    Erase,
};

// One entry of a patch. `value` is empty for Erase.
struct PatchEntry {
    PatchOp op = PatchOp::Set;
    PropertyPath path;
    OwnedValue value;
};

class Patch {
public:
    // A patch without a root type and without entries, for places that hold no patch.
    Patch() noexcept = default;

    // An empty patch of values of `root`.
    explicit Patch(const TypeInfo& root) noexcept : m_root(&root) {}

    // The type the patch edits; null for a default-constructed patch.
    [[nodiscard]] const TypeInfo* rootType() const noexcept { return m_root; }

    [[nodiscard]] std::span<const PatchEntry> entries() const noexcept { return m_entries; }

    [[nodiscard]] std::span<PatchEntry> entries() noexcept { return m_entries; }

    // True when the patch has no entries.
    [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }

    // Append an entry. Precondition (asserted; nothing is appended when it fails): the patch has a
    // root type, and `path` starts from a type with its id. The value's type is checked when the
    // patch is prepared.
    void set(PropertyPath path, OwnedValue value);
    void insert(PropertyPath path, OwnedValue value);
    void erase(PropertyPath path);

private:
    void append(PatchOp op, PropertyPath path, OwnedValue value);

    const TypeInfo* m_root = nullptr;
    std::vector<PatchEntry> m_entries;
};

// The result of preparing a patch: the patched copy of the root and the patch that undoes it.
struct PreparedPatch {
    OwnedValue value;
    Patch inverse;
};

// Applies `patch` to a copy of the value of patch.rootType() at `root` and builds the inverse
// patch: set becomes set with the old value, insert at i becomes erase at i, erase at i becomes
// insert at i of the old element, in reverse order. Fails with the first entry's error; `root` is
// only read. Preconditions: `root` is a complete object of the patch's root type; the patch has a
// root type (asserted; fails with "reflection.patch.wrong_type").
[[nodiscard]] Expected<PreparedPatch> preparePatch(const Patch& patch, const void* root);

// preparePatch, then moves the patched value into `root`. Returns the inverse patch. On failure
// `root` is unchanged.
[[nodiscard]] Expected<Patch> applyPatch(const Patch& patch, void* root);

// A copy of the value `path` reaches inside the value of path.rootType() at `root`. Fails with the
// resolve errors (PropertyPath.h).
[[nodiscard]] Expected<OwnedValue> readValue(const PropertyPath& path, const void* root);

// Replaces every Entity the values of the patch's entries hold with map.function(context, entity),
// as ith::remapEntities does for one value. Precondition (asserted): map.function is set.
void remapEntities(Patch& patch, const EntityMap& map);

} // namespace ith
