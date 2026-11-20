# Validates the changelog fragments in changelog.d/ (ADR-0002).
#
# Each fragment is changelog.d/<slug>.<kind>.md, where <kind> is a Keep a Changelog category,
# and holds one or more Markdown bullet points ("- ...").
#
# Usage: cmake -DREPOSITORY_DIR=<repo> -P CheckChangelog.cmake

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED REPOSITORY_DIR)
    message(FATAL_ERROR "REPOSITORY_DIR must be defined.")
endif()

set(kinds added changed deprecated removed fixed security)
file(GLOB fragments RELATIVE "${REPOSITORY_DIR}/changelog.d" "${REPOSITORY_DIR}/changelog.d/*")

set(problems "")
set(count 0)
foreach(name IN LISTS fragments)
    if(name STREQUAL "README.md" OR IS_DIRECTORY "${REPOSITORY_DIR}/changelog.d/${name}")
        continue()
    endif()
    if(NOT name MATCHES "^[a-z0-9][a-z0-9-]*\\.([a-z]+)\\.md$")
        list(APPEND problems "${name}: the name must be <slug>.<kind>.md with a lower-case slug")
        continue()
    endif()
    if(NOT CMAKE_MATCH_1 IN_LIST kinds)
        list(APPEND problems "${name}: '${CMAKE_MATCH_1}' is not one of: ${kinds}")
        continue()
    endif()
    file(STRINGS "${REPOSITORY_DIR}/changelog.d/${name}" lines)
    set(bullets 0)
    foreach(line IN LISTS lines)
        if(line MATCHES "^- .+")
            math(EXPR bullets "${bullets} + 1")
        endif()
    endforeach()
    if(bullets EQUAL 0)
        list(APPEND problems "${name}: holds no bullet point ('- ...')")
    endif()
    math(EXPR count "${count} + 1")
endforeach()

if(problems)
    list(JOIN problems "\n  " text)
    message(FATAL_ERROR "Changelog fragment problems:\n  ${text}")
endif()
message(STATUS "Changelog: ${count} fragment(s) valid.")
