file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" VERSION_STR)
string(STRIP "${VERSION_STR}" VERSION_STR)

string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" _ ${VERSION_STR})
set(MAJOR ${CMAKE_MATCH_1})
set(MINOR ${CMAKE_MATCH_2})
set(BUILD ${CMAKE_MATCH_3})

math(EXPR BUILD "${BUILD} + 1")
set(NEW_VERSION "${MAJOR}.${MINOR}.${BUILD}")

file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" "${NEW_VERSION}\n")

set(HEADER_CONTENT "#pragma once\n\n#define DIVO_VERSION \"${NEW_VERSION}\"\n")
set(HEADER_FILE "${CMAKE_CURRENT_SOURCE_DIR}/include/version.hpp")

if(EXISTS ${HEADER_FILE})
    file(READ ${HEADER_FILE} OLD_HEADER)
    if(NOT "${OLD_HEADER}" STREQUAL "${HEADER_CONTENT}")
        file(WRITE ${HEADER_FILE} "${HEADER_CONTENT}")
    endif()
else()
    file(WRITE ${HEADER_FILE} "${HEADER_CONTENT}")
endif()