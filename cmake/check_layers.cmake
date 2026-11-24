# Enforces the dependency direction between modules.
#
# Every target sees the whole include/ directory, so the compiler alone would
# let, say, algo include a Bybit header. This script reads the #include lines
# of each module and fails if one points to a module it may not depend on.
#
# Usage: cmake -DROOT=<source dir> -P check_layers.cmake

cmake_minimum_required(VERSION 3.20)  # Script mode starts with no policies (IN_LIST needs CMP0057).

# module : modules it may include (itself included)
set(PEEWEE_LAYER_RULES
    "core:core"
    "book:core book"
    "venue:core venue"
    "oms:core venue oms"
    "algo:core book venue oms algo"
    "engine:core book venue oms algo engine"
    "venues:core book venue venues transport"
    "transport:core transport"
    "client:core book venue oms algo engine venues client")

set(violations "")

foreach(rule IN LISTS PEEWEE_LAYER_RULES)
    string(REPLACE ":" ";" parts "${rule}")
    list(GET parts 0 module)
    list(GET parts 1 allowed)
    string(REPLACE " " ";" allowed "${allowed}")

    file(GLOB_RECURSE sources
        "${ROOT}/include/peewee/${module}/*.hpp"
        "${ROOT}/src/${module}/*.hpp"
        "${ROOT}/src/${module}/*.cpp")

    foreach(source IN LISTS sources)
        file(STRINGS "${source}" includes REGEX "^[ \t]*#[ \t]*include[ \t]*\"(peewee/[a-z_]+|transport)/")
        foreach(line IN LISTS includes)
            if(line MATCHES "\"peewee/([a-z_]+)/")
                set(target "${CMAKE_MATCH_1}")
            else()
                set(target "transport")
            endif()
            if(NOT target IN_LIST allowed)
                file(RELATIVE_PATH rel "${ROOT}" "${source}")
                string(STRIP "${line}" line)
                string(APPEND violations "\n  ${rel}: module '${module}' may not include '${target}' (${line})")
            endif()
        endforeach()
    endforeach()
endforeach()

if(violations)
    message(FATAL_ERROR "Module dependency rules violated:${violations}\n"
                        "See the rules in cmake/check_layers.cmake.")
endif()
