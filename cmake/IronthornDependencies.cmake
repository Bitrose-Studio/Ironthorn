# Third-party dependencies.
#
# Every dependency is an archive pinned by URL and SHA-256, so a build never depends on a
# mutable branch or tag. To add or upgrade one: compute the hash with scripts/hash-url.ps1,
# update the entry here, and record license and purpose in docs/development/third-party.md.
# Dependencies stay behind engine interfaces; their types never appear in public engine APIs.

include(FetchContent)

# ith_declare_dependency(<name> URL <url> SHA256 <hash> [SOURCE_SUBDIR <dir>])
function(ith_declare_dependency name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "URL;SHA256;SOURCE_SUBDIR" "")
    set(extra_arguments "")
    if(arg_SOURCE_SUBDIR)
        list(APPEND extra_arguments SOURCE_SUBDIR "${arg_SOURCE_SUBDIR}")
    endif()
    FetchContent_Declare(${name}
        URL "${arg_URL}"
        URL_HASH "SHA256=${arg_SHA256}"
        DOWNLOAD_EXTRACT_TIMESTAMP FALSE
        EXCLUDE_FROM_ALL
        SYSTEM
        ${extra_arguments})
endfunction()

# Sets a cache option of a dependency before its project is added.
macro(ith_dependency_option name value)
    set(${name} ${value} CACHE INTERNAL "")
endmacro()

# --- doctest: unit test framework (MIT) -------------------------------------------------
if(IRONTHORN_BUILD_TESTS)
    ith_declare_dependency(doctest
        URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
        SHA256 174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331)
    ith_dependency_option(DOCTEST_WITH_TESTS OFF)
    ith_dependency_option(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF)
    ith_dependency_option(DOCTEST_NO_INSTALL ON)
    FetchContent_MakeAvailable(doctest)
endif()

