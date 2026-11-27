// The POSIX implementation of FileSystem.h, with the Linux additions where they exist
// (renameat2, /proc/self/exe). Built on every platform and empty on Windows, so every tool sees
// both implementations.

#ifndef _WIN32

#include "ith/file_system/FileSystem.h"
#include "ith/file_system/FileSystemInternal.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/StringUtils.h"

#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace ith::fs {
namespace {

using detail::pathError;

// Frees what the C library allocated with malloc (realpath's result).
struct FreeDeleter {
    void operator()(char* text) const noexcept { std::free(text); }
};

// Closes a file descriptor.
class FileDescriptor {
public:
    explicit FileDescriptor(int descriptor) noexcept : m_descriptor(descriptor) {}

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&&) = delete;
    FileDescriptor& operator=(FileDescriptor&&) = delete;

    ~FileDescriptor() { static_cast<void>(close()); }

    [[nodiscard]] int get() const noexcept { return m_descriptor; }

    [[nodiscard]] bool valid() const noexcept { return m_descriptor >= 0; }

    // Closes the descriptor now; false when close() reports an error.
    bool close() noexcept {
        if (!valid()) {
            return true;
        }
        const int result = ::close(m_descriptor);
        m_descriptor = -1;
        return result == 0 || errno == EINTR;
    }

private:
    int m_descriptor;
};

// Closes a directory stream.
class DirectoryStream {
public:
    explicit DirectoryStream(DIR* stream) noexcept : m_stream(stream) {}

    DirectoryStream(const DirectoryStream&) = delete;
    DirectoryStream& operator=(const DirectoryStream&) = delete;
    DirectoryStream(DirectoryStream&&) = delete;
    DirectoryStream& operator=(DirectoryStream&&) = delete;

    ~DirectoryStream() {
        if (m_stream != nullptr) {
            closedir(m_stream);
        }
    }

    [[nodiscard]] DIR* get() const noexcept { return m_stream; }

private:
    DIR* m_stream;
};

// strerror_r has a GNU form returning the text and an XSI form returning an int.
[[maybe_unused]] const char* errorText(int result, const char* buffer) noexcept {
    return result == 0 ? buffer : "unknown error";
}

[[maybe_unused]] const char* errorText(const char* result, const char* /*buffer*/) noexcept {
    return result;
}

std::string systemMessage(int error) {
    std::array<char, 256> buffer{};
    return errorText(strerror_r(error, buffer.data(), buffer.size()), buffer.data());
}

const char* codeFor(int error) {
    switch (error) {
        case ENOENT:
            return "fs.not_found";
        case EEXIST:
            return "fs.already_exists";
        case EACCES:
        case EPERM:
        case EROFS:
            return "fs.access_denied";
        case EBUSY:
        case ETXTBSY:
            return "fs.busy";
        case ENOTDIR:
            return "fs.not_a_directory";
        case EISDIR:
            return "fs.is_a_directory";
        case ENOTEMPTY:
            return "fs.not_empty";
        case ENAMETOOLONG:
            return "fs.invalid_path";
        case ENOSPC:
        case EDQUOT:
            return "fs.no_space";
        case ENOSYS:
        case EOPNOTSUPP:
            return "fs.unsupported";
        default:
            return "fs.io";
    }
}

Unexpected<Error> posixError(std::string_view what, std::string_view path, int error) {
    return pathError(codeFor(error), what, path, systemMessage(error));
}

