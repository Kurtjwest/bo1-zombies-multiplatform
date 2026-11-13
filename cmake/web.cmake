# Browser build of the engine: WebAssembly with threads, built with Emscripten (build-web.sh).
#
#   emcmake cmake -S . -B build-web -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-web
#
# Produces build-web/web/ (the page: index.html, BO1Zombies.js, BO1Zombies.wasm and the page's scripts) and
# build-web/gamedata/ (the repository's mods/ and data/main/). tools/web-server.py serves both on the local network
# together with the game folder: the game reads the original game's files from that server while it runs (byte ranges
# over HTTP, src/platform/web/web_fs.cpp), so nothing is copied into the browser.
#
# The engine runs on the Win32 compatibility layer as on macOS and Linux (cmake/port.cmake), in Web Workers (one per
# engine thread, sharing the WebAssembly memory). The page's own thread keeps the DOM: it passes input to the game
# through shared memory and draws the frames the render thread reads back (src/platform/web). Direct3D 9 is implemented on
# WebGL 2 (src/platform/d3d9mtl over src/platform/web/webgl_rhi.cpp), with MojoShader's GLSL ES 3 output.

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The browser build is 32-bit WebAssembly (wasm32); MEMORY64 is not supported")
endif()
message(STATUS "BO1 Zombies for the browser (WebAssembly, Emscripten ${EMSCRIPTEN_VERSION})")

# Every object of the program, the libraries built with it included, shares the threads' memory (-pthread: atomics
# and bulk memory), handles the engine's setjmp/longjmp error recovery (Com_Error) and C++ exceptions with
# WebAssembly's own exception handling, and has the SIMD instructions the engine's SSE code is translated to.
set(BO1_WEB_TARGET_FLAGS "-pthread -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -msimd128")
string(APPEND CMAKE_C_FLAGS " ${BO1_WEB_TARGET_FLAGS}")
string(APPEND CMAKE_CXX_FLAGS " ${BO1_WEB_TARGET_FLAGS}")
string(APPEND CMAKE_EXE_LINKER_FLAGS " ${BO1_WEB_TARGET_FLAGS}")
# (the libraries' own configure scripts get the same, cmake/deps_target.cmake: libvpx recovers from errors with
# setjmp/longjmp, and one program has one kind of longjmp)
set(BO1_TARGET_CFLAGS "-pthread -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -msimd128")
# no shared libraries, and no position-independent code (the program is one module)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

# ---- the web platform layer: the page bridge (input, the canvas, sound, controllers), the game folder over HTTP and
# the WebGL 2 renderer ----
file(GLOB BO1_PORT_PLATFORM_SOURCES CONFIGURE_DEPENDS "${SRC_DIR}/platform/web/*.cpp")
# (the Linux build's image file encoder is portable C++: screenshots)
list(APPEND BO1_PORT_PLATFORM_SOURCES "${SRC_DIR}/platform/linux/image_writer_linux.cpp")
set(BO1_PORT_MOJOSHADER_PROFILE GLSLES3)
include("${CMAKE_CURRENT_LIST_DIR}/port.cmake")
set_target_properties(bo1_mojoshader PROPERTIES POSITION_INDEPENDENT_CODE OFF)

# no Steam in a browser: the stand-in library (src/platform/common/steam_stub), linked in (SteamAPI_Init fails and the
# game runs offline, as it does elsewhere without a Steam client)
add_library(steam_api STATIC "${PLATFORM_DIR}/common/steam_stub/steam_api_stub.cpp")
set_target_properties(steam_api PROPERTIES CXX_STANDARD 17)
target_link_libraries(${BIN_NAME} PRIVATE steam_api)

