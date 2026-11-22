// Tests of the ith_reflect generator: the golden output for a fixture header, the accepted
// syntax, and every refused construct with its message and line (ADR-0018 rule 12).

#include "ith/foundation/Hash.h"

#include "ith_reflect/Generator.h"

#include <doctest/doctest.h>

// doctest prints std::string_view operands of failed checks through an ostream.
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ith::reflect::Diagnostic;
using ith::reflect::GenerateOptions;
using ith::reflect::ModuleKind;
using ith::reflect::SourceHeader;

std::string readText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    CHECK_MESSAGE(file.good(), "can't read " << path.string());
    std::string text(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>{});
    // Git may check text files out with CRLF line endings; the generator writes LF.
    std::erase(text, '\r');
    return text;
}

ith::Expected<std::string, std::vector<Diagnostic>> generateOne(std::string_view text,
                                                                ModuleKind kind = ModuleKind::Runtime) {
    const std::vector<SourceHeader> headers = {
        SourceHeader{.path = "Test.h", .includePath = "Test.h", .text = std::string(text)}};
    return ith::reflect::generate(GenerateOptions{.module = "test", .kind = kind}, headers);
}

// The diagnostics for `text`, which must be refused.
std::vector<Diagnostic> errorsOf(std::string_view text, ModuleKind kind = ModuleKind::Runtime) {
    auto result = generateOne(text, kind);
    REQUIRE_MESSAGE(!result.hasValue(), "expected the header to be refused:\n" << text);
    return result.error();
}

// Checks that `text` is refused with exactly one diagnostic at `line` whose message contains
// `message`.
void checkRefused(std::string_view text, int line, std::string_view message) {
    const std::vector<Diagnostic> errors = errorsOf(text);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].file == "Test.h");
    CHECK(errors[0].line == line);
    CHECK_MESSAGE(errors[0].message.find(message) != std::string::npos, errors[0].message);
}

std::string generatedText(std::string_view text) {
    auto result = generateOne(text);
    if (!result) {
        for (const Diagnostic& diagnostic : result.error()) {
            MESSAGE(ith::reflect::formatDiagnostic(diagnostic));
        }
    }
    REQUIRE(result.hasValue());
    return *result;
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

// The id literal the generator writes for `name`.
std::string idLiteral(std::string_view name) {
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "0x%016llxull", static_cast<unsigned long long>(ith::fnv1a64(name)));
    return buffer.data();
}

} // namespace