std::int64_t toNanoseconds(const struct stat& status) noexcept {
#ifdef __APPLE__
    const timespec& time = status.st_mtimespec;
#else
    const timespec& time = status.st_mtim;
#endif
    return static_cast<std::int64_t>(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}

// The FileInfo of an entry from its lstat() result; `follow` stats the link target.
template <typename Follow>
FileInfo infoOf(const struct stat& status, Follow follow) {
    if (S_ISLNK(status.st_mode)) {
        struct stat target{};
        if (!follow(target)) {
            return FileInfo{};
        }
        if (S_ISREG(target.st_mode)) {
            return FileInfo{.kind = FileKind::FileLink,
                            .size = static_cast<std::uint64_t>(target.st_size),
                            .modificationTime = toNanoseconds(target)};
        }
        return FileInfo{.kind = S_ISDIR(target.st_mode) ? FileKind::DirectoryLink : FileKind::Other,
                        .modificationTime = toNanoseconds(target)};
    }
    if (S_ISREG(status.st_mode)) {
        return FileInfo{.kind = FileKind::File,
                        .size = static_cast<std::uint64_t>(status.st_size),
                        .modificationTime = toNanoseconds(status)};
    }
    return FileInfo{.kind = S_ISDIR(status.st_mode) ? FileKind::Directory : FileKind::Other,
                    .modificationTime = toNanoseconds(status)};
}

// Writes all of `bytes`, retrying interrupted and partial writes.
bool writeAll(int descriptor, std::string_view bytes) noexcept {
    while (!bytes.empty()) {
        const ssize_t written = ::write(descriptor, bytes.data(), bytes.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

int fsyncRetrying(int descriptor) noexcept {
    int result = 0;
    do {
        result = ::fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    return result;
}

// Flushes the directory entry list of `directory` ("" is the working directory). File systems that
// can't flush a directory (EINVAL) are taken as they are.
Expected<void> flushDirectory(std::string_view directory) {
    const std::string path = directory.empty() ? std::string(".") : std::string(directory);
    const FileDescriptor descriptor(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!descriptor.valid()) {
        return posixError("can't open the directory", path, errno);
    }
    if (fsyncRetrying(descriptor.get()) != 0 && errno != EINVAL) {
        return posixError("can't flush the directory", path, errno);
    }
    return {};
}

// Removes a temporary file unless it was renamed into place.
class TemporaryFile {
public:
    explicit TemporaryFile(std::string path) : m_path(std::move(path)) {}

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    TemporaryFile(TemporaryFile&&) = delete;
    TemporaryFile& operator=(TemporaryFile&&) = delete;

    ~TemporaryFile() {
        if (!m_kept) {
            ::unlink(m_path.c_str());
        }
    }

    [[nodiscard]] const std::string& path() const noexcept { return m_path; }

    void keep() noexcept { m_kept = true; }

private:
    std::string m_path;
    bool m_kept = false;
};

bool sameFile(const std::string& a, const std::string& b) {
    struct stat first{};
    struct stat second{};
    return ::lstat(a.c_str(), &first) == 0 && ::lstat(b.c_str(), &second) == 0 && first.st_dev == second.st_dev &&
           first.st_ino == second.st_ino;
}

// Renames `from` to `to`, two names of the same file that differ only in case, through a
// temporary name: rename() does nothing when both names are links to the same file.
Expected<void> renameCase(const std::string& from, const std::string& to) {
    const std::string temporary = detail::temporaryPathFor(from);
    if (::rename(from.c_str(), temporary.c_str()) != 0) {
        return posixError("can't move '" + from + "' to", to, errno);
    }
    if (::rename(temporary.c_str(), to.c_str()) != 0) {
        const int error = errno;
        static_cast<void>(::rename(temporary.c_str(), from.c_str()));
        return posixError("can't move '" + from + "' to", to, error);
    }
    return {};
}

// The existing target `to` blocks the move, unless it is `from` itself under another case.
Expected<void> moveOverExisting(const std::string& from, const std::string& to) {
    if (from != to && equalsIgnoreCaseAscii(from, to) && sameFile(from, to)) {
        return renameCase(from, to);
    }
    return pathError("fs.already_exists", "can't move '" + from + "' to", to, "the target exists");
}

std::string homeDirectory() {
    if (const char* home = std::getenv("HOME"); home != nullptr && isAbsolutePath(home)) {
        return normalizePath(home);
    }
    std::array<char, 4096> buffer{};
    struct passwd entry{};
    struct passwd* result = nullptr;
    if (getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &result) == 0 && result != nullptr &&
        result->pw_dir != nullptr && isAbsolutePath(result->pw_dir)) {
        return normalizePath(result->pw_dir);
    }
    return {};
}

// $<variable> when it is an absolute path, else <home>/<fallback>.
Expected<std::string> xdgDirectory(const char* variable, std::string_view fallback) {
    if (const char* value = std::getenv(variable); value != nullptr && isAbsolutePath(value)) {
        return normalizePath(value);
    }
    const std::string home = homeDirectory();
    if (home.empty()) {
        return makeError("fs.not_found", std::string("can't find the home directory for $") + variable);
    }
    return joinPath(home, fallback);
}

} // namespace

Expected<std::string> readFile(std::string_view path, std::size_t maxSize) {
    Expected<std::string> native = detail::checkedPath(path);
    if (!native) {
        return native;
    }
    // Non-blocking, so opening a FIFO doesn't wait for a writer; only a regular file is read, and
    // reads block as usual again.
    const FileDescriptor file(::open(native->c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (!file.valid()) {
        const int error = errno;
        if (error == ENXIO) { // a socket, or a device without a driver
            return pathError("fs.unsupported", "can't read", *native, "it is not a regular file");
        }
        bool wouldBlock = error == EAGAIN;
#if EWOULDBLOCK != EAGAIN
        wouldBlock = wouldBlock || error == EWOULDBLOCK;
#endif
        if (wouldBlock) { // a write lease another process holds
            return pathError("fs.busy", "can't open", *native, "another process holds it");
        }
        return posixError("can't open", *native, error);
    }
    struct stat status{};
    if (::fstat(file.get(), &status) != 0) {
        return posixError("can't get the size of", *native, errno);
    }
    if (S_ISDIR(status.st_mode)) {
        return pathError("fs.is_a_directory", "can't read", *native, "it is a directory");
    }
    if (!S_ISREG(status.st_mode)) {
        return pathError("fs.unsupported", "can't read", *native, "it is not a regular file");
    }
    const int flags = ::fcntl(file.get(), F_GETFL);
    if (flags == -1 || ::fcntl(file.get(), F_SETFL, flags & ~O_NONBLOCK) == -1) {
        return posixError("can't read", *native, errno);
    }
    const auto size = static_cast<std::uint64_t>(status.st_size);
    if (size > maxSize) {
        return makeError("fs.too_large", "'" + *native + "' has " + std::to_string(size) + " bytes; the limit is " +
                                             std::to_string(maxSize));
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t read = ::read(file.get(), bytes.data() + done, bytes.size() - done);
        if (read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return posixError("can't read", *native, errno);
        }
        if (read == 0) {
            break; // the file got shorter
        }
        done += static_cast<std::size_t>(read);
    }
    bytes.resize(done);
    return bytes;
}

Expected<void> writeFileAtomic(std::string_view path, std::string_view bytes, const WriteOptions& options) {
    Expected<std::string> target = detail::checkedPath(path);
    if (!target) {
        return makeUnexpected(std::move(target).error());
    }
    const std::string_view name = fileName(*target);
    if (name.empty() || name == "." || name == "..") {
        return pathError("fs.invalid_path", "can't write", *target, "the path has no file name");
    }
    struct stat existing{};
    const bool targetExists = ::stat(target->c_str(), &existing) == 0;
    if (targetExists && S_ISDIR(existing.st_mode)) {
        return pathError("fs.is_a_directory", "can't write", *target, "it is a directory");
    }
    const std::string_view parent = parentPath(*target);
    std::vector<std::string> created;
    if (options.createParents && !parent.empty()) {
        if (Expected<void> made = detail::createDirectoriesTracked(parent, created); !made) {
            return made;
        }
    }
    const bool durable = options.durability == Durability::Durable;
    TemporaryFile temporary(detail::temporaryPathFor(*target));
    {
        FileDescriptor file(::open(temporary.path().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666));
        if (!file.valid()) {
            const int error = errno;
            temporary.keep(); // nothing was created
            return posixError("can't create", temporary.path(), error);
        }
        if (!writeAll(file.get(), bytes)) {
            return posixError("can't write", temporary.path(), errno);
        }
        if (detail::faultAt(detail::kFaultAfterTemporaryWrite)) {
            return detail::injectedFault(temporary.path());
        }
        if (targetExists && ::fchmod(file.get(), existing.st_mode & 07777) != 0) {
            return posixError("can't set the permissions of", temporary.path(), errno);
        }
        if (durable && fsyncRetrying(file.get()) != 0) {
            return posixError("can't flush", temporary.path(), errno);
        }
        if (detail::faultAt(detail::kFaultAfterFlush)) {
            return detail::injectedFault(temporary.path());
        }
        if (!file.close()) {
            return posixError("can't close", temporary.path(), errno);
        }
    }
    if (detail::faultAt(detail::kFaultBeforeRename)) {
        return detail::injectedFault(temporary.path());
    }
    if (::rename(temporary.path().c_str(), target->c_str()) != 0) {
        return posixError("can't replace", *target, errno);
    }
    temporary.keep();
    if (!durable) {
        return {};
    }
    // The new name, and the names of the directories this call created, reach the disk only when
    // their directories are flushed.
    if (Expected<void> flushed = flushDirectory(parent); !flushed) {
        return flushed;
    }
    for (const std::string& directory : created) {
        if (const std::string_view above = parentPath(directory); above != parent) {
            if (Expected<void> flushed = flushDirectory(above); !flushed) {
                return flushed;
            }
        }
    }
    return {};
}

Expected<std::optional<FileInfo>> fileInfo(std::string_view path) {
    Expected<std::string> native = detail::checkedPath(path);
    if (!native) {
        return makeUnexpected(std::move(native).error());
    }
    struct stat status{};
    if (::lstat(native->c_str(), &status) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return std::optional<FileInfo>();
        }
        return posixError("can't get information about", *native, errno);
    }
    return std::optional<FileInfo>(
        infoOf(status, [&](struct stat& target) { return ::stat(native->c_str(), &target) == 0; }));
}

Expected<std::vector<DirectoryEntry>> listDirectory(std::string_view path) {
    Expected<std::string> native = detail::checkedPath(path);
    if (!native) {
        return makeUnexpected(std::move(native).error());
    }
    const DirectoryStream stream(::opendir(native->c_str()));
    if (stream.get() == nullptr) {
        return posixError("can't list", *native, errno);
    }
    const int directory = ::dirfd(stream.get());
    std::vector<DirectoryEntry> entries;
    for (;;) {
        errno = 0;
        const dirent* item = ::readdir(stream.get());
        if (item == nullptr) {
            if (errno != 0) {
                return posixError("can't list", *native, errno);
            }
            break;
        }
        const std::string_view name(item->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        struct stat status{};
        if (::fstatat(directory, item->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
            continue; // removed since it was listed
        }
        const FileInfo info =
            infoOf(status, [&](struct stat& target) { return ::fstatat(directory, item->d_name, &target, 0) == 0; });
        // A name with a backslash is legal here, but every fs function takes '\' for a separator,
        // so the entry can't be addressed by its name.
        const bool utf8 = isValidUtf8(name);
        entries.push_back(DirectoryEntry{.name = utf8 ? std::string(name) : detail::lossyUtf8(name),
                                         .kind = info.kind,
                                         .size = info.size,
                                         .validName = utf8 && name.find('\\') == std::string_view::npos});
    }
    return entries;
}

Expected<void> remove(std::string_view path) {
    Expected<std::string> native = detail::checkedPath(path);
    if (!native) {
        return makeUnexpected(std::move(native).error());
    }
    struct stat status{};
    if (::lstat(native->c_str(), &status) != 0) {
        return posixError("can't delete", *native, errno);
    }
    const int result = S_ISDIR(status.st_mode) ? ::rmdir(native->c_str()) : ::unlink(native->c_str());
    if (result != 0) {
        const int error = errno == EEXIST ? ENOTEMPTY : errno;
        return posixError("can't delete", *native, error);
    }
    return {};
}

Expected<void> move(std::string_view from, std::string_view to) {
    Expected<std::string> source = detail::checkedPath(from);
    if (!source) {
        return makeUnexpected(std::move(source).error());
    }
    Expected<std::string> target = detail::checkedPath(to);
    if (!target) {
        return makeUnexpected(std::move(target).error());
    }
#ifdef __linux__
    if (::renameat2(AT_FDCWD, source->c_str(), AT_FDCWD, target->c_str(), RENAME_NOREPLACE) == 0) {
        return {};
    }
    const int error = errno;
    if (error == EEXIST) {
        return moveOverExisting(*source, *target);
    }
    if (error != EINVAL && error != ENOSYS && error != EOPNOTSUPP) {
        return posixError("can't move '" + *source + "' to", *target, error);
    }
#endif
    // Without RENAME_NOREPLACE: check, then rename. Another process could still create the target
    // in between.
    struct stat existing{};
    if (::lstat(target->c_str(), &existing) == 0) {
        return moveOverExisting(*source, *target);
    }
    if (::rename(source->c_str(), target->c_str()) != 0) {
        return posixError("can't move '" + *source + "' to", *target, errno);
    }
    return {};
}

namespace detail {

Expected<void> makeDirectory(std::string_view path) {
    const std::string native(path);
    if (::mkdir(native.c_str(), 0777) != 0) {
        return posixError("can't create the directory", native, errno);
    }
    return {};
}

std::optional<std::string> resolvedPath(const std::string& path) {
    const std::unique_ptr<char, FreeDeleter> resolved(::realpath(path.c_str(), nullptr));
    if (resolved == nullptr) {
        return std::nullopt;
    }
    return normalizePath(resolved.get());
}

std::optional<std::size_t> matchedPrefix(std::string_view text, std::string_view prefix) {
    if (!text.starts_with(prefix)) {
        return std::nullopt;
    }
    return prefix.size();
}

} // namespace detail

bool hasUnusableName(std::string_view path) noexcept {
    return path.find('\\') != std::string_view::npos; // every function here reads it as a separator
}

Expected<std::string> executableDirectory() {
#ifdef __linux__
    std::string buffer(256, '\0');
    for (;;) {
        const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            return posixError("can't read", "/proc/self/exe", errno);
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            buffer.resize(static_cast<std::size_t>(length));
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    const std::string normalized = normalizePath(buffer);
    return std::string(parentPath(normalized));
#else
    return makeError("fs.unsupported", "the executable directory is not known on this platform");
#endif
}

Expected<std::string> userConfigDirectory() {
    return xdgDirectory("XDG_CONFIG_HOME", ".config");
}

Expected<std::string> userCacheDirectory() {
    return xdgDirectory("XDG_CACHE_HOME", ".cache");
}

} // namespace ith::fs

#endif // _WIN32
