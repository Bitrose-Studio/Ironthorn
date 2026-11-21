#include "ith_reflect/Lexer.h"

#include <cstddef>
#include <string>
#include <utility>

namespace ith::reflect {
namespace {

bool isIdentifierStart(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '_';
}

bool isDigit(char character) {
    return character >= '0' && character <= '9';
}

bool isIdentifierPart(char character) {
    return isIdentifierStart(character) || isDigit(character);
}

// A UTF-8 byte order mark, which some editors put at the start of a file.
constexpr std::string_view kByteOrderMark = "\xEF\xBB\xBF";

bool isBlank(char character) {
    return character == ' ' || character == '\t' || character == '\r' || character == '\f' || character == '\v';
}

class Lexer {
public:
    Lexer(std::string_view path, std::string_view text) : m_path(path), m_text(text) {}

    Expected<std::vector<Token>, Diagnostic> run() {
        if (m_text.starts_with(kByteOrderMark)) {
            m_position = 3; // a UTF-8 byte order mark
        }
        bool atLineStart = true;
        while (m_position < m_text.size()) {
            const char character = m_text[m_position];
            if (character == '\n') {
                ++m_line;
                ++m_position;
                atLineStart = true;
            } else if (isBlank(character)) {
                ++m_position;
            } else if (character == '#' && atLineStart) {
                if (!directive()) {
                    return makeUnexpected(m_error);
                }
            } else if (startsWith("//")) {
                skipLineComment();
            } else if (startsWith("/*")) {
                if (!skipBlockComment()) {
                    return makeUnexpected(m_error);
                }
            } else {
                atLineStart = false;
                if (!token()) {
                    return makeUnexpected(m_error);
                }
            }
        }
        if (m_conditionalDepth != 0) {
            return makeUnexpected(
                Diagnostic{.file = std::string(m_path), .line = m_line, .message = "#if without a matching #endif"});
        }
        return std::move(m_tokens);
    }

private:
    bool startsWith(std::string_view prefix) const { return m_text.substr(m_position).starts_with(prefix); }

    bool fail(int line, std::string message) {
        m_error = Diagnostic{.file = std::string(m_path), .line = line, .message = std::move(message)};
        return false;
    }

    void push(TokenKind kind, std::size_t begin, int line) {
        m_tokens.push_back(Token{.kind = kind,
                                 .text = m_text.substr(begin, m_position - begin),
                                 .line = line,
                                 .conditionalDepth = m_conditionalDepth});
    }

    // The length of a backslash-newline at `index` (2, or 3 with a CR), or 0 when there is none.
    std::size_t lineContinuation(std::size_t index) const {
        const std::string_view rest = m_text.substr(index);
        if (rest.starts_with("\\\n")) {
            return 2;
        }
        return rest.starts_with("\\\r\n") ? 3 : 0;
    }

    void skipLineComment() {
        while (m_position < m_text.size() && m_text[m_position] != '\n') {
            ++m_position;
        }
    }

    bool skipBlockComment() {
        const int line = m_line;
        const std::size_t end = m_text.find("*/", m_position + 2);
        if (end == std::string_view::npos) {
            return fail(line, "unterminated /* comment");
        }
        for (std::size_t index = m_position; index < end; ++index) {
            m_line += m_text[index] == '\n' ? 1 : 0;
        }
        m_position = end + 2;
        return true;
    }

    // A preprocessor line, with its continuations. Only #if nesting is tracked.
    bool directive() {
        const int line = m_line;
        ++m_position; // '#'
        while (m_position < m_text.size() && isBlank(m_text[m_position])) {
            ++m_position;
        }
        const std::size_t nameBegin = m_position;
        while (m_position < m_text.size() && isIdentifierPart(m_text[m_position])) {
            ++m_position;
        }
        const std::string_view name = m_text.substr(nameBegin, m_position - nameBegin);
        while (m_position < m_text.size() && m_text[m_position] != '\n') {
            if (const std::size_t length = lineContinuation(m_position); length > 0) {
                ++m_line;
                m_position += length - 1;
            } else if (startsWith("/*")) {
                if (!skipBlockComment()) {
                    return false;
                }
                continue;
            }
            ++m_position;
        }
        if (name == "if" || name == "ifdef" || name == "ifndef") {
            ++m_conditionalDepth;
        } else if (name == "endif") {
            if (m_conditionalDepth == 0) {
                return fail(line, "#endif without a matching #if");
            }
            --m_conditionalDepth;
        }
        return true;
    }

