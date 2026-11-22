// A lint fixture, never compiled: file I/O outside engine/ith/file_system. The
// file_io_confined_refuses_file_io test expects the check to report all four uses; the
// file_io_confined test checks engine/ith only.

#include <filesystem>
#include <fstream>

void openTwice() {
    FILE* file = fopen("a.txt", "rb");
    HANDLE handle = CreateFileW(L"a.txt", 0, 0, nullptr, OPEN_EXISTING, 0, nullptr);
}
