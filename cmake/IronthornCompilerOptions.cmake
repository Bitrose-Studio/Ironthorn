# Compiler flags shared by Ironthorn targets (ADR-0001).
#
#   ith::build_configuration  definitions that say which kind of build this is
#   ith::build_options        warnings and language rules for every Ironthorn target
#   ith::runtime_options      what runtime modules add: no exceptions, no RTTI
#
# Third-party code is never compiled with our warning level.

add_library(ith_build_configuration INTERFACE)
add_library(ith::build_configuration ALIAS ith_build_configuration)

# Debug: full checks. RelWithDebInfo ("development"): optimized, asserts on - the everyday
# configuration. Release ("shipping"): what players get, asserts off.
target_compile_definitions(ith_build_configuration INTERFACE
    $<$<CONFIG:Debug>:ITH_DEBUG=1>
    $<$<NOT:$<CONFIG:Release>>:ITH_ENABLE_ASSERTS=1>
    $<$<CONFIG:Release>:ITH_SHIPPING=1>)

add_library(ith_build_options INTERFACE)
add_library(ith::build_options ALIAS ith_build_options)
target_compile_features(ith_build_options INTERFACE cxx_std_20)
target_link_libraries(ith_build_options INTERFACE ith::build_configuration)

if(WIN32)
    target_compile_definitions(ith_build_options INTERFACE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE)
endif()

set(ith_clang_warnings
    -Wshadow
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wimplicit-fallthrough
    -Wextra-semi
    -Wthread-safety)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(ith_build_options INTERFACE
        /W4 /permissive- /utf-8 /Zc:preprocessor /Zc:__cplusplus)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(ith_build_options INTERFACE /W4 /utf-8 ${ith_clang_warnings})
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(ith_build_options INTERFACE -Wall -Wextra -Wpedantic ${ith_clang_warnings})
else()
    target_compile_options(ith_build_options INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wimplicit-fallthrough)
endif()

# Clang 19+ warns when a designated initializer leaves out members that have default member
# initializers; omitting them is the point of designated initializers. GCC reports the same under
# -Wmissing-field-initializers.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-Wmissing-designated-field-initializers ITH_HAS_DESIGNATED_FIELD_WARNING)
    if(ITH_HAS_DESIGNATED_FIELD_WARNING)
        target_compile_options(ith_build_options INTERFACE -Wno-missing-designated-field-initializers)
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(ith_build_options INTERFACE -Wno-missing-field-initializers)
endif()

# Exceptions are off for every Ironthorn target, tests included, so that the standard library
# is configured the same way in every translation unit that shares engine types (ADR-0001).
if(MSVC)
    # CMake adds /EHsc to every MSVC target by default; remove it so /EHs-c- is not overridden.
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    target_compile_options(ith_build_options INTERFACE /EHs-c-)
    target_compile_definitions(ith_build_options INTERFACE _HAS_EXCEPTIONS=0)
else()
    target_compile_options(ith_build_options INTERFACE -fno-exceptions)
endif()

# RTTI is off in runtime modules; the engine has its own reflection. Tests and tools may keep it.
add_library(ith_runtime_options INTERFACE)
add_library(ith::runtime_options ALIAS ith_runtime_options)
if(MSVC)
    target_compile_options(ith_runtime_options INTERFACE /GR-)
else()
    target_compile_options(ith_runtime_options INTERFACE -fno-rtti)
endif()

if(IRONTHORN_ENABLE_SANITIZERS AND IRONTHORN_ENABLE_TSAN)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be combined. Pick one.")
endif()
if(IRONTHORN_ENABLE_SANITIZERS OR IRONTHORN_ENABLE_TSAN)
    if(MSVC)
        message(FATAL_ERROR "Sanitizer builds are only supported with Clang and GCC.")
    endif()
    if(IRONTHORN_ENABLE_TSAN)
        set(ith_sanitizers thread)
    else()
        set(ith_sanitizers address,undefined)
    endif()
    # Sanitizer runtimes must see every translation unit, third-party code included.
    add_compile_options(-fsanitize=${ith_sanitizers} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${ith_sanitizers})
endif()
