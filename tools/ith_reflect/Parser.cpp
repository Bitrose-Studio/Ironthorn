#include "ith_reflect/Parser.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>

namespace ith::reflect {
namespace {

enum class Target : std::uint8_t {
    Type,
    Field,
    Enum
};

enum class ValueKind : std::uint8_t {
    Flag,
    Number,
    Text,
    Name,
    Meta
};

struct AttributeSpec {
    std::string_view name;
    ValueKind kind;
    bool onType;
    bool onField;
    bool onEnum;
};

constexpr std::array<AttributeSpec, 14> kAttributes = {{
    {.name = "EditorOnly", .kind = ValueKind::Flag, .onType = true, .onField = true, .onEnum = true},
    {.name = "Transient", .kind = ValueKind::Flag, .onType = true, .onField = true, .onEnum = false},
    {.name = "ReadOnly", .kind = ValueKind::Flag, .onType = false, .onField = true, .onEnum = false},
    {.name = "Replicated", .kind = ValueKind::Flag, .onType = false, .onField = true, .onEnum = false},
    {.name = "SaveGame", .kind = ValueKind::Flag, .onType = false, .onField = true, .onEnum = false},
    {.name = "Flags", .kind = ValueKind::Flag, .onType = false, .onField = false, .onEnum = true},
    {.name = "Min", .kind = ValueKind::Number, .onType = false, .onField = true, .onEnum = false},
    {.name = "Max", .kind = ValueKind::Number, .onType = false, .onField = true, .onEnum = false},
    {.name = "Unit", .kind = ValueKind::Text, .onType = false, .onField = true, .onEnum = false},
    {.name = "Category", .kind = ValueKind::Text, .onType = true, .onField = true, .onEnum = true},
    {.name = "DisplayName", .kind = ValueKind::Text, .onType = true, .onField = true, .onEnum = true},
    {.name = "Tooltip", .kind = ValueKind::Text, .onType = true, .onField = true, .onEnum = true},
    {.name = "FormerName", .kind = ValueKind::Name, .onType = true, .onField = true, .onEnum = true},
    {.name = "Meta", .kind = ValueKind::Meta, .onType = true, .onField = true, .onEnum = true},
}};

constexpr std::array<std::string_view, 5> kRoles = {"Struct", "Component", "Resource", "Event", "Asset"};

// Keywords that start declarations ITH_FIELD can't annotate.
constexpr std::array<std::string_view, 16> kNonFieldKeywords = {
    "static",  "constexpr", "consteval", "constinit", "const", "using", "typedef", "friend",
    "virtual", "template",  "struct",    "class",     "enum",  "union", "inline",  "operator",
};

std::string_view targetName(Target target) {
    switch (target) {
        case Target::Type:
            return "a type";
        case Target::Field:
            return "a field";
        case Target::Enum:
            return "an enum";
    }
    return "";
}

bool isIdentifier(std::string_view text) {
    if (text.empty() || (text[0] >= '0' && text[0] <= '9')) {
        return false;
    }
    return std::ranges::all_of(text, [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_';
    });
}

// "name" or "a::b::name".
bool isQualifiedName(std::string_view text) {
    std::size_t begin = 0;
    while (true) {
        const std::size_t separator = text.find("::", begin);
        if (!isIdentifier(text.substr(begin, separator - begin))) {
            return false;
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        begin = separator + 2;
    }
}

bool isAnnotation(const Token& token) {
    return token.kind == TokenKind::Identifier &&
           (token.is("ITH_TYPE") || token.is("ITH_FIELD") || token.is("ITH_ENUM"));
}

struct Scope {
    enum class Kind : std::uint8_t {
        Namespace,
        Class,
        Other
    };

    Kind kind = Kind::Other;
    std::vector<std::string> names; // Namespace: its components; empty for an anonymous namespace
    std::string name;               // Class: its name; empty for an anonymous class
    bool atNamespaceScope = false;  // Class: declared directly in a namespace
    bool templated = false;         // Class: a template or a template specialization
    bool hasBases = false;          // Class: has a base clause
    bool qualified = false;         // Class: defined with a qualified name (struct ns::X {)
    bool isUnion = false;
    int typeIndex = -1; // Class: index of its ParsedType when annotated with ITH_TYPE
};

class Parser {
public:
    Parser(std::string_view path, const std::vector<Token>& tokens, ModuleKind kind)
        : m_path(path), m_tokens(tokens), m_kind(kind) {}

    Expected<std::vector<ParsedType>, std::vector<Diagnostic>> run() {
        while (m_position < m_tokens.size()) {
            if (!step()) {
                break;
            }
        }
        if (m_diagnostics.empty() && !m_scopes.empty()) {
            error(m_tokens.empty() ? 1 : m_tokens.back().line, "a '{' is not closed by the end of the file");
        }
        if (!m_diagnostics.empty()) {
            return makeUnexpected(std::move(m_diagnostics));
        }
        return std::move(m_types);
    }

private:
    const Token& token(std::size_t index) const { return m_tokens[index]; }

    bool isPunctuation(std::size_t index, std::string_view text) const {
        return index < m_tokens.size() && m_tokens[index].kind == TokenKind::Punctuation && m_tokens[index].is(text);
    }

    bool isWord(std::size_t index, std::string_view text) const {
        return index < m_tokens.size() && m_tokens[index].kind == TokenKind::Identifier && m_tokens[index].is(text);
    }

    void error(int line, std::string message) {
        m_diagnostics.push_back(Diagnostic{.file = std::string(m_path), .line = line, .message = std::move(message)});
    }

    // The index after the group that opens at `index` with '(', '[', '{' or '<'. Only the
    // group's own bracket kind is counted. A group that is never closed is reported, and the
    // result is the token count.
    std::size_t skipGroup(std::size_t index) {
        const int line = token(index).line;
        const std::string_view open = token(index).text;
        std::string_view close = ">";
        if (open == "(") {
            close = ")";
        } else if (open == "[") {
            close = "]";
        } else if (open == "{") {
            close = "}";
        }
        int depth = 0;
        for (; index < m_tokens.size(); ++index) {
            if (isPunctuation(index, open)) {
                ++depth;
            } else if (isPunctuation(index, close) && --depth == 0) {
                return index + 1;
            }
        }
        error(line, "'" + std::string(open) + "' is not closed");
        return m_tokens.size();
    }

    bool atNamespaceScope() const {
        return std::ranges::all_of(m_scopes, [](const Scope& scope) { return scope.kind == Scope::Kind::Namespace; });
    }

    bool inAnonymousNamespace() const {
        return std::ranges::any_of(
            m_scopes, [](const Scope& scope) { return scope.kind == Scope::Kind::Namespace && scope.names.empty(); });
    }

    std::string qualify(const std::string& name) const {
        std::string result;
        for (const Scope& scope : m_scopes) {
            for (const std::string& part : scope.names) {
                result += part + "::";
            }
        }
        return result + name;
    }

    // Handles the token at m_position. Returns false when parsing can't continue.
    bool step() {
        const Token& current = token(m_position);
        if (current.kind == TokenKind::Identifier) {
            if (current.is("namespace")) {
                parseNamespace();
            } else if (current.is("template")) {
                ++m_position;
                if (isPunctuation(m_position, "<")) {
                    m_position = skipGroup(m_position);
                }
                m_pendingTemplate = true;
            } else if (current.is("enum")) {
                parseEnum();
            } else if (current.is("struct") || current.is("class") || current.is("union")) {
                parseClassHead();
            } else if (current.is("ITH_TYPE")) {
                parseTypeAnnotation();
            } else if (current.is("ITH_FIELD")) {
                parseFieldAnnotation();
            } else if (current.is("ITH_ENUM")) {
                parseEnumAnnotation();
            } else {
                ++m_position;
            }
            return true;
        }
        if (current.kind == TokenKind::Punctuation) {
            if (current.is("(")) {
                m_position = skipGroup(m_position);
                return true;
            }
            if (current.is("{")) {
                m_scopes.push_back(Scope{.kind = Scope::Kind::Other});
                m_pendingTemplate = false;
            } else if (current.is("}")) {
                if (m_scopes.empty()) {
                    error(current.line, "'}' without a matching '{'");
                    return false;
                }
                m_scopes.pop_back();
            } else if (current.is(";")) {
                m_pendingTemplate = false;
            }
        }
        ++m_position;
        return true;
    }

    void parseNamespace() {
        ++m_position;
        Scope scope{.kind = Scope::Kind::Namespace};
        while (m_position < m_tokens.size()) {
            const Token& current = token(m_position);
            if (isPunctuation(m_position, "{")) {
                ++m_position;
                m_scopes.push_back(std::move(scope));
                return;
            }
            if (isPunctuation(m_position, ";") || isPunctuation(m_position, "=")) {
                return; // a namespace alias; the main loop skips the rest
            }
            if (isPunctuation(m_position, "[")) {
                m_position = skipGroup(m_position);
                continue;
            }
            if (current.kind == TokenKind::Identifier && !current.is("inline")) {
                scope.names.emplace_back(current.text);
            }
            ++m_position;
        }
    }

    void parseClassHead() {
        const bool isUnion = token(m_position).is("union");
        ++m_position;
        std::string name;
        bool qualified = false;
        bool hasBases = false;
        bool hasTemplateArguments = false;
        while (m_position < m_tokens.size()) {
            const Token& current = token(m_position);
            if (isPunctuation(m_position, "(") || isPunctuation(m_position, "[")) {
                m_position = skipGroup(m_position);
                continue;
            }
            if (isPunctuation(m_position, "<")) {
                hasTemplateArguments = !hasBases || hasTemplateArguments;
                m_position = skipGroup(m_position);
                continue;
            }
            if (isPunctuation(m_position, "{") || isPunctuation(m_position, ";") || isPunctuation(m_position, "}")) {
                break;
            }
            if (isPunctuation(m_position, ":")) {
                hasBases = true;
            } else if (!hasBases && current.kind == TokenKind::Identifier && !current.is("final")) {
                name = current.text;
                qualified = m_position > 0 && isPunctuation(m_position - 1, "::");
            }
            ++m_position;
        }
        if (!isPunctuation(m_position, "{")) {
            m_pendingTemplate = false; // a declaration, not a definition; the main loop goes on
            return;
        }
        m_scopes.push_back(Scope{.kind = Scope::Kind::Class,
                                 .name = std::move(name),
                                 .atNamespaceScope = atNamespaceScope(),
                                 .templated = m_pendingTemplate || hasTemplateArguments,
                                 .hasBases = hasBases,
                                 .qualified = qualified,
                                 .isUnion = isUnion});
        m_pendingTemplate = false;
        ++m_position;
    }

    // Parses the parenthesized attributes at m_position, which must be '('. Moves past them.
    // Returns false (after reporting) on any problem.
    bool parseAttributes(Target target, int line, ParsedAttributes& attributes) {
        if (!isPunctuation(m_position, "(")) {
            error(line, "an annotation needs parentheses, e.g. ITH_FIELD()");
            return false;
        }
        const std::size_t end = skipGroup(m_position);
        if (!isPunctuation(end - 1, ")")) {
            m_position = m_tokens.size(); // skipGroup reported it
            return false;
        }
        const std::size_t first = m_position + 1;
        const std::size_t last = end - 1; // the ')'
        m_position = end;

        std::vector<std::pair<std::size_t, std::size_t>> items;
        std::size_t begin = first;
        int depth = 0;
        for (std::size_t index = first; index < last; ++index) {
            if (isPunctuation(index, "(")) {
                ++depth;
            } else if (isPunctuation(index, ")")) {
                --depth;
            } else if (depth == 0 && isPunctuation(index, ",")) {
                items.emplace_back(begin, index);
                begin = index + 1;
            }
        }
        if (begin < last || !items.empty()) {
            items.emplace_back(begin, last);
        }

        bool ok = true;
        std::vector<std::string_view> seen;
        for (std::size_t item = 0; item < items.size(); ++item) {
            ok = parseAttribute(target, line, item == 0, items[item].first, items[item].second, seen, attributes) && ok;
        }
        if (target == Target::Type && attributes.role.empty() && ok) {
            error(line, "ITH_TYPE needs a role first: Struct, Component, Resource, Event or Asset");
            ok = false;
        }
        if (target == Target::Enum && attributes.subject.empty() && ok) {
            error(line, "ITH_ENUM needs the enum's name first: ITH_ENUM(Name, ...)");
            ok = false;
        }
        return ok;
    }

    bool parseAttribute(Target target, int line, bool isFirst, std::size_t begin, std::size_t end,
                        std::vector<std::string_view>& seen, ParsedAttributes& attributes) {
        if (begin == end) {
            error(line, "empty attribute in an annotation");
            return false;
        }
        const Token& nameToken = token(begin);
        const std::string_view name = nameToken.text;
        const std::size_t length = end - begin;
        if (target == Target::Enum && isFirst) {
            if (length != 1 || nameToken.kind != TokenKind::Identifier) {
                error(line, "ITH_ENUM needs the enum's name first: ITH_ENUM(Name, ...)");
                return false;
            }
            attributes.subject = name;
            return true;
        }
        if (nameToken.kind != TokenKind::Identifier) {
            error(line, "expected an attribute name, found '" + std::string(name) + "'");
            return false;
        }

        if (std::ranges::find(kRoles, name) != kRoles.end()) {
            if (target != Target::Type || !isFirst || length != 1) {
                error(line, "'" + std::string(name) + "' is a role; it is only allowed first in ITH_TYPE");
                return false;
            }
            attributes.role = name;
            return true;
        }
        if (target == Target::Type && isFirst) {
            error(line, "ITH_TYPE needs a role first: Struct, Component, Resource, Event or Asset");
            return false;
        }

        const auto spec = std::ranges::find(kAttributes, name, &AttributeSpec::name);
        if (spec == kAttributes.end()) {
            error(line, "unknown attribute '" + std::string(name) + "'");
            return false;
        }
        bool allowed = spec->onEnum;
        if (target == Target::Type) {
            allowed = spec->onType;
        } else if (target == Target::Field) {
            allowed = spec->onField;
        }
        if (!allowed) {
            error(line, "'" + std::string(name) + "' can't be used on " + std::string(targetName(target)));
            return false;
        }
        if (std::ranges::find(seen, name) != seen.end()) {
            error(line, "duplicate attribute '" + std::string(name) + "'");
            return false;
        }
        seen.push_back(name);

        const bool hasValue = length >= 3 && isPunctuation(begin + 1, "=");
        switch (spec->kind) {
            case ValueKind::Flag:
                if (length != 1) {
                    error(line, "'" + std::string(name) + "' is a flag and takes no value");
                    return false;
                }
                if (name == "EditorOnly" && m_kind != ModuleKind::Editor) {
                    error(line, "EditorOnly is only allowed in editor modules (ADR-0017)");
                    return false;
                }
                attributes.flags.emplace_back(name);
                return true;
            case ValueKind::Number:
                return parseNumber(line, name, hasValue, begin + 2, end,
                                   name == "Min" ? attributes.min : attributes.max);
            case ValueKind::Text: {
                if (!hasValue || length != 3 || token(begin + 2).kind != TokenKind::String ||
                    !token(begin + 2).text.starts_with('"')) {
                    error(line, "'" + std::string(name) + "' needs a plain string literal, e.g. " + std::string(name) +
                                    " = \"...\"");
                    return false;
                }
                std::string* value = &attributes.displayName;
                if (name == "Unit") {
                    value = &attributes.unit;
                } else if (name == "Category") {
                    value = &attributes.category;
                } else if (name == "Tooltip") {
                    value = &attributes.tooltip;
                }
                *value = token(begin + 2).text;
                return true;
            }
            case ValueKind::Name:
                return parseFormerName(target, line, hasValue && length == 3 ? begin + 2 : end, end, attributes);
            case ValueKind::Meta:
                return parseMeta(line, begin + 1, end, attributes);
        }
        return false;
    }

    bool parseNumber(int line, std::string_view name, bool hasValue, std::size_t begin, std::size_t end,
                     std::string& value) {
        const bool signedValue =
            hasValue && end - begin == 2 && (isPunctuation(begin, "-") || isPunctuation(begin, "+"));
        const std::size_t numberIndex = signedValue ? begin + 1 : begin;
        if (!hasValue || end - numberIndex != 1 || token(numberIndex).kind != TokenKind::Number) {
            error(line, "'" + std::string(name) + "' needs a number, e.g. " + std::string(name) + " = 0");
            return false;
        }
        value = (signedValue ? std::string(token(begin).text) : std::string()) + std::string(token(numberIndex).text);
        return true;
    }

    bool parseFormerName(Target target, int line, std::size_t index, std::size_t end, ParsedAttributes& attributes) {
        const bool qualifiedAllowed = target != Target::Field;
        if (index < end && token(index).kind == TokenKind::String && token(index).text.starts_with('"')) {
            const std::string_view text = token(index).text;
            const std::string_view value = text.substr(1, text.size() - 2);
            if (qualifiedAllowed ? isQualifiedName(value) : isIdentifier(value)) {
                attributes.formerName = value;
                return true;
            }
        }
        error(line, qualifiedAllowed ? "'FormerName' needs the previous qualified name, e.g. FormerName = \"game::Hp\""
                                     : "'FormerName' needs the previous field name, e.g. FormerName = \"hp\"");
        return false;
    }

    bool parseMeta(int line, std::size_t open, std::size_t end, ParsedAttributes& attributes) {
        const char* usage = R"('Meta' takes key-value strings, e.g. Meta(Widget = "Slider", Step = "0.5"))";
        if (open >= end || !isPunctuation(open, "(") || !isPunctuation(end - 1, ")")) {
            error(line, usage);
            return false;
        }
        std::size_t index = open + 1;
        const std::size_t close = end - 1;
        while (index < close) {
            if (index + 3 > close || token(index).kind != TokenKind::Identifier || !isPunctuation(index + 1, "=") ||
                token(index + 2).kind != TokenKind::String || !token(index + 2).text.starts_with('"') ||
                (index + 3 < close && !isPunctuation(index + 3, ","))) {
                error(line, usage);
                return false;
            }
            const std::string key(token(index).text);
            if (std::ranges::find(attributes.meta, key, &std::pair<std::string, std::string>::first) !=
                attributes.meta.end()) {
                error(line, "duplicate Meta key '" + key + "'");
                return false;
            }
            attributes.meta.emplace_back(key, token(index + 2).text);
            index += 4;
        }
        return true;
    }

    void parseTypeAnnotation() {
        const Token& annotation = token(m_position);
        ++m_position;
        ParsedAttributes attributes;
        bool ok = parseAttributes(Target::Type, annotation.line, attributes);
        if (isPunctuation(m_position, ";")) {
            error(annotation.line, "ITH_TYPE is written without a semicolon");
            ok = false;
        }
        if (m_scopes.empty() || m_scopes.back().kind != Scope::Kind::Class) {
            error(annotation.line, "ITH_TYPE must be in the body of a struct or class at namespace scope");
            return;
        }
        Scope& scope = m_scopes.back();
        const char* refusal = nullptr;
        if (annotation.conditionalDepth > 0) {
            refusal = "annotations inside #if blocks are not supported";
        } else if (scope.isUnion) {
            refusal = "unions can't be reflected";
        } else if (!scope.atNamespaceScope) {
            refusal = "nested types can't be reflected; declare the type at namespace scope";
        } else if (scope.name.empty()) {
            refusal = "anonymous types can't be reflected";
        } else if (inAnonymousNamespace()) {
            refusal = "types in an anonymous namespace can't be reflected";
        } else if (scope.templated) {
            refusal = "templates can't be reflected";
        } else if (scope.qualified) {
            refusal = "define a reflected type inside its namespace, not with a qualified name";
        } else if (scope.hasBases) {
            refusal = "reflected types can't have base classes; use composition (ADR-0018)";
        } else if (scope.typeIndex >= 0) {
            refusal = "a type has only one ITH_TYPE";
        }
        if (refusal != nullptr) {
            error(annotation.line, refusal);
            return;
        }
        if (!ok) {
            return;
        }
        scope.typeIndex = static_cast<int>(m_types.size());
        m_types.push_back(ParsedType{.isEnum = false,
                                     .qualifiedName = qualify(scope.name),
                                     .file = std::string(m_path),
                                     .line = annotation.line,
                                     .attributes = std::move(attributes)});
    }

    void parseFieldAnnotation() {
        const Token& annotation = token(m_position);
        ++m_position;
        ParsedAttributes attributes;
        const bool ok = parseAttributes(Target::Field, annotation.line, attributes);

        if (m_scopes.empty() || m_scopes.back().kind != Scope::Kind::Class || m_scopes.back().typeIndex < 0) {
            error(annotation.line,
                  "ITH_FIELD must be in the body of a type annotated with ITH_TYPE, after the ITH_TYPE line");
            return;
        }
        if (annotation.conditionalDepth > 0) {
            error(annotation.line, "annotations inside #if blocks are not supported");
            return;
        }

        // The declaration: everything up to the ';' at this nesting level.
        const std::size_t begin = m_position;
        std::size_t end = begin;
        int depth = 0;
        for (; end < m_tokens.size(); ++end) {
            if (depth == 0 && (isPunctuation(end, ";") || isPunctuation(end, "}") || isAnnotation(token(end)))) {
                break;
            }
            if (isPunctuation(end, "(") || isPunctuation(end, "[") || isPunctuation(end, "{")) {
                ++depth;
            } else if (isPunctuation(end, ")") || isPunctuation(end, "]") || isPunctuation(end, "}")) {
                --depth;
            }
        }
        if (begin == end || end >= m_tokens.size() || !isPunctuation(end, ";")) {
            error(annotation.line, "ITH_FIELD must be followed by a data member declaration");
            m_position = end;
            return;
        }
        m_position = end + 1;

        const Token& first = token(begin);
        if (std::ranges::find(kNonFieldKeywords, first.text) != kNonFieldKeywords.end()) {
            error(first.line, "ITH_FIELD can't annotate a '" + std::string(first.text) +
                                  "' declaration; only non-static, non-const data members are reflected");
            return;
        }
        if (!checkFieldTokens(begin, end, annotation.conditionalDepth)) {
            return;
        }

        // The declarator ends at the first '=', '{', '[', ':', ',' or '(' outside brackets.
        std::size_t stop = end;
        int angle = 0;
        for (std::size_t index = begin; index < end; ++index) {
            if (isPunctuation(index, "<")) {
                ++angle;
            } else if (isPunctuation(index, ">") && angle > 0) {
                --angle;
            } else if (angle == 0 && token(index).kind == TokenKind::Punctuation &&
                       (token(index).is("=") || token(index).is("{") || token(index).is("[") || token(index).is(":") ||
                        token(index).is(",") || token(index).is("("))) {
                stop = index;
                break;
            }
        }
        const int line = token(begin).line;
        if (stop < end) {
            const std::string_view at = token(stop).text;
            if (at == "(") {
                error(line, "ITH_FIELD must annotate a data member, not a function");
                return;
            }
            if (at == "[") {
                error(line, "C arrays can't be reflected; use std::array");
                return;
            }
            if (at == ":") {
                error(line, "bit-fields can't be reflected");
                return;
            }
        }
        if (stop - begin < 2 || token(stop - 1).kind != TokenKind::Identifier) {
            error(line, "expected a type and a field name after ITH_FIELD");
            return;
        }
        if (isPunctuation(stop - 2, "*") || isPunctuation(stop - 2, "&")) {
            error(line, "pointer and reference fields can't be reflected");
            return;
        }
        if (hasSecondDeclarator(stop, end)) {
            error(line, "declare one field per ITH_FIELD, one declarator per line");
            return;
        }
        if (!ok) {
            return;
        }
        m_types[static_cast<std::size_t>(m_scopes.back().typeIndex)].fields.push_back(
            ParsedField{.name = std::string(token(stop - 1).text), .line = line, .attributes = std::move(attributes)});
    }

    // Refuses #if blocks, attributes and alignas in the field declaration [begin, end).
    bool checkFieldTokens(std::size_t begin, std::size_t end, int conditionalDepth) {
        for (std::size_t index = begin; index < end; ++index) {
            if (token(index).conditionalDepth != conditionalDepth) {
                error(token(index).line, "#if blocks can't split a reflected field");
                return false;
            }
            if (isPunctuation(index, "[") && isPunctuation(index + 1, "[")) {
                error(token(index).line, "attributes such as [[no_unique_address]] can't be used on reflected fields");
                return false;
            }
            if (isWord(index, "alignas")) {
                error(token(index).line, "alignas can't be used on reflected fields");
                return false;
            }
        }
        return true;
    }

    // True when a ',' outside brackets follows the declarator ending at `stop`.
    bool hasSecondDeclarator(std::size_t stop, std::size_t end) const {
        int depth = 0;
        int angle = 0;
        for (std::size_t index = stop; index < end; ++index) {
            const Token& current = token(index);
            if (current.kind != TokenKind::Punctuation) {
                continue;
            }
            if (current.is("(") || current.is("[") || current.is("{")) {
                ++depth;
            } else if (current.is(")") || current.is("]") || current.is("}")) {
                --depth;
            } else if (current.is("<") && index > 0 && token(index - 1).kind == TokenKind::Identifier) {
                ++angle; // a template argument list, as far as a scanner can tell
            } else if (current.is(">") && angle > 0) {
                --angle;
            } else if (current.is(",") && depth == 0 && angle == 0) {
                return true;
            }
        }
        return false;
    }

    // An enum declaration, kept until the next token so that an ITH_ENUM after it can use it.
    struct EnumDeclaration {
        std::string name;
        int line = 0;
        std::size_t end = 0;       // index of the token after the enum's ';'
        std::size_t bodyBegin = 0; // index of the first token inside the braces
        std::size_t bodyEnd = 0;   // index of the closing brace
        bool hasBody = false;
        bool scoped = false;
        bool qualified = false;
        bool boolUnderlying = false;
        bool atNamespaceScope = false;
        bool inAnonymousNamespace = false;
        bool templated = false;
    };

    void parseEnum() {
        EnumDeclaration declaration{.line = token(m_position).line,
                                    .atNamespaceScope = atNamespaceScope(),
                                    .inAnonymousNamespace = inAnonymousNamespace(),
                                    .templated = m_pendingTemplate};
        m_pendingTemplate = false;
        m_lastEnum.reset();
        ++m_position;
        declaration.scoped = isWord(m_position, "class") || isWord(m_position, "struct");
        if (declaration.scoped) {
            ++m_position;
        }
        bool colon = false;
        std::string underlying;
        while (m_position < m_tokens.size()) {
            const Token& current = token(m_position);
            if (current.kind == TokenKind::Identifier && current.is("ITH_ENUM")) {
                error(current.line, "ITH_ENUM goes after the enum, without a semicolon: ITH_ENUM(Name, ...)");
                ++m_position;
                continue;
            }
            if (isPunctuation(m_position, "[") || isPunctuation(m_position, "(")) {
                m_position = skipGroup(m_position);
                continue;
            }
            if (isPunctuation(m_position, "{") || isPunctuation(m_position, ";") || isPunctuation(m_position, "}")) {
                break;
            }
            if (isPunctuation(m_position, ":")) {
                colon = true;
            } else if (colon) {
                underlying += current.text;
            } else if (current.kind == TokenKind::Identifier) {
                declaration.name = current.text;
                declaration.qualified = isPunctuation(m_position - 1, "::");
            }
            ++m_position;
        }
        declaration.boolUnderlying = underlying == "bool";
        if (isPunctuation(m_position, "{")) {
            declaration.hasBody = true;
            declaration.bodyBegin = m_position + 1;
            m_position = skipGroup(m_position);
            declaration.bodyEnd = m_position - 1;
        }
        if (isPunctuation(m_position, ";")) {
            ++m_position;
            declaration.end = m_position;
            m_lastEnum = std::move(declaration);
        }
    }

    void parseEnumAnnotation() {
        const Token& annotation = token(m_position);
        const std::size_t annotationIndex = m_position;
        ++m_position;
        ParsedAttributes attributes;
        bool ok = parseAttributes(Target::Enum, annotation.line, attributes);
        if (isPunctuation(m_position, ";")) {
            error(annotation.line, "ITH_ENUM is written without a semicolon");
            ok = false;
        }
        if (!m_lastEnum || m_lastEnum->end != annotationIndex) {
            error(annotation.line, "ITH_ENUM must directly follow the enum class it names");
            return;
        }
        const EnumDeclaration declaration = *m_lastEnum;
        if (!attributes.subject.empty() && attributes.subject != declaration.name) {
            error(annotation.line,
                  "ITH_ENUM names '" + attributes.subject + "', but the enum before it is '" + declaration.name + "'");
            return;
        }

        const char* refusal = nullptr;
        if (annotation.conditionalDepth > 0) {
            refusal = "annotations inside #if blocks are not supported";
        } else if (!declaration.scoped) {
            refusal = "only enum class can be reflected; ITH_ENUM can't annotate a plain enum";
        } else if (!declaration.atNamespaceScope) {
            refusal = "nested enums can't be reflected; declare the enum at namespace scope";
        } else if (declaration.inAnonymousNamespace) {
            refusal = "enums in an anonymous namespace can't be reflected";
        } else if (declaration.templated) {
            refusal = "templates can't be reflected";
        } else if (declaration.qualified) {
            refusal = "define a reflected enum inside its namespace, not with a qualified name";
        } else if (declaration.boolUnderlying) {
            refusal = "enums with a bool underlying type can't be reflected";
        } else if (!declaration.hasBody) {
            refusal = "a reflected enum needs its list of enumerators";
        }
        if (refusal != nullptr) {
            error(annotation.line, refusal);
            return;
        }
        if (!ok) {
            return;
        }
        ParsedType type{.isEnum = true,
                        .qualifiedName = qualify(declaration.name),
                        .file = std::string(m_path),
                        .line = annotation.line,
                        .attributes = std::move(attributes)};
        if (parseEnumerators(declaration.bodyBegin, declaration.bodyEnd, annotation.conditionalDepth,
                             type.enumerators)) {
            m_types.push_back(std::move(type));
        }
    }

    // Reads the enumerator names in [begin, end), the body of a reflected enum without its braces.
    bool parseEnumerators(std::size_t begin, std::size_t end, int conditionalDepth, std::vector<std::string>& names) {
        bool expectName = true;
        for (std::size_t index = begin; index < end; ++index) {
            const Token& current = token(index);
            if (current.conditionalDepth != conditionalDepth) {
                error(current.line, "#if blocks are not supported inside a reflected enum");
                return false;
            }
            if (expectName) {
                if (current.kind != TokenKind::Identifier) {
                    error(current.line, "expected an enumerator name, found '" + std::string(current.text) + "'");
                    return false;
                }
                names.emplace_back(current.text);
                expectName = false;
            } else if (isPunctuation(index, ",")) {
                expectName = true;
            } else if (isPunctuation(index, "(") || isPunctuation(index, "[") || isPunctuation(index, "{")) {
                index = skipGroup(index) - 1; // a value expression or an attribute
            }
            // Other tokens belong to an enumerator's value, which the compiler evaluates.
        }
        return true;
    }

    std::string_view m_path;
    const std::vector<Token>& m_tokens;
    ModuleKind m_kind;
    std::size_t m_position = 0;
    bool m_pendingTemplate = false;
    std::vector<Scope> m_scopes;
    std::vector<ParsedType> m_types;
    std::vector<Diagnostic> m_diagnostics;
    std::optional<EnumDeclaration> m_lastEnum;
};

} // namespace

Expected<ModuleKind> parseModuleKind(std::string_view text) {
    if (text == "runtime") {
        return ModuleKind::Runtime;
    }
    if (text == "client") {
        return ModuleKind::Client;
    }
    if (text == "tools") {
        return ModuleKind::Tools;
    }
    if (text == "editor") {
        return ModuleKind::Editor;
    }
    return makeError("reflect.bad_kind",
                     "unknown module kind '" + std::string(text) + "'; expected runtime, client, tools or editor");
}

Expected<std::vector<ParsedType>, std::vector<Diagnostic>> parseHeader(std::string_view path, std::string_view text,
                                                                       ModuleKind kind) {
    Expected<std::vector<Token>, Diagnostic> tokens = tokenize(path, text);
    if (!tokens) {
        return makeUnexpected(std::vector<Diagnostic>{std::move(tokens).error()});
    }
    return Parser(path, *tokens, kind).run();
}

} // namespace ith::reflect
