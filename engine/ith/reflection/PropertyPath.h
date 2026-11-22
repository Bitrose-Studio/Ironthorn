#pragma once

// PropertyPath: the address of a value inside a reflected value (ADR-0033).
//
//   const auto path = ith::PropertyPath::parse(ith::typeOf<game::Squad>(), "members[2].health.current");
//   if (path) {
//       const ith::TypeInfo& leaf = path->leafType();      // float
//       const ith::FieldInfo* field = path->leafField();   // "current": Min, Max, tooltip, ...
//       if (const auto value = ith::resolve(*path, &squad)) {
//           *static_cast<float*>(*value) = 10.0f;
//       }
//   }
//
//   // A component path starts with the component's fully qualified type name.
//   const auto x = ith::PropertyPath::parseQualified(registry, "ith::LocalTransform.value.translation.x");
//
// Text: a field name, then any number of ".name" and "[index]" steps; the empty text is the whole
// value. The first step may also be "[index]" when the root is an array. Names are C identifiers;
// indices are decimal, without sign or leading zeros. This is the syntax of the archives' error
// paths without their "$" (ADR-0020 rule 2).
// - std::optional is transparent: "shield.amount" steps through an optional `shield`, and "shield"
//   is the whole optional. Resolving through an empty optional fails.
// - An ith::Entity is a leaf: a path never steps through it.
// - Fields are found by their names and FormerNames; toString prints current names, so a path
//   written under an old name is rewritten when it is printed again.
//
// Parsing checks everything that doesn't depend on a value: names, kinds and fixed-array bounds.
// The leaf type, its last field and the flags along the path are known from the path alone.
// Resolving checks the rest: a std::vector index and an empty optional.
//
// A PropertyPath refers to static type records and is valid for as long as they are (until the
// module that owns them is unloaded). Paths are plain values: copying and comparing them is
// thread-safe; resolving follows the thread-safety of the value it reads.
//
// Errors (ith::Error, messages name the path and, for syntax, the byte offset):
// - "reflection.path.syntax": not the grammar above, or an index beyond std::size_t;
// - "reflection.path.unknown_type": a qualified path whose type isn't registered;
// - "reflection.path.unknown_field": a struct has no field (or FormerName) of that name;
// - "reflection.path.not_a_struct": a field step on something other than a struct;
// - "reflection.path.not_indexable": an index step on something other than an array;
// - "reflection.path.index_out_of_range": an index beyond a std::array's size (parsing) or a
//   std::vector's size (resolving);
// - "reflection.path.empty_optional": resolving steps through an empty optional.

#include "ith/foundation/Expected.h"
#include "ith/reflection/TypeInfo.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

class TypeRegistry;

// One step of a compiled path.
struct PathStep {
    enum class Kind : std::uint8_t {
        Field,  // `index` is the field's position in TypeInfo::fields
        Index,  // `index` is the element index of a std::array or std::vector
        Unwrap, // the value of a std::optional; not printed
    };

    Kind kind = Kind::Field;
    std::size_t index = 0;

    friend bool operator==(const PathStep&, const PathStep&) = default;
};

class PropertyPath {
public:
    // The empty path of `root`: the whole value.
    explicit PropertyPath(const TypeInfo& root) noexcept;

    // Parses `text` as a path into a value of `root`. Fails with the errors listed above.
    [[nodiscard]] static Expected<PropertyPath> parse(const TypeInfo& root, std::string_view text);

    // Parses a qualified path: a fully qualified type name, found in `registry` (FormerNames
    // included), optionally followed by "." or "[" and a path into that type, e.g.
    // "game::Light.intensity" or "game::Light". The name ends at the first "." or "[".
    [[nodiscard]] static Expected<PropertyPath> parseQualified(const TypeRegistry& registry, std::string_view text);

    // The type the path starts from.
    [[nodiscard]] const TypeInfo& rootType() const noexcept { return *m_root; }

    // The type of the value the path reaches.
    [[nodiscard]] const TypeInfo& leafType() const noexcept { return *m_leaf; }

    // The last field the path steps into (its attributes: Min, Max, Unit, tooltip, ...); null for
    // the empty path and for a path that steps only through elements of the root.
    [[nodiscard]] const FieldInfo* leafField() const noexcept { return m_leafField; }

    // The flags of every field along the path, ORed (ReadOnly, Transient, EditorOnly, ...).
    [[nodiscard]] ReflectionFlags chainFlags() const noexcept { return m_chainFlags; }

    [[nodiscard]] std::span<const PathStep> steps() const noexcept { return m_steps; }

    // True for the path of the whole value.
    [[nodiscard]] bool empty() const noexcept { return m_steps.empty(); }

    // The path without its last step: for "items[3]" the path "items", for "a.b" the path "a".
    // Precondition (asserted): the path isn't empty; otherwise the result is the empty path.
    [[nodiscard]] PropertyPath withoutLastStep() const;

    // The path as text, with current field names, e.g. "members[2].health.current"; empty for the
    // whole value.
    [[nodiscard]] std::string toString() const;

    // The qualified text: the root type's name, then the path, e.g. "game::Light.intensity".
    [[nodiscard]] std::string toQualifiedString() const;

    // True when `other` starts with this path: the same root type and every step of this path.
    // A path is a prefix of itself; the empty path is a prefix of every path of its root type.
    [[nodiscard]] bool isPrefixOf(const PropertyPath& other) const noexcept;

    // The same root type (by id) and the same steps.
    friend bool operator==(const PropertyPath& a, const PropertyPath& b) noexcept;

private:
    // Parses the steps of `text` from `position` and appends them; errors name the whole text.
    Expected<void> parseSteps(std::string_view text, std::size_t position);
    // Appends an Unwrap step for each optional the leaf is.
    void unwrapOptionals();
    // Appends a step that is valid for the current leaf and moves the leaf on.
    void pushStep(const PathStep& step);
    Expected<void> appendField(std::string_view text, std::string_view name);
    Expected<void> appendIndex(std::string_view text, std::size_t index);

    const TypeInfo* m_root;
    const TypeInfo* m_leaf;
    const FieldInfo* m_leafField = nullptr;
    ReflectionFlags m_chainFlags = ReflectionFlags::None;
    std::vector<PathStep> m_steps;
};

// The value `path` reaches inside the value of path.rootType() at `root`, which is a complete
// object of that type. Fails with "reflection.path.index_out_of_range" or
// "reflection.path.empty_optional". The pointer is valid until the value at `root` changes shape.
[[nodiscard]] Expected<void*> resolve(const PropertyPath& path, void* root);
[[nodiscard]] Expected<const void*> resolve(const PropertyPath& path, const void* root);

} // namespace ith
