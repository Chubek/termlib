cmake_minimum_required(VERSION 3.16)
project(NovoParse VERSION 0.1.0 LANGUAGES C CXX)
include(GNUInstallDirs)
include(CTest)

# Keep the vendored dependency focused on the parser runtime.
set(BUILD_TESTS OFF CACHE BOOL "Build libglr's own tests")
set(BUILD_EXAMPLES OFF CACHE BOOL "Build libglr's examples")
set(BUILD_DOCUMENTATION OFF CACHE BOOL "Build libglr documentation")
set(EMBED_MD_DOCS OFF CACHE BOOL "Install libglr documentation")
set(BUILD_DISAMBSTD OFF CACHE BOOL "Build libglr's C++ disambiguators")
set(BUILD_REWRITELIB OFF CACHE BOOL "Install libglr rewrite examples")
set(BUILD_MOOSEDOG OFF CACHE BOOL "Build libglr's other frontend")
set(INSTALL_GLRpp OFF CACHE BOOL "Install libglr's C++ wrapper")
set(ENABLE_CACHE OFF CACHE BOOL "Enable libglr's database cache")
add_subdirectory(third_party/libglr EXCLUDE_FROM_ALL)

add_library(novoparse_runtime SHARED runtime/NovoParse.c)
set_target_properties(novoparse_runtime PROPERTIES OUTPUT_NAME novoparse VERSION ${PROJECT_VERSION} SOVERSION 0)
target_compile_features(novoparse_runtime PRIVATE c_std_11)
target_include_directories(novoparse_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/novoparse>)
target_link_libraries(novoparse_runtime PRIVATE libglr)
target_compile_options(novoparse_runtime PRIVATE -Wall -Wextra -Wpedantic)

find_program(NOVOPARSE_D_COMPILER NAMES ldc2 dmd gdc)
if(NOT NOVOPARSE_D_COMPILER)
    message(FATAL_ERROR "NovoParse requires a D compiler: ldc2, dmd, or gdc")
endif()
file(GLOB NOVOPARSE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/*.d")
file(GLOB NOVOPARSE_PROFILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/profiles/*.sexp")
get_filename_component(D_COMPILER_NAME "${NOVOPARSE_D_COMPILER}" NAME)
set(D_OUTPUT "-of=${CMAKE_CURRENT_BINARY_DIR}/novoparse")
if(D_COMPILER_NAME MATCHES "gdc")
    set(D_OUTPUT -o "${CMAKE_CURRENT_BINARY_DIR}/novoparse")
endif()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/novoparse"
    COMMAND "${NOVOPARSE_D_COMPILER}" "-I${CMAKE_CURRENT_SOURCE_DIR}/source"
        "-J${CMAKE_CURRENT_SOURCE_DIR}/profiles" ${D_OUTPUT} ${NOVOPARSE_SOURCES}
    DEPENDS ${NOVOPARSE_SOURCES} ${NOVOPARSE_PROFILES}
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    VERBATIM)
add_custom_target(novoparse_cli ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/novoparse")

install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/novoparse" DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS novoparse_runtime LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES runtime/NovoParse.h runtime/NovoParse.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/novoparse)
install(FILES runtime/NovoParse.py runtime/NovoParse.d include/NovoParse.di DESTINATION ${CMAKE_INSTALL_DATADIR}/novoparse/runtime)
install(FILES profiles/C.sexp profiles/C++.sexp profiles/D.sexp profiles/Python.sexp DESTINATION ${CMAKE_INSTALL_DATADIR}/novoparse/profiles)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/novoparse/licenses)
install(FILES third_party/libglr/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/novoparse/licenses RENAME libglr-MIT.txt)
install(FILES third_party/libglr/third_party/klib/LICENSE.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/novoparse/licenses RENAME klib-MIT.txt)
install(FILES third_party/libglr/third_party/ctl/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/novoparse/licenses RENAME ctl-MIT.txt)
install(FILES third_party/libglr/third_party/sfsexp/COPYING third_party/libglr/third_party/sfsexp/LICENSE_LGPL
    DESTINATION ${CMAKE_INSTALL_DATADIR}/novoparse/licenses)

if(ENABLE_TEST_SANITIZERS)
    target_compile_options(novoparse_runtime PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(novoparse_runtime PUBLIC -fsanitize=address,undefined)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
