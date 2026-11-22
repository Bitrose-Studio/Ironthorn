// Property paths (ADR-0033): parsing, printing, resolving, prefixes and every error.

#include "ith/reflection/PropertyPath.h"

#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeOf.h"
#include "ith/reflection/TypeRegistry.h"

#include "ArchiveTypes.h"
#include "AssertCapture.h"
#include "Loadout.h"
#include "PathTestSupport.h"
#include "ReflectedTypes.h"
#include "TestValues.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <ostream> // doctest prints std::string_view operands through an ostream
#include <string>
#include <string_view>
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

using ith::PathStep;
using ith::PropertyPath;
using ith::test::Containers;
using ith::test::Hit;
using ith::test::Inventory;
using ith::test::Vector3;

PropertyPath parsed(const ith::TypeInfo& root, std::string_view text) {
    ith::Expected<PropertyPath> path = PropertyPath::parse(root, text);
    REQUIRE_MESSAGE(path.hasValue(), messageOf(path));
    return *path;
}

// The code of the error parsing `text` gives, and whether its message contains `part`.
void checkParseError(const ith::TypeInfo& root, std::string_view text, std::string_view code, std::string_view part) {
    CAPTURE(text);
    const ith::Expected<PropertyPath> path = PropertyPath::parse(root, text);
    REQUIRE_FALSE(path.hasValue());
    CHECK(path.error().code == code);
    CAPTURE(path.error().message);
    CHECK(path.error().message.find(part) != std::string::npos);
}

Containers makeContainers() {
    Containers value;
    value.fixed = {1, 2, 3};
    value.names = {"a", "b"};
    value.points = {Vector3{.x = 1.0f}, Vector3{.y = 2.0f}};
    value.nested = {{1, 2}, {3}};
    return value;
}

} // namespace

