#pragma once

// The ith_reflect generator: turns a module's annotated headers into its .reflect.cpp
// (ADR-0018).
//
// The output holds one record per annotated type and the module's registration function
// ith::generated::registerReflection_<module>. It computes ids (fnv1a64 of the names) and
// copies names and attributes; sizes, offsets and field types are left to the compiler through
// sizeof, offsetof and decltype, so a scanner mistake becomes a compile error rather than wrong
// data. The output is deterministic: the same headers always give the same text.

#include "ith/foundation/Expected.h"

#include "ith_reflect/Lexer.h"
#include "ith_reflect/Parser.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ith::reflect {

// One header of the module.
struct SourceHeader {
    std::string path;        // as reported in diagnostics
    std::string includePath; // as written in the generated #include, e.g. "ith/ecs/World.h"
    std::string text;        // the header's contents
};

// True for a valid module name: one or more lower-case letters, digits and '_'.
bool isModuleName(std::string_view name);

struct GenerateOptions {
    std::string module; // the module name; lower-case letters, digits and '_'
    ModuleKind kind = ModuleKind::Runtime;
};

// Generates the .reflect.cpp text for the module made of `headers`. Fails with every problem
// found: unsupported syntax, unknown or misplaced attributes, and two types with the same name.
Expected<std::string, std::vector<Diagnostic>> generate(const GenerateOptions& options,
                                                        std::span<const SourceHeader> headers);

} // namespace ith::reflect
