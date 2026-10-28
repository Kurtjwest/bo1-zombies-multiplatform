# The clang flags the decompiled engine compiles with wherever it is built with clang: macOS and Linux
# (cmake/port.cmake) and Windows with MinGW (cmake/windows.cmake). The MSVC project keeps its own (CMakeLists.txt).
include_guard(GLOBAL)

set(BO1_CLANG_LANGUAGE_FLAGS
    # MSVC language extensions used throughout the decompiled engine (__int64, __declspec(align), __forceinline,
    # _InterlockedXxx/_BitScanXxx builtins, __debugbreak, anonymous struct members, ...)
    -fms-extensions
    # The engine (like the MSVC build) relies on wrapping signed arithmetic and on type punning.
    -fwrapv
    -fno-strict-aliasing
    # Decompiled functions sometimes fall off the end on paths that never happen; keep MSVC's behaviour
    # (an unspecified value) instead of treating those paths as unreachable.
    -fno-strict-return
    -fno-delete-null-pointer-checks
    # ... and newer Clang releases turn the warning about those paths into an error by default
    -Wno-return-type
    # The engine was written for MSVC on x86, where a volatile read is an acquire and a volatile write a release
    # (/volatile:ms, and x86 stores and loads are ordered anyway). Its cross-thread flags and counters (render
    # command counts, worker states, the dvar read/write counts, ...) depend on that; ARM reorders plain accesses,
    # so volatile accesses get the same acquire/release semantics here (ldapr/stlr; free on x86).
    -fms-volatile
    # SSE arithmetic never fuses a multiply and an add; keep the same rounding (Clang would contract a * b + c
    # into an FMA on ARM), so physics, collision and prediction compute what the x86 game computes.
    -ffp-contract=off
    # MSVC's char is signed on every processor; ARM Linux's is unsigned by default
    -fsigned-char
)

# The decompiled code also computes pointers outside their arrays (a copy loop starts one byte before both buffers,
# I_strncpyz; a table is walked from its end, ...), which MSVC compiled as plain address arithmetic. clang marks
# such arithmetic as staying inside the object, and once a function is inlined where that object is visible (a local
# array, as ThinLTO inlines across files) the out-of-object pointer makes the code undefined: I_strncpyz into a
# stack buffer lost its loop exits and ran off the stack. -fwrapv-pointer (clang 20 and later) keeps it plain
# arithmetic; ThinLTO is only used with it (bo1_release_tuning).
function(bo1_check_wrapv_pointer out)
    include(CheckCXXCompilerFlag)
    unset(CMAKE_GENERATOR_PLATFORM) # (CMakeLists.txt sets a Visual Studio platform for WIN32, which Ninja refuses)
    check_cxx_compiler_flag(-fwrapv-pointer BO1_HAVE_WRAPV_POINTER)
    set(${out} ${BO1_HAVE_WRAPV_POINTER} PARENT_SCOPE)
endfunction()
bo1_check_wrapv_pointer(BO1_WRAPV_POINTER)
if(BO1_WRAPV_POINTER)
    list(APPEND BO1_CLANG_LANGUAGE_FLAGS -fwrapv-pointer)
endif()
set(BO1_CLANG_COMMON_WARNING_FLAGS
    -Wno-ignored-attributes
    -Wno-microsoft
    -Wno-deprecated-declarations
    -Wno-unknown-warning-option
)
set(BO1_CLANG_CXX_FLAGS
    # MSVC /permissive equivalents for decompiled code
    -Wno-c++11-narrowing
    -Wno-register
    -Wno-writable-strings
    -Wno-invalid-offsetof
    -Wno-unused-value
    -Wno-switch
    -Wno-parentheses
    -Wno-dangling-else
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-missing-braces
    -Wno-null-conversion
    -Wno-tautological-compare
    -Wno-tautological-constant-out-of-range-compare
    -Wno-shift-negative-value
    -Wno-constant-conversion
    -Wno-unused-private-field
    -Wno-inconsistent-missing-override
    -Wno-delete-non-abstract-non-virtual-dtor
    -Wno-format
    -Wno-format-security
    -Wno-char-subscripts
    -Wno-unknown-pragmas
    -Wno-pointer-bool-conversion
    -Wno-undefined-var-template
    -Wno-c++20-designator
    -Wno-nonportable-include-path
    -Wno-comment
    -Werror=return-stack-address
)

