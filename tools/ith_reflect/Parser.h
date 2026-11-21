#pragma once

// The parser of the ith_reflect generator: finds the annotated declarations of one header.
//
// It tracks namespaces, braces and templates, and reads ITH_TYPE, ITH_FIELD and ITH_ENUM with
// the declarations they annotate. It doesn't understand C++ types: the generated code is
// compiled by the real compiler, which works out sizes, offsets and field types. Syntax outside
// the supported subset (ADR-0018 rule 3) is refused with a diagnostic rather than guessed.

#include "ith/foundation/Expected.h"

#include "ith_reflect/Lexer.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ith::reflect {

// The kind of the module being generated (ADR-0017). Only editor modules may hold EditorOnly
// types and fields.
enum class ModuleKind : std::uint8_t {
    Runtime,
    Client,
    Tools,
    Editor
};

// Parses "runtime", "client", "tools" or "editor"; fails with "reflect.bad_kind".
Expected<ModuleKind> parseModuleKind(std::string_view text);

// The attributes of one annotation, as written in the header.
struct ParsedAttributes {
    std::string role;                                      // ITH_TYPE only: Struct, Component, Resource, Event or Asset
    std::string subject;                                   // ITH_ENUM only: the name of the enum it annotates
    std::vector<std::string> flags;                        // flag names (ith::ReflectionFlags) in the order written
    std::string min;                                       // number as written (with its sign), empty when absent
    std::string max;                                       // number as written (with its sign), empty when absent
    std::string unit;                                      // string literal with quotes, empty when absent
    std::string category;                                  // string literal with quotes, empty when absent
    std::string displayName;                               // string literal with quotes, empty when absent
    std::string tooltip;                                   // string literal with quotes, empty when absent
    std::string formerName;                                // the former name without quotes, empty when absent
    std::vector<std::pair<std::string, std::string>> meta; // key, string literal with quotes
};

struct ParsedField {
    std::string name;
    int line = 0;
    ParsedAttributes attributes;
};

struct ParsedType {
    bool isEnum = false;
    std::string qualifiedName; // e.g. "game::Health"
    std::string file;
    int line = 0;
    ParsedAttributes attributes;
    std::vector<ParsedField> fields;      // structs
    std::vector<std::string> enumerators; // enums
};

// Finds the annotated types of `text`, the contents of the header at `path`. Returns every
// problem found, each with its line, when the header uses unsupported syntax.
Expected<std::vector<ParsedType>, std::vector<Diagnostic>> parseHeader(std::string_view path, std::string_view text,
                                                                       ModuleKind kind);

} // namespace ith::reflect
