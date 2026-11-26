# Third-party dependencies.
#
# Every dependency is an archive pinned by URL and SHA-256, so a build never depends on a
# mutable branch or tag. To add or upgrade one: compute the hash with scripts/hash-url.ps1,
# update the entry here, and record license and purpose in docs/development/third-party.md.
# Dependencies stay behind engine interfaces; their types never appear in public engine APIs.

include(FetchContent)

# ith_declare_dependency(<name> URL <url> SHA256 <hash> [SOURCE_SUBDIR <dir>] [DOWNLOAD_NAME <file>])
#
# DOWNLOAD_NAME names the downloaded file. CMake unpacks an archive only when its name ends in a
# known archive extension, so a zip archive under another name (a NuGet .nupkg) is downloaded as
# <something>.zip.
function(ith_declare_dependency name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "URL;SHA256;SOURCE_SUBDIR;DOWNLOAD_NAME" "")
    set(extra_arguments "")
    if(arg_SOURCE_SUBDIR)
        list(APPEND extra_arguments SOURCE_SUBDIR "${arg_SOURCE_SUBDIR}")
    endif()
    if(arg_DOWNLOAD_NAME)
        list(APPEND extra_arguments DOWNLOAD_NAME "${arg_DOWNLOAD_NAME}")
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

# --- yyjson: JSON reader and writer (MIT) ------------------------------------------------
# Private to the serialization module (ADR-0010); only its Json.cpp includes yyjson.h. Built as
# a static library from the release archive with the engine's toolchain flags, so sanitizer
# builds instrument it too. File I/O and the incremental reader are off (the engine does its own
# I/O), and nesting is limited to the engine's kMaxDepth (64) in yyjson's reader and writer as
# well as in the engine's own code.
ith_declare_dependency(yyjson
    URL https://github.com/ibireme/yyjson/archive/refs/tags/0.13.0.tar.gz
    SHA256 34e0f62a2bc11ab20d601e8ca1cc2b2079503aa45119a19133d89d19b94a0fae)
ith_dependency_option(YYJSON_INSTALL OFF)
ith_dependency_option(YYJSON_BUILD_TESTS OFF)
ith_dependency_option(YYJSON_BUILD_FUZZER OFF)
ith_dependency_option(YYJSON_BUILD_MISC OFF)
ith_dependency_option(YYJSON_BUILD_DOC OFF)
ith_dependency_option(YYJSON_DISABLE_FILE ON)
ith_dependency_option(YYJSON_DISABLE_INCR_READER ON)
ith_dependency_option(YYJSON_READER_DEPTH_LIMIT 64)
ith_dependency_option(YYJSON_WRITER_DEPTH_LIMIT 64)
FetchContent_MakeAvailable(yyjson)

# --- glm: vectors, matrices and quaternions (MIT) ------------------------------------------
# Header only, behind the math module's aliases (ADR-0006). glm's own CMakeLists.txt is not used:
# it sets directory-wide compile options and builds a static library of every header. Its util/
# directory has no CMakeLists.txt, so as SOURCE_SUBDIR it makes FetchContent only unpack the
# archive. The configuration defines are interface definitions of ith::glm, so every translation
# unit that can include glm sees the same configuration (ADR-0006 rule 3). Never add a SIMD or
# alignment define (GLM_FORCE_INTRINSICS, GLM_FORCE_SSE*, GLM_FORCE_AVX*,
# GLM_FORCE_DEFAULT_ALIGNED_GENTYPES): each changes layout or constexpr-ness. Only the math module
# links ith::glm, and only its files include glm (the glm_includes test).
ith_declare_dependency(glm
    URL https://github.com/g-truc/glm/archive/refs/tags/1.0.3.tar.gz
    SHA256 6775e47231a446fd086d660ecc18bcd076531cfedd912fbd66e576b118607001
    SOURCE_SUBDIR util)
FetchContent_MakeAvailable(glm)
add_library(ith_glm INTERFACE)
add_library(ith::glm ALIAS ith_glm)
target_include_directories(ith_glm SYSTEM INTERFACE "${glm_SOURCE_DIR}")
target_compile_definitions(ith_glm INTERFACE
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_CTOR_INIT
    GLM_FORCE_XYZW_ONLY
    GLM_FORCE_QUAT_DATA_XYZW)