# --- Slang and DXC: the shader compiler (tools only, never linked into games) ----------
# Both are their publishers' prebuilt releases: building either from source takes LLVM and far
# longer than the engine. Slang is linked as a shared library and loads DXC's dxcompiler and
# dxil libraries from its own directory to write DXIL. Their exceptions and RTTI stay inside
# these shared libraries; Ironthorn code only calls Slang's COM-style interface (ADR-0011).
#
# ITH_SHADER_COMPILER_RUNTIME_FILES lists the shared libraries that must sit next to an
# executable that compiles shaders; ith_copy_shader_compiler_runtime() puts them there. Slang's
# standard-module folder is not copied: it is needed only once shaders get `import` search
# paths.
if(IRONTHORN_BUILD_TOOLS)
    set(ith_slang_version 2026.22.1)
    set(ith_dxc_version 1.9.2609)
    set(ith_slang_url https://github.com/shader-slang/slang/releases/download/v${ith_slang_version})
    set(ith_dxc_url https://github.com/microsoft/DirectXShaderCompiler/releases/download/v${ith_dxc_version})
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
        message(FATAL_ERROR "The shader compiler tools are available for x86-64 only. "
            "Configure with -DIRONTHORN_BUILD_TOOLS=OFF to build without them.")
    endif()
    if(WIN32)
        ith_declare_dependency(slang
            URL ${ith_slang_url}/slang-${ith_slang_version}-windows-x86_64.zip
            SHA256 e72344f7b0323cec8b039c534caaf4680e7ba629489fc796ea4c7f7adb7deb68)
        ith_declare_dependency(dxc
            URL ${ith_dxc_url}/dxc_2026_09_29.zip
            SHA256 ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Slang's "glibc-2.28" archive is its build without the LLVM-based CPU back end, which
        # the engine doesn't use and which makes the other archive almost three times larger.
        # Its name is not the toolchain minimum: DXC's Linux libraries need glibc 2.38
        # (docs/development/building.md).
        ith_declare_dependency(slang
            URL ${ith_slang_url}/slang-${ith_slang_version}-linux-x86_64-glibc-2.28.tar.gz
            SHA256 0c0e243ac15b7878237e6854313f45c791408227d5bc634f3ad1ac487edbfaf5)
        ith_declare_dependency(dxc
            URL ${ith_dxc_url}/linux_dxc_2026_09_28.x86_x64.tar.gz
            SHA256 96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1)
    else()
        message(FATAL_ERROR "The shader compiler tools are available on Windows and Linux only. "
            "Configure with -DIRONTHORN_BUILD_TOOLS=OFF to build without them.")
    endif()
    FetchContent_MakeAvailable(slang dxc)

    add_library(ith_slang SHARED IMPORTED GLOBAL)
    add_library(ith::slang ALIAS ith_slang)
    target_include_directories(ith_slang SYSTEM INTERFACE "${slang_SOURCE_DIR}/include")
    if(WIN32)
        set_target_properties(ith_slang PROPERTIES
            IMPORTED_LOCATION "${slang_SOURCE_DIR}/bin/slang-compiler.dll"
            IMPORTED_IMPLIB "${slang_SOURCE_DIR}/lib/slang-compiler.lib")
        set(ITH_SHADER_COMPILER_RUNTIME_FILES
            "${slang_SOURCE_DIR}/bin/slang-compiler.dll"
            "${slang_SOURCE_DIR}/bin/slang-glslang.dll"
            "${dxc_SOURCE_DIR}/bin/x64/dxcompiler.dll"
            "${dxc_SOURCE_DIR}/bin/x64/dxil.dll")
    else()
        set(ith_slang_library libslang-compiler.so.0.${ith_slang_version})
        set(ith_dxc_libraries "${dxc_SOURCE_DIR}/lib/libdxcompiler.so" "${dxc_SOURCE_DIR}/lib/libdxil.so")
        set_target_properties(ith_slang PROPERTIES
            IMPORTED_LOCATION "${slang_SOURCE_DIR}/lib/${ith_slang_library}"
            IMPORTED_SONAME "${ith_slang_library}")
        set(ITH_SHADER_COMPILER_RUNTIME_FILES
            "${slang_SOURCE_DIR}/lib/${ith_slang_library}"
            "${slang_SOURCE_DIR}/lib/libslang-glslang-${ith_slang_version}.so"
            ${ith_dxc_libraries})
    endif()
    foreach(file IN LISTS ITH_SHADER_COMPILER_RUNTIME_FILES)
        if(NOT EXISTS "${file}")
            message(FATAL_ERROR "The shader compiler release has no ${file}; correct cmake/IronthornDependencies.cmake.")
        endif()
    endforeach()

    # ith_copy_shader_compiler_runtime(<target>)
    #
    # Copies Slang's and DXC's shared libraries next to the executable <target> as a post-build
    # step, so it runs from the build tree without PATH or LD_LIBRARY_PATH changes. The copy runs
    # whenever <target> is relinked. On Linux the executable loads them from its own directory
    # only, and Slang finds DXC there through its own $ORIGIN run path.
    function(ith_copy_shader_compiler_runtime target)
        set(commands
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${ITH_SHADER_COMPILER_RUNTIME_FILES} "$<TARGET_FILE_DIR:${target}>")
        if(NOT WIN32)
            # libdxcompiler.so looks for libdxil.so (whose soname is libdxildll.so) through its own
            # run path, $ORIGIN/../lib. A second copy of both there lets DXC find its validator
            # however Slang loaded DXC.
            list(APPEND commands
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/../lib"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${ith_dxc_libraries} "$<TARGET_FILE_DIR:${target}>/../lib")
            set_target_properties(${target} PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            ${commands}
            COMMENT "Copying the shader compiler runtime next to ${target}"
            VERBATIM)
    endfunction()
endif()

# --- SDL3: windows, events and input devices (zlib) --------------------------------------
# Static and private to the platform module (ADR-0012); its headers appear only in platform
# source files. Built from the maintainers' release archive with the engine's toolchain flags,
# so sanitizer builds instrument it too. Audio, rendering, GPU, camera, tray, haptics, sensors
# and OpenGL are off: audio has its own library, the RHI renders, and gamepad rumble and sensors
# don't need SDL's haptic and sensor subsystems. On Linux, X11 and Wayland support are compiled
# in only when their development packages are installed (docs/development/building.md).
if(IRONTHORN_BUILD_CLIENT)
    ith_declare_dependency(sdl3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
        SHA256 f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f)
    ith_dependency_option(SDL_SHARED OFF)
    ith_dependency_option(SDL_STATIC ON)
    ith_dependency_option(SDL_TEST_LIBRARY OFF)
    ith_dependency_option(SDL_TESTS OFF)
    ith_dependency_option(SDL_EXAMPLES OFF)
    ith_dependency_option(SDL_INSTALL OFF)
    ith_dependency_option(SDL_UNINSTALL OFF)
    ith_dependency_option(SDL_AUDIO OFF)
    ith_dependency_option(SDL_RENDER OFF)
    ith_dependency_option(SDL_GPU OFF)
    ith_dependency_option(SDL_CAMERA OFF)
    ith_dependency_option(SDL_TRAY OFF)
    ith_dependency_option(SDL_HAPTIC OFF)
    ith_dependency_option(SDL_SENSOR OFF)
    ith_dependency_option(SDL_OPENGL OFF)
    ith_dependency_option(SDL_OPENGLES OFF)
    ith_dependency_option(SDL_VULKAN ON)
    ith_dependency_option(SDL_DIALOG ON)
    ith_dependency_option(SDL_POWER ON)
    ith_dependency_option(SDL_JOYSTICK ON)
    ith_dependency_option(SDL_HIDAPI ON)
    FetchContent_MakeAvailable(sdl3)
endif()
