# Termlib -- PTY/TTY management, Terminfo, session recording, connections,
# terminal orchestration and the Termscript automation language.
cmake_minimum_required(VERSION 3.15)
project(Termlib LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
include(GNUInstallDirs)
include(CTest)

# NovoParse compiles termscript/Termscript.ng into a C descriptor header.  The
# runtime adapter in termscript/ts_parser_ng.c maps NovoParse syntax trees into
# the backend-independent AST consumed by the VM, C, and QBE backends.
set(TERMLIB_PARENT_BUILD_TESTING ${BUILD_TESTING})
set(BUILD_TESTING OFF)
add_subdirectory(third_party/novoparse EXCLUDE_FROM_ALL)
set(BUILD_TESTING ${TERMLIB_PARENT_BUILD_TESTING})
set(TERMLIB_NOVOPARSE ${CMAKE_CURRENT_BINARY_DIR}/third_party/novoparse/novoparse)
set(TERMLIB_PARSER_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/termscript)
set(TERMLIB_PARSER_H ${TERMLIB_PARSER_DIR}/TermscriptParser.h)
add_custom_command(
  OUTPUT ${TERMLIB_PARSER_H}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${TERMLIB_PARSER_DIR}
  COMMAND ${TERMLIB_NOVOPARSE} generate
          ${CMAKE_CURRENT_SOURCE_DIR}/termscript/Termscript.ng
          --profile C -o ${TERMLIB_PARSER_H}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/termscript/Termscript.ng
          novoparse_cli
  COMMENT "Generating Termscript parser descriptor (NovoParse)"
  VERBATIM
)

# ── libtermlib (core: no Termscript) ─────────────────────────────────
add_library(termlib STATIC
  termlib.c
  src/dt_util.c
  src/dt_terminfo.c
  src/dt_tiexpand.c
  src/dt_tty.c
  src/dt_pty.c
  src/dt_record.c
  src/dt_conn.c
  src/dt_puppeteer.c
)

target_include_directories(termlib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/termlib>
)
target_include_directories(termlib PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(termlib PUBLIC Threads::Threads)

# ── libtermscript_standalone (embedding + extension API, no Termlib) ─
add_library(termscript_standalone STATIC
  termscript/ts_runtime.c
  termscript/ts_parser_ng.c
  ${TERMLIB_PARSER_H}
)
target_include_directories(termscript_standalone PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(termscript_standalone PRIVATE
  ${TERMLIB_PARSER_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/novoparse/runtime
)
target_link_libraries(termscript_standalone PRIVATE Threads::Threads novoparse_runtime)

set(TS_JITERATI_DEFAULT OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(TS_JITERATI_DEFAULT ON)
endif()
option(TERMSCRIPT_JITERATI "Build experimental Jiterati execution backend (host AMD64)" ${TS_JITERATI_DEFAULT})
if(TERMSCRIPT_JITERATI)
  if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "The current Jiterati executable backend only supports host AMD64")
  endif()
  set(TS_JITERATI_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/jiterati)
  if(NOT EXISTS "${TS_JITERATI_ROOT}/include/Jiterati.hpp" OR
     NOT EXISTS "${TS_JITERATI_ROOT}/BE/AMD64/Backend.cpp")
    message(FATAL_ERROR "TERMSCRIPT_JITERATI requires the bundled third_party/jiterati sources")
  endif()
  target_sources(termscript_standalone PRIVATE
    termscript/ts_jiterati.cpp
    third_party/jiterati/src/JIT.cpp
    third_party/jiterati/BE/AMD64/Backend.cpp
  )
  target_compile_features(termscript_standalone PRIVATE cxx_std_17)
  target_compile_definitions(termscript_standalone PRIVATE TERMSCRIPT_HAVE_JITERATI=1)
  target_include_directories(termscript_standalone SYSTEM PRIVATE
    ${TS_JITERATI_ROOT}
    ${TS_JITERATI_ROOT}/include
  )
endif()

# ── libtermscript (Termlib Termscript bindings; links standalone) ─────
add_library(termscript STATIC
  src/dt_termscript.c
)
target_include_directories(termscript PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/termlib>
)
target_include_directories(termscript PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(termscript PUBLIC termlib termscript_standalone)

# ── libtermscript_stdlib (hybrid C + Termscript companion modules) ─
add_subdirectory(termscript/stdlib)
if(TARGET termscript_stdlib)
  # Every DT_TermVM carries the stdlib natively (registered in
  # dt_termscript_create), so the driver and transpiled programs get
  # G:import out of the box.
  target_link_libraries(termscript PUBLIC termscript_stdlib)
  target_compile_definitions(termscript PRIVATE TERMSCRIPT_HAVE_STDLIB=1)
endif()
if(TERMSCRIPT_JITERATI)
  foreach(lib IN LISTS CMAKE_CXX_IMPLICIT_LINK_LIBRARIES)
    if(lib MATCHES "^(stdc\\+\\+|c\\+\\+|c\\+\\+abi)$")
      list(APPEND TERMSCRIPT_NATIVE_FLAGS "-l${lib}")
      list(APPEND TERMSCRIPT_STATIC_FLAGS "-l${lib}")
    endif()
  endforeach()
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(termlib PRIVATE -Wall -Wextra)
  target_compile_options(termscript PRIVATE -Wall -Wextra)
  target_compile_options(termscript_standalone PRIVATE -Wall -Wextra)
endif()

# ── termscript driver ────────────────────────────────────────────────
# NOTE: the binary cannot land next to the target (domlibs/termlib/)
# because OUTPUT_NAME "termscript" would collide with the termscript/
# source directory; it goes to the build-tree bin/ instead.
add_executable(termscript-driver termscript/driver.c)
set_target_properties(termscript-driver PROPERTIES
  OUTPUT_NAME termscript
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
target_link_libraries(termscript-driver PRIVATE termscript)
set(TS_NATIVE_DEFAULT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(TS_NATIVE_DEFAULT ON)
endif()
option(TERMSCRIPT_NATIVE "Build QBE/Keystone native backend (Linux x86-64)" ${TS_NATIVE_DEFAULT})
if(TERMSCRIPT_NATIVE)
  if(NOT TS_NATIVE_DEFAULT)
    message(FATAL_ERROR "The QBE/Keystone object writer requires Linux x86-64")
  endif()
  include(termscript/native.cmake)
endif()
set(TS_DRIVER_BUILD_LIBS "\"$<TARGET_FILE:termscript>\",\n")
set(TS_DRIVER_INSTALL_LIBS "\"-ltermscript\",\n")
if(TARGET termscript_stdlib)
  target_compile_definitions(termscript-driver PRIVATE TS_DRIVER_HAS_STDLIB=1)
  string(APPEND TS_DRIVER_BUILD_LIBS "\"$<TARGET_FILE:termscript_stdlib>\",\n")
  string(APPEND TS_DRIVER_INSTALL_LIBS "\"-ltermscript_stdlib\",\n")
endif()
string(APPEND TS_DRIVER_BUILD_LIBS "\"$<TARGET_FILE:termscript_standalone>\",\n\"-Wl,-rpath,$<TARGET_FILE_DIR:novoparse_runtime>\",\n\"$<TARGET_FILE:novoparse_runtime>\",\n\"$<TARGET_FILE:termlib>\",\n")
string(APPEND TS_DRIVER_INSTALL_LIBS "\"-ltermscript_standalone\",\n\"-lnovoparse\",\n\"-ltermlib\",\n")
if(TARGET lexbor_static)
  string(APPEND TS_DRIVER_BUILD_LIBS "\"$<TARGET_FILE:lexbor_static>\",\n")
  string(APPEND TS_DRIVER_INSTALL_LIBS "\"-llexbor_static\",\n")
endif()
set(TS_DRIVER_FLAGS "\"-pthread\",\n")
set(TS_DRIVER_STATIC_FLAGS "\"-pthread\",\n")
foreach(flag IN LISTS TERMSCRIPT_NATIVE_FLAGS)
  string(APPEND TS_DRIVER_FLAGS "\"${flag}\",\n")
endforeach()
foreach(flag IN LISTS TERMSCRIPT_STATIC_FLAGS)
  string(APPEND TS_DRIVER_STATIC_FLAGS "\"${flag}\",\n")
endforeach()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/termscript_driver_config.h" CONTENT
"#define TS_DRIVER_BUILD_INCLUDE \"${CMAKE_CURRENT_SOURCE_DIR}\"
#define TS_DRIVER_INSTALL_INCLUDE \"${CMAKE_INSTALL_FULL_INCLUDEDIR}\"
#define TS_DRIVER_INSTALL_LIBDIR \"${CMAKE_INSTALL_FULL_LIBDIR}\"
static const char *const ts_driver_build_libs[] = {${TS_DRIVER_BUILD_LIBS} NULL};
static const char *const ts_driver_install_libs[] = {${TS_DRIVER_INSTALL_LIBS} NULL};
static const char *const ts_driver_native_flags[] = {${TS_DRIVER_FLAGS} NULL};
static const char *const ts_driver_static_flags[] = {${TS_DRIVER_STATIC_FLAGS} NULL};
")
target_include_directories(termscript-driver PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(termscript-driver PRIVATE -Wall -Wextra)
endif()

# ── install ──────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS termlib termscript termscript_standalone
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(TARGETS termscript-driver
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES termlib.h termlib.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/termlib
)
install(FILES termscript/termscript.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/termscript
)
install(FILES termscript/Termscript.ng
  DESTINATION ${CMAKE_INSTALL_DATADIR}/termlib
)

# Project documentation and the 22-chapter manual.
install(FILES README.md GUIDE.md INSTALL.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
install(DIRECTORY manual/
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/manual
  FILES_MATCHING PATTERN "*.md"
)
add_custom_target(termlib-manual
  SOURCES README.md GUIDE.md INSTALL.md
          ${CMAKE_CURRENT_SOURCE_DIR}/manual/termscript.md
          ${CMAKE_CURRENT_SOURCE_DIR}/manual/01-overview.md
          ${CMAKE_CURRENT_SOURCE_DIR}/manual/22-portability-testing.md
)

if(BUILD_TESTING)
  add_test(NAME termscript_parser_regression COMMAND
    $<TARGET_FILE:termscript-driver>
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_parser_regression.tsc)
  if(TERMSCRIPT_NATIVE)
    add_test(NAME termscript_native COMMAND ${CMAKE_COMMAND}
      -DDRIVER=$<TARGET_FILE:termscript-driver>
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/native-test
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_native.cmake)
    set_tests_properties(termscript_native PROPERTIES TIMEOUT 60)
  endif()
  add_executable(test_termlib_cpp tests/test_termlib_cpp.cpp)
  target_link_libraries(test_termlib_cpp PRIVATE termscript)
  add_test(NAME termlib_cpp COMMAND test_termlib_cpp)
  add_executable(test_termscript_abi tests/test_termscript_abi.c)
  target_link_libraries(test_termscript_abi PRIVATE termscript_standalone)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_termscript_abi PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME termscript_abi COMMAND test_termscript_abi)
  if(TARGET termscript_stdlib)
    add_executable(test_termscript_emulator tests/test_termscript_emulator.c)
    add_executable(test_termscript_compositor tests/test_termscript_compositor.c)
    target_compile_definitions(test_termscript_compositor PRIVATE _GNU_SOURCE)
    target_link_libraries(test_termscript_compositor PRIVATE termscript_stdlib)
    add_test(NAME termscript_compositor COMMAND test_termscript_compositor)
    set_tests_properties(termscript_compositor PROPERTIES TIMEOUT 30)
    add_test(NAME termscript_compositor_backends COMMAND ${CMAKE_COMMAND}
      -DDRIVER=$<TARGET_FILE:termscript-driver>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_compositor.tsc
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/compositor-backend-test
      -DNATIVE=${TERMSCRIPT_NATIVE}
      "-DMARKER=compositor and emulator OK"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_backends.cmake)
    target_compile_definitions(test_termscript_emulator PRIVATE _GNU_SOURCE)
    target_link_libraries(test_termscript_emulator PRIVATE termscript_stdlib Threads::Threads)
    add_test(NAME termscript_emulator COMMAND test_termscript_emulator)
    set_tests_properties(termscript_emulator PROPERTIES TIMEOUT 15)
    find_program(TERMSCRIPT_XVFB_RUN xvfb-run)
    if(TERMSCRIPT_XVFB_RUN AND TERMSCRIPT_X11 AND TARGET PkgConfig::TS_X11)
      add_test(NAME termscript_x11 COMMAND ${TERMSCRIPT_XVFB_RUN} -a $<TARGET_FILE:test_termscript_emulator> --x11)
      add_test(NAME termscript_compositor_x11 COMMAND ${TERMSCRIPT_XVFB_RUN} -a $<TARGET_FILE:test_termscript_compositor> --x11)
      set_tests_properties(termscript_compositor_x11 PROPERTIES TIMEOUT 30)
      set_tests_properties(termscript_x11 PROPERTIES TIMEOUT 15)
    endif()
    add_test(NAME termscript_emulator_backends COMMAND ${CMAKE_COMMAND}
      -DDRIVER=$<TARGET_FILE:termscript-driver>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_emulator.tsc
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/emulator-backend-test
      -DNATIVE=${TERMSCRIPT_NATIVE}
      "-DMARKER=emulator modules OK"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_backends.cmake)
    add_executable(test_termscript_stdlib tests/test_termscript_stdlib.c)
    target_compile_definitions(test_termscript_stdlib PRIVATE _GNU_SOURCE)
    target_link_libraries(test_termscript_stdlib PRIVATE termscript_stdlib)
    add_test(NAME termscript_stdlib COMMAND test_termscript_stdlib)
    add_test(NAME termscript_companions_backends COMMAND ${CMAKE_COMMAND}
      -DDRIVER=$<TARGET_FILE:termscript-driver>
      -DSTDLIB=${CMAKE_CURRENT_SOURCE_DIR}/termscript/stdlib
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/companion-backend-test
      -DNATIVE=${TERMSCRIPT_NATIVE}
      -DJITERATI=${TERMSCRIPT_JITERATI}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_companions.cmake)
    set_tests_properties(termscript_companions_backends PROPERTIES TIMEOUT 120)
    add_executable(test_termscript_shell tests/test_termscript_shell.c)
    target_compile_definitions(test_termscript_shell PRIVATE _GNU_SOURCE)
    target_link_libraries(test_termscript_shell PRIVATE termscript_stdlib)
    add_test(NAME termscript_shell COMMAND test_termscript_shell)
    add_test(NAME termscript_backends COMMAND ${CMAKE_COMMAND}
      -DDRIVER=$<TARGET_FILE:termscript-driver>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_terminal.tsc
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/backend-test
      -DNATIVE=${TERMSCRIPT_NATIVE}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_backends.cmake)
    add_test(NAME termscript_shell_backends COMMAND ${CMAKE_COMMAND}
      -DDRIVER=$<TARGET_FILE:termscript-driver>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_shell.tsc
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/shell-backend-test
      -DNATIVE=${TERMSCRIPT_NATIVE}
      "-DMARKER=shell stdlib OK"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_backends.cmake)
    set_tests_properties(termscript_stdlib termscript_backends termscript_shell
      termscript_shell_backends PROPERTIES TIMEOUT 30)
  endif()
endif()
