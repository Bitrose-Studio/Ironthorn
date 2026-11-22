// The file system module (ADR-0035 rules 2 to 7): reads, atomic writes with their fault points,
// file information, listings, directories, remove, move and the known folders. Each test works in
// its own temporary folder.

#include "ith/file_system/FileSystem.h"

#include "ith/file_system/Path.h"
#include "ith/foundation/Uuid.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = ith::fs;

namespace {

// Removes `path` and everything below it through the module itself, which handles long paths.
void removeTree(const std::string& path) {
    const ith::Expected<std::optional<fs::FileInfo>> info = fs::fileInfo(path);
    if (!info || !info->has_value()) {
        return;
    }
    if ((*info)->kind == fs::FileKind::Directory) {
        if (ith::Expected<std::vector<fs::DirectoryEntry>> entries = fs::listDirectory(path)) {
            for (const fs::DirectoryEntry& entry : *entries) {
                removeTree(fs::joinPath(path, entry.name));
            }
        }
    }
    static_cast<void>(fs::remove(path));
}

// An empty folder in the system's temporary folder, removed with its content at the end.
class TempDirectory {
public:
    TempDirectory() {
        std::error_code error;
        const std::u8string base = std::filesystem::temp_directory_path(error).generic_u8string();
        REQUIRE_FALSE(error);
        m_path = fs::joinPath(std::string(base.begin(), base.end()), "ith-fs-test-" + ith::Uuid::generate().toString());
        REQUIRE(fs::createDirectories(m_path).hasValue());
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    TempDirectory(TempDirectory&&) = delete;
    TempDirectory& operator=(TempDirectory&&) = delete;

    ~TempDirectory() { removeTree(m_path); }

    // `relative` inside the folder, or the folder itself.
    [[nodiscard]] std::string path(std::string_view relative = {}) const { return fs::joinPath(m_path, relative); }

private:
    std::string m_path;
};

// The text of the file, or "<error code>".
std::string readText(const std::string& path) {
    ith::Expected<std::string> text = fs::readFile(path, std::size_t{1} << 30);
    return text ? *text : "<" + text.error().code + ">";
}

std::string errorCode(const ith::Expected<void>& result) {
    return result ? std::string() : result.error().code;
}

// The names in a directory, sorted.
std::vector<std::string> namesIn(const std::string& path) {
    std::vector<std::string> names;
    if (ith::Expected<std::vector<fs::DirectoryEntry>> entries = fs::listDirectory(path)) {
        for (const fs::DirectoryEntry& entry : *entries) {
            names.push_back(entry.name);
        }
    }
    std::ranges::sort(names);
    return names;
}

std::optional<fs::FileKind> kindOf(const std::string& path) {
    const ith::Expected<std::optional<fs::FileInfo>> info = fs::fileInfo(path);
    REQUIRE(info.hasValue());
    if (!info || !info->has_value()) {
        return std::nullopt;
    }
    return (*info)->kind;
}

std::filesystem::path nativeOf(const std::string& path) {
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
}

} // namespace

TEST_SUITE("file_system.files") {
    TEST_CASE("a write is read back, replaced, and creates its parents") {
        const TempDirectory directory;
        const std::string file = directory.path("a/b/c.txt");
        REQUIRE(fs::writeFileAtomic(file, "first").hasValue());
        CHECK(readText(file) == "first");
        REQUIRE(fs::writeFileAtomic(file, "second, longer").hasValue());
        CHECK(readText(file) == "second, longer");
        REQUIRE(fs::writeFileAtomic(file, "", {.durability = fs::Durability::Atomic}).hasValue());
        CHECK(readText(file) == "");
        CHECK(namesIn(directory.path("a/b")) == std::vector<std::string>{"c.txt"});

        const std::string binary("\0\x01\xff\r\n", 5);
        REQUIRE(fs::writeFileAtomic(directory.path("bin"), binary).hasValue());
        CHECK(readText(directory.path("bin")) == binary);

        // Backslashes are accepted on input.
        REQUIRE(fs::writeFileAtomic(directory.path() + "\\x\\y.txt", "slashes").hasValue());
        CHECK(readText(directory.path("x/y.txt")) == "slashes");
    }

    TEST_CASE("writes fail without parents when asked, on directories and on bad paths") {
        const TempDirectory directory;
        CHECK(errorCode(fs::writeFileAtomic(directory.path("missing/c.txt"), "x", {.createParents = false})) ==
              "fs.not_found");
        CHECK(namesIn(directory.path()).empty());
        REQUIRE(fs::createDirectories(directory.path("folder")).hasValue());
        CHECK(errorCode(fs::writeFileAtomic(directory.path("folder"), "x")) == "fs.is_a_directory");
        CHECK(errorCode(fs::writeFileAtomic("", "x")) == "fs.invalid_path");
        CHECK(errorCode(fs::writeFileAtomic(directory.path("bad\xff.txt"), "x")) == "fs.invalid_path");
        CHECK(errorCode(fs::writeFileAtomic(directory.path() + std::string("/nul\0.txt", 9), "x")) ==
              "fs.invalid_path");
        CHECK(namesIn(directory.path()) == std::vector<std::string>{"folder"});
    }

    TEST_CASE("readFile checks the size first and reports what is missing") {
        const TempDirectory directory;
        REQUIRE(fs::writeFileAtomic(directory.path("big.bin"), std::string(100, 'x')).hasValue());
        const ith::Expected<std::string> small = fs::readFile(directory.path("big.bin"), 99);
        REQUIRE_FALSE(small.hasValue());
        if (!small) {
            CHECK(small.error().code == "fs.too_large");
            CHECK(small.error().message.find("big.bin") != std::string::npos);
        }
        CHECK(fs::readFile(directory.path("big.bin"), 100).hasValue());
        const ith::Expected<std::string> missing = fs::readFile(directory.path("none.bin"), 100);
        REQUIRE_FALSE(missing.hasValue());
        if (!missing) {
            CHECK(missing.error().code == "fs.not_found");
            CHECK(missing.error().message.find("none.bin") != std::string::npos);
        }
        CHECK(fs::readFile(directory.path(), 100).error().code == "fs.is_a_directory");
    }

    TEST_CASE("temporary file names are recognized") {
        CHECK(fs::isTemporaryFileName("Ship.meta.tmp-3f2b8c1e-9d4a-4c1b-8e2f-0a1b2c3d4e5f"));
        CHECK(fs::isTemporaryFileName("C:/x/Ship.meta.tmp-3f2b8c1e-9d4a-4c1b-8e2f-0a1b2c3d4e5f"));
        CHECK_FALSE(fs::isTemporaryFileName(".tmp-3f2b8c1e-9d4a-4c1b-8e2f-0a1b2c3d4e5f"));
        CHECK_FALSE(fs::isTemporaryFileName("Ship.meta.tmp-not-a-uuid-at-all-not-a-uuid-at-all"));
        CHECK_FALSE(fs::isTemporaryFileName("Ship.meta"));
        CHECK_FALSE(fs::isTemporaryFileName("x.tmp-3f2b8c1e-9d4a-4c1b-8e2f-0a1b2c3d4e5f/Ship"));
    }

#ifndef ITH_SHIPPING
    TEST_CASE("a write that fails at any point keeps the old file whole and leaves no temporary file") {
        using fs::testing::WriteFault;
        for (const WriteFault fault :
             {WriteFault::AfterTemporaryWrite, WriteFault::AfterFlush, WriteFault::BeforeRename}) {
            for (const fs::Durability durability : {fs::Durability::Durable, fs::Durability::Atomic}) {
                CAPTURE(static_cast<int>(fault));
                CAPTURE(static_cast<int>(durability));
                const TempDirectory directory;
                const std::string file = directory.path("level.ithmap");
                REQUIRE(fs::writeFileAtomic(file, "old content").hasValue());
                fs::testing::setWriteFault(fault);
                const ith::Expected<void> written =
                    fs::writeFileAtomic(file, "new content", {.durability = durability});
                const ith::Expected<void> fresh = fs::writeFileAtomic(directory.path("fresh.txt"), "new");
                fs::testing::setWriteFault(WriteFault::None);
                CHECK(errorCode(written) == "fs.io");
                CHECK(errorCode(fresh) == "fs.io");
                CHECK(readText(file) == "old content");
                CHECK(namesIn(directory.path()) == std::vector<std::string>{"level.ithmap"});
            }
        }
        // The fault is per thread: another thread writes normally meanwhile.
        const TempDirectory directory;
        fs::testing::setWriteFault(WriteFault::BeforeRename);
        bool otherWrote = false;
        std::thread other([&] { otherWrote = fs::writeFileAtomic(directory.path("other.txt"), "x").hasValue(); });
        other.join();
        fs::testing::setWriteFault(WriteFault::None);
        CHECK(otherWrote);
    }
#endif

    TEST_CASE("readers see the old or the new file, never a partial one") {
        const TempDirectory directory;
        const std::string file = directory.path("shared.bin");
        constexpr std::size_t kSize = 64 * 1024;
        REQUIRE(fs::writeFileAtomic(file, std::string(kSize, 'a')).hasValue());
        std::atomic<bool> done = false;
        std::atomic<int> badReads = 0;
        std::thread reader([&] {
            while (!done.load()) {
                const ith::Expected<std::string> text = fs::readFile(file, kSize);
                if (!text || text->size() != kSize || (text->find_first_not_of((*text)[0]) != std::string::npos)) {
                    ++badReads;
                }
            }
        });
        int failedWrites = 0;
        for (int index = 0; index < 100; ++index) {
            const char fill = index % 2 == 0 ? 'b' : 'a';
            const ith::Expected<void> written =
                fs::writeFileAtomic(file, std::string(kSize, fill), {.durability = fs::Durability::Atomic});
            failedWrites += written ? 0 : 1;
        }
        done = true;
        reader.join();
        CHECK(failedWrites == 0);
        CHECK(badReads.load() == 0);
        CHECK(namesIn(directory.path()) == std::vector<std::string>{"shared.bin"});
    }

    TEST_CASE("long paths and names outside ASCII work") {
        const TempDirectory directory;
        std::string relative;
        for (int level = 0; level < 6; ++level) {
            relative += std::string(60, static_cast<char>('a' + level)) + "/";
        }
        relative += "\xc3\x89t\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x9a\x80.txt"; // "Été 日本 🚀.txt"
        const std::string file = directory.path(relative);
        REQUIRE(file.size() > 400);
        REQUIRE(fs::writeFileAtomic(file, "far away").hasValue());
        CHECK(readText(file) == "far away");
        const ith::Expected<std::optional<fs::FileInfo>> info = fs::fileInfo(file);
        REQUIRE(info.hasValue());
        CHECK((info && info->has_value() && (*info)->kind == fs::FileKind::File && (*info)->size == 8));
        const std::string parent(fs::parentPath(file));
        const ith::Expected<std::vector<fs::DirectoryEntry>> entries = fs::listDirectory(parent);
        REQUIRE(entries.hasValue());
        if (entries) {
            REQUIRE(entries->size() == 1);
            CHECK(entries->front().name == fs::fileName(file));
            CHECK(entries->front().validName);
        }
        const std::string renamed =
            fs::joinPath(parent, "\xd0\x9a\xd0\xbe\xd1\x80\xd0\xb0\xd0\xb1\xd0\xbb\xd1\x8c.txt");
        REQUIRE(fs::move(file, renamed).hasValue());
        CHECK(readText(renamed) == "far away");
        CHECK(fs::remove(renamed).hasValue());
        CHECK(namesIn(parent).empty());
    }

#ifdef _WIN32
    TEST_CASE("a replacement held up by another program fails with fs.busy after retrying") {
        // std::ifstream opens without delete sharing, as other programs do.
        const TempDirectory directory;
        const std::string file = directory.path("held.txt");
        REQUIRE(fs::writeFileAtomic(file, "old").hasValue());
        {
            std::ifstream held(nativeOf(file), std::ios::binary);
            REQUIRE(held.is_open());
            const auto start = std::chrono::steady_clock::now();
            const ith::Expected<void> written = fs::writeFileAtomic(file, "new");
            const auto elapsed = std::chrono::steady_clock::now() - start;
            CHECK(errorCode(written) == "fs.busy");
            CHECK(elapsed >= std::chrono::milliseconds(150));
            CHECK(elapsed < std::chrono::seconds(5));
        }
        CHECK(readText(file) == "old");
        CHECK(namesIn(directory.path()) == std::vector<std::string>{"held.txt"});
        // Once the other program let go, the write goes through.
        CHECK(fs::writeFileAtomic(file, "new").hasValue());
        CHECK(readText(file) == "new");
    }

#else
    TEST_CASE("a replacement keeps the target's permission bits") {
        const TempDirectory directory;
        const std::string file = directory.path("script.sh");
        REQUIRE(fs::writeFileAtomic(file, "old").hasValue());
        REQUIRE(::chmod(file.c_str(), 0750) == 0);
        REQUIRE(fs::writeFileAtomic(file, "new").hasValue());
        struct stat status{};
        REQUIRE(::stat(file.c_str(), &status) == 0);
        CHECK((status.st_mode & 07777) == 0750);
        REQUIRE(::chmod(file.c_str(), 0604) == 0);
        REQUIRE(fs::writeFileAtomic(file, "newer", {.durability = fs::Durability::Atomic}).hasValue());
        REQUIRE(::stat(file.c_str(), &status) == 0);
        CHECK((status.st_mode & 07777) == 0604);
        CHECK(readText(file) == "newer");
    }

    TEST_CASE("names that are not valid UTF-8 are listed with a lossy name") {
        const TempDirectory directory;
        const std::string bad = directory.path() + "/Bad\xff\xfe.txt";
        const int descriptor = ::open(bad.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
        REQUIRE(descriptor >= 0);
        ::close(descriptor);
        REQUIRE(fs::writeFileAtomic(directory.path("good.txt"), "x").hasValue());
        const ith::Expected<std::vector<fs::DirectoryEntry>> entries = fs::listDirectory(directory.path());
        REQUIRE(entries.hasValue());
        if (entries) {
            REQUIRE(entries->size() == 2);
            const auto invalid = std::ranges::find(*entries, false, &fs::DirectoryEntry::validName);
            REQUIRE(invalid != entries->end());
            if (invalid != entries->end()) {
                CHECK(invalid->name == "Bad\xef\xbf\xbd\xef\xbf\xbd.txt");
                CHECK(invalid->kind == fs::FileKind::File);
            }
        }
        ::unlink(bad.c_str());
    }
#endif
}

TEST_SUITE("file_system.directories") {
    TEST_CASE("fileInfo reports kinds, sizes and times, and nothing for missing paths") {
        const TempDirectory directory;
        REQUIRE(fs::writeFileAtomic(directory.path("file.txt"), "12345").hasValue());
        const ith::Expected<std::optional<fs::FileInfo>> file = fs::fileInfo(directory.path("file.txt"));
        REQUIRE(file.hasValue());
        if (file && file->has_value()) {
            CHECK((*file)->kind == fs::FileKind::File);
            CHECK((*file)->size == 5);
            const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            CHECK((*file)->modificationTime > now - std::int64_t{3600} * 1'000'000'000);
            CHECK((*file)->modificationTime < now + std::int64_t{3600} * 1'000'000'000);
        }
        CHECK(kindOf(directory.path()) == fs::FileKind::Directory);
        CHECK(kindOf(directory.path("missing")) == std::nullopt);
        CHECK(kindOf(directory.path("missing/deeper")) == std::nullopt);
        CHECK(kindOf(directory.path("file.txt/below")) == std::nullopt);
    }

    TEST_CASE("listDirectory lists one level with kinds and sizes") {
        const TempDirectory directory;
        REQUIRE(fs::writeFileAtomic(directory.path("b.txt"), "abc").hasValue());
        REQUIRE(fs::createDirectories(directory.path("a/inner")).hasValue());
        const ith::Expected<std::vector<fs::DirectoryEntry>> entries = fs::listDirectory(directory.path());
        REQUIRE(entries.hasValue());
        if (entries) {
            std::vector<fs::DirectoryEntry> sorted = *entries;
            std::ranges::sort(sorted, {}, &fs::DirectoryEntry::name);
            REQUIRE(sorted.size() == 2);
            CHECK(sorted[0].name == "a");
            CHECK(sorted[0].kind == fs::FileKind::Directory);
            CHECK(sorted[0].size == 0);
            CHECK(sorted[1].name == "b.txt");
            CHECK(sorted[1].kind == fs::FileKind::File);
            CHECK(sorted[1].size == 3);
        }
        CHECK(fs::listDirectory(directory.path("missing")).error().code == "fs.not_found");
        CHECK(fs::listDirectory(directory.path("b.txt")).error().code == "fs.not_a_directory");
        CHECK(namesIn(directory.path("a/inner")).empty());
    }

    TEST_CASE("symbolic links are reported as links, not followed") {
        const TempDirectory directory;
        REQUIRE(fs::writeFileAtomic(directory.path("target.txt"), "abcd").hasValue());
        REQUIRE(fs::createDirectories(directory.path("folder")).hasValue());
        std::error_code error;
        std::filesystem::create_symlink(nativeOf(directory.path("target.txt")), nativeOf(directory.path("file.link")),
                                        error);
        if (error) {
            // Windows needs developer mode or a privilege for symbolic links.
            MESSAGE("symbolic links can't be created here: " << error.message());
            return;
        }
        std::filesystem::create_directory_symlink(nativeOf(directory.path("folder")),
                                                  nativeOf(directory.path("folder.link")), error);
        REQUIRE_FALSE(error);
        std::filesystem::create_symlink(nativeOf(directory.path("gone.txt")), nativeOf(directory.path("dangling.link")),
                                        error);
        REQUIRE_FALSE(error);
        CHECK(kindOf(directory.path("file.link")) == fs::FileKind::FileLink);
        CHECK(kindOf(directory.path("folder.link")) == fs::FileKind::DirectoryLink);
        CHECK(kindOf(directory.path("dangling.link")) == fs::FileKind::Other);
        const ith::Expected<std::vector<fs::DirectoryEntry>> entries = fs::listDirectory(directory.path());
        REQUIRE(entries.hasValue());
        if (entries) {
            const auto link = std::ranges::find(*entries, std::string("file.link"), &fs::DirectoryEntry::name);
            REQUIRE(link != entries->end());
            if (link != entries->end()) {
                CHECK(link->kind == fs::FileKind::FileLink);
                CHECK(link->size == 4);
            }
        }
        CHECK(readText(directory.path("file.link")) == "abcd");
        // A link to a directory counts as one for createDirectories, and remove removes the link.
        CHECK(fs::createDirectories(directory.path("folder.link")).hasValue());
        CHECK(fs::remove(directory.path("folder.link")).hasValue());
        CHECK(fs::remove(directory.path("file.link")).hasValue());
        CHECK(fs::remove(directory.path("dangling.link")).hasValue());
        CHECK(readText(directory.path("target.txt")) == "abcd");
        CHECK(kindOf(directory.path("folder")) == fs::FileKind::Directory);
    }

    TEST_CASE("createDirectories creates the missing levels and accepts existing ones") {
        const TempDirectory directory;
        REQUIRE(fs::createDirectories(directory.path("x/y/z")).hasValue());
        CHECK(kindOf(directory.path("x/y/z")) == fs::FileKind::Directory);
        CHECK(fs::createDirectories(directory.path("x/y/z")).hasValue());
        CHECK(fs::createDirectories(directory.path("x/y")).hasValue());
        REQUIRE(fs::writeFileAtomic(directory.path("file"), "x").hasValue());
        CHECK(errorCode(fs::createDirectories(directory.path("file"))) == "fs.not_a_directory");
        CHECK(errorCode(fs::createDirectories(directory.path("file/below"))) == "fs.not_a_directory");
    }

    TEST_CASE("remove deletes files and empty directories only") {
        const TempDirectory directory;
        REQUIRE(fs::writeFileAtomic(directory.path("full/file.txt"), "x").hasValue());
        REQUIRE(fs::createDirectories(directory.path("empty")).hasValue());
        CHECK(errorCode(fs::remove(directory.path("full"))) == "fs.not_empty");
        CHECK(fs::remove(directory.path("full/file.txt")).hasValue());
        CHECK(fs::remove(directory.path("full")).hasValue());
        CHECK(fs::remove(directory.path("empty")).hasValue());
        CHECK(errorCode(fs::remove(directory.path("empty"))) == "fs.not_found");
        CHECK(namesIn(directory.path()).empty());
    }

    TEST_CASE("move renames, never replaces, and allows a change of case") {
        const TempDirectory directory;
        REQUIRE(fs::writeFileAtomic(directory.path("Ship.txt"), "ship").hasValue());
        REQUIRE(fs::writeFileAtomic(directory.path("Boat.txt"), "boat").hasValue());
        CHECK(errorCode(fs::move(directory.path("Ship.txt"), directory.path("Boat.txt"))) == "fs.already_exists");
        CHECK(readText(directory.path("Ship.txt")) == "ship");
        CHECK(readText(directory.path("Boat.txt")) == "boat");

        REQUIRE(fs::move(directory.path("Ship.txt"), directory.path("ship.txt")).hasValue());
        CHECK(namesIn(directory.path()) == std::vector<std::string>{"Boat.txt", "ship.txt"});
        CHECK(readText(directory.path("ship.txt")) == "ship");

        REQUIRE(fs::createDirectories(directory.path("Sea")).hasValue());
        REQUIRE(fs::move(directory.path("ship.txt"), directory.path("Sea/Ship.txt")).hasValue());
        CHECK(readText(directory.path("Sea/Ship.txt")) == "ship");
        REQUIRE(fs::move(directory.path("Sea"), directory.path("Ocean")).hasValue());
        CHECK(readText(directory.path("Ocean/Ship.txt")) == "ship");

        CHECK(errorCode(fs::move(directory.path("none.txt"), directory.path("other.txt"))) == "fs.not_found");
        CHECK(errorCode(fs::move(directory.path("Boat.txt"), directory.path("missing/Boat.txt"))) == "fs.not_found");
        CHECK(namesIn(directory.path()) == std::vector<std::string>{"Boat.txt", "Ocean"});
    }
}

TEST_SUITE("file_system.known_folders") {
    TEST_CASE("the executable directory holds this test") {
        const ith::Expected<std::string> executable = fs::executableDirectory();
        REQUIRE(executable.hasValue());
        if (executable) {
            CHECK(fs::isAbsolutePath(*executable));
            CHECK(fs::normalizePath(*executable) == *executable);
            CHECK(kindOf(*executable) == fs::FileKind::Directory);
            const std::vector<std::string> names = namesIn(*executable);
            CHECK(std::ranges::any_of(
                names, [](const std::string& name) { return name.starts_with("ith_file_system_tests"); }));
        }
    }

    TEST_CASE("the user folders are absolute and normalized") {
        for (const ith::Expected<std::string>& folder : {fs::userConfigDirectory(), fs::userCacheDirectory()}) {
            REQUIRE(folder.hasValue());
            if (folder) {
                CHECK(fs::isAbsolutePath(*folder));
                CHECK(fs::normalizePath(*folder) == *folder);
            }
        }
    }

#ifndef _WIN32
    TEST_CASE("the user folders follow the XDG variables when they are absolute") {
        // Saved and restored, so other tests see the real environment.
        const auto saved = [](const char* name) -> std::optional<std::string> {
            const char* value = std::getenv(name);
            return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
        };
        const std::optional<std::string> config = saved("XDG_CONFIG_HOME");
        const std::optional<std::string> cache = saved("XDG_CACHE_HOME");
        const std::optional<std::string> home = saved("HOME");

        ::setenv("HOME", "/home/test-user", 1);
        ::setenv("XDG_CONFIG_HOME", "/srv/config/", 1);
        ::setenv("XDG_CACHE_HOME", "/srv//cache", 1);
        CHECK(fs::userConfigDirectory().valueOr(std::string()) == "/srv/config");
        CHECK(fs::userCacheDirectory().valueOr(std::string()) == "/srv/cache");
        // A relative value is invalid by the XDG rules and ignored.
        ::setenv("XDG_CONFIG_HOME", "relative/config", 1);
        CHECK(fs::userConfigDirectory().valueOr(std::string()) == "/home/test-user/.config");
        ::unsetenv("XDG_CONFIG_HOME");
        ::unsetenv("XDG_CACHE_HOME");
        CHECK(fs::userConfigDirectory().valueOr(std::string()) == "/home/test-user/.config");
        CHECK(fs::userCacheDirectory().valueOr(std::string()) == "/home/test-user/.cache");

        const auto restore = [](const char* name, const std::optional<std::string>& value) {
            if (value) {
                ::setenv(name, value->c_str(), 1);
            } else {
                ::unsetenv(name);
            }
        };
        restore("XDG_CONFIG_HOME", config);
        restore("XDG_CACHE_HOME", cache);
        restore("HOME", home);
    }
#endif
}
