// Patches (ADR-0033): set, insert and erase, inverses, all-or-nothing failure, entity remapping,
// and a patch with its inverse on every reflected test type. Also OwnedValue.

#include "ith/reflection/Patch.h"

#include "ith/foundation/Entity.h"
#include "ith/reflection/OwnedValue.h"
#include "ith/reflection/PropertyPath.h"
#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"
#include "ith/reflection/TypeRegistry.h"

#include "ArchiveTypes.h"
#include "AssertCapture.h"
#include "PathTestSupport.h"
#include "ReflectedTypes.h"
#include "TestValues.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <ostream> // doctest prints std::string_view operands through an ostream
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// How many times one failed precondition asserts: none when asserts are compiled out.
#ifdef ITH_ENABLE_ASSERTS
constexpr int kAsserts = 1;
#else
constexpr int kAsserts = 0;
#endif

// The message of a failed Expected, for test output.
template <typename Result>
std::string messageOf(const Result& result) {
    return result.hasValue() ? std::string() : result.error().message;
}

using ith::OwnedValue;
using ith::Patch;
using ith::PropertyPath;
using ith::test::Containers;
using ith::test::Hit;
using ith::test::Vector3;

template <typename Root>
PropertyPath path(std::string_view text) {
    ith::Expected<PropertyPath> parsed = PropertyPath::parse(ith::typeOf<Root>(), text);
    REQUIRE_MESSAGE(parsed.hasValue(), messageOf(parsed));
    return *parsed;
}

bool same(const Containers& a, const Containers& b) {
    return ith::typeOf<Containers>().equals(&a, &b);
}

Containers makeContainers() {
    Containers value;
    value.fixed = {1, 2, 3};
    value.names = {"a", "b"};
    value.points = {Vector3{.x = 1.0f}, Vector3{.x = 2.0f}, Vector3{.x = 3.0f}};
    value.nested = {{1, 2}, {3}};
    return value;
}

std::vector<float> xs(const Containers& value) {
    std::vector<float> result;
    for (const Vector3& point : value.points) {
        result.push_back(point.x);
    }
    return result;
}

// Applies `patch` and checks that its inverse restores the value.
void checkUndo(Containers& value, const Patch& patch) {
    const Containers before = value;
    ith::Expected<Patch> inverse = ith::applyPatch(patch, &value);
    REQUIRE_MESSAGE(inverse.hasValue(), messageOf(inverse));
    Containers after = value;
    REQUIRE(ith::applyPatch(*inverse, &after).hasValue());
    CHECK(same(after, before));
}

void checkFails(const Patch& patch, std::string_view code, std::string_view part) {
    Containers value = makeContainers();
    const Containers before = value;
    const ith::Expected<Patch> result = ith::applyPatch(patch, &value);
    REQUIRE_FALSE(result.hasValue());
    CHECK(result.error().code == code);
    CAPTURE(result.error().message);
    CHECK(result.error().message.find(part) != std::string::npos);
    CHECK(same(value, before));
}

Vector3 point(float x) {
    return Vector3{.x = x};
}

} // namespace

TEST_SUITE("reflection.owned_value") {
    TEST_CASE("holds, copies and moves a value of any reflected type") {
        const OwnedValue empty;
        CHECK_FALSE(empty.hasValue());
        CHECK(empty.type() == nullptr);
        CHECK(empty.get() == nullptr);
        CHECK(empty.as<float>() == nullptr);

        const OwnedValue defaults(ith::typeOf<Vector3>());
        REQUIRE(defaults.as<Vector3>() != nullptr);
        CHECK(defaults.as<Vector3>()->x == 0.0f);

        OwnedValue names = OwnedValue::of(std::vector<std::string>{"a", "b"});
        CHECK(names.type()->id == ith::typeOf<std::vector<std::string>>().id);
        CHECK(names.as<std::vector<float>>() == nullptr);
        REQUIRE(names.as<const std::vector<std::string>>() != nullptr);

        const OwnedValue copy = names;
        names.as<std::vector<std::string>>()->push_back("c");
        CHECK(copy.as<std::vector<std::string>>()->size() == 2);

        OwnedValue moved = std::move(names);
        CHECK(moved.as<std::vector<std::string>>()->size() == 3);
        CHECK_FALSE(names.hasValue());  // NOLINT(bugprone-use-after-move): moved-from is empty
        CHECK(names.type() == nullptr); // NOLINT(bugprone-use-after-move)

        OwnedValue assigned = OwnedValue::of(1.0f);
        assigned = copy;
        CHECK(assigned.as<std::vector<std::string>>()->size() == 2);
        assigned = empty;
        CHECK_FALSE(assigned.hasValue());
        const OwnedValue copiedEmpty = empty;
        CHECK_FALSE(copiedEmpty.hasValue());

        const std::string text = "copied";
        const OwnedValue copied = OwnedValue::copyOf(ith::typeOf<std::string>(), &text);
        CHECK(*copied.as<std::string>() == "copied");
    }

    TEST_CASE("moveInto replaces the target and asserts without a value") {
        OwnedValue value = OwnedValue::of(std::string("new"));
        std::string target = "old";
        value.moveInto(&target);
        CHECK(target == "new");
        CHECK(value.hasValue());

        ith::test::AssertCapture capture;
        OwnedValue empty;
        empty.moveInto(&target);
        CHECK(capture.count() == kAsserts);
        CHECK(target == "new");
    }
}

