# NDK build for the flutter_tessera Android plugin.
#
# Produces a single libtessera.so containing the Tessera engine, the shared C
# bridge, and the JNI layer. Dart FFI opens this .so for the tessera_* symbols
# (Tessera.fromHandle) and Kotlin System.loadLibrary("tessera") uses its Java_*
# entry points.
#
# Requires SDL3 built for Android. Point SDL3_SOURCE_DIR at the SDL3 source
# checkout (SDL ships an Android CMake build); it is added as a subdirectory.
#
# NOTE: reference build config — not exercised in the authoring environment (no
# NDK/emulator). The SPIR-V shaders it renders ARE verified on Vulkan.
cmake_minimum_required(VERSION 3.22)
project(tessera_android C)

set(CMAKE_C_STANDARD 11)

# Repo root: android/src/main/cpp -> ... -> <repo>
get_filename_component(TESSERA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
set(TESSERA_ROOT "${TESSERA_ROOT}" CACHE PATH "Tessera repo root")
set(TP ${TESSERA_ROOT}/third_party)

# ---- SDL3 (Android) ---------------------------------------------------------
if(NOT DEFINED SDL3_SOURCE_DIR)
    set(SDL3_SOURCE_DIR "${TESSERA_ROOT}/third_party/SDL"
        CACHE PATH "SDL3 source dir (Android)")
endif()
add_subdirectory("${SDL3_SOURCE_DIR}" SDL3-build)

# ---- engine sources ---------------------------------------------------------
file(GLOB_RECURSE TESSERA_SOURCES CONFIGURE_DEPENDS ${TESSERA_ROOT}/src/*.c)
list(REMOVE_ITEM TESSERA_SOURCES ${TESSERA_ROOT}/src/thirdparty_impl.c)

# Vendored Lua 5.4 (interpreter library only — no lua/luac standalones),
# mirroring the root CMakeLists' tessera_thirdparty setup.
file(GLOB LUA_SOURCES ${TP}/lua/*.c)
list(REMOVE_ITEM LUA_SOURCES ${TP}/lua/lua.c ${TP}/lua/luac.c
     ${TP}/lua/loslib.c ${TP}/lua/liolib.c ${TP}/lua/linit.c)

add_library(tessera SHARED
    ${TESSERA_SOURCES}
    ${TESSERA_ROOT}/src/thirdparty_impl.c
    ${LUA_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/tessera_bridge.c
    ${CMAKE_CURRENT_SOURCE_DIR}/tessera_jni.c)

target_include_directories(tessera
    PUBLIC  ${TESSERA_ROOT}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${TESSERA_ROOT}/src
            ${TP}
            ${TP}/cglm/include
            ${TP}/lua)

target_compile_definitions(tessera PRIVATE
    TESSERA_BUILD_SHARED
    # Overridden at runtime by ftessera_create(asset_dir): the Kotlin side
    # extracts the APK's shaders/ to a filesystem dir and passes it.
    TESSERA_ASSET_DIR="assets")

# Export the tessera_* + Java_* symbols (engine builds with hidden visibility).
set_target_properties(tessera PROPERTIES C_VISIBILITY_PRESET default)

find_library(ANDROID_LIB android)
find_library(LOG_LIB log)
target_link_libraries(tessera
    SDL3::SDL3
    ${ANDROID_LIB}
    ${LOG_LIB})
