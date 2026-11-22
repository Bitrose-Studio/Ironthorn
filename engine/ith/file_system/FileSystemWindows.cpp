// The Win32 implementation of FileSystem.h: the wide functions, with "\\?\" paths. Built on every
// platform and empty outside Windows, so every tool sees both implementations.

#ifdef _WIN32

#include "ith/file_system/FileSystem.h"
#include "ith/file_system/FileSystemInternal.h"
#include "ith/file_system/Path.h"

#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace ith::fs {
namespace {

using detail::pathError;

// Closes a file or directory handle.
class FileHandle {
public:
    explicit FileHandle(HANDLE handle) noexcept : m_handle(handle) {}

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&&) = delete;
    FileHandle& operator=(FileHandle&&) = delete;

    ~FileHandle() { close(); }

    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }

    [[nodiscard]] bool valid() const noexcept { return m_handle != INVALID_HANDLE_VALUE; }

    // Closes the handle now; false when CloseHandle fails.
    bool close() noexcept {
        if (!valid()) {
            return true;
        }
        const BOOL closed = CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
        return closed != FALSE;
    }

private:
    HANDLE m_handle;
};

// Closes a FindFirstFileExW search.
class FindHandle {
public:
    explicit FindHandle(HANDLE handle) noexcept : m_handle(handle) {}

    FindHandle(const FindHandle&) = delete;
    FindHandle& operator=(const FindHandle&) = delete;
    FindHandle(FindHandle&&) = delete;
    FindHandle& operator=(FindHandle&&) = delete;

    ~FindHandle() {
        if (m_handle != INVALID_HANDLE_VALUE) {
            FindClose(m_handle);
        }
    }

    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }

private:
    HANDLE m_handle;
};

// UTF-16 to UTF-8. An unpaired surrogate becomes U+FFFD and clears `valid`.
std::string toUtf8(const std::wstring& text, bool* valid = nullptr) {
    if (valid != nullptr) {
        *valid = true;
    }
    if (text.empty()) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    DWORD flags = WC_ERR_INVALID_CHARS;
    int size = WideCharToMultiByte(CP_UTF8, flags, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (size == 0) {
        if (valid != nullptr) {
            *valid = false;
        }
        flags = 0;
        size = WideCharToMultiByte(CP_UTF8, flags, text.data(), length, nullptr, 0, nullptr, nullptr);
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, flags, text.data(), length, result.data(), size, nullptr, nullptr);
    return result;
}

// UTF-8 (already checked) to UTF-16.
std::wstring toWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), length, result.data(), size);
    return result;
}

// `path` without a "\\?\" or "\\?\UNC\" prefix, so it normalizes like any other path.
std::string_view withoutVerbatimPrefix(std::string_view path, std::string& storage) {
    const auto startsWith = [&](std::string_view prefix) {
        return path.size() >= prefix.size() &&
               std::equal(prefix.begin(), prefix.end(), path.begin(), [](char a, char b) {
                   return a == b || ((a == '\\' || a == '/') && (b == '\\' || b == '/')) ||
                          (a >= 'a' && a <= 'z' && b == a - 'a' + 'A');
               });
    };
    if (startsWith(R"(\\?\unc\)")) {
        storage = "//" + std::string(path.substr(8));
        return storage;
    }
    if (startsWith(R"(\\?\)")) {
        return path.substr(4);
    }
    return path;
}

// The operating system's text for `error`, without the trailing line break.
std::string systemMessage(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    if (length == 0 || buffer == nullptr) {
        return "Windows error " + std::to_string(error);
    }
    std::string text = toUtf8(std::wstring(buffer, length));
    LocalFree(buffer);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.')) {
        text.pop_back();
    }
    return text;
}

const char* codeFor(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
            return "fs.not_found";
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            return "fs.already_exists";
        case ERROR_ACCESS_DENIED:
        case ERROR_WRITE_PROTECT:
        case ERROR_PRIVILEGE_NOT_HELD:
            return "fs.access_denied";
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return "fs.busy";
        case ERROR_DIRECTORY:
            return "fs.not_a_directory";
        case ERROR_DIR_NOT_EMPTY:
            return "fs.not_empty";
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_FILENAME_EXCED_RANGE:
            return "fs.invalid_path";
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return "fs.no_space";
        case ERROR_NOT_SUPPORTED:
        case ERROR_CALL_NOT_IMPLEMENTED:
        case ERROR_INVALID_FUNCTION:
            return "fs.unsupported";
        default:
            return "fs.io";
    }
}

