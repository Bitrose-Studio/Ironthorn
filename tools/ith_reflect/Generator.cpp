#include "ith_reflect/Generator.h"

#include "ith/foundation/Assert.h"
#include "ith/foundation/Hash.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>
#include <utility>

namespace ith::reflect {
namespace {

std::string idLiteral(std::string_view name) {
    std::array<char, 16> digits{};
    const auto result = std::to_chars(digits.data(), digits.data() + digits.size(), fnv1a64(name), 16);
    const std::string hex(digits.data(), result.ptr);
    return "0x" + std::string(16 - hex.size(), '0') + hex + "ull";
}

std::string flagsExpression(const std::vector<std::string>& flags) {
    if (flags.empty()) {
        return "::ith::ReflectionFlags::None";
    }
    std::string text;
    for (const std::string& flag : flags) {
        text += (text.empty() ? "" : " | ") + std::string("::ith::ReflectionFlags::") + flag;
    }
    return text;
}

// The meta array of an annotation, as a local constant named `name`; empty when it has none.
std::string metaArray(const ParsedAttributes& attributes, const std::string& name, std::string_view indent) {
    if (attributes.meta.empty()) {
        return "";
    }
    std::string text(indent);
    // Unused in Shipping, where ITH_REFLECTION_META drops it.
    text += "[[maybe_unused]] static constexpr ::ith::MetaEntry " + name + "[] = {\n";
    for (const auto& [key, value] : attributes.meta) {
        text += indent;
        text += "    {.key = ITH_REFLECTION_TEXT(\"" + key + "\"), .value = ITH_REFLECTION_TEXT(";
        text += value + ")},\n";
    }
    text += indent;
    text += "};\n";
    return text;
}

// A braced ith::Attributes initializer, in the order of the struct's members.
std::string attributesExpression(const ParsedAttributes& attributes, const std::string& metaName) {
    std::vector<std::string> parts;
    if (!attributes.category.empty()) {
        parts.push_back(".category = ITH_REFLECTION_TEXT(" + attributes.category + ")");
    }
    if (!attributes.displayName.empty()) {
        parts.push_back(".displayName = ITH_REFLECTION_TEXT(" + attributes.displayName + ")");
    }
    if (!attributes.tooltip.empty()) {
        parts.push_back(".tooltip = ITH_REFLECTION_TEXT(" + attributes.tooltip + ")");
    }
    if (!attributes.unit.empty()) {
        parts.push_back(".unit = " + attributes.unit);
    }
    if (!attributes.formerName.empty()) {
        parts.push_back(".formerName = \"" + attributes.formerName + "\"");
    }
    if (!attributes.min.empty()) {
        parts.push_back(".min = " + attributes.min);
    }
    if (!attributes.max.empty()) {
        parts.push_back(".max = " + attributes.max);
    }
    if (!attributes.meta.empty()) {
        parts.push_back(".meta = ITH_REFLECTION_META(" + metaName + ")");
    }
    std::string text = "{";
    for (std::size_t index = 0; index < parts.size(); ++index) {
        text += (index == 0 ? "" : ", ") + parts[index];
    }
    return text + "}";
}

std::string location(const ParsedType& type, const SourceHeader& header) {
    return header.includePath + ":" + std::to_string(type.line);
}

// Defines the function ITH_ENUM declared in the enum's namespace.
void writeEnum(std::string& out, const ParsedType& type, const SourceHeader& header) {
    const std::size_t separator = type.qualifiedName.rfind("::");
    const std::string scope = separator == std::string::npos ? "" : type.qualifiedName.substr(0, separator + 2);
    out += "// " + type.qualifiedName + ", from " + location(type, header) + "\n";
    out += "const ::ith::TypeInfo& " + scope + "ithEnumTypeInfo(::" + type.qualifiedName + "*) noexcept {\n";
    out += "    using Self = ::" + type.qualifiedName + ";\n";
    out += metaArray(type.attributes, "kMeta", "    ");
    std::string enumerators = "std::span<const ::ith::EnumeratorInfo>()";
    if (!type.enumerators.empty()) {
        out += "    static constexpr ::ith::EnumeratorInfo kEnumerators[] = {\n";
        for (const std::string& name : type.enumerators) {
            out += "        {.name = \"" + name + "\", .value = static_cast<std::int64_t>(Self::";
            out += name + ")},\n";
        }
        out += "    };\n";
        enumerators = "kEnumerators";
    }
    out += "    static constexpr ::ith::TypeInfo kType = ::ith::detail::makeEnumType<Self>(\n";
    out += "        " + idLiteral(type.qualifiedName) + ", \"" + type.qualifiedName + "\", " +
           flagsExpression(type.attributes.flags) + ",\n";
    out += "        " + attributesExpression(type.attributes, "kMeta") + ", " + enumerators + ");\n";
    out += "    return kType;\n";
    out += "}\n\n";
}

// FormerName collisions inside one type make loading ambiguous (ADR-0019 rule 5).
void checkFormerNames(const ParsedType& type, std::vector<Diagnostic>& diagnostics) {
    for (std::size_t index = 0; index < type.fields.size(); ++index) {
        const ParsedField& field = type.fields[index];
        const std::string& former = field.attributes.formerName;
        if (former.empty()) {
            continue;
        }
        for (std::size_t other = 0; other < type.fields.size(); ++other) {
            if (other == index) {
                continue;
            }
            const ParsedField& otherField = type.fields[other];
            std::string problem;
            if (otherField.name == former) {
                problem = "the FormerName '" + former + "' of '" + field.name + "' is the name of another field";
            } else if (other < index && otherField.attributes.formerName == former) {
                problem =
                    "'" + field.name + "' and '" + otherField.name + "' have the same FormerName '" + former + "'";
            }
            if (!problem.empty()) {
                diagnostics.push_back(Diagnostic{.file = type.file, .line = field.line, .message = problem});
                break;
            }
        }
    }
}

void writeStruct(std::string& out, const ParsedType& type, const SourceHeader& header) {
    out += "// " + type.qualifiedName + ", from " + location(type, header) + "\n";
    out += "const ::ith::TypeInfo& " + type.qualifiedName + "::ithTypeInfo() noexcept {\n";
    out += "    using Self = ::" + type.qualifiedName + ";\n";
    out += "    static_assert(!std::is_polymorphic_v<Self>, \"" + type.qualifiedName +
           ": reflected types can't have virtual functions\");\n";
    out += metaArray(type.attributes, "kMeta", "    ");
    for (std::size_t index = 0; index < type.fields.size(); ++index) {
        out += metaArray(type.fields[index].attributes, "kMeta" + std::to_string(index), "    ");
    }
    std::string fields = "std::span<const ::ith::FieldInfo>()";
    if (!type.fields.empty()) {
        out += "    static constexpr ::ith::FieldInfo kFields[] = {\n";
        for (std::size_t index = 0; index < type.fields.size(); ++index) {
            const ParsedField& field = type.fields[index];
            out += "        {\n";
            out += "            .id = " + idLiteral(field.name) + ",\n";
            out += "            .name = \"" + field.name + "\",\n";
            out += "            .offset = offsetof(Self, " + field.name + "),\n";
            out += "            .type = &::ith::typeOf<decltype(Self::" + field.name + ")>,\n";
            out += "            .flags = " + flagsExpression(field.attributes.flags) + ",\n";
            out +=
                "            .attributes = " + attributesExpression(field.attributes, "kMeta" + std::to_string(index)) +
                ",\n";
            out += "        },\n";
        }
        out += "    };\n";
        fields = "kFields";
    }
    out += "    static constexpr ::ith::TypeInfo kType = ::ith::detail::makeStructType<Self>(\n";
    out += "        " + idLiteral(type.qualifiedName) + ", \"" + type.qualifiedName +
           "\", ::ith::TypeRole::" + type.attributes.role + ", " + flagsExpression(type.attributes.flags) + ",\n";
    out += "        " + attributesExpression(type.attributes, "kMeta") + ", " + fields + ");\n";
    out += "    return kType;\n";
    out += "}\n\n";
}

} // namespace

bool isModuleName(std::string_view name) {
    return !name.empty() && std::ranges::all_of(name, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_';
    });
}

