// libFuzzer target for the property path parser (ADR-0033): parses arbitrary bytes as a path into
// several reflected test types and as a qualified path through a registry of every test type.
// A path that parses must print to text that parses back to the same path, and must resolve or
// fail cleanly on a filled value, and set, insert and erase patches built from it must apply and
// undo exactly. Built only in Clang sanitizer builds; CTest runs a short smoke
// run seeded with tests/reflection/fuzz_seeds, and longer runs are started by hand:
//
//   ith_reflection_path_fuzz -max_total_time=600 <corpus directory> tests/reflection/fuzz_seeds

#include "ith/reflection/OwnedValue.h"
#include "ith/reflection/Patch.h"
#include "ith/reflection/PropertyPath.h"
#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"
#include "ith/reflection/TypeRegistry.h"

#include "ArchiveTypes.h"
#include "Loadout.h"
#include "PathTestSupport.h"
#include "ReflectedTypes.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace {

// Applies `patch` to a copy of `value`; when it applies, its inverse must apply and give `value`
// back, compared through the type's record.
template <typename T>
void applyAndUndo(const ith::Patch& patch, const T& value) {
    T copy = value;
    const auto inverse = ith::applyPatch(patch, &copy);
    if (!inverse.hasValue()) {
        if (!ith::typeOf<T>().equals(&copy, &value)) {
            std::abort(); // a failed patch changed its target
        }
        return;
    }
    if (!ith::applyPatch(*inverse, &copy).hasValue() || !ith::typeOf<T>().equals(&copy, &value)) {
        std::abort();
    }
}

// A path that parsed: its text parses back to the same path, and it resolves or fails cleanly.
// Then patches built from it, with a default leaf or element value, apply and undo on a filled
// value: a set, and for an index into a std::vector an insert and an erase.
template <typename T>
void check(const ith::PropertyPath& path, const T& value) {
    const auto again = ith::PropertyPath::parse(path.rootType(), path.toString());
    if (!again.hasValue() || !(*again == path)) {
        std::abort();
    }
    static_cast<void>(ith::resolve(path, static_cast<const void*>(&value)));

    ith::Patch set(path.rootType());
    set.set(path, ith::OwnedValue(path.leafType()));
    applyAndUndo(set, value);

    const bool vectorIndex = !path.empty() && path.steps().back().kind == ith::PathStep::Kind::Index &&
                             path.withoutLastStep().leafType().kind == ith::TypeKind::DynamicArray;
    if (vectorIndex) {
        ith::Patch insert(path.rootType());
        insert.insert(path, ith::OwnedValue(path.leafType()));
        applyAndUndo(insert, value);
        ith::Patch erase(path.rootType());
        erase.erase(path);
        applyAndUndo(erase, value);
    }
}

template <typename T>
void parseAs(std::string_view text, const T& value) {
    const auto path = ith::PropertyPath::parse(ith::typeOf<T>(), text);
    if (path.hasValue()) {
        check(*path, value);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const ith::TypeRegistry registry = ith::test::makeTestRegistry();
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    ith::test::Containers containers;
    containers.points.resize(2);
    containers.nested = {{1, 2}, {}};
    containers.maybePoint = ith::test::Vector3{};
    parseAs(text, containers);
    ith::test::gear::Loadout loadout;
    loadout.history.resize(3);
    parseAs(text, loadout);
    parseAs(text, ith::test::archive::Squad{});
    parseAs(text, std::vector<ith::test::Vector3>(2));

    const auto qualified = ith::PropertyPath::parseQualified(registry, text);
    if (qualified.hasValue()) {
        const auto again = ith::PropertyPath::parseQualified(registry, qualified->toQualifiedString());
        if (!again.hasValue() || !(*again == *qualified)) {
            std::abort();
        }
    }
    return 0;
}