# --- xxHash: 128-bit content hashes for the derived-data cache (BSD-2-Clause) -------------
# Header only and private to asset_database (ADR-0015 rule 11): Hash128.cpp includes xxhash.h
# with XXH_INLINE_ALL, so no library is built. The archive has no top-level CMakeLists.txt, so
# FetchContent only unpacks it; its command-line tool (cli/, GPL-2.0) is never compiled.
if(IRONTHORN_BUILD_TOOLS)
    ith_declare_dependency(xxhash
        URL https://github.com/Cyan4973/xxHash/archive/refs/tags/v0.8.4.tar.gz
        SHA256 5738270935e7c3d38a79b3adf7c9692566ce7895a25f67de43ad52ab504acd32)
    FetchContent_MakeAvailable(xxhash)
    add_library(ith_xxhash INTERFACE)
    add_library(ith::xxhash ALIAS ith_xxhash)
    target_include_directories(ith_xxhash SYSTEM INTERFACE "${xxhash_SOURCE_DIR}")
endif()

# --- Tracy: the profiler client (BSD-3-Clause) --------------------------------------------
# Only with IRONTHORN_PROFILING (ADR-0007 rules 11 and 13), and only foundation links it (the
# tracy_confined test). Tracy's own CMake project is not used: it looks for the AMD ROCm profiler
# and would build its integration, the one part of the client that throws. public/ has no
# CMakeLists.txt, so as SOURCE_SUBDIR it makes FetchContent only unpack the archive, and the client
# is built here from its one source file, without exceptions or RTTI and without our warning level.
# The configuration defines are public, because every translation unit that includes Tracy must
# see the same ones: collect only while a viewer is connected, listen on localhost only, don't
# announce the process on the network, install no crash handler (aborts and failed asserts behave
# as in every other build), and fall back to a portable timer on CPUs and virtual machines without
# an invariant TSC instead of ending the process at start-up. TRACY_FIBERS, TRACY_MANUAL_LIFETIME
# and TRACY_ROCPROF stay off: the job system has no fibers, and the profiler starts and stops with
# the process. System tracing and sampling stay on (docs/development/logging.md, "Profiling").
if(IRONTHORN_PROFILING)
    ith_declare_dependency(tracy
        URL https://github.com/wolfpld/tracy/archive/refs/tags/v0.15.0.tar.gz
        SHA256 3d57da54aa67d7875da1e8fecd0bd5957675cef73c27dd52bffb6ab8d290a5e6
        SOURCE_SUBDIR public)
    FetchContent_MakeAvailable(tracy)
    find_package(Threads REQUIRED)
    add_library(ith_tracy STATIC "${tracy_SOURCE_DIR}/public/TracyClient.cpp")
    add_library(ith::tracy ALIAS ith_tracy)
    # A system include: Tracy's headers don't build cleanly at our warning level (-Wextra-semi).
    target_include_directories(ith_tracy SYSTEM PUBLIC "${tracy_SOURCE_DIR}/public")
    # IRONTHORN_PROFILING_ON_DEMAND off drops TRACY_ON_DEMAND: zones are then active without a
    # viewer and their data stays queued in memory, for CI's profiling test runs.
    if(IRONTHORN_PROFILING_ON_DEMAND)
        target_compile_definitions(ith_tracy PUBLIC TRACY_ON_DEMAND)
    endif()
    target_compile_definitions(ith_tracy PUBLIC
        TRACY_ENABLE
        TRACY_ONLY_LOCALHOST
        TRACY_NO_BROADCAST
        TRACY_NO_CRASH_HANDLER
        TRACY_TIMER_FALLBACK)
    if(MSVC)
        target_compile_definitions(ith_tracy PRIVATE _HAS_EXCEPTIONS=0)
        target_compile_options(ith_tracy PRIVATE /EHs-c-)
    else()
        target_compile_options(ith_tracy PRIVATE -fno-exceptions)
    endif()
    target_link_libraries(ith_tracy PRIVATE ith::runtime_options Threads::Threads ${CMAKE_DL_LIBS})
    if(WIN32)
        target_link_libraries(ith_tracy PRIVATE ws2_32 dbghelp advapi32 user32)
    endif()
    set_target_properties(ith_tracy PROPERTIES FOLDER "ThirdParty")
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
if(ITH_BUILD_SHADER_COMPILER)
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

    if(WIN32)
        set(ith_slangc "${slang_SOURCE_DIR}/bin/slangc.exe")
        set(ith_dxc_directory "${dxc_SOURCE_DIR}/bin/x64")
    else()
        set(ith_slangc "${slang_SOURCE_DIR}/bin/slangc")
        set(ith_dxc_directory "${dxc_SOURCE_DIR}/lib")
    endif()
    if(NOT EXISTS "${ith_slangc}")
        message(FATAL_ERROR "The Slang release has no ${ith_slangc}; correct cmake/IronthornDependencies.cmake.")
    endif()

    # ith_embed_shaders(<target> SOURCE <file.slang> ENTRY_POINTS <name>:<vertex|pixel|compute>... [DXIL])
    #
    # Compiles entry points of a self-contained Slang source at build time with the pinned slangc,
    # with the shader_compiler module's settings (ADR-0011: SPIR-V 1.6 checked by the SPIR-V
    # validator and keeping the entry point's name, with DXIL also signed DXIL Shader Model 6.6
    # from the pinned DXC; column-major matrices), into C++ headers in the build tree that
    # <target>'s sources include. For entry point `name` they are <name>.spirv.h and, with DXIL,
    # <name>.dxil.h, defining `const unsigned char k<Name>Spirv[]` and its size
    # `const size_t k<Name>Spirv_sizeInBytes` (and k<Name>Dxil...), where <Name> is the name with an
    # upper-case first letter. Include them inside a namespace that says `using std::size_t;`,
    # since the headers name size_t unqualified. Each target gets its own directory, so two targets
    # may embed entry points of the same name.
    #
    # The SPIR-V compilation defines ITH_SPIRV=1, as the module does, so that one source can reach
    # the RHI's bindless buffers through the Vulkan descriptor set and, for DXIL, through
    # ResourceDescriptorHeap.
    #
    # For samples and tests that need fixed shaders without linking the tools-only compiler. Games
    # load cooked shaders instead (ADR-0011 rule 9; ADR-0023, embedded shaders are compiled at build
    # time with slangc). The options are the engine's shader compile options, listed once in
    # docs/development/shaders.md and used by ShaderCompiler::compile() too: a change here changes
    # that list and the module in the same commit.
    function(ith_embed_shaders target)
        cmake_parse_arguments(PARSE_ARGV 1 arg "DXIL" "SOURCE" "ENTRY_POINTS")
        cmake_path(ABSOLUTE_PATH arg_SOURCE BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE source)
        set(directory "${CMAKE_CURRENT_BINARY_DIR}/embedded_shaders/${target}")
        # slangc does not create it, and only Ninja creates output directories for custom commands.
        file(MAKE_DIRECTORY "${directory}")
        set(outputs "")
        foreach(entry_point IN LISTS arg_ENTRY_POINTS)
            if(NOT entry_point MATCHES "^([A-Za-z_][A-Za-z0-9_]*):(vertex|pixel|compute)$")
                message(FATAL_ERROR "ith_embed_shaders: '${entry_point}' is not <name>:<vertex|pixel|compute>")
            endif()
            set(name "${CMAKE_MATCH_1}")
            set(stage "${CMAKE_MATCH_2}")
            if(stage STREQUAL "pixel")
                set(stage fragment)
            endif()
            string(SUBSTRING "${name}" 0 1 first)
            string(SUBSTRING "${name}" 1 -1 rest)
            string(TOUPPER "${first}" first)
            set(common "${source}" -entry ${name} -stage ${stage} -matrix-layout-column-major -source-embed-style u8)

            set(spirv "${directory}/${name}.spirv.h")
            add_custom_command(
                OUTPUT "${spirv}"
                COMMAND "${ith_slangc}" ${common} -target spirv -profile spirv_1_6 -D ITH_SPIRV=1
                    -fvk-use-entrypoint-name -validate-with-downstream-compiler
                    -source-embed-name k${first}${rest}Spirv -o "${spirv}"
                    -depfile "${spirv}.d"
                DEPENDS "${source}" "${ith_slangc}"
                DEPFILE "${spirv}.d"
                COMMENT "Compiling ${arg_SOURCE}:${name} to SPIR-V"
                VERBATIM)
            list(APPEND outputs "${spirv}")

            if(arg_DXIL)
                # -dxc-path: without it, slangc may load another DXC, e.g. one from PATH.
                set(dxil "${directory}/${name}.dxil.h")
                add_custom_command(
                    OUTPUT "${dxil}"
                    COMMAND "${ith_slangc}" ${common} -target dxil -profile sm_6_6 -dxc-path "${ith_dxc_directory}"
                        -source-embed-name k${first}${rest}Dxil -o "${dxil}"
                        -depfile "${dxil}.d"
                    DEPENDS "${source}" "${ith_slangc}" ${ITH_SHADER_COMPILER_RUNTIME_FILES}
                    DEPFILE "${dxil}.d"
                    COMMENT "Compiling ${arg_SOURCE}:${name} to DXIL"
                    VERBATIM)
                list(APPEND outputs "${dxil}")
            endif()
        endforeach()
        target_sources(${target} PRIVATE ${outputs})
        target_include_directories(${target} PRIVATE "${directory}")
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