TEST_SUITE("reflection.property_path") {
    TEST_CASE("a path reaches a nested field, with its type, field and flags") {
        const PropertyPath path = parsed(ith::typeOf<Hit>(), "where.y");
        CHECK(path.rootType().id == ith::typeOf<Hit>().id);
        CHECK(path.leafType().id == ith::typeOf<float>().id);
        REQUIRE(path.leafField() != nullptr);
        CHECK(path.leafField()->name == "y");
        CHECK(path.toString() == "where.y");
        CHECK(path.toQualifiedString() == "ith::test::Hit.where.y");
        REQUIRE(path.steps().size() == 2);
        CHECK(path.steps()[0] == PathStep{.kind = PathStep::Kind::Field, .index = 3});
        CHECK(path.steps()[1] == PathStep{.kind = PathStep::Kind::Field, .index = 1});

        Hit hit;
        hit.where.y = 4.0f;
        const auto value = ith::resolve(path, &hit);
        REQUIRE(value.hasValue());
        CHECK(*value == &hit.where.y);
        const Hit& constHit = hit;
        const auto constValue = ith::resolve(path, static_cast<const void*>(&constHit));
        REQUIRE(constValue.hasValue());
        CHECK(*static_cast<const float*>(*constValue) == 4.0f);
    }

    TEST_CASE("the empty path is the whole value") {
        const PropertyPath path = parsed(ith::typeOf<Hit>(), "");
        CHECK(path.empty());
        CHECK(path.leafType().id == ith::typeOf<Hit>().id);
        CHECK(path.leafField() == nullptr);
        CHECK(path.toString().empty());
        CHECK(path.toQualifiedString() == "ith::test::Hit");
        CHECK(path == PropertyPath(ith::typeOf<Hit>()));
        Hit hit;
        CHECK(*ith::resolve(path, &hit) == &hit);
    }

    TEST_CASE("a FormerName finds the field, and the path prints the current name") {
        const PropertyPath path = parsed(ith::typeOf<Hit>(), "target.maxHealth");
        CHECK(path.toString() == "target.maximum");
        CHECK(path == parsed(ith::typeOf<Hit>(), "target.maximum"));
        REQUIRE(path.leafField() != nullptr);
        CHECK(path.leafField()->attributes.min == 1.0);
    }

    TEST_CASE("array elements: fixed bounds when parsing, vector bounds when resolving") {
        Containers value = makeContainers();
        CHECK(*ith::resolve(parsed(ith::typeOf<Containers>(), "fixed[2]"), &value) == &value.fixed[2]);
        CHECK(*ith::resolve(parsed(ith::typeOf<Containers>(), "points[1].y"), &value) == &value.points[1].y);
        CHECK(*ith::resolve(parsed(ith::typeOf<Containers>(), "nested[1][0]"), &value) == value.nested[1].data());

        const PropertyPath beyond = parsed(ith::typeOf<Containers>(), "points[2].y");
        const auto missing = ith::resolve(beyond, &value);
        REQUIRE_FALSE(missing.hasValue());
        CHECK(missing.error().code == "reflection.path.index_out_of_range");
        CHECK(missing.error().message.find("points[2].y") != std::string::npos);
        value.points.resize(3);
        CHECK(*ith::resolve(beyond, &value) == &value.points[2].y);

        const PropertyPath element = parsed(ith::typeOf<Containers>(), "names[0]");
        CHECK(element.leafType().id == ith::typeOf<std::string>().id);
        REQUIRE(element.leafField() != nullptr);
        CHECK(element.leafField()->name == "names");
    }

    TEST_CASE("optionals are transparent") {
        const PropertyPath whole = parsed(ith::typeOf<Containers>(), "maybePoint");
        CHECK(whole.leafType().kind == ith::TypeKind::Optional);
        const PropertyPath inner = parsed(ith::typeOf<Containers>(), "maybePoint.z");
        REQUIRE(inner.steps().size() == 3);
        CHECK(inner.steps()[1].kind == PathStep::Kind::Unwrap);
        CHECK(inner.toString() == "maybePoint.z");
        CHECK(whole.isPrefixOf(inner));

        Containers value;
        const auto empty = ith::resolve(inner, &value);
        REQUIRE_FALSE(empty.hasValue());
        CHECK(empty.error().code == "reflection.path.empty_optional");
        CHECK(*ith::resolve(whole, &value) == &value.maybePoint);
        value.maybePoint = Vector3{.z = 5.0f};
        CHECK(*ith::resolve(inner, &value) == &value.maybePoint->z);
    }

    TEST_CASE("flags along the path are combined") {
        CHECK(
            ith::hasFlags(parsed(ith::typeOf<Inventory>(), "m_capacity").chainFlags(), ith::ReflectionFlags::ReadOnly));
        CHECK(ith::hasFlags(parsed(ith::typeOf<Inventory>(), "m_items[0]").chainFlags(),
                            ith::ReflectionFlags::Transient));
        CHECK(parsed(ith::typeOf<Hit>(), "where.x").chainFlags() == ith::ReflectionFlags::None);
        CHECK(ith::hasFlags(parsed(ith::typeOf<Hit>(), "target.current").chainFlags(),
                            ith::ReflectionFlags::Replicated | ith::ReflectionFlags::SaveGame));
    }

    TEST_CASE("prefixes and equality") {
        const ith::TypeInfo& type = ith::typeOf<Containers>();
        const PropertyPath points = parsed(type, "points");
        const PropertyPath second = parsed(type, "points[1]");
        const PropertyPath secondY = parsed(type, "points[1].y");
        CHECK(points.isPrefixOf(secondY));
        CHECK(second.isPrefixOf(secondY));
        CHECK(secondY.isPrefixOf(secondY));
        CHECK(PropertyPath(type).isPrefixOf(secondY));
        CHECK_FALSE(secondY.isPrefixOf(second));
        CHECK_FALSE(parsed(type, "points[0]").isPrefixOf(secondY));
        CHECK_FALSE(PropertyPath(ith::typeOf<Hit>()).isPrefixOf(secondY));
        CHECK(second != secondY);
        CHECK(PropertyPath(type) != PropertyPath(ith::typeOf<Hit>()));
    }

    TEST_CASE("withoutLastStep drops one step") {
        const ith::TypeInfo& type = ith::typeOf<Containers>();
        const PropertyPath parent = parsed(type, "points[1].y").withoutLastStep();
        CHECK(parent == parsed(type, "points[1]"));
        CHECK(parent.leafType().id == ith::typeOf<Vector3>().id);
        CHECK(parent.withoutLastStep() == parsed(type, "points"));
        CHECK(parent.withoutLastStep().withoutLastStep().empty());

        ith::test::AssertCapture capture;
        CHECK(PropertyPath(type).withoutLastStep().empty());
        CHECK(capture.count() == kAsserts);
    }

    TEST_CASE("syntax errors name the byte offset") {
        const ith::TypeInfo& type = ith::typeOf<Containers>();
        const std::string_view syntax = "reflection.path.syntax";
        checkParseError(type, ".fixed", syntax, "expected a field name (at byte 0)");
        checkParseError(type, "1fixed", syntax, "expected a field name (at byte 0)");
        checkParseError(type, "points.", syntax, "expected a field name (at byte 7)");
        checkParseError(type, "points..x", syntax, "expected a field name (at byte 7)");
        checkParseError(type, "points x", syntax, "expected '.' or '[' (at byte 6)");
        checkParseError(type, "fixed[", syntax, "expected an index (at byte 6)");
        checkParseError(type, "fixed[-1]", syntax, "expected an index (at byte 6)");
        checkParseError(type, "fixed[01]", syntax, "no leading zeros (at byte 6)");
        checkParseError(type, "fixed[1", syntax, "expected ']' (at byte 7)");
        checkParseError(type, "fixed[1x]", syntax, "expected ']' (at byte 7)");
        checkParseError(type, "points[99999999999999999999999]", syntax, "too large (at byte 7)");
        checkParseError(type, "fixed[0]x", syntax, "expected '.' or '[' (at byte 8)");
    }

    TEST_CASE("type errors name the path and the type") {
        const ith::TypeInfo& type = ith::typeOf<Containers>();
        checkParseError(type, "nope", "reflection.path.unknown_field", "ith::test::Containers has no field 'nope'");
        checkParseError(type, "points[0].w", "reflection.path.unknown_field", "has no field 'w'");
        checkParseError(type, "names.size", "reflection.path.not_a_struct", "std::vector");
        checkParseError(type, "maybe.x", "reflection.path.not_a_struct", "double");
        checkParseError(type, "points[0][1]", "reflection.path.not_indexable", "ith::test::Vector3");
        checkParseError(type, "maybe[0]", "reflection.path.not_indexable", "double");
        checkParseError(type, "fixed[3]", "reflection.path.index_out_of_range", "3 elements");
        checkParseError(ith::typeOf<Hit>(), "[0]", "reflection.path.not_indexable", "ith::test::Hit");
        checkParseError(ith::typeOf<ith::test::archive::Link>(), "target.index", "reflection.path.not_a_struct",
                        "ith::Entity");
    }

    TEST_CASE("a path may start with an index when the root is an array") {
        const ith::TypeInfo& type = ith::typeOf<std::vector<Vector3>>();
        const PropertyPath path = parsed(type, "[1].x");
        CHECK(path.toString() == "[1].x");
        std::vector<Vector3> points(2);
        CHECK(*ith::resolve(path, &points) == &points[1].x);
    }

    TEST_CASE("qualified paths start with a registered type name") {
        const ith::TypeRegistry registry = ith::test::makeTestRegistry();
        const auto path = PropertyPath::parseQualified(registry, "ith::test::Hit.where.x");
        REQUIRE(path.hasValue());
        CHECK(*path == parsed(ith::typeOf<Hit>(), "where.x"));
        CHECK(path->toQualifiedString() == "ith::test::Hit.where.x");

        const auto renamed = PropertyPath::parseQualified(registry, "ith::test::Damage.target.maxHealth");
        REQUIRE(renamed.hasValue());
        CHECK(renamed->toQualifiedString() == "ith::test::Hit.target.maximum");

        const auto whole = PropertyPath::parseQualified(registry, "ith::test::Containers");
        REQUIRE(whole.hasValue());
        CHECK(whole->empty());

        const auto unknown = PropertyPath::parseQualified(registry, "ith::test::Nope.x");
        REQUIRE_FALSE(unknown.hasValue());
        CHECK(unknown.error().code == "reflection.path.unknown_type");
        CHECK(unknown.error().message.find("'ith::test::Nope'") != std::string::npos);

        for (const std::string_view text : {".x", "", "[0]"}) {
            CAPTURE(text);
            const auto noName = PropertyPath::parseQualified(registry, text);
            REQUIRE_FALSE(noName.hasValue());
            CHECK(noName.error().code == "reflection.path.syntax");
            CHECK(noName.error().message.find("expected a type name (at byte 0)") != std::string::npos);
        }
        const auto trailing = PropertyPath::parseQualified(registry, "ith::test::Hit.");
        REQUIRE_FALSE(trailing.hasValue());
        CHECK(trailing.error().message.find("expected a field name (at byte 15)") != std::string::npos);
        const auto dotIndex = PropertyPath::parseQualified(registry, "ith::test::Containers.[0]");
        REQUIRE_FALSE(dotIndex.hasValue());
        CHECK(dotIndex.error().message.find("expected a field name (at byte 22)") != std::string::npos);
        const auto later = PropertyPath::parseQualified(registry, "ith::test::Containers.points[x]");
        REQUIRE_FALSE(later.hasValue());
        CHECK(later.error().message.find("expected an index (at byte 29)") != std::string::npos);
    }

    TEST_CASE("every reachable value of every reflected test type has a path") {
        const ith::TypeRegistry registry = ith::test::makeTestRegistry();
        std::uint64_t seed = 0;
        int paths = 0;
        for (const ith::TypeInfo* type : registry.types()) {
            if (type->kind != ith::TypeKind::Struct) {
                continue;
            }
            const std::string name(type->name);
            CAPTURE(name);
            const ith::test::Object value(*type);
            ith::test::fill(*type, value.get(), seed);
            for (const ith::test::ReachedPath& reached : ith::test::reachablePaths(*type, value.get())) {
                CAPTURE(reached.text);
                ++paths;
                const PropertyPath path = parsed(*type, reached.text);
                CHECK(path.leafType().id == reached.type->id);
                CHECK(path.toString() == reached.text);
                const auto resolved = ith::resolve(path, static_cast<const void*>(value.get()));
                REQUIRE(resolved.hasValue());
                CHECK(*resolved == reached.value);

                const auto qualified = PropertyPath::parseQualified(registry, path.toQualifiedString());
                REQUIRE(qualified.hasValue());
                CHECK(*qualified == path);
            }
        }
        CHECK(paths > 200);
    }
}