Unexpected<Error> windowsError(std::string_view what, std::string_view path, DWORD error) {
    return pathError(codeFor(error), what, path, systemMessage(error));
}

// A checked, normalized path in the form the wide functions take: "\\?\C:\..." or
// "\\?\UNC\server\share\..." when absolute, with '\' separators.
struct NativePath {
    std::string utf8;
    std::wstring wide;
};

Expected<NativePath> nativePath(std::string_view path) {
    std::string storage;
    Expected<std::string> normalized = detail::checkedPath(withoutVerbatimPrefix(path, storage));
    if (!normalized) {
        return makeUnexpected(std::move(normalized).error());
    }
    std::wstring wide = toWide(*normalized);
    std::ranges::replace(wide, L'/', L'\\');
    if (normalized->starts_with("//")) {
        wide = L"\\\\?\\UNC\\" + wide.substr(2);
    } else if (rootLength(*normalized) == 3) {
        wide = L"\\\\?\\" + wide;
    }
    return NativePath{.utf8 = std::move(*normalized), .wide = std::move(wide)};
}

constexpr std::int64_t kUnixEpochInFileTime = 116444736000000000; // 1970-01-01 in 100 ns since 1601

std::int64_t toUnixNanoseconds(const FILETIME& time) noexcept {
    const auto ticks =
        static_cast<std::int64_t>((static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime);
    return (ticks - kUnixEpochInFileTime) * 100;
}

std::uint64_t sizeOf(DWORD high, DWORD low) noexcept {
    return (static_cast<std::uint64_t>(high) << 32) | low;
}

bool isLinkTag(DWORD attributes, DWORD reparseTag) noexcept {
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
           (reparseTag == IO_REPARSE_TAG_SYMLINK || reparseTag == IO_REPARSE_TAG_MOUNT_POINT);
}

// What a link at `path` points to: FileLink or DirectoryLink with the target's size and time, or
// Other when the target is missing.
FileInfo linkTargetInfo(const std::wstring& path) {
    const FileHandle target(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    BY_HANDLE_FILE_INFORMATION information{};
    if (!target.valid() || GetFileInformationByHandle(target.get(), &information) == FALSE) {
        return FileInfo{};
    }
    const bool directory = (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return FileInfo{.kind = directory ? FileKind::DirectoryLink : FileKind::FileLink,
                    .size = directory ? 0 : sizeOf(information.nFileSizeHigh, information.nFileSizeLow),
                    .modificationTime = toUnixNanoseconds(information.ftLastWriteTime)};
}

// The kind of an entry that is not a link.
FileKind plainKind(DWORD attributes) noexcept {
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return FileKind::Directory;
    }
    return (attributes & FILE_ATTRIBUTE_DEVICE) != 0 ? FileKind::Other : FileKind::File;
}

// The reparse tag of the entry at `path`, 0 when it can't be read.
DWORD reparseTagOf(const std::wstring& path) {
    const FileHandle entry(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                       nullptr));
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!entry.valid() || GetFileInformationByHandleEx(entry.get(), FileAttributeTagInfo, &tag, sizeof(tag)) == FALSE) {
        return 0;
    }
    return tag.ReparseTag;
}

