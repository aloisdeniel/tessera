cmake_minimum_required(VERSION 3.20)
project(tessera C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TESSERA_BUILD_SHARED "Build the shared library" ON)
option(TESSERA_BUILD_EXAMPLES "Build example programs" ON)
option(TESSERA_BUILD_TESTS "Build tests" ON)
option(TESSERA_WERROR "Treat warnings as errors" OFF)
option(TESSERA_ASAN "Enable Address/UB sanitizers" OFF)

# ---- dependencies -----------------------------------------------------------
find_package(SDL3 REQUIRED CONFIG)

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# Header-only vendored deps compiled once here (single-TU IMPLEMENTATION),
# plus vendored Lua 5.4 (interpreter library only — no lua/luac standalones).
file(GLOB LUA_SOURCES ${TP}/lua/*.c)
list(REMOVE_ITEM LUA_SOURCES ${TP}/lua/lua.c ${TP}/lua/luac.c)
add_library(tessera_thirdparty STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/thirdparty_impl.c
    ${LUA_SOURCES})
target_include_directories(tessera_thirdparty PUBLIC
    ${TP}
    ${TP}/cglm/include
    ${TP}/lua)
set_target_properties(tessera_thirdparty PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- library ----------------------------------------------------------------
file(GLOB_RECURSE TESSERA_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
list(REMOVE_ITEM TESSERA_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/thirdparty_impl.c)
# src/platform/<name>/ holds per-platform embedding shims; only the matching
# platform compiles them into the library (web is its own executable target).
list(FILTER TESSERA_SOURCES EXCLUDE REGEX "src/platform/")

if(TESSERA_BUILD_SHARED)
    add_library(tessera SHARED ${TESSERA_SOURCES})
    target_compile_definitions(tessera PRIVATE TESSERA_BUILD_SHARED)
else()
    add_library(tessera STATIC ${TESSERA_SOURCES})
    # The static library exists to be EMBEDDED (force-loaded) into a host app
    # that reaches the public API through FFI dlsym — the iOS/Android bindings do
    # exactly this. With C_VISIBILITY_PRESET hidden (below), the public tessera_*
    # symbols would be compiled hidden and never make it into the host's dynamic
    # symbol table, so dlsym(RTLD_DEFAULT, "tessera_*") fails at runtime. Defining
    # TESSERA_BUILD_SHARED makes TESSERA_API expand to visibility("default") (see
    # include/tessera.h), which overrides the hidden preset for the public API and
    # keeps those symbols exported (and alive past -dead_strip).
    target_compile_definitions(tessera PRIVATE TESSERA_BUILD_SHARED)
endif()

target_include_directories(tessera
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${TP}
            ${TP}/cglm/include)

target_link_libraries(tessera PUBLIC SDL3::SDL3 PRIVATE tessera_thirdparty)
target_compile_definitions(tessera PRIVATE
    TESSERA_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")

set(TESSERA_WARN -Wall -Wextra -Wno-unused-parameter)
if(TESSERA_WERROR)
    list(APPEND TESSERA_WARN -Werror)
endif()
target_compile_options(tessera PRIVATE ${TESSERA_WARN})
set_target_properties(tessera PROPERTIES C_VISIBILITY_PRESET hidden)

if(TESSERA_ASAN)
    target_compile_options(tessera PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(tessera PUBLIC -fsanitize=address,undefined)
endif()

# ---- web (Emscripten + WebGPU) ----------------------------------------------
# Built by tools/web/build_web.sh against the WebGPU-patched SDL staged in
# build-web/SDL-src (see tools/web/patch_sdl.sh). Produces a modularized
# tessera_web.js/.wasm pair with the WGSL shaders embedded in MEMFS.
if(EMSCRIPTEN)
    if(NOT DEFINED TESSERA_WEB_EXPORTS_FILE OR NOT DEFINED TESSERA_WEB_SHADER_DIR)
        message(FATAL_ERROR "Use tools/web/build_web.sh for the Emscripten build "
                            "(it stages shaders and generates the export list).")
    endif()
    add_executable(tessera_web ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/web/tessera_web.c)
    target_include_directories(tessera_web PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(tessera_web PRIVATE tessera SDL3::SDL3 tessera_thirdparty)
    set_target_properties(tessera_web PROPERTIES SUFFIX ".js")
    target_link_options(tessera_web PRIVATE
        "--no-entry"
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=createTesseraModule"
        "-sENVIRONMENT=web"
        "-sASYNCIFY=1"
        "-sASYNCIFY_STACK_SIZE=131072"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sEXPORTED_FUNCTIONS=@${TESSERA_WEB_EXPORTS_FILE}"
        "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,getValue,setValue,UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAP8,HEAPU8,HEAP16,HEAPU16,HEAP32,HEAPU32,HEAPF32,HEAPF64"
        "--use-port=emdawnwebgpu"
        "--embed-file=${TESSERA_WEB_SHADER_DIR}@/tessera_assets/shaders")
    # Re-link when the embedded WGSL shaders change — the embed happens at
    # link time and CMake would otherwise consider the target up to date.
    file(GLOB TESSERA_WEB_WGSL ${TESSERA_WEB_SHADER_DIR}/*.wgsl)
    set_property(TARGET tessera_web APPEND PROPERTY LINK_DEPENDS ${TESSERA_WEB_WGSL})
endif()

# ---- examples ---------------------------------------------------------------
if(TESSERA_BUILD_EXAMPLES)
    file(GLOB EXAMPLE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/examples/*)
    foreach(dir ${EXAMPLE_DIRS})
        if(IS_DIRECTORY ${dir})
            file(GLOB EX_SRC ${dir}/*.c)
            if(EX_SRC)
                get_filename_component(name ${dir} NAME)
                add_executable(example_${name} ${EX_SRC})
                target_link_libraries(example_${name} PRIVATE tessera SDL3::SDL3)
                target_include_directories(example_${name} PRIVATE
                    ${CMAKE_CURRENT_SOURCE_DIR}/include
                    ${TP})
            endif()
        endif()
    endforeach()
endif()

# ---- tests ------------------------------------------------------------------
if(TESSERA_BUILD_TESTS)
    enable_testing()
    # A static build of the sources with default visibility, so tests can reach
    # internal (normally hidden) symbols like ts_layout_solve / ts_slotmap_*.
    add_library(tessera_testlib STATIC ${TESSERA_SOURCES})
    target_include_directories(tessera_testlib
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src
               ${TP} ${TP}/cglm/include)
    target_link_libraries(tessera_testlib PUBLIC SDL3::SDL3 PRIVATE tessera_thirdparty)
    target_compile_definitions(tessera_testlib PRIVATE
        TESSERA_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets")

    file(GLOB TEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c)
    foreach(src ${TEST_SRC})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE tessera_testlib SDL3::SDL3 tessera_thirdparty)
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${TP} ${TP}/cglm/include)
        add_test(NAME ${name} COMMAND ${name})
    endforeach()

    # FFI binding drift gate: diff the Lua cdef + Dart ffi struct layouts
    # against the canonical C ABI dump (test_ffi_layout --dump).
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME ffi_binding_drift
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_ffi_bindings.py
                --dump-tool $<TARGET_FILE:test_ffi_layout>
                --lua  ${CMAKE_CURRENT_SOURCE_DIR}/bindings/lua/tessera.lua
                --dart ${CMAKE_CURRENT_SOURCE_DIR}/bindings/dart/lib/src/ffi.dart)
    endif()
endif()
