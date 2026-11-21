#pragma once

// Helpers for the asset database tests: a temporary project folder and a test importer.
//
// The test importer reads ".objects" files, a line-based text format that lists named objects:
//
//   mesh Body/Hull = payload text
//   mesh                  (an unnamed mesh)
//   material Steel
//   !broken               (a line starting with '!' makes the import fail)
//   include Parts/Hull.txt  (reads a file next to the source: an object "file Parts/Hull.txt" whose
//                           payload is the file's content)
//   refer Textures/Rock.png (a reference to another file)
//
// Each object's payload is its text after " = ", followed by "@<scale>" from the settings.

#include "ith/asset_database/FileIo.h"
#include "ith/asset_database/Importer.h"
#include "ith/foundation/StringUtils.h"
#include "ith/foundation/Uuid.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ith::test {

// A project folder with an empty Content folder in the system's temporary folder, deleted with
// everything in it when the object is destroyed.
class TempProject {
public:
    TempProject() {
        std::error_code error;
        m_directory = std::filesystem::temp_directory_path(error) / ("ith-asset-test-" + Uuid::generate().toString());
        REQUIRE_FALSE(error);
        std::filesystem::create_directories(m_directory / "Content", error);
        REQUIRE_FALSE(error);
    }

    TempProject(const TempProject&) = delete;
    TempProject& operator=(const TempProject&) = delete;
    TempProject(TempProject&&) = delete;
    TempProject& operator=(TempProject&&) = delete;

    ~TempProject() {
        std::error_code error;
        std::filesystem::remove_all(m_directory, error);
    }

    [[nodiscard]] const std::filesystem::path& directory() const { return m_directory; }

    // The file at `path` relative to Content.
    [[nodiscard]] std::filesystem::path content(std::string_view path) const {
        return m_directory / "Content" / detail::asset_files::fromUtf8(path);
    }

    // Writes a file relative to Content, creating its folders.
    void write(std::string_view path, std::string_view text) const {
        CHECK(detail::asset_files::writeFileAtomically(content(path), text).hasValue());
    }

    // The text of a file relative to Content, or nullopt when it can't be read.
    [[nodiscard]] std::optional<std::string> read(std::string_view path) const {
        Expected<std::string> text = detail::asset_files::readFile(content(path), std::size_t{1} << 30);
        return text ? std::optional<std::string>(*text) : std::nullopt;
    }

    // True when a file exists at `path` with exactly this name (case included).
    [[nodiscard]] bool exists(std::string_view path) const {
        const std::filesystem::path file = content(path);
        std::error_code error;
        for (std::filesystem::directory_iterator entry(file.parent_path(), error);
             !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
            if (entry->path().filename() == file.filename()) {
                return true;
            }
        }
        return false;
    }

private:
    std::filesystem::path m_directory;
};

// The importer of ".objects" files described at the top of this file.
class ObjectsImporter final : public Importer {
public:
    // `calls`, when set, counts the imports. `version` is the importer version.
    explicit ObjectsImporter(std::atomic<int>* calls = nullptr, std::uint32_t version = 1)
        : m_calls(calls), m_version(version) {}

    [[nodiscard]] std::string_view id() const override { return "test.objects"; }

    [[nodiscard]] std::uint32_t version() const override { return m_version; }

    [[nodiscard]] std::span<const std::string_view> extensions() const override { return kExtensions; }

    [[nodiscard]] serialization::Value defaultSettings() const override {
        serialization::Value settings = serialization::Value::object();
        settings.set("scale", serialization::Value::integer(1));
        return settings;
    }

    [[nodiscard]] Expected<std::vector<std::string>>
    dependencies(std::span<const std::byte> source, const serialization::Value& /*settings*/) const override {
        std::vector<std::string> paths;
        for (const std::string_view rawLine : split(textOf(source), '\n')) {
            if (const std::string_view line = trim(rawLine); line.starts_with(kInclude)) {
                paths.emplace_back(line.substr(kInclude.size()));
            }
        }
        return paths;
    }

    [[nodiscard]] Expected<ImportOutput> import(std::span<const std::byte> source, std::span<const ImportFile> files,
                                                const serialization::Value& settings) const override {
        if (m_calls != nullptr) {
            ++*m_calls;
        }
        const serialization::Value* scaleValue = settings.find("scale");
        const std::int64_t scale =
            scaleValue != nullptr ? scaleValue->toNumber<std::int64_t>().value_or(1) : std::int64_t{1};
        const std::string_view text = textOf(source);
        ImportOutput output;
        for (const std::string_view rawLine : split(text, '\n')) {
            const std::string_view line = trim(rawLine);
            if (line.empty()) {
                continue;
            }
            if (line.front() == '!') {
                return makeError("import.malformed", "the source has a '!' line");
            }
            if (line.starts_with(kRefer)) {
                output.references.emplace_back(line.substr(kRefer.size()));
                continue;
            }
            if (line.starts_with(kInclude)) {
                const std::string_view path = line.substr(kInclude.size());
                const auto file = std::ranges::find(files, path, &ImportFile::path);
                if (file == files.end()) {
                    return makeError("import.malformed", "the file '" + std::string(path) + "' was not passed");
                }
                output.objects.push_back(ImportedObject{
                    .type = "file", .name = std::string(path), .payload = {file->bytes.begin(), file->bytes.end()}});
                continue;
            }
            std::string_view head = line;
            std::string payload;
            if (const std::size_t equals = line.find(" = "); equals != std::string_view::npos) {
                head = line.substr(0, equals);
                payload = std::string(line.substr(equals + 3));
            }
            payload += "@" + std::to_string(scale);
            const std::size_t space = head.find(' ');
            ImportedObject object;
            object.type = std::string(head.substr(0, space));
            object.name = space == std::string_view::npos ? std::string() : std::string(trim(head.substr(space + 1)));
            const auto* bytes = reinterpret_cast<const std::byte*>(payload.data());
            object.payload.assign(bytes, bytes + payload.size());
            output.objects.push_back(std::move(object));
        }
        return output;
    }

private:
    static constexpr std::string_view kInclude = "include ";
    static constexpr std::string_view kRefer = "refer ";
    static constexpr std::array<std::string_view, 1> kExtensions = {".objects"};

    static std::string_view textOf(std::span<const std::byte> bytes) {
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }

    std::atomic<int>* m_calls;
    std::uint32_t m_version;
};

} // namespace ith::test
