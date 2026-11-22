#include "ith/file_system/Path.h"

#include <vector>

namespace ith::fs {
namespace {

constexpr bool isSlash(char character) noexcept {
    return character == '/' || character == '\\';
}

constexpr bool isAsciiLetter(char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

// The position of the first slash at or after `start`, or the size of `path`.
std::size_t nextSlash(std::string_view path, std::size_t start) noexcept {
    while (start < path.size() && !isSlash(path[start])) {
        ++start;
    }
    return start;
}

} // namespace

std::size_t rootLength(std::string_view path) noexcept {
    if (path.size() >= 2 && isSlash(path[0]) && isSlash(path[1]) && (path.size() == 2 || !isSlash(path[2]))) {
        const std::size_t serverEnd = nextSlash(path, 2);
        return serverEnd == path.size() ? serverEnd : nextSlash(path, serverEnd + 1);
    }
    if (!path.empty() && isSlash(path[0])) {
        return 1;
    }
    if (path.size() >= 3 && isAsciiLetter(path[0]) && path[1] == ':' && isSlash(path[2])) {
        return 3;
    }
    return 0;
}

bool isAbsolutePath(std::string_view path) noexcept {
    return rootLength(path) != 0;
}

std::string normalizePath(std::string_view path) {
    if (path.empty()) {
        return {};
    }
    const std::size_t rootSize = rootLength(path);
    std::string result(path.substr(0, rootSize));
    for (char& character : result) {
        character = isSlash(character) ? '/' : character;
    }
    std::vector<std::string_view> segments;
    std::size_t start = rootSize;
    while (start < path.size()) {
        const std::size_t end = nextSlash(path, start);
        const std::string_view segment = path.substr(start, end - start);
        start = end + 1;
        if (segment.empty() || segment == ".") {
            continue;
        }
        if (segment == "..") {
            if (!segments.empty() && segments.back() != "..") {
                segments.pop_back();
            } else if (rootSize == 0) {
                segments.push_back(segment);
            }
            continue;
        }
        segments.push_back(segment);
    }
    for (const std::string_view segment : segments) {
        if (!result.empty() && result.back() != '/') {
            result += '/';
        }
        result += segment;
    }
    if (result.empty()) {
        return ".";
    }
    return result;
}

std::string joinPath(std::string_view base, std::string_view relative) {
    if (relative.empty()) {
        return normalizePath(base);
    }
    if (base.empty() || isAbsolutePath(relative)) {
        return normalizePath(relative);
    }
    std::string joined(base);
    if (!isSlash(joined.back())) {
        joined += '/';
    }
    joined += relative;
    return normalizePath(joined);
}

std::string_view parentPath(std::string_view path) noexcept {
    const std::size_t rootSize = rootLength(path);
    if (path.size() <= rootSize) {
        return path;
    }
    const std::size_t slash = path.rfind('/');
    if (slash == std::string_view::npos || slash < rootSize) {
        return path.substr(0, rootSize);
    }
    return path.substr(0, slash);
}

std::string_view fileName(std::string_view path) noexcept {
    const std::size_t rootSize = rootLength(path);
    if (path.size() <= rootSize) {
        return {};
    }
    const std::size_t slash = path.rfind('/');
    if (slash == std::string_view::npos) {
        return path;
    }
    return slash < rootSize ? path.substr(rootSize) : path.substr(slash + 1);
}

std::string_view extension(std::string_view path) noexcept {
    const std::string_view name = fileName(path);
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || name == "..") {
        return {};
    }
    return name.substr(dot);
}

} // namespace ith::fs