// The volume and file id of `path`, following links; false when it can't be opened.
bool fileIdentity(const std::wstring& path, BY_HANDLE_FILE_INFORMATION& information) {
    const FileHandle file(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    return file.valid() && GetFileInformationByHandle(file.get(), &information) != FALSE;
}

bool sameFile(const std::wstring& a, const std::wstring& b) {
    BY_HANDLE_FILE_INFORMATION first{};
    BY_HANDLE_FILE_INFORMATION second{};
    return fileIdentity(a, first) && fileIdentity(b, second) &&
           first.dwVolumeSerialNumber == second.dwVolumeSerialNumber && first.nFileIndexHigh == second.nFileIndexHigh &&
           first.nFileIndexLow == second.nFileIndexLow;
}

// Deletes a temporary file unless it was renamed into place.
class TemporaryFile {
public:
    explicit TemporaryFile(std::wstring path) : m_path(std::move(path)) {}

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    TemporaryFile(TemporaryFile&&) = delete;
    TemporaryFile& operator=(TemporaryFile&&) = delete;

    ~TemporaryFile() {
        if (!m_kept) {
            DeleteFileW(m_path.c_str());
        }
    }

    [[nodiscard]] const std::wstring& path() const noexcept { return m_path; }

    void keep() noexcept { m_kept = true; }

private:
    std::wstring m_path;
    bool m_kept = false;
};

// Renames `temporary` over `target`, retrying sharing violations and access denials for about
// 200 ms: virus scanners and the search indexer open new files for a moment.
Expected<void> replaceWithRetry(const std::wstring& temporary, const NativePath& target, DWORD flags) {
    constexpr auto kRetryTime = std::chrono::milliseconds(200);
    const auto start = std::chrono::steady_clock::now();
    DWORD delay = 1;
    for (;;) {
        if (MoveFileExW(temporary.c_str(), target.wide.c_str(), flags) != FALSE) {
            return {};
        }
        const DWORD error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) {
            return windowsError("can't replace", target.utf8, error);
        }
        const DWORD attributes = GetFileAttributesW(target.wide.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return pathError("fs.is_a_directory", "can't replace", target.utf8, "it is a directory");
        }
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
            return pathError("fs.access_denied", "can't replace", target.utf8, "the file is read-only");
        }
        if (std::chrono::steady_clock::now() - start >= kRetryTime) {
            return pathError("fs.busy", "can't replace", target.utf8,
                             "another program keeps the file open (" + systemMessage(error) + ")");
        }
        Sleep(delay);
        delay = std::min<DWORD>(delay * 2, 50);
    }
}

Expected<std::string> knownFolder(const KNOWNFOLDERID& id, std::string_view name) {
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(result) || raw == nullptr) {
        CoTaskMemFree(raw);
        return makeError("fs.not_found", "can't find the " + std::string(name) + " folder");
    }
    const std::string path = toUtf8(std::wstring(raw));
    CoTaskMemFree(raw);
    std::string storage;
    return normalizePath(withoutVerbatimPrefix(path, storage));
}

} // namespace

Expected<std::string> readFile(std::string_view path, std::size_t maxSize) {
    Expected<NativePath> native = nativePath(path);
    if (!native) {
        return makeUnexpected(std::move(native).error());
    }
    const FileHandle file(CreateFileW(native->wide.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid()) {
        const DWORD error = GetLastError();
        const DWORD attributes = GetFileAttributesW(native->wide.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return pathError("fs.is_a_directory", "can't read", native->utf8, "it is a directory");
        }
        return windowsError("can't open", native->utf8, error);
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file.get(), &size) == FALSE) {
        return windowsError("can't get the size of", native->utf8, GetLastError());
    }
    const auto bytesInFile = static_cast<std::uint64_t>(size.QuadPart);
    if (bytesInFile > maxSize) {
        return makeError("fs.too_large", "'" + native->utf8 + "' has " + std::to_string(bytesInFile) +
                                             " bytes; the limit is " + std::to_string(maxSize));
    }
    std::string bytes(static_cast<std::size_t>(bytesInFile), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, std::size_t{1} << 30));
        DWORD read = 0;
        if (ReadFile(file.get(), bytes.data() + done, chunk, &read, nullptr) == FALSE) {
            return windowsError("can't read", native->utf8, GetLastError());
        }
        if (read == 0) {
            break; // the file got shorter
        }
        done += read;
    }
    bytes.resize(done);
    return bytes;
}