# --- Vulkan: Vulkan-Headers (Apache-2.0 OR MIT), Vulkan Memory Allocator (MIT) ----------
# For the rhi_vulkan client module (ADR-0009), on Windows and Linux. Only Vulkan's C API is used,
# without prototypes: rhi_vulkan opens the system's Vulkan loader at run time and fetches its
# functions itself, so no Vulkan library is linked. VMA is one header; rhi_vulkan compiles its
# implementation in one of its own source files, with the engine's flags (no exceptions, no
# RTTI). VMA's include directory has no CMakeLists.txt, so as SOURCE_SUBDIR it makes FetchContent
# only unpack the archive. Both are private to rhi_vulkan.
if(IRONTHORN_BUILD_CLIENT)
    ith_declare_dependency(vulkan_headers
        URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
        SHA256 4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e)
    ith_dependency_option(VULKAN_HEADERS_ENABLE_TESTS OFF)
    ith_dependency_option(VULKAN_HEADERS_ENABLE_INSTALL OFF)
    ith_dependency_option(VULKAN_HEADERS_ENABLE_MODULE OFF)

    ith_declare_dependency(vma
        URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.5.0.tar.gz
        SHA256 c5f3db848fca9cb9726b41f256cc3007773321fa92a86d1e0902e66474e8a9e3
        SOURCE_SUBDIR include)

    FetchContent_MakeAvailable(vulkan_headers vma)

    add_library(ith_vma INTERFACE)
    add_library(ith::vma ALIAS ith_vma)
    target_include_directories(ith_vma SYSTEM INTERFACE "${vma_SOURCE_DIR}/include")
    target_link_libraries(ith_vma INTERFACE Vulkan::Headers)
