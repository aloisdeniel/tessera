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

# Header-only vendored deps compiled once here (single-TU IMPLEMENTATION).
add_library(tessera_thirdparty STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/thirdparty_impl.c)
target_include_directories(tessera_thirdparty PUBLIC
    ${TP}
    ${TP}/cglm/include)
set_target_properties(tessera_thirdparty PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- library ----------------------------------------------------------------
file(GLOB_RECURSE TESSERA_SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
list(REMOVE_ITEM TESSERA_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/thirdparty_impl.c)

if(TESSERA_BUILD_SHARED)
    add_library(tessera SHARED ${TESSERA_SOURCES})
    target_compile_definitions(tessera PRIVATE TESSERA_BUILD_SHARED)
else()
    add_library(tessera STATIC ${TESSERA_SOURCES})
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
                    ${CMAKE_CURRENT_SOURCE_DIR}/include)
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
endif()