set(BO1_WEB_OUT "${CMAKE_BINARY_DIR}/web")
set_target_properties(${BIN_NAME} PROPERTIES
    OUTPUT_NAME "BO1Zombies"
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${BO1_WEB_OUT}")

target_compile_definitions(${BIN_NAME} PRIVATE
    BO1_WEB=1
    # the Direct3D 9 layer draws through WebGL 2 (src/platform/web/webgl_rhi.cpp): the OpenGL renderer's shader
    # language (the BO1 GLSL profile, which the WebGL renderer adapts to GLSL ES 3.00) and variants
    BO1_RHI_OPENGL=1
    BO1_RHI_WEBGL=1
    _GNU_SOURCE
)
# the engine's SSE code: Emscripten's SSE headers on WebAssembly SIMD (MMX: src/platform/win32compat/include/mmintrin.h)
target_compile_options(${BIN_NAME} PRIVATE -msse4.2)
# long double as the Windows game has it, its double: WebAssembly's is a 128-bit software type, which would make the
# engine's structures with one bigger than the code allocating them expects (the menu parser's tokens) and print
# garbage where the engine passes one to printf. (The driver accepts the option for x86 only; the compiler itself
# takes it for any target. The engine calls none of the C library's long double functions.)
target_compile_options(${BIN_NAME} PRIVATE "SHELL:-Xclang -mlong-double-64")
# WasmFS's own headers (the game folder's HTTP backend, web_fs.cpp)
target_include_directories(${BIN_NAME} PRIVATE "${EMSCRIPTEN_ROOT_PATH}/system/lib/wasmfs")

set(BO1_WEB_DIR "${SRC_DIR}/platform/web")
set(BO1_WEB_LINK_FLAGS
    # the engine's main() on a thread of its own (a Web Worker): the page's thread never blocks
    -sPROXY_TO_PTHREAD
    # the threads are started as the engine creates them; a few are ready before main() to save the round trips
    -sPTHREAD_POOL_SIZE=12
    -sPTHREAD_POOL_SIZE_STRICT=0
    # the engine's stacks: it keeps big structures on its main thread's (the macOS build links a 32 MB one; main() runs
    # on a thread with this stack, PROXY_TO_PTHREAD), and Windows gives its other threads 1 MB
    -sSTACK_SIZE=32MB
    -sDEFAULT_PTHREAD_STACK_SIZE=2MB
    # memory: grows as the game needs it, up to the 4 GB wasm32 can address (the game uses 1-2 GB)
    -sINITIAL_MEMORY=512MB
    -sALLOW_MEMORY_GROWTH=1
    -sMAXIMUM_MEMORY=4GB
    # the file system the game folder is mounted in (web_fs.cpp)
    -sWASMFS
    -sFORCE_FILESYSTEM=1
    # WebGL 2 (the render thread makes its context itself, on a canvas of its own: webgl_rhi.cpp, bo1_web_lib.js)
    -sMIN_WEBGL_VERSION=2
    -sMAX_WEBGL_VERSION=2
    -sGL_POOL_TEMP_BUFFERS=0
    -lGL
    # the page and its workers only
    -sENVIRONMENT=web,worker
    -sEXIT_RUNTIME=0
    -sINVOKE_RUN=1
    # (Module.HEAPU8, which the page reads the shared block through, is always there in this Emscripten)
    -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_Bridge_WebSharedState
    --js-library "${BO1_WEB_DIR}/bo1_web_lib.js"
)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND BO1_WEB_LINK_FLAGS -sASSERTIONS=1 -gsource-map)
else()
    # (the functions' names stay in the module: a crash's stack in the browser's console names them)
    list(APPEND BO1_WEB_LINK_FLAGS -sASSERTIONS=0 --profiling-funcs)
endif()
target_link_options(${BIN_NAME} PRIVATE ${BO1_WEB_LINK_FLAGS})
set_property(TARGET ${BIN_NAME} APPEND PROPERTY LINK_DEPENDS "${BO1_WEB_DIR}/bo1_web_lib.js")

# ---- the page: index.html and its scripts next to the program ----
file(GLOB BO1_WEB_PAGE_FILES CONFIGURE_DEPENDS "${BO1_WEB_DIR}/page/*")
add_custom_target(web_page ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BO1_WEB_OUT}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BO1_WEB_PAGE_FILES} "${BO1_WEB_OUT}/"
    DEPENDS ${BO1_WEB_PAGE_FILES}
    COMMENT "Copying the page to web/")

# ---- the repository's own game files (mods/, data/main/) in gamedata/: tools/web-server.py lays them over the game
# folder's (as the Linux build installs them into its game folder). Rebuilt every build. ----
add_custom_target(bundle_game_data ALL
    COMMAND ${CMAKE_COMMAND} -E ${BO1_RM_DIR} "${CMAKE_BINARY_DIR}/gamedata"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/mods" "${CMAKE_BINARY_DIR}/gamedata/mods"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/data/main" "${CMAKE_BINARY_DIR}/gamedata/main"
    COMMENT "Copying mods/ and data/main/ to gamedata/")