Expected<void> writeFileAtomic(std::string_view path, std::string_view bytes, const WriteOptions& options) {
    Expected<NativePath> target = nativePath(path);
    if (!target) {
        return makeUnexpected(std::move(target).error());
    }
    const std::string_view name = fileName(target->utf8);
    if (name.empty() || name == "." || name == "..") {
        return pathError("fs.invalid_path", "can't write", target->utf8, "the path has no file name");
    }
    const DWORD attributes = GetFileAttributesW(target->wide.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return pathError("fs.is_a_directory", "can't write", target->utf8, "it is a directory");
    }
    if (const std::string_view parent = parentPath(target->utf8); options.createParents && !parent.empty()) {
        if (Expected<void> created = createDirectories(parent); !created) {
            return created;
        }
    }
    const std::string temporaryUtf8 = detail::temporaryPathFor(target->utf8);
    Expected<NativePath> temporaryPath = nativePath(temporaryUtf8);
    if (!temporaryPath) {
        return makeUnexpected(std::move(temporaryPath).error());
    }
    const bool durable = options.durability == Durability::Durable;
    TemporaryFile temporary(std::move(temporaryPath->wide));
    {
        FileHandle file(CreateFileW(temporary.path().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.valid()) {
            const DWORD error = GetLastError();
            temporary.keep(); // nothing was created
            return windowsError("can't create", temporaryUtf8, error);
        }
        std::size_t done = 0;
        while (done < bytes.size()) {
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, std::size_t{1} << 30));
            DWORD written = 0;
            if (WriteFile(file.get(), bytes.data() + done, chunk, &written, nullptr) == FALSE) {
                return windowsError("can't write", temporaryUtf8, GetLastError());
            }
            done += written;
        }
        if (detail::faultAt(detail::kFaultAfterTemporaryWrite)) {
            return detail::injectedFault(temporaryUtf8);
        }
        if (durable && FlushFileBuffers(file.get()) == FALSE) {
            return windowsError("can't flush", temporaryUtf8, GetLastError());
        }
        if (detail::faultAt(detail::kFaultAfterFlush)) {
            return detail::injectedFault(temporaryUtf8);
        }
        if (!file.close()) {
            return windowsError("can't close", temporaryUtf8, GetLastError());
        }
    }
    if (detail::faultAt(detail::kFaultBeforeRename)) {
        return detail::injectedFault(temporaryUtf8);
    }
    const auto flags = static_cast<DWORD>(
        durable ? MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
                : MOVEFILE_REPLACE_EXISTING); // NOLINT(bugprone-signed-bitwise): Win32 flag macros are int
    if (Expected<void> replaced = replaceWithRetry(temporary.path(), *target, flags); !replaced) {
        return replaced;
    }
    temporary.keep();
    return {};
}

Expected<std::optional<FileInfo>> fileInfo(std::string_view path) {
    Expected<NativePath> native = nativePath(path);
    if (!native) {
        return makeUnexpected(std::move(native).error());
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(native->wide.c_str(), GetFileExInfoStandard, &data) == FALSE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_INVALID_NAME ||
            error == ERROR_DIRECTORY || error == ERROR_BAD_NETPATH || error == ERROR_BAD_NET_NAME) {
            return std::optional<FileInfo>();
        }
        return windowsError("can't get information about", native->utf8, error);
    }
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
        isLinkTag(data.dwFileAttributes, reparseTagOf(native->wide))) {
        return std::optional<FileInfo>(linkTargetInfo(native->wide));
    }
    const FileKind kind = plainKind(data.dwFileAttributes);
    return std::optional<FileInfo>(
        FileInfo{.kind = kind,
                 .size = kind == FileKind::File ? sizeOf(data.nFileSizeHigh, data.nFileSizeLow) : 0,
                 .modificationTime = toUnixNanoseconds(data.ftLastWriteTime)});
}