Expected<std::string, std::vector<Diagnostic>> generate(const GenerateOptions& options,
                                                        std::span<const SourceHeader> headers) {
    ITH_ASSERT_MSG(isModuleName(options.module), "generate: the module name must be lower-case letters, digits, '_'");

    std::vector<Diagnostic> diagnostics;
    // Each type with the index of its header, in declaration order.
    std::vector<std::pair<ParsedType, std::size_t>> types;
    for (std::size_t index = 0; index < headers.size(); ++index) {
        Expected<std::vector<ParsedType>, std::vector<Diagnostic>> parsed =
            parseHeader(headers[index].path, headers[index].text, options.kind);
        if (!parsed) {
            diagnostics.insert(diagnostics.end(), parsed.error().begin(), parsed.error().end());
            continue;
        }
        for (ParsedType& type : *parsed) {
            const auto previous = std::ranges::find_if(
                types, [&](const auto& entry) { return entry.first.qualifiedName == type.qualifiedName; });
            if (previous != types.end()) {
                diagnostics.push_back(Diagnostic{.file = type.file,
                                                 .line = type.line,
                                                 .message = "'" + type.qualifiedName + "' is already reflected at " +
                                                            previous->first.file + ":" +
                                                            std::to_string(previous->first.line)});
                continue;
            }
            checkFormerNames(type, diagnostics);
            types.emplace_back(std::move(type), index);
        }
    }
    if (!diagnostics.empty()) {
        return makeUnexpected(std::move(diagnostics));
    }

    std::string out;
    out += "// Generated by ith_reflect from the annotated headers of the module '" + options.module + "'.\n";
    out += "// Do not edit: the build rewrites this file whenever one of those headers changes.\n\n";
    out += "#include \"ith/reflection/TypeOf.h\"\n";
    out += "#include \"ith/reflection/TypeRegistry.h\"\n\n";
    std::vector<std::size_t> included;
    for (const auto& [type, header] : types) {
        if (std::ranges::find(included, header) == included.end()) {
            included.push_back(header);
            out += "#include \"" + headers[header].includePath + "\"\n";
        }
    }
    out += included.empty() ? "" : "\n";
    out +=
        "#include <cstddef>\n#include <cstdint>\n#include <span>\n#include <string_view>\n#include <type_traits>\n\n";
    // offsetof is supported for every reflected type (no virtual functions, no bases), but the
    // standard only guarantees it for standard-layout types, which a std::string member isn't
    // in every library (MSVC debug builds).
    out += "#if defined(__clang__)\n#pragma clang diagnostic ignored \"-Winvalid-offsetof\"\n"
           "#elif defined(__GNUC__)\n#pragma GCC diagnostic ignored \"-Winvalid-offsetof\"\n#endif\n\n";
    out += "ITH_DECLARE_REFLECTION_MODULE(" + options.module + ")\n\n";

    // Enum records first: struct records refer to them through typeOf.
    for (const auto& [type, header] : types) {
        if (type.isEnum) {
            writeEnum(out, type, headers[header]);
        }
    }
    for (const auto& [type, header] : types) {
        if (!type.isEnum) {
            writeStruct(out, type, headers[header]);
        }
    }

    out += "void ith::generated::registerReflection_" + options.module + "(::ith::TypeRegistry& registry) {\n";
    if (types.empty()) {
        out += "    registry.addAll({});\n";
    } else {
        out += "    const ::ith::TypeInfo* const types[] = {\n";
        for (const auto& [type, header] : types) {
            out += "        &::ith::typeOf<::" + type.qualifiedName + ">(),\n";
        }
        out += "    };\n";
        out += "    registry.addAll(types);\n";
    }
    out += "}\n";
    return out;
}

} // namespace ith::reflect
