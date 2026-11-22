# Files and directories on the operating system's own APIs (ADR-0035): UTF-8 paths and the
# lexical path helpers, whole-file reads, atomic writes with a durability choice, file
# information, directory listings, directory creation, remove, a move that never replaces, and
# the known folders. A runtime module: the dedicated server, the cooker and the editor all need
# files. The one place for file I/O under engine/ith (the file_io_confined test). Both
# implementations are built everywhere; each is empty on the other platform.
if(WIN32)
    set(ith_file_system_libraries shell32 ole32)
else()
    set(ith_file_system_libraries "")
endif()

ith_add_module(file_system
    LAYER platform
    SOURCES
        FileSystem.h
        FileSystem.cpp
        FileSystemInternal.h
        FileSystemPosix.cpp
        FileSystemWindows.cpp
        Path.h
        Path.cpp
    DEPENDS
        foundation
    PRIVATE_LIBRARIES
        ${ith_file_system_libraries})
