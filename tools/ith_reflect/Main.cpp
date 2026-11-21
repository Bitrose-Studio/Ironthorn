// ith_reflect: writes a module's reflection records from its annotated headers (ADR-0018).
//
//   ith_reflect --module <name> --kind <runtime|client|tools|editor> --include-root <dir>
//               --output <file.reflect.cpp> <header>...
//
// Headers are included in the output relative to --include-root. The output is rewritten only
// when its content changes, so an unchanged module isn't recompiled. Errors are printed as
// "file:line: error: message" and the exit code is 1; usage errors exit with 2.

#include "ith_reflect/Generator.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct Arguments {
    std::string module;
    std::string kind;
    fs::path includeRoot;
    fs::path output;
    std::vector<fs::path> headers;
};

int usage(const char* problem) {
    std::fprintf(stderr,
                 "ith_reflect: %s\n"
                 "usage: ith_reflect --module <name> --kind <runtime|client|tools|editor> --include-root <dir>\n"
                 "                   --output <file.reflect.cpp> <header>...\n",
                 problem);
    return 2;
}

std::optional<Arguments> parseArguments(std::span<char*> arguments) {
    Arguments result;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];
        const bool hasValue = index + 1 < arguments.size();
        if (argument == "--module" && hasValue) {
            result.module = arguments[++index];
        } else if (argument == "--kind" && hasValue) {
            result.kind = arguments[++index];
        } else if (argument == "--include-root" && hasValue) {
            result.includeRoot = arguments[++index];
        } else if (argument == "--output" && hasValue) {
            result.output = arguments[++index];
        } else if (argument.starts_with("--")) {
            return std::nullopt;
        } else {
            result.headers.emplace_back(argument);
        }
    }
    if (result.module.empty() || result.kind.empty() || result.includeRoot.empty() || result.output.empty()) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::string> readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace

int main(int argc, char** argv) {
    const std::optional<Arguments> arguments =
        parseArguments(std::span<char*>(argv, static_cast<std::size_t>(argc)).subspan(1));
    if (!arguments) {
        return usage("missing or unknown arguments");
    }
    if (!ith::reflect::isModuleName(arguments->module)) {
        return usage("the module name must be lower-case letters, digits and '_'");
    }
    const ith::Expected<ith::reflect::ModuleKind> kind = ith::reflect::parseModuleKind(arguments->kind);
    if (!kind) {
        return usage(kind.error().message.c_str());
    }

    const fs::path root = arguments->includeRoot.lexically_normal();
    std::vector<ith::reflect::SourceHeader> headers;
    for (const fs::path& path : arguments->headers) {
        std::optional<std::string> text = readFile(path);
        const fs::path relative = path.lexically_normal().lexically_relative(root);
        if (!text) {
            std::fprintf(stderr, "%s:1: error: can't read the header\n", path.string().c_str());
            return 1;
        }
        if (relative.empty() || *relative.begin() == "..") {
            std::fprintf(stderr, "%s:1: error: the header is not under the include root %s\n", path.string().c_str(),
                         root.string().c_str());
            return 1;
        }
        headers.push_back(ith::reflect::SourceHeader{
            .path = path.string(), .includePath = relative.generic_string(), .text = std::move(*text)});
    }

    const ith::Expected<std::string, std::vector<ith::reflect::Diagnostic>> generated =
        ith::reflect::generate({.module = arguments->module, .kind = *kind}, headers);
    if (!generated) {
        for (const ith::reflect::Diagnostic& diagnostic : generated.error()) {
            std::fprintf(stderr, "%s\n", ith::reflect::formatDiagnostic(diagnostic).c_str());
        }
        return 1;
    }

    // Leave an unchanged file alone, so its timestamp doesn't trigger a recompile.
    if (readFile(arguments->output) == *generated) {
        return 0;
    }
    std::error_code error;
    fs::create_directories(arguments->output.parent_path(), error);
    std::ofstream file(arguments->output, std::ios::binary | std::ios::trunc);
    file << *generated;
    file.close();
    if (!file) {
        std::fprintf(stderr, "%s:1: error: can't write the generated file\n", arguments->output.string().c_str());
        return 1;
    }
    return 0;
}
