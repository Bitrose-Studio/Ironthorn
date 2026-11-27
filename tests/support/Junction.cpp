#include "Junction.h"

#ifdef _WIN32

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

namespace ith::test {

namespace {

std::wstring wide(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    std::ranges::replace(result, L'/', L'\\');
    return result;
}

// The header of a mount point's REPARSE_DATA_BUFFER (ntifs.h, not in the user-mode headers).
struct MountPointHeader {
    DWORD reparseTag;
    WORD reparseDataLength;
    WORD reserved;
    WORD substituteNameOffset;
    WORD substituteNameLength;
    WORD printNameOffset;
    WORD printNameLength;
};

} // namespace

bool makeJunction(const std::string& link, const std::string& target) {
    const std::wstring linkPath = wide(link);
    std::wstring printName = wide(target);
    while (printName.size() > 3 && printName.back() == L'\\') {
        printName.pop_back();
    }
    const std::wstring substituteName = LR"(\??\)" + printName;
    if (CreateDirectoryW(linkPath.c_str(), nullptr) == FALSE) {
        return false;
    }
    const std::size_t substituteBytes = substituteName.size() * sizeof(wchar_t);
    const std::size_t printBytes = printName.size() * sizeof(wchar_t);
    // Both names with their terminating NUL, after the header.
    std::vector<std::byte> buffer(sizeof(MountPointHeader) + substituteBytes + printBytes + 2 * sizeof(wchar_t));
    MountPointHeader header{};
    header.reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    header.reparseDataLength = static_cast<WORD>(buffer.size() - 8); // what follows the first 8 bytes
    header.substituteNameOffset = 0;
    header.substituteNameLength = static_cast<WORD>(substituteBytes);
    header.printNameOffset = static_cast<WORD>(substituteBytes + sizeof(wchar_t));
    header.printNameLength = static_cast<WORD>(printBytes);
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::memcpy(buffer.data() + sizeof(header), substituteName.data(), substituteBytes);
    std::memcpy(buffer.data() + sizeof(header) + substituteBytes + sizeof(wchar_t), printName.data(), printBytes);
    const HANDLE folder = CreateFileW(linkPath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (folder == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD returned = 0;
    const BOOL set = DeviceIoControl(folder, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()),
                                     nullptr, 0, &returned, nullptr);
    CloseHandle(folder);
    return set != FALSE;
}

} // namespace ith::test

#else

namespace ith::test {

bool makeJunction(const std::string& /*link*/, const std::string& /*target*/) {
    return false;
}

} // namespace ith::test

#endif
