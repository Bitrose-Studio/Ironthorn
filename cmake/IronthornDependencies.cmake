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