endif()

# --- Direct3D 12: DirectX-Headers (MIT), D3D12 Memory Allocator (MIT), Agility SDK ---------
# Windows only, for the rhi_d3d12 client module (ADR-0009). DirectX-Headers and the Agility SDK
# have the same version and are always upgraded together: the headers declare exactly the API
# of the D3D12Core.dll the game ships. Both are private to rhi_d3d12.
#
# The Agility SDK is Microsoft's NuGet package of the Direct3D 12 runtime. Only its
# D3D12Core.dll and, outside Shipping, d3d12SDKLayers.dll (the debug layer) are used; its
# license allows them to be redistributed with an application, for use on Windows only
# (docs/development/third-party.md). ith_use_d3d12_agility_sdk() puts them next to an executable.
if(WIN32 AND IRONTHORN_BUILD_CLIENT)
    set(ith_d3d12_version 1.620.2)
    # D3D12SDKVersion, which an executable exports to load this D3D12Core.dll: the minor version.
    string(REGEX REPLACE "^1\\.([0-9]+)\\..*$" "\\1" ITH_D3D12_SDK_VERSION "${ith_d3d12_version}")

    ith_declare_dependency(directx_headers
        URL https://github.com/microsoft/DirectX-Headers/archive/refs/tags/v${ith_d3d12_version}.tar.gz
        SHA256 2b916ef1be5db35313f56d60499274fed39db37b54a1f0befa8aa49041c14855)
    ith_dependency_option(DXHEADERS_BUILD_TEST OFF)
    ith_dependency_option(DXHEADERS_BUILD_GOOGLE_TEST OFF)
    ith_dependency_option(DXHEADERS_INSTALL OFF)

    # D3D12MA is one source file, built here: its own CMake project looks DirectX-Headers up with
    # find_package and installs an export set that needs DirectX-Headers installed too. Its
    # include directory has no CMakeLists.txt, so as SOURCE_SUBDIR it makes FetchContent only
    # unpack the archive.
    ith_declare_dependency(d3d12ma
        URL https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator/archive/refs/tags/v3.3.0.tar.gz
        SHA256 04e3ef68cf2608c14c74a0b0190fb3c5e3cbb62e22fb8282ea62a9411d054133
        SOURCE_SUBDIR include)

    # The package has no CMakeLists.txt, so FetchContent only unpacks it.
    ith_declare_dependency(d3d12_agility_sdk
        URL https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/${ith_d3d12_version}/microsoft.direct3d.d3d12.${ith_d3d12_version}.nupkg
        SHA256 70489669c2bbeaf318b62d72b0ac3e753d521387bfc848c9763791c4e920bfcc
        DOWNLOAD_NAME microsoft.direct3d.d3d12.${ith_d3d12_version}.zip)

    FetchContent_MakeAvailable(directx_headers d3d12ma d3d12_agility_sdk)

    # Against DirectX-Headers instead of the Windows SDK's Direct3D 12 headers, and without
    # exceptions or RTTI like the engine (D3D12MA documents that it uses neither).
    add_library(ith_d3d12ma STATIC "${d3d12ma_SOURCE_DIR}/src/D3D12MemAlloc.cpp")
    add_library(ith::d3d12ma ALIAS ith_d3d12ma)
    target_include_directories(ith_d3d12ma SYSTEM PUBLIC "${d3d12ma_SOURCE_DIR}/include")
    target_compile_definitions(ith_d3d12ma PUBLIC D3D12MA_USING_DIRECTX_HEADERS PRIVATE _HAS_EXCEPTIONS=0)
    target_compile_options(ith_d3d12ma PRIVATE /EHs-c- /GR-)
    target_link_libraries(ith_d3d12ma PUBLIC Microsoft::DirectX-Headers d3d12 dxgi)
    set_target_properties(ith_d3d12ma PROPERTIES FOLDER "ThirdParty")

    # The target architecture of the compiler (x64, ARM64), not of the machine that builds.
    if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
        set(ith_d3d12_bin "${d3d12_agility_sdk_SOURCE_DIR}/build/native/bin/x64")
    elseif(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
        set(ith_d3d12_bin "${d3d12_agility_sdk_SOURCE_DIR}/build/native/bin/arm64")
    else()
        message(FATAL_ERROR "The Direct3D 12 Agility SDK has no runtime for ${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}.")
    endif()
    foreach(file D3D12Core.dll d3d12SDKLayers.dll)
        if(NOT EXISTS "${ith_d3d12_bin}/${file}")
            message(FATAL_ERROR "The Agility SDK package has no ${file}; correct cmake/IronthornDependencies.cmake.")
        endif()
    endforeach()

    # ith_use_d3d12_agility_sdk(<target>)
    #
    # Makes the executable <target> run on the Agility SDK's Direct3D 12 runtime instead of the
    # one in Windows (ADR-0009): it compiles in the exports D3D12SDKVersion and D3D12SDKPath
    # (engine/ith/rhi_d3d12/AgilitySdkExports.cpp; Windows reads them from the executable, never
    # from a library), and copies D3D12Core.dll, plus d3d12SDKLayers.dll outside Release
    # (Shipping), to <executable directory>/D3D12/ after every link. ith_add_application calls it
    # for player and editor applications that link rhi_select; test executables that create
    # Direct3D 12 devices call it themselves.
    function(ith_use_d3d12_agility_sdk target)
        target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/engine/ith/rhi_d3d12/AgilitySdkExports.cpp")
        target_compile_definitions(${target} PRIVATE ITH_D3D12_SDK_VERSION=${ITH_D3D12_SDK_VERSION})
        set(directory "$<TARGET_FILE_DIR:${target}>/D3D12")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${directory}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ith_d3d12_bin}/D3D12Core.dll"
                "$<$<NOT:$<CONFIG:Release>>:${ith_d3d12_bin}/d3d12SDKLayers.dll>"
                "${directory}"
            COMMENT "Copying the Direct3D 12 Agility SDK runtime next to ${target}"
            COMMAND_EXPAND_LISTS
            VERBATIM)
    endfunction()