    bool token() {
        const std::size_t begin = m_position;
        const int line = m_line;
        const char character = m_text[m_position];

        if (isIdentifierStart(character)) {
            while (m_position < m_text.size() && isIdentifierPart(m_text[m_position])) {
                ++m_position;
            }
            const std::string_view word = m_text.substr(begin, m_position - begin);
            const char next = m_position < m_text.size() ? m_text[m_position] : '\0';
            if (next == '"' && (word == "R" || word == "u8R" || word == "uR" || word == "UR" || word == "LR")) {
                return rawString(begin, line);
            }
            if ((next == '"' || next == '\'') && (word == "u8" || word == "u" || word == "U" || word == "L")) {
                return quoted(begin, line, next);
            }
            push(TokenKind::Identifier, begin, line);
            return true;
        }
        if (isDigit(character) ||
            (character == '.' && m_position + 1 < m_text.size() && isDigit(m_text[m_position + 1]))) {
            number();
            push(TokenKind::Number, begin, line);
            return true;
        }
        if (character == '"' || character == '\'') {
            return quoted(begin, line, character);
        }
        if (startsWith("::")) {
            m_position += 2;
        } else {
            ++m_position;
        }
        push(TokenKind::Punctuation, begin, line);
        return true;
    }

    // m_position is at the first character, a digit or '.'.
    void number() {
        ++m_position;
        while (m_position < m_text.size()) {
            const char character = m_text[m_position];
            const char previous = m_text[m_position - 1];
            const bool exponentSign = (character == '+' || character == '-') &&
                                      (previous == 'e' || previous == 'E' || previous == 'p' || previous == 'P');
            if (!isIdentifierPart(character) && character != '.' && character != '\'' && !exponentSign) {
                return;
            }
            ++m_position;
        }
    }

    // A string or character literal; m_position is at its prefix or opening quote.
    bool quoted(std::size_t begin, int line, char quote) {
        while (m_text[m_position] != quote) {
            ++m_position; // the prefix
        }
        ++m_position;
        while (m_position < m_text.size() && m_text[m_position] != quote) {
            if (m_text[m_position] == '\n') {
                break;
            }
            if (const std::size_t length = lineContinuation(m_position); length > 0) {
                ++m_line; // a backslash-newline inside the literal
                m_position += length;
            } else {
                m_position += m_text[m_position] == '\\' ? 2 : 1;
            }
        }
        if (m_position >= m_text.size() || m_text[m_position] != quote) {
            return fail(line, quote == '"' ? "unterminated string literal" : "unterminated character literal");
        }
        ++m_position;
        push(quote == '"' ? TokenKind::String : TokenKind::Character, begin, line);
        return true;
    }

    // A raw string literal R"delimiter(...)delimiter"; m_position is at the opening quote.
    bool rawString(std::size_t begin, int line) {
        const std::size_t open = m_text.find('(', m_position);
        if (open == std::string_view::npos) {
            return fail(line, "malformed raw string literal");
        }
        const std::string terminator = ")" + std::string(m_text.substr(m_position + 1, open - m_position - 1)) + "\"";
        const std::size_t end = m_text.find(terminator, open);
        if (end == std::string_view::npos) {
            return fail(line, "unterminated raw string literal");
        }
        for (std::size_t index = m_position; index < end; ++index) {
            m_line += m_text[index] == '\n' ? 1 : 0;
        }
        m_position = end + terminator.size();
        push(TokenKind::String, begin, line);
        return true;
    }

    std::string_view m_path;
    std::string_view m_text;
    std::size_t m_position = 0;
    int m_line = 1;
    int m_conditionalDepth = 0;
    std::vector<Token> m_tokens;
    Diagnostic m_error;
};

} // namespace

std::string formatDiagnostic(const Diagnostic& diagnostic) {
    return diagnostic.file + ":" + std::to_string(diagnostic.line) + ": error: " + diagnostic.message;
}

Expected<std::vector<Token>, Diagnostic> tokenize(std::string_view path, std::string_view text) {
    return Lexer(path, text).run();
}

} // namespace ith::reflect
