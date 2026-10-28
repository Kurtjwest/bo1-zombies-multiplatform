# zlib-ng (zlib license) for the engine's decompression of fastfiles and .iwd files (src/qcommon/bo1_inflate.h), the
# work a map load spends most of its database thread's time on: about 1.5x the bundled zlib 1.2.3's inflate (Kino's
# fastfile: 0.30 s against 0.44 s), the same bytes out. Built from its official repository as part of this build (its
# own CMake project, with the build's toolchain), with its native zng_ API, so it sits beside the bundled zlib that the
# rest of the engine keeps using (compression, the network code). The MSVC build keeps the bundled zlib.
#
# BO1_ZLIBNG OFF, or no network on the first build: the bundled zlib decompresses, as before. For offline builds point
# BO1_ZLIBNG_SOURCE_DIR at a zlib-ng checkout of the tag.

option(BO1_ZLIBNG "Decompress fastfiles and .iwd files with zlib-ng (src/qcommon/bo1_inflate.h)" ON)
set(BO1_ZLIBNG_TAG "2.2.5" CACHE STRING "zlib-ng release tag to build")
set(BO1_ZLIBNG_SOURCE_DIR "" CACHE PATH "Use this zlib-ng source tree instead of downloading one")

function(bo1_add_zlibng target)
    if(NOT BO1_ZLIBNG)
        message(STATUS "zlib-ng: off (BO1_ZLIBNG); the bundled zlib decompresses")
        return()
    endif()
    set(_src "${BO1_ZLIBNG_SOURCE_DIR}")
    if(NOT _src)
        set(_src "${CMAKE_BINARY_DIR}/zlib-ng-src")
        if(NOT EXISTS "${_src}/CMakeLists.txt")
            find_package(Git QUIET)
            if(GIT_EXECUTABLE)
                message(STATUS "zlib-ng: downloading ${BO1_ZLIBNG_TAG}")
                execute_process(
                    COMMAND "${GIT_EXECUTABLE}" -c advice.detachedHead=false clone --quiet --depth 1
                            --branch "${BO1_ZLIBNG_TAG}" https://github.com/zlib-ng/zlib-ng.git "${_src}"
                    RESULT_VARIABLE _result OUTPUT_QUIET ERROR_QUIET)
            else()
                set(_result 1)
            endif()
            if(NOT _result EQUAL 0)
                file(REMOVE_RECURSE "${_src}")
                message(STATUS "zlib-ng: off (could not download it); the bundled zlib decompresses")
                return()
            endif()
        endif()
    endif()

    # its static library only: native API (zng_ names), no tests, gzip file functions or install, and the processor
    # features chosen at run time (not -march=native)
    set(ZLIB_COMPAT OFF CACHE BOOL "" FORCE)
    set(ZLIB_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(ZLIBNG_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(WITH_GTEST OFF CACHE BOOL "" FORCE)
    set(WITH_GZFILEOP OFF CACHE BOOL "" FORCE)
    set(WITH_NATIVE_INSTRUCTIONS OFF CACHE BOOL "" FORCE)
    set(WITH_RUNTIME_CPU_DETECTION ON CACHE BOOL "" FORCE)
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    add_subdirectory("${_src}" "${CMAKE_BINARY_DIR}/zlib-ng" EXCLUDE_FROM_ALL)

    target_sources(${target} PRIVATE "${SRC_DIR}/platform/zlibng/zlibng_inflate.cpp")
    target_link_libraries(${target} PRIVATE zlib)
    target_compile_definitions(${target} PRIVATE BO1_ZLIB_NG)
    message(STATUS "zlib-ng: on (${BO1_ZLIBNG_TAG}) for fastfile and .iwd decompression")
endfunction()