# ---- Release builds (cmake/port.cmake, cmake/windows.cmake): what the retail game's release build had over the
# port's plain -O2 ----
# BO1_RELEASE_ASSERTS: the decompiled code's assertion checks (about 13,900; src/universal/assertive.h). The retail
# release build had none; off, they compile away (BO1_NO_ASSERTS). On, a failed check logs itself and stops the game,
# as in the Debug and RelWithDebInfo builds, which always keep them.
option(BO1_RELEASE_ASSERTS "Keep the engine's assertion checks in Release builds" OFF)
# BO1_LTO: ThinLTO, whole-program optimization across the engine's files (the retail game was built with MSVC's
# whole-program optimization, LTCG). Off when the toolchain cannot link with it (Linux: needs lld).
option(BO1_LTO "Whole-program optimization (ThinLTO) in Release builds" ON)

function(bo1_release_tuning target)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        return()
    endif()
    if(NOT BO1_RELEASE_ASSERTS)
        target_compile_definitions(${target} PRIVATE BO1_NO_ASSERTS)
    endif()
    if(NOT BO1_LTO)
        message(STATUS "ThinLTO: off (BO1_LTO)")
        return()
    endif()
    if(NOT BO1_WRAPV_POINTER)
        message(STATUS "ThinLTO: off (it needs clang 20 or later, for -fwrapv-pointer: see BO1_CLANG_LANGUAGE_FLAGS)")
        return()
    endif()
    # the linker options: ThinLTO, its cache (an incremental link re-optimizes only what changed), and on Linux lld
    # (the cross toolchains already link with it; a native build otherwise links with the system's linker, which needs
    # the LLVM gold plugin for LTO, and the 64-bit build's --wrap of the malloc family is tested with lld)
    set(_cache "${CMAKE_BINARY_DIR}/thinlto-cache")
    if(APPLE)
        set(_link -flto=thin "-Wl,-cache_path_lto,${_cache}")
    else()
        set(_link -flto=thin "-Wl,--thinlto-cache-dir=${_cache}")
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_EXE_LINKER_FLAGS MATCHES "fuse-ld=lld")
            list(APPEND _link -fuse-ld=lld)
        endif()
    endif()
    string(REGEX MATCH "^[0-9]+" CMAKE_CXX_COMPILER_VERSION_MAJOR "${CMAKE_CXX_COMPILER_VERSION}")
    include(CheckCXXSourceCompiles)
    # (CMakeLists.txt sets a Visual Studio platform for WIN32, which a test project with Ninja refuses)
    unset(CMAKE_GENERATOR_PLATFORM)
    set(CMAKE_REQUIRED_FLAGS -flto=thin)
    set(CMAKE_REQUIRED_LINK_OPTIONS ${_link})
    check_cxx_source_compiles("int main() { return 0; }" BO1_THINLTO_LINKS)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(NOT BO1_THINLTO_LINKS)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            # (clang uses the ld.lld next to it first: lld-<version> of the same LLVM version, as the ThinLTO
            # summaries clang writes are those of its own LLVM)
            message(STATUS "ThinLTO: off (it links with the lld of clang's LLVM version: Debian/Ubuntu lld, or "
                           "lld-${CMAKE_CXX_COMPILER_VERSION_MAJOR} with a versioned clang; Fedora lld, Arch lld)")
        else()
            message(STATUS "ThinLTO: off (the toolchain cannot link with it: it needs the lld of clang's LLVM version)")
        endif()
        return()
    endif()
    message(STATUS "ThinLTO: on")
    target_compile_options(${target} PRIVATE -flto=thin)
    target_link_options(${target} PRIVATE ${_link})
endfunction()