TEST_SUITE("reflection.patch") {
    TEST_CASE("set replaces a value and its inverse restores the old one") {
        Containers value = makeContainers();
        Patch patch(ith::typeOf<Containers>());
        patch.set(path<Containers>("points[1].x"), OwnedValue::of(9.0f));
        patch.set(path<Containers>("names"), OwnedValue::of(std::vector<std::string>{"z"}));
        patch.set(path<Containers>("maybePoint"), OwnedValue::of(std::optional<Vector3>(point(4.0f))));
        const Containers before = value;
        ith::Expected<Patch> inverse = ith::applyPatch(patch, &value);
        REQUIRE(inverse.hasValue());
        CHECK(value.points[1].x == 9.0f);
        CHECK(value.names == std::vector<std::string>{"z"});
        REQUIRE(value.maybePoint.has_value());
        CHECK(value.maybePoint->x == 4.0f);

        REQUIRE(inverse->entries().size() == 3);
        CHECK(inverse->entries()[0].path == path<Containers>("maybePoint"));
        CHECK(inverse->entries()[2].path == path<Containers>("points[1].x"));
        CHECK(*inverse->entries()[2].value.as<float>() == 2.0f);
        REQUIRE(ith::applyPatch(*inverse, &value).hasValue());
        CHECK(same(value, before));
    }

    TEST_CASE("set on the empty path replaces the whole value") {
        Containers value = makeContainers();
        Containers replacement;
        replacement.names = {"only"};
        Patch patch(ith::typeOf<Containers>());
        patch.set(PropertyPath(ith::typeOf<Containers>()), OwnedValue::of(replacement));
        checkUndo(value, patch);
        CHECK(same(value, replacement));
    }

    TEST_CASE("set through an engaged optional, and a fixed-array element") {
        Containers value = makeContainers();
        value.maybePoint = point(1.0f);
        Patch patch(ith::typeOf<Containers>());
        patch.set(path<Containers>("maybePoint.y"), OwnedValue::of(5.0f));
        patch.set(path<Containers>("fixed[0]"), OwnedValue::of(std::int32_t{7}));
        checkUndo(value, patch);
        CHECK(value.maybePoint->y == 5.0f);
        CHECK(value.fixed[0] == 7);
    }

    TEST_CASE("insert before an element or at the end, erase an element") {
        for (std::size_t index = 0; index <= 3; ++index) {
            CAPTURE(index);
            Containers value = makeContainers();
            Patch patch(ith::typeOf<Containers>());
            patch.insert(path<Containers>("points[" + std::to_string(index) + "]"), OwnedValue::of(point(8.0f)));
            checkUndo(value, patch);
            std::vector<float> expected = {1.0f, 2.0f, 3.0f};
            expected.insert(expected.begin() + static_cast<std::ptrdiff_t>(index), 8.0f);
            CHECK(xs(value) == expected);
        }
        for (std::size_t index = 0; index < 3; ++index) {
            CAPTURE(index);
            Containers value = makeContainers();
            Patch patch(ith::typeOf<Containers>());
            patch.erase(path<Containers>("points[" + std::to_string(index) + "]"));
            checkUndo(value, patch);
            std::vector<float> expected = {1.0f, 2.0f, 3.0f};
            expected.erase(expected.begin() + static_cast<std::ptrdiff_t>(index));
            CHECK(xs(value) == expected);
        }
    }

    TEST_CASE("each entry sees the result of the entries before it") {
        Containers value = makeContainers();
        Patch patch(ith::typeOf<Containers>());
        patch.insert(path<Containers>("points[0]"), OwnedValue::of(point(0.0f)));
        patch.set(path<Containers>("points[0].y"), OwnedValue::of(6.0f)); // the inserted element
        patch.erase(path<Containers>("points[3]"));                       // the old last element
        patch.insert(path<Containers>("points[3]"), OwnedValue::of(point(4.0f)));
        patch.insert(path<Containers>("nested[1][0]"), OwnedValue::of(std::int32_t{5}));
        patch.erase(path<Containers>("names[0]"));
        checkUndo(value, patch);
        CHECK(xs(value) == std::vector<float>{0.0f, 1.0f, 2.0f, 4.0f});
        CHECK(value.points[0].y == 6.0f);
        CHECK(value.nested[1] == std::vector<std::int32_t>{5, 3});
        CHECK(value.names == std::vector<std::string>{"b"});
    }

    TEST_CASE("insert and erase only on std::vector elements, within the size") {
        Patch beyond(ith::typeOf<Containers>());
        beyond.insert(path<Containers>("points[4]"), OwnedValue::of(point(0.0f)));
        checkFails(beyond, "reflection.path.index_out_of_range", "entry 0: path 'points[4]': index 4");

        Patch eraseEnd(ith::typeOf<Containers>());
        eraseEnd.erase(path<Containers>("points[3]"));
        checkFails(eraseEnd, "reflection.path.index_out_of_range", "3 elements");

        for (const std::string_view text : {"fixed[0]", "maybePoint", "points", "points[0].x", ""}) {
            CAPTURE(text);
            Patch insert(ith::typeOf<Containers>());
            insert.insert(path<Containers>(text), OwnedValue::of(std::int32_t{0}));
            checkFails(insert, "reflection.patch.not_resizable", "is not an element of a std::vector");
            Patch erase(ith::typeOf<Containers>());
            erase.erase(path<Containers>(text));
            checkFails(erase, "reflection.patch.not_resizable", "is not an element of a std::vector");
        }

        Patch inner(ith::typeOf<Containers>());
        inner.insert(path<Containers>("nested[5][0]"), OwnedValue::of(std::int32_t{0}));
        checkFails(inner, "reflection.path.index_out_of_range", "nested[5]");
    }

    TEST_CASE("values of another type are refused") {
        Patch wrong(ith::typeOf<Containers>());
        wrong.set(path<Containers>("points[0].x"), OwnedValue::of(1.0));
        checkFails(wrong, "reflection.patch.wrong_type", "holds float, the value is double");

        Patch empty(ith::typeOf<Containers>());
        empty.set(path<Containers>("points[0].x"), OwnedValue());
        checkFails(empty, "reflection.patch.wrong_type", "the value is empty");

        Patch element(ith::typeOf<Containers>());
        element.insert(path<Containers>("points[0]"), OwnedValue::of(1.0f));
        checkFails(element, "reflection.patch.wrong_type", "holds ith::test::Vector3");

        Patch whole(ith::typeOf<Containers>());
        whole.set(path<Containers>("maybePoint"), OwnedValue::of(point(1.0f)));
        checkFails(whole, "reflection.patch.wrong_type", "std::optional");
    }

    TEST_CASE("a failing entry changes nothing, also after entries that succeeded") {
        Patch patch(ith::typeOf<Containers>());
        patch.set(path<Containers>("names[0]"), OwnedValue::of(std::string("changed")));
        patch.erase(path<Containers>("points[0]"));
        patch.set(path<Containers>("maybePoint.x"), OwnedValue::of(1.0f)); // the optional is empty
        checkFails(patch, "reflection.path.empty_optional", "entry 2: ");

        Containers value = makeContainers();
        const ith::Expected<ith::PreparedPatch> prepared = ith::preparePatch(patch, &value);
        CHECK_FALSE(prepared.hasValue());
    }

    TEST_CASE("preparePatch leaves the root alone; committing the value applies it") {
        Containers value = makeContainers();
        const Containers before = value;
        Patch patch(ith::typeOf<Containers>());
        patch.set(path<Containers>("fixed[1]"), OwnedValue::of(std::int32_t{-4}));
        ith::Expected<ith::PreparedPatch> prepared = ith::preparePatch(patch, &value);
        REQUIRE(prepared.hasValue());
        CHECK(same(value, before));
        CHECK(prepared->value.as<Containers>()->fixed[1] == -4);
        CHECK(prepared->inverse.rootType()->id == ith::typeOf<Containers>().id);
        prepared->value.moveInto(&value);
        CHECK(value.fixed[1] == -4);
    }

    TEST_CASE("an empty patch changes nothing and has an empty inverse") {
        Containers value = makeContainers();
        const Containers before = value;
        const Patch patch(ith::typeOf<Containers>());
        CHECK(patch.empty());
        const ith::Expected<Patch> inverse = ith::applyPatch(patch, &value);
        REQUIRE(inverse.hasValue());
        CHECK(inverse->empty());
        CHECK(same(value, before));
    }

    TEST_CASE("programmer errors assert") {
        ith::test::AssertCapture capture;
        Patch patch(ith::typeOf<Containers>());
        patch.set(path<Hit>("amount"), OwnedValue::of(1.0));
        CHECK(capture.count() == kAsserts);
        CHECK(patch.empty());

        Containers value;
        const Patch none;
        CHECK(none.rootType() == nullptr);
        const ith::Expected<Patch> result = ith::applyPatch(none, &value);
        CHECK(capture.count() == 2 * kAsserts);
        REQUIRE_FALSE(result.hasValue());
        CHECK(result.error().code == "reflection.patch.wrong_type");
    }

    TEST_CASE("readValue copies the value at a path") {
        const Containers value = makeContainers();
        const auto read = ith::readValue(path<Containers>("points[2]"), &value);
        REQUIRE(read.hasValue());
        CHECK(read->as<Vector3>()->x == 3.0f);
        const auto missing = ith::readValue(path<Containers>("points[3]"), &value);
        REQUIRE_FALSE(missing.hasValue());
        CHECK(missing.error().code == "reflection.path.index_out_of_range");
    }

    TEST_CASE("remapEntities rewrites the entities in the patch's values") {
        using ith::Entity;
        using ith::test::archive::Link;
        using ith::test::archive::Squad;
        Patch patch(ith::typeOf<Squad>());
        patch.set(path<Squad>("leader"), OwnedValue::of(Entity::fromParts(3, 1, 1)));
        patch.insert(path<Squad>("links[0]"),
                     OwnedValue::of(std::optional<Link>(Link{.target = Entity::fromParts(4, 1, 1)})));
        patch.erase(path<Squad>("members[0]"));
        patch.set(path<Squad>("link.weight"), OwnedValue::of(std::int32_t{2}));
        ith::remapEntities(patch, {.function = [](void*, Entity entity) {
                               return Entity::fromParts(entity.index(), entity.generation(), 2);
                           }});
        CHECK(*patch.entries()[0].value.as<Entity>() == Entity::fromParts(3, 1, 2));
        CHECK(patch.entries()[1].value.as<std::optional<Link>>()->value().target == Entity::fromParts(4, 1, 2));
        CHECK(*patch.entries()[3].value.as<std::int32_t>() == 2);
    }

    TEST_CASE("a patch over every reachable value, and its inverse, on every reflected test type") {
        const ith::TypeRegistry registry = ith::test::makeTestRegistry();
        std::uint64_t seed = 0;
        int types = 0;
        for (const ith::TypeInfo* type : registry.types()) {
            if (type->kind != ith::TypeKind::Struct || type->fields.empty()) {
                continue;
            }
            ++types;
            const std::string name(type->name);
            CAPTURE(name);
            const ith::test::Object original(*type);
            ith::test::fill(*type, original.get(), seed);
            const ith::test::Object other(*type);
            ith::test::fill(*type, other.get(), seed);
            ith::test::fill(*type, other.get(), seed);

            // Set every leaf the other value also reaches; then, innermost vectors first, insert a
            // default element at the front and erase the last one.
            Patch patch(*type);
            std::vector<PropertyPath> vectors;
            for (const ith::test::ReachedPath& reached : ith::test::reachablePaths(*type, original.get())) {
                const PropertyPath reachedPath = *PropertyPath::parse(*type, reached.text);
                if (reached.type->kind == ith::TypeKind::DynamicArray) {
                    vectors.push_back(reachedPath);
                }
                if (reached.type->kind == ith::TypeKind::Struct || reached.type->container != nullptr) {
                    continue;
                }
                if (ith::Expected<OwnedValue> value = ith::readValue(reachedPath, other.get())) {
                    patch.set(reachedPath, std::move(*value));
                }
            }
            for (auto vector = vectors.rbegin(); vector != vectors.rend(); ++vector) {
                const std::size_t size = vector->leafType().container->size(*ith::resolve(*vector, original.get()));
                const ith::TypeInfo& element = vector->leafType().element();
                patch.insert(*PropertyPath::parse(*type, vector->toString() + "[0]"), OwnedValue(element));
                patch.erase(*PropertyPath::parse(*type, vector->toString() + "[" + std::to_string(size) + "]"));
            }

            const ith::test::Object value(*type);
            type->destroy(value.get());
            type->copy(value.get(), original.get());
            ith::Expected<Patch> inverse = ith::applyPatch(patch, value.get());
            REQUIRE_MESSAGE(inverse.hasValue(), messageOf(inverse));
            CHECK(inverse->entries().size() == patch.entries().size());
            CHECK_FALSE(type->equals(value.get(), original.get()));
            const ith::Expected<Patch> redo = ith::applyPatch(*inverse, value.get());
            REQUIRE_MESSAGE(redo.hasValue(), messageOf(redo));
            CHECK(type->equals(value.get(), original.get()));

            // The inverse of the inverse is the patch again.
            REQUIRE(ith::applyPatch(*redo, value.get()).hasValue());
            const ith::test::Object again(*type);
            type->destroy(again.get());
            type->copy(again.get(), original.get());
            REQUIRE(ith::applyPatch(patch, again.get()).hasValue());
            CHECK(type->equals(value.get(), again.get()));
        }
        CHECK(types > 10);
    }
}