endif()

# --- Dear ImGui (docking branch): the editor UI (MIT) ------------------------------------
# Editor only and private to the editor_ui module (ADR-0029 rules 1 to 5; the imgui_confined test
# checks that no other module links or includes it). The archive has no top-level CMakeLists.txt,
# so FetchContent only unpacks it; the core sources are built here without exceptions or RTTI and
# without our warning level, like every dependency. The configuration header is a public
# definition (IMGUI_USER_CONFIG), so every translation unit that includes imgui.h sees the same
# configuration: IM_ASSERT is ITH_ASSERT, ImWchar is 32 bits wide, and ImGui has no allocator,
# Win32 or shell functions of its own. Sanitizer builds instrument it too (ADR-0029 rule 19).
if(IRONTHORN_BUILD_EDITOR)
    ith_declare_dependency(imgui
        URL https://github.com/ocornut/imgui/archive/refs/tags/v1.93.0-docking.tar.gz
        SHA256 746c265540bd3aa109570f29492ef4d8f5b421dd9fd0049c35ca35f21496f1a5)
    FetchContent_MakeAvailable(imgui)
    add_library(ith_imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/imgui_demo.cpp")
    add_library(ith::imgui ALIAS ith_imgui)
    target_include_directories(ith_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}")
    target_compile_definitions(ith_imgui PUBLIC "IMGUI_USER_CONFIG=\"ith/editor_ui/ImGuiConfig.h\"")
    # The configuration header includes ith/foundation/Assert.h, found through foundation's include
    # directory; the header itself belongs to editor_ui, which links this target (no cycle).
    target_link_libraries(ith_imgui PUBLIC ith::foundation)
    target_link_libraries(ith_imgui PRIVATE ith::runtime_options)
    set_target_properties(ith_imgui PROPERTIES FOLDER "ThirdParty")

    # Editor fonts: data files under the SIL Open Font License (Inter, JetBrains Mono) and ISC with
    # MIT (Lucide's icon font), not code (docs/development/third-party.md). The archives have no
    # CMakeLists.txt, so FetchContent only unpacks them. The Lucide font archive has no licence
    # file; it comes from the same release tag, pinned too.
    ith_declare_dependency(font_inter
        URL https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
        SHA256 9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e)
    ith_declare_dependency(font_jetbrains_mono
        URL https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip
        SHA256 6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf)
    ith_declare_dependency(font_lucide
        URL https://github.com/lucide-icons/lucide/releases/download/1.77.0/lucide-font-1.77.0.zip
        SHA256 f7bb13abd330802d45938dd3d54cb001bcedeb126528c981b7df395a8ca4ccef)
    FetchContent_MakeAvailable(font_inter font_jetbrains_mono font_lucide)
    set(ITH_LUCIDE_LICENSE "${CMAKE_BINARY_DIR}/_deps/font_lucide-LICENSE")
    file(DOWNLOAD https://raw.githubusercontent.com/lucide-icons/lucide/1.77.0/LICENSE "${ITH_LUCIDE_LICENSE}"
        EXPECTED_HASH SHA256=b495047bd93a9b06913511076f504daba17d5bbeb3e0650f3bb53a4220329c57)
endif()
