#pragma once

// Helpers of the editor_core tests: a type registry with the engine, editor_core and test types,
// commands run with JSON text arguments, and the world as level text, which is the same for the
// same world (ADR-0034 rule 9).

#include "ith/editor_core/EditorSession.h"
#include "ith/foundation/Uuid.h"
#include "ith/level/Level.h"
#include "ith/reflection/TypeRegistry.h"
#include "ith/serialization/Json.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <ostream> // doctest prints std::string operands through an ostream
#include <string>
#include <string_view>

ITH_DECLARE_REFLECTION_MODULE(ecs)
ITH_DECLARE_REFLECTION_MODULE(editor_core)
ITH_DECLARE_REFLECTION_MODULE(test_editor_core)

namespace ith::test::editor {

inline const TypeRegistry& types() {
    static const TypeRegistry kTypes = [] {
        TypeRegistry result;
        generated::registerReflection_ecs(result);
        generated::registerReflection_editor_core(result);
        generated::registerReflection_test_editor_core(result);
        return result;
    }();
    return kTypes;
}

// A stable id for tests: n in the low bits.
inline Uuid idOf(std::uint64_t n) {
    return Uuid(0xED, n);
}

inline std::string idText(std::uint64_t n) {
    return idOf(n).toString();
}

inline serialization::Value json(std::string_view text) {
    auto parsed = serialization::parseJson(text);
    REQUIRE_MESSAGE(parsed.hasValue(), (parsed.hasValue() ? std::string() : parsed.error().message));
    return std::move(*parsed);
}

// Runs a command that must succeed.
inline serialization::Value run(EditorSession& session, std::string_view id, std::string_view args = "{}") {
    auto result = session.execute(id, json(args));
    REQUIRE_MESSAGE(result.hasValue(),
                    (std::string(id) + ": " + (result.hasValue() ? std::string() : result.error().message)));
    return std::move(*result);
}

// Runs a command that must fail with `code`.
inline void fails(EditorSession& session, std::string_view id, std::string_view args, std::string_view code) {
    auto result = session.execute(id, json(args));
    REQUIRE_FALSE(result.hasValue());
    CAPTURE(result.error().message);
    CHECK(result.error().code == code);
}

// The world as level text.
inline std::string levelText(const EditorSession& session) {
    auto text = saveLevel(session.document().world(), session.document().levelId(), types());
    REQUIRE_MESSAGE(text.hasValue(), (text.hasValue() ? std::string() : text.error().message));
    return std::move(*text);
}

} // namespace ith::test::editor