Expected<std::vector<DirectoryEntry>> listDirectory(std::string_view path) {
    Expected<NativePath> native = nativePath(path);
    if (!native) {
        return makeUnexpected(std::move(native).error());
    }
    const DWORD attributes = GetFileAttributesW(native->wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return windowsError("can't list", native->utf8, GetLastError());
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return pathError("fs.not_a_directory", "can't list", native->utf8, "it is not a directory");
    }
    std::wstring prefix = native->wide;
    if (prefix.back() != L'\\') {
        prefix += L'\\';
    }
    std::vector<DirectoryEntry> entries;
    WIN32_FIND_DATAW data{};
    const FindHandle search(FindFirstFileExW((prefix + L'*').c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                             nullptr, FIND_FIRST_EX_LARGE_FETCH));
    if (search.get() == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            return entries; // an empty drive root has no "." either
        }
        return windowsError("can't list", native->utf8, error);
    }
    do {
        const std::wstring_view name(data.cFileName);
        if (name == L"." || name == L"..") {
            continue;
        }
        DirectoryEntry entry;
        entry.name = toUtf8(std::wstring(name), &entry.validName);
        if (isLinkTag(data.dwFileAttributes, data.dwReserved0)) {
            const FileInfo target = linkTargetInfo(prefix + std::wstring(name));
            entry.kind = target.kind;
            entry.size = target.size;
        } else {
            entry.kind = plainKind(data.dwFileAttributes);
            entry.size = entry.kind == FileKind::File ? sizeOf(data.nFileSizeHigh, data.nFileSizeLow) : 0;
        }
        entries.push_back(std::move(entry));
    } while (FindNextFileW(search.get(), &data) != FALSE);
    if (const DWORD error = GetLastError(); error != ERROR_NO_MORE_FILES) {
        return windowsError("can't list", native->utf8, error);
    }
    return entries;
}

Expected<void> remove(std::string_view path) {
    Expected<NativePath> native = nativePath(path);
    if (!native) {
        return makeUnexpected(std::move(native).error());
    }
    const DWORD attributes = GetFileAttributesW(native->wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return windowsError("can't delete", native->utf8, GetLastError());
    }
    // A directory link carries the directory attribute and is removed like an empty directory.
    const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const BOOL removed = directory ? RemoveDirectoryW(native->wide.c_str()) : DeleteFileW(native->wide.c_str());
    if (removed == FALSE) {
        return windowsError("can't delete", native->utf8, GetLastError());
    }
    return {};
}

Expected<void> move(std::string_view from, std::string_view to) {
    Expected<NativePath> source = nativePath(from);
    if (!source) {
        return makeUnexpected(std::move(source).error());
    }
    Expected<NativePath> target = nativePath(to);
    if (!target) {
        return makeUnexpected(std::move(target).error());
    }
    if (MoveFileExW(source->wide.c_str(), target->wide.c_str(), 0) != FALSE) {
        return {};
    }
    const DWORD error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
        return windowsError("can't move '" + source->utf8 + "' to", target->utf8, error);
    }
    // Only a change of case of the same file may go ahead over an existing name.
    const bool caseOnly = CompareStringOrdinal(source->wide.c_str(), -1, target->wide.c_str(), -1, TRUE) == CSTR_EQUAL;
    if (!caseOnly || !sameFile(source->wide, target->wide)) {
        return windowsError("can't move '" + source->utf8 + "' to", target->utf8, error);
    }
    if (MoveFileExW(source->wide.c_str(), target->wide.c_str(), MOVEFILE_REPLACE_EXISTING) == FALSE) {
        return windowsError("can't move '" + source->utf8 + "' to", target->utf8, GetLastError());
    }
    return {};
}

namespace detail {

Expected<void> makeDirectory(std::string_view path) {
    Expected<NativePath> native = nativePath(path);
    if (!native) {
        return makeUnexpected(std::move(native).error());
    }
    if (CreateDirectoryW(native->wide.c_str(), nullptr) == FALSE) {
        return windowsError("can't create the directory", native->utf8, GetLastError());
    }
    return {};
}

} // namespace detail

Expected<std::string> executableDirectory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return makeError("fs.not_found", "can't find the executable: " + systemMessage(GetLastError()));
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    const std::string path = toUtf8(buffer);
    std::string storage;
    const std::string normalized = normalizePath(withoutVerbatimPrefix(path, storage));
    return std::string(parentPath(normalized));
}

Expected<std::string> userConfigDirectory() {
    return knownFolder(FOLDERID_RoamingAppData, "roaming application data");
}

Expected<std::string> userCacheDirectory() {
    return knownFolder(FOLDERID_LocalAppData, "local application data");
}

} // namespace ith::fs

#endif // _WIN32
