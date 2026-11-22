#pragma once

// Test helpers for property paths and patches (ADR-0033): every path a value reaches, found by
// walking the value through its records alone, and a registry of every reflected test type.

#include "ith/reflection/TypeInfo.h"
#include "ith/reflection/TypeRegistry.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

ITH_DECLARE_REFLECTION_MODULE(test_reflected)
ITH_DECLARE_REFLECTION_MODULE(test_archive)
ITH_DECLARE_REFLECTION_MODULE(test_archive_editor)

namespace ith::test {

// A registry with the leaf and math types and every reflected test type.
inline TypeRegistry makeTestRegistry() {
    TypeRegistry registry;
    generated::registerReflection_test_reflected(registry);
    generated::registerReflection_test_archive(registry);
    generated::registerReflection_test_archive_editor(registry);
    return registry;
}

// One path a value reaches: its text, the type at its end and where that value is.
struct ReachedPath {
    std::string text;
    const TypeInfo* type = nullptr;
    const void* value = nullptr;
};

namespace detail {

inline std::string fieldText(const std::string& prefix, std::string_view name) {
    return prefix.empty() ? std::string(name) : prefix + "." + std::string(name);
}

inline void collectPaths(const TypeInfo& type, const void* value, const std::string& text, bool record,
                         std::vector<ReachedPath>& paths) {
    if (record) {
        paths.push_back(ReachedPath{.text = text, .type = &type, .value = value});
    }
    switch (type.kind) {
        case TypeKind::Struct:
            for (const FieldInfo& field : type.fields) {
                collectPaths(field.type(), static_cast<const std::byte*>(value) + field.offset,
                             fieldText(text, field.name), true, paths);
            }
            break;
        case TypeKind::FixedArray:
        case TypeKind::DynamicArray:
            for (std::size_t index = 0; index < type.container->size(value); ++index) {
                collectPaths(type.element(), type.container->constAt(value, index),
                             text + "[" + std::to_string(index) + "]", true, paths);
            }
            break;
        case TypeKind::Optional:
            // Transparent: the value of an optional has the optional's own path.
            if (type.container->size(value) == 1) {
                collectPaths(type.element(), type.container->constAt(value, 0), text, false, paths);
            }
            break;
        default:
            break;
    }
}

} // namespace detail

// Every path the value of `type` at `value` reaches, the whole value ("") first, in field and
// element order. An engaged optional and its value share one path, reported as the optional.
inline std::vector<ReachedPath> reachablePaths(const TypeInfo& type, const void* value) {
    std::vector<ReachedPath> paths;
    detail::collectPaths(type, value, std::string(), true, paths);
    return paths;
}

} // namespace ith::test