TEST_SUITE("reflect_tool.generator") {
    TEST_CASE("the fixture header generates the golden output") {
        const std::filesystem::path directory = "tests/reflect_tool";
        const std::vector<SourceHeader> headers = {SourceHeader{.path = "tests/reflect_tool/fixtures/Basic.h",
                                                                .includePath = "fixtures/Basic.h",
                                                                .text = readText(directory / "fixtures/Basic.h")}};
        auto generated = ith::reflect::generate(GenerateOptions{.module = "basic"}, headers);
        REQUIRE(generated.hasValue());

        const std::filesystem::path golden = directory / "golden/Basic.reflect.cpp.golden";
        const bool matches = *generated == readText(golden);
        if (!matches) {
            // Leaves the new output in the build tree, so a deliberate change can be reviewed
            // and copied over the golden file.
            const std::filesystem::path actual = "build/Basic.reflect.cpp.actual";
            std::filesystem::create_directories(actual.parent_path());
            std::ofstream(actual, std::ios::binary) << *generated;
            MESSAGE("the output differs from " << golden.string() << "; it was written to " << actual.string());
        }
        CHECK(matches);
    }

    TEST_CASE("the output is deterministic") {
        const std::string_view text = "namespace a { struct T {\nITH_TYPE(Struct)\nITH_FIELD()\nint x;\n}; }\n";
        CHECK(generatedText(text) == generatedText(text));
    }

    TEST_CASE("ids are the FNV-1a hashes of the qualified type name and of the field name") {
        const std::string text =
            generatedText("namespace foo { struct T {\nITH_TYPE(Struct)\nITH_FIELD()\nint bar;\n}; }\n"
                          "struct foobar {\nITH_TYPE(Struct)\n};\n");
        CHECK(contains(text, idLiteral("foo::T")));
        CHECK(contains(text, idLiteral("bar")));
        CHECK(contains(text, "0x85944171f73967e8ull")); // the published FNV-1a 64 vector for "foobar"
    }

    TEST_CASE("a module without annotated types still gets its registration function") {
        const std::string text = generatedText("struct Plain { int x; };\n");
        CHECK(contains(text, "void ith::generated::registerReflection_test(::ith::TypeRegistry& registry) {"));
        CHECK(contains(text, "registry.addAll({});"));
        CHECK_FALSE(contains(text, "#include \"Test.h\""));
    }

    TEST_CASE("accepted syntax") {
        SUBCASE("annotated type in nested namespaces, with comments and blank lines") {
            const std::string text = generatedText("namespace a::b {\n"
                                                   "namespace c {\n"
                                                   "struct alignas(8) T final {\n"
                                                   "    ITH_TYPE(Component) // the type\n"
                                                   "\n"
                                                   "    /* a comment */ ITH_FIELD(Min = -1)\n"
                                                   "    std::array<int, 3> values = {1, 2, 3};\n"
                                                   "    ITH_FIELD()\n"
                                                   "    std::vector<std::pair<int, int>> pairs{{1, 2}};\n"
                                                   "    ITH_FIELD()\n"
                                                   "    int\n"
                                                   "        split;\n"
                                                   "};\n"
                                                   "}\n"
                                                   "}\n");
            CHECK(contains(text, "\"a::b::c::T\""));
            CHECK(contains(text, "offsetof(Self, values)"));
            CHECK(contains(text, "offsetof(Self, pairs)"));
            CHECK(contains(text, "offsetof(Self, split)"));
            CHECK(contains(text, ".min = -1"));
        }

        SUBCASE("an #if block elsewhere in the type, and an include guard style #ifndef outside it") {
            const std::string text = generatedText("struct T {\n"
                                                   "    ITH_TYPE(Struct)\n"
                                                   "    ITH_FIELD()\n"
                                                   "    int x = 0;\n"
                                                   "#ifdef SOMETHING\n"
                                                   "    int debugOnly = 0;\n"
                                                   "#endif\n"
                                                   "};\n");
            CHECK(contains(text, "offsetof(Self, x)"));
            CHECK_FALSE(contains(text, "debugOnly"));
        }

        SUBCASE("Immutable on a component") {
            const std::string text = generatedText("struct T {\nITH_TYPE(Component, Immutable)\n};\n");
            CHECK(contains(text, "::ith::ReflectionFlags::Immutable"));
        }

        SUBCASE("EditorOnly in an editor module") {
            auto result = generateOne("struct T {\nITH_TYPE(Component, EditorOnly)\n};\n", ModuleKind::Editor);
            REQUIRE(result.hasValue());
            CHECK(contains(*result, "::ith::ReflectionFlags::EditorOnly"));
        }

        SUBCASE("enumerators with values, attributes and a trailing comma") {
            const std::string text = generatedText("enum class E : int {\n"
                                                   "    A = (1 << 2),\n"
                                                   "    B [[deprecated]] = A,\n"
                                                   "    C,\n"
                                                   "};\n"
                                                   "ITH_ENUM(E)\n");
            CHECK(contains(text, "const ::ith::TypeInfo& ithEnumTypeInfo(::E*) noexcept {"));
            CHECK(contains(text, "static_cast<std::int64_t>(Self::A)"));
            CHECK(contains(text, "static_cast<std::int64_t>(Self::B)"));
            CHECK(contains(text, "static_cast<std::int64_t>(Self::C)"));
        }
    }

    TEST_CASE("refused: annotations inside #if blocks") {
        checkRefused("#if 1\nstruct T {\nITH_TYPE(Struct)\n};\n#endif\n", 3,
                     "annotations inside #if blocks are not supported");
        checkRefused("struct T {\nITH_TYPE(Struct)\n#ifdef X\nITH_FIELD()\nint x;\n#endif\n};\n", 4,
                     "annotations inside #if blocks are not supported");
        checkRefused("#ifndef X\nenum class E { A };\nITH_ENUM(E)\n#endif\n", 3,
                     "annotations inside #if blocks are not supported");
        checkRefused("struct T {\nITH_TYPE(Struct)\nITH_FIELD()\n#if X\nint x;\n#else\nfloat x;\n#endif\n};\n", 5,
                     "#if blocks can't split a reflected field");
        checkRefused("enum class E {\nA,\n#if X\nB,\n#endif\n};\nITH_ENUM(E)\n", 4,
                     "#if blocks are not supported inside a reflected enum");
    }

    TEST_CASE("refused: types outside namespace scope, templates and inheritance") {
        checkRefused("struct Outer {\nstruct Inner {\nITH_TYPE(Struct)\n};\n};\n", 3,
                     "nested types can't be reflected");
        checkRefused("struct Outer {\nenum class E { A };\nITH_ENUM(E)\n};\n", 3, "nested enums can't be reflected");
        checkRefused("void f() {\nstruct Local {\nITH_TYPE(Struct)\n};\n}\n", 3, "nested types can't be reflected");
        checkRefused("template <typename T>\nstruct Box {\nITH_TYPE(Struct)\n};\n", 3, "templates can't be reflected");
        checkRefused("template <>\nstruct Box<int> {\nITH_TYPE(Struct)\n};\n", 3, "templates can't be reflected");
        checkRefused("struct Base {};\nstruct T : Base {\nITH_TYPE(Struct)\n};\n", 3,
                     "reflected types can't have base classes");
        checkRefused("union U {\nITH_TYPE(Struct)\n};\n", 2, "unions can't be reflected");
        checkRefused("namespace {\nstruct T {\nITH_TYPE(Struct)\n};\n}\n", 3, "anonymous namespace");
        checkRefused("namespace a { struct T; }\nstruct a::T {\nITH_TYPE(Struct)\n};\n", 3,
                     "not with a qualified name");
        checkRefused("struct {\nITH_TYPE(Struct)\n} anonymous;\n", 2, "anonymous types can't be reflected");
        checkRefused("enum Plain { A };\nITH_ENUM(Plain)\n", 2, "only enum class can be reflected");
        checkRefused("enum class Opaque : int;\nITH_ENUM(Opaque)\n", 2, "needs its list of enumerators");
        checkRefused("enum class Truth : bool { No, Yes };\nITH_ENUM(Truth)\n", 2, "bool underlying type");
        checkRefused("namespace {\nenum class E { A };\nITH_ENUM(E)\n}\n", 3, "anonymous namespace");
        checkRefused("struct Base {};\nstruct Derived : Base {\nITH_TYPE(Struct)\n};\n", 3, "base classes");
    }

    TEST_CASE("refused: misplaced annotations") {
        checkRefused("ITH_TYPE(Struct)\n", 1, "ITH_TYPE must be in the body of a struct or class");
        checkRefused("struct T {\nITH_TYPE(Struct)\nITH_TYPE(Struct)\n};\n", 3, "only one ITH_TYPE");
        checkRefused("struct T {\nITH_TYPE(Struct);\n};\n", 2, "without a semicolon");
        checkRefused("struct T {\nITH_FIELD()\nint x;\n};\n", 2, "ITH_FIELD must be in the body of a type annotated");
        checkRefused("struct T {\nITH_FIELD()\nint x;\nITH_TYPE(Struct)\n};\n", 2,
                     "ITH_FIELD must be in the body of a type annotated");
        checkRefused("ITH_FIELD()\nint x;\n", 1, "ITH_FIELD must be in the body of a type annotated");
        checkRefused("enum class ITH_ENUM(E) E { A };\n", 1, "ITH_ENUM goes after the enum");
        checkRefused("enum class E { A };\nITH_ENUM(F)\n", 2, "ITH_ENUM names 'F', but the enum before it is 'E'");
        checkRefused("enum class E { A };\nint x;\nITH_ENUM(E)\n", 3, "must directly follow the enum class");
        checkRefused("ITH_ENUM(E)\n", 1, "must directly follow the enum class");
        checkRefused("enum class E { A } value;\nITH_ENUM(E)\n", 2, "must directly follow the enum class");
        checkRefused("enum class E { A };\nITH_ENUM(E);\n", 2, "without a semicolon");
        checkRefused("enum class E { A };\nITH_ENUM()\n", 2, "needs the enum's name first");
        checkRefused("enum class E { A };\nITH_ENUM(Tooltip = \"x\")\n", 2, "needs the enum's name first");
        checkRefused("struct T {\nITH_TYPE\n};\n", 2, "needs parentheses");
    }

    TEST_CASE("refused: declarations ITH_FIELD can't annotate") {
        const auto field = [](std::string_view declaration) {
            return "struct T {\nITH_TYPE(Struct)\nITH_FIELD()\n" + std::string(declaration) + "\n};\n";
        };
        checkRefused(field("int f() const;"), 4, "not a function");
        checkRefused(field("static int count;"), 4, "can't annotate a 'static' declaration");
        checkRefused(field("const int fixed = 1;"), 4, "can't annotate a 'const' declaration");
        checkRefused(field("using Alias = int;"), 4, "can't annotate a 'using' declaration");
        checkRefused(field("int values[4];"), 4, "use std::array");
        checkRefused(field("unsigned bits : 3;"), 4, "bit-fields can't be reflected");
        checkRefused(field("int a, b;"), 4, "one declarator per line");
        checkRefused(field("int a = 1, b = 2;"), 4, "one declarator per line");
        checkRefused(field("int* pointer;"), 4, "pointer and reference fields");
        checkRefused(field("int& reference;"), 4, "pointer and reference fields");
        checkRefused(field("[[no_unique_address]] Empty empty;"), 4, "[[no_unique_address]] can't be used");
        checkRefused(field("[[msvc::no_unique_address]] Empty empty;"), 4, "[[no_unique_address]] can't be used");
        checkRefused(field("Empty empty [[maybe_unused]];"), 4, "attributes such as");
        checkRefused(field("alignas(16) float aligned;"), 4, "alignas can't be used on reflected fields");
        checkRefused("struct T {\nITH_TYPE(Struct)\nITH_FIELD()\n};\n", 3, "followed by a data member declaration");
        checkRefused("struct T {\nITH_TYPE(Struct)\nITH_FIELD()\nITH_FIELD()\nint x;\n};\n", 3,
                     "followed by a data member declaration");
    }

    TEST_CASE("refused: attributes") {
        const auto type = [](std::string_view arguments) {
            return "struct T {\nITH_TYPE(" + std::string(arguments) + ")\n};\n";
        };
        const auto field = [](std::string_view arguments) {
            return "struct T {\nITH_TYPE(Struct)\nITH_FIELD(" + std::string(arguments) + ")\nint x;\n};\n";
        };
        checkRefused(type(""), 2, "ITH_TYPE needs a role first");
        checkRefused(type("Category = \"A\""), 2, "ITH_TYPE needs a role first");
        checkRefused(type("Component, Struct"), 2, "only allowed first in ITH_TYPE");
        checkRefused(field("Component"), 3, "only allowed first in ITH_TYPE");
        checkRefused(field("Colour = \"red\""), 3, "unknown attribute 'Colour'");
        checkRefused(field("replicated"), 3, "unknown attribute 'replicated'");
        checkRefused(type("Struct, Min = 0"), 2, "'Min' can't be used on a type");
        checkRefused(type("Struct, Replicated"), 2, "'Replicated' can't be used on a type");
        checkRefused(field("Flags"), 3, "'Flags' can't be used on a field");
        checkRefused("enum class E { A };\nITH_ENUM(E, Unit = \"cm\")\n", 2, "'Unit' can't be used on an enum");
        checkRefused(field("SaveGame, SaveGame"), 3, "duplicate attribute 'SaveGame'");
        checkRefused(field("Replicated = 1"), 3, "is a flag and takes no value");
        checkRefused(field("Min = \"zero\""), 3, "'Min' needs a number");
        checkRefused(field("Max"), 3, "'Max' needs a number");
        checkRefused(field("Tooltip = 3"), 3, "'Tooltip' needs a plain string literal");
        checkRefused(field("Tooltip = u8\"text\""), 3, "'Tooltip' needs a plain string literal");
        checkRefused(field("FormerName = \"old::name\""), 3, "the previous field name");
        checkRefused(type("Struct, FormerName = \"not a name\""), 2, "the previous qualified name");
        checkRefused(field("Meta(Widget)"), 3, "'Meta' takes key-value strings");
        checkRefused(field("Meta(Widget = 3)"), 3, "'Meta' takes key-value strings");
        checkRefused(field("Meta(A = \"1\", A = \"2\")"), 3, "duplicate Meta key 'A'");
        checkRefused(field(", SaveGame"), 3, "empty attribute");
        checkRefused(type("Component, EditorOnly"), 2, "EditorOnly is only allowed in editor modules");
        checkRefused(field("EditorOnly"), 3, "EditorOnly is only allowed in editor modules");
        checkRefused(type("Struct, Immutable"), 2, "Immutable is only allowed on a type with the Component role");
        checkRefused(type("Resource, Immutable"), 2, "Immutable is only allowed on a type with the Component role");
        checkRefused(type("Event, Immutable"), 2, "Immutable is only allowed on a type with the Component role");
        checkRefused(field("Immutable"), 3, "'Immutable' can't be used on a field");
        checkRefused("enum class E { A };\nITH_ENUM(E, Immutable)\n", 2, "'Immutable' can't be used on an enum");
        checkRefused(type("Component, Immutable, Immutable"), 2, "duplicate attribute 'Immutable'");
    }

    TEST_CASE("refused: malformed headers") {
        checkRefused("/* never closed\nstruct T {};\n", 1, "unterminated /* comment");
        checkRefused("const char* text = \"never closed;\n", 1, "unterminated string literal");
        checkRefused("const char* text = R\"x(never closed\n", 1, "unterminated raw string literal");
        checkRefused("#endif\n", 1, "#endif without a matching #if");
        checkRefused("#if X\nstruct T {};\n", 3, "#if without a matching #endif");
        checkRefused("namespace a {\nstruct T {};\n", 2, "not closed by the end of the file");
        checkRefused("struct T {};\n}\n", 2, "'}' without a matching '{'");
        checkRefused("void f(\nint x;\n", 1, "'(' is not closed");
    }

    TEST_CASE("FormerName collisions inside a type are refused") {
        const auto type = [](std::string_view fields) {
            return "struct T {\nITH_TYPE(Struct)\n" + std::string(fields) + "};\n";
        };
        checkRefused(type("ITH_FIELD()\nint a;\nITH_FIELD(FormerName = \"a\")\nint b;\n"), 6,
                     "the FormerName 'a' of 'b' is the name of another field");
        checkRefused(type("ITH_FIELD(FormerName = \"old\")\nint a;\nITH_FIELD(FormerName = \"old\")\nint b;\n"), 6,
                     "'b' and 'a' have the same FormerName 'old'");
        CHECK(generateOne(type("ITH_FIELD(FormerName = \"old\")\nint a;\nITH_FIELD()\nint b;\n")).hasValue());
    }

    TEST_CASE("headers that start with a number, a byte order mark or use CRLF line endings") {
        // A number as the very first character must not read before the text.
        CHECK(generateOne("1").hasValue());
        CHECK(generateOne(".5").hasValue());
        CHECK(generateOne("1;\nstruct T {\nITH_TYPE(Struct)\n};\n").hasValue());
        CHECK(generateOne(".5;\nstruct T {\nITH_TYPE(Struct)\n};\n").hasValue());

        auto bom = generateOne("\xEF\xBB\xBFstruct T {\nITH_TYPE(Struct)\n};\n");
        REQUIRE(bom.hasValue());
        CHECK(contains(*bom, "\"T\""));

        // The \r characters are kept: line numbers and continuations must still be right.
        const std::string crlf = "#define LONG_MACRO(x) \\\r\n"
                                 "    (x)\r\n"
                                 "const char* text = \"a \\\r\n"
                                 "b\";\r\n"
                                 "struct T {\r\n"
                                 "    ITH_TYPE(Struct)\r\n"
                                 "    ITH_FIELD(Bogus)\r\n"
                                 "    int x;\r\n"
                                 "};\r\n";
        const std::vector<Diagnostic> errors = errorsOf(crlf);
        REQUIRE(errors.size() == 1);
        CHECK(errors[0].line == 7);
    }

    TEST_CASE("every problem in a header is reported, each with its line") {
        const std::vector<Diagnostic> errors = errorsOf("struct T {\n"
                                                        "    ITH_TYPE(Struct, Bogus)\n"
                                                        "};\n"
                                                        "struct U {\n"
                                                        "    ITH_TYPE(Struct)\n"
                                                        "    ITH_FIELD(Min = \"x\")\n"
                                                        "    int x;\n"
                                                        "};\n");
        REQUIRE(errors.size() == 2);
        CHECK(errors[0].line == 2);
        CHECK(errors[1].line == 6);
        CHECK(ith::reflect::formatDiagnostic(errors[0]) == "Test.h:2: error: unknown attribute 'Bogus'");
    }

    TEST_CASE("two types with the same qualified name in one module are refused") {
        const std::vector<SourceHeader> headers = {
            SourceHeader{
                .path = "A.h", .includePath = "A.h", .text = "namespace n { struct T {\nITH_TYPE(Struct)\n}; }"},
            SourceHeader{
                .path = "B.h", .includePath = "B.h", .text = "namespace n {\nenum class T { X };\nITH_ENUM(T)\n}"},
        };
        auto result = ith::reflect::generate(GenerateOptions{.module = "test"}, headers);
        REQUIRE_FALSE(result.hasValue());
        REQUIRE(result.error().size() == 1);
        CHECK(ith::reflect::formatDiagnostic(result.error()[0]) ==
              "B.h:3: error: 'n::T' is already reflected at A.h:2");
    }

    TEST_CASE("module kinds") {
        CHECK(*ith::reflect::parseModuleKind("runtime") == ModuleKind::Runtime);
        CHECK(*ith::reflect::parseModuleKind("client") == ModuleKind::Client);
        CHECK(*ith::reflect::parseModuleKind("tools") == ModuleKind::Tools);
        CHECK(*ith::reflect::parseModuleKind("editor") == ModuleKind::Editor);
        const auto unknown = ith::reflect::parseModuleKind("game");
        REQUIRE_FALSE(unknown.hasValue());
        CHECK(unknown.error().code == "reflect.bad_kind");
    }
}
