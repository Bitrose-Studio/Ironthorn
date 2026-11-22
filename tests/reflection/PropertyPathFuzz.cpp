// libFuzzer target for the property path parser (ADR-0033): parses arbitrary bytes as a path into
// several reflected test types and as a qualified path through a registry of every test type.
// A path that parses must print to text that parses back to the same path, and must resolve or
// fail cleanly on a filled value. Built only in Clang sanitizer builds; CTest runs a short smoke
// run seeded with tests/reflection/fuzz_seeds, and longer runs are started by hand:
//
//   ith_reflection_path_fuzz -max_total_time=600 <corpus directory> tests/reflection/fuzz_seeds

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

// A path that parsed: its text parses back to the same path, and it resolves or fails cleanly.
template <typename T>
void check(const ith::PropertyPath& path, const T& value) {
    const auto again = ith::PropertyPath::parse(path.rootType(), path.toString());
    if (!again.hasValue() || !(*again == path)) {
        std::abort();
    }
    static_cast<void>(ith::resolve(path, static_cast<const void*>(&value)));
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
