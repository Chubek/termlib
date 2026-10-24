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
  termscript/source/runtime.c
  termscript/source/parser.c
  termscript/backends/C.cpp
  termscript/backends/QBE.cpp
  ${TERMLIB_PARSER_H}
)
target_include_directories(termscript_standalone PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(termscript_standalone PRIVATE
  ${TERMLIB_PARSER_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/novoparse/runtime
  ${CMAKE_CURRENT_SOURCE_DIR}/termscript
  ${CMAKE_CURRENT_SOURCE_DIR}/termscript/include
)
target_link_libraries(termscript_standalone PRIVATE Threads::Threads novoparse_runtime)
if(UNIX)
  target_link_libraries(termscript_standalone PUBLIC m)
endif()

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
    termscript/backends/Jiterati.cpp
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
add_subdirectory(termscript/stdlib/crdt)
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
add_executable(termscript-driver termscript/source/driver.c)
set_target_properties(termscript-driver PROPERTIES
  OUTPUT_NAME termscript
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
if(APPLE)
  set_property(TARGET termscript-driver PROPERTY INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
elseif(UNIX)
  set_property(TARGET termscript-driver PROPERTY INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
target_link_libraries(termscript-driver PRIVATE termscript)
add_executable(termak termscript/tools/termak.c)
target_compile_definitions(termak PRIVATE
  TERMAK_BUILD_ENTRY="${CMAKE_CURRENT_SOURCE_DIR}/termscript/termak/Termakfile.rb"
  TERMAK_INSTALL_ENTRY="${CMAKE_INSTALL_FULL_DATADIR}/termscript/termak/Termakfile.rb")
set_target_properties(termak PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
install(TARGETS termak RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT termak)
install(DIRECTORY termscript/termak/ DESTINATION ${CMAKE_INSTALL_DATADIR}/termscript/termak COMPONENT termak
  FILES_MATCHING PATTERN "*.rb")
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
if(UNIX)
  string(APPEND TS_DRIVER_FLAGS "\"-lm\",\n")
  string(APPEND TS_DRIVER_STATIC_FLAGS "\"-lm\",\n")
endif()
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
if(TARGET termscript_stdlib)
  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/bin/xw3m
    COMMAND $<TARGET_FILE:termscript-driver>
      -o ${CMAKE_CURRENT_BINARY_DIR}/bin/xw3m
      ${CMAKE_CURRENT_SOURCE_DIR}/xw3m/xw3m.tsc
    DEPENDS termscript-driver ${CMAKE_CURRENT_SOURCE_DIR}/xw3m/xw3m.tsc
    COMMENT "Compiling the Termscript xw3m sample browser" VERBATIM)
  add_custom_target(xw3m DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/bin/xw3m)
  install(DIRECTORY xw3m/ DESTINATION ${CMAKE_INSTALL_DATADIR}/termscript/xw3m)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(termscript-driver PRIVATE -Wall -Wextra)
endif()

# ── install ──────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS termlib termscript termscript_standalone novoparse_runtime
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
install(FILES termscript/include/Termscript.h termscript/include/Termscript.hpp
  termscript/include/TSC-API.h termscript/include/TSC-Module.h
  termscript/include/TSC-Backend.h termscript/include/TSC-Protocol.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/termscript/include)
install(FILES termscript/Termscript.ng
  DESTINATION ${CMAKE_INSTALL_DATADIR}/termlib
)
install(FILES termscript/IMPLEMENTATION.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES termscript/MIGRATING-0.4.md DESTINATION ${CMAKE_INSTALL_DOCDIR}/termscript)
install(FILES termscript/termak/README.md DESTINATION ${CMAKE_INSTALL_DOCDIR}/termak COMPONENT termak)

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
  add_executable(test_replay_export tests/test_replay_export.c)
  target_link_libraries(test_replay_export PRIVATE termlib)
  add_test(NAME replay_export COMMAND test_replay_export)
  set_tests_properties(replay_export PROPERTIES TIMEOUT 30)
  add_executable(test_connections tests/test_connections.c)
  target_link_libraries(test_connections PRIVATE termlib)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_connections PRIVATE -Wall -Wextra -Werror)
  endif()
  add_test(NAME connections COMMAND test_connections)
  set_tests_properties(connections PROPERTIES TIMEOUT 10)
  if(UNIX AND NOT APPLE AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_executable(test_replay_export_alloc tests/test_replay_export.c)
    target_compile_definitions(test_replay_export_alloc PRIVATE TS_TEST_ALLOC=1)
    target_compile_options(test_replay_export_alloc PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(test_replay_export_alloc PRIVATE termlib)
    target_link_options(test_replay_export_alloc PRIVATE
      -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
    add_test(NAME replay_export_alloc COMMAND test_replay_export_alloc)
    set_tests_properties(replay_export_alloc PROPERTIES TIMEOUT 30)
    add_executable(test_connections_alloc tests/test_connections.c)
    target_compile_definitions(test_connections_alloc PRIVATE TS_TEST_ALLOC=1)
    target_compile_options(test_connections_alloc PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(test_connections_alloc PRIVATE termlib)
    target_link_options(test_connections_alloc PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc)
    add_test(NAME connections_alloc COMMAND test_connections_alloc)
    set_tests_properties(connections_alloc PROPERTIES TIMEOUT 10)
  endif()
  find_program(TERMAK_RUBY ruby)
  if(TERMAK_RUBY)
    add_test(NAME termak COMMAND ${TERMAK_RUBY} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_termak.rb)
  endif()
  add_executable(test_termscript_modern tests/test_termscript_modern.c)
  target_link_libraries(test_termscript_modern PRIVATE termscript_standalone)
  add_test(NAME termscript_modern COMMAND test_termscript_modern)
  add_executable(test_termscript_numeric tests/test_termscript_numeric.c)
  target_link_libraries(test_termscript_numeric PRIVATE termscript_standalone)
  add_test(NAME termscript_numeric COMMAND test_termscript_numeric)
  set_tests_properties(termscript_numeric PROPERTIES TIMEOUT 30)
  add_test(NAME termscript_numeric_sections COMMAND ${CMAKE_COMMAND}
    -DDRIVER=$<TARGET_FILE:termscript-driver>
    -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_numeric_sections.tsc
    -DWORK=${CMAKE_CURRENT_BINARY_DIR}/numeric-section-test
    -DNATIVE=${TERMSCRIPT_NATIVE} -DJITERATI=${TERMSCRIPT_JITERATI}
    "-DMARKER=numeric sections OK"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_backends.cmake)
  add_test(NAME termscript_numeric_failures COMMAND ${CMAKE_COMMAND}
    -DDRIVER=$<TARGET_FILE:termscript-driver>
    -DWORK=${CMAKE_CURRENT_BINARY_DIR}/numeric-failure-test
    -DNATIVE=${TERMSCRIPT_NATIVE} -DJITERATI=${TERMSCRIPT_JITERATI}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_numeric_failures.cmake)
  set_tests_properties(termscript_numeric_sections termscript_numeric_failures PROPERTIES TIMEOUT 120)
  add_executable(test_termscript_parser_reuse tests/test_termscript_parser_reuse.c)
  target_link_libraries(test_termscript_parser_reuse PRIVATE termscript_standalone Threads::Threads)
  add_test(NAME termscript_parser_reuse COMMAND test_termscript_parser_reuse)
  set_tests_properties(termscript_parser_reuse PROPERTIES TIMEOUT 60)
  add_executable(test_termscript_cpp tests/test_termscript_cpp.cpp)
  target_compile_features(test_termscript_cpp PRIVATE cxx_std_17)
  target_link_libraries(test_termscript_cpp PRIVATE termscript_standalone)
  add_test(NAME termscript_cpp COMMAND test_termscript_cpp)
  if(TARGET termscript_stdlib)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
      add_test(NAME termscript_openai COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_termscript_openai.py
        $<TARGET_FILE:termscript-driver> vm c)
    endif()
    add_executable(test_termscript_browser_core tests/test_termscript_browser_core.c)
    target_link_libraries(test_termscript_browser_core PRIVATE termscript_stdlib)
    add_test(NAME termscript_browser_core COMMAND test_termscript_browser_core)
    if(UNIX AND NOT APPLE AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      add_executable(test_termscript_numeric_alloc tests/test_termscript_numeric_alloc.c)
      target_link_libraries(test_termscript_numeric_alloc PRIVATE termscript_stdlib)
      target_compile_options(test_termscript_numeric_alloc PRIVATE -Wall -Wextra -Werror)
      target_link_options(test_termscript_numeric_alloc PRIVATE
        -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=strdup -Wl,--wrap=strndup)
      add_test(NAME termscript_numeric_alloc COMMAND test_termscript_numeric_alloc)
      set_tests_properties(termscript_numeric_alloc PROPERTIES TIMEOUT 30)
    endif()
    add_executable(test_termscript_numeric_stdlib tests/test_termscript_numeric.c)
    target_compile_definitions(test_termscript_numeric_stdlib PRIVATE
      TS_NUMERIC_STDLIB=1 TS_NUMERIC_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_numeric.tsc")
    target_link_libraries(test_termscript_numeric_stdlib PRIVATE termscript_stdlib)
    add_test(NAME termscript_numeric_stdlib COMMAND test_termscript_numeric_stdlib)
    set_tests_properties(termscript_numeric_stdlib PROPERTIES TIMEOUT 60)
    add_test(NAME termscript_numeric_backends COMMAND ${CMAKE_COMMAND}
      -DDRIVER=$<TARGET_FILE:termscript-driver>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_numeric.tsc
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/numeric-backend-test
      -DNATIVE=${TERMSCRIPT_NATIVE} -DJITERATI=${TERMSCRIPT_JITERATI}
      "-DMARKER=finite numeric core OK"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_backends.cmake)
    set_tests_properties(termscript_numeric_backends PROPERTIES TIMEOUT 60)
    add_executable(test_termscript_stdio tests/test_termscript_stdio.c)
    target_link_libraries(test_termscript_stdio PRIVATE termscript_stdlib)
    add_test(NAME termscript_stdio COMMAND test_termscript_stdio)
    set_tests_properties(termscript_stdio PROPERTIES TIMEOUT 10)
    find_program(TS_REPLAY_NODE node)
    find_program(TS_REPLAY_PYTHON python3)
    find_program(TS_REPLAY_ASSEMBLER NAMES wasm-as wat2wasm)
    if(TS_REPLAY_NODE AND TS_REPLAY_PYTHON AND TS_REPLAY_ASSEMBLER)
      add_test(NAME replay_export_runtime COMMAND ${TS_REPLAY_PYTHON}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay_exports.py
        $<TARGET_FILE:test_replay_export> ${TS_REPLAY_NODE} ${TS_REPLAY_ASSEMBLER}
        $<TARGET_FILE:termscript-driver> ${CMAKE_CURRENT_BINARY_DIR}/replay-export-test
        ${TERMSCRIPT_NATIVE} ${TERMSCRIPT_JITERATI})
      set_tests_properties(replay_export_runtime PROPERTIES TIMEOUT 120)
    endif()
    add_executable(test_termscript_infrastructure tests/test_termscript_infrastructure.c)
    target_link_libraries(test_termscript_infrastructure PRIVATE termscript_stdlib)
    add_test(NAME termscript_infrastructure COMMAND test_termscript_infrastructure)
    set_tests_properties(termscript_infrastructure PROPERTIES TIMEOUT 20)
    add_test(NAME termscript_infrastructure_backends COMMAND ${CMAKE_COMMAND}
      -DDRIVER=$<TARGET_FILE:termscript-driver>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_infrastructure.tsc
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/infrastructure-backend-test
      -DNATIVE=${TERMSCRIPT_NATIVE} -DJITERATI=${TERMSCRIPT_JITERATI}
      "-DMARKER=infrastructure modules OK"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_backends.cmake)
    set_tests_properties(termscript_infrastructure_backends PROPERTIES TIMEOUT 60)
    add_test(NAME termscript_device_backends COMMAND ${CMAKE_COMMAND}
      -DDRIVER=$<TARGET_FILE:termscript-driver>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_devices.tsc
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/device-backend-test
      -DNATIVE=${TERMSCRIPT_NATIVE} -DJITERATI=${TERMSCRIPT_JITERATI}
      "-DMARKER=device modules OK"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_backends.cmake)
    set_tests_properties(termscript_device_backends PROPERTIES TIMEOUT 30)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      add_executable(test_termscript_devices tests/test_termscript_devices.c)
      target_link_libraries(test_termscript_devices PRIVATE termscript_stdlib)
      target_link_options(test_termscript_devices PRIVATE -Wl,--wrap=ioctl -Wl,--wrap=write)
      add_test(NAME termscript_devices COMMAND test_termscript_devices)
      set_tests_properties(termscript_devices PROPERTIES TIMEOUT 10)
    endif()
    if(TERMSCRIPT_WAYLAND AND TARGET PkgConfig::TS_WAYLAND_SERVER)
      add_executable(test_termscript_wayland tests/test_termscript_wayland.c)
      target_link_libraries(test_termscript_wayland PRIVATE termscript_stdlib PkgConfig::TS_WAYLAND_SERVER)
      add_test(NAME termscript_wayland COMMAND test_termscript_wayland)
      set_tests_properties(termscript_wayland PROPERTIES TIMEOUT 10)
    endif()
    add_executable(test_termscript_wayland_unavailable
      tests/test_termscript_wayland_unavailable.c termscript/stdlib/wayland/wayland.c)
    target_link_libraries(test_termscript_wayland_unavailable PRIVATE termscript_stdlib)
    add_test(NAME termscript_wayland_unavailable COMMAND test_termscript_wayland_unavailable)
    add_executable(test_termscript_parsers tests/test_termscript_parsers.c)
    target_compile_definitions(test_termscript_parsers PRIVATE
      TS_TEST_SHELL_EXAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/termscript/examples/tsh.tsc")
    target_link_libraries(test_termscript_parsers PRIVATE termscript_stdlib)
    add_test(NAME termscript_parsers COMMAND test_termscript_parsers)
    set_tests_properties(termscript_parsers PROPERTIES TIMEOUT 30)
    add_test(NAME termscript_parsers_backends COMMAND ${CMAKE_COMMAND}
      -DDRIVER=$<TARGET_FILE:termscript-driver>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_parsers.tsc
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/parsers-backend-test
      -DNATIVE=${TERMSCRIPT_NATIVE} -DJITERATI=${TERMSCRIPT_JITERATI}
      "-DMARKER=parser modules OK"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_backends.cmake)
    set_tests_properties(termscript_parsers_backends PROPERTIES TIMEOUT 30)
    if(UNIX AND NOT APPLE AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      add_executable(test_termscript_infrastructure_alloc tests/test_termscript_infrastructure_alloc.c)
      target_link_libraries(test_termscript_infrastructure_alloc PRIVATE termscript_stdlib)
      target_link_options(test_termscript_infrastructure_alloc PRIVATE
        -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc
        -Wl,--wrap=strdup -Wl,--wrap=free)
      add_test(NAME termscript_infrastructure_alloc COMMAND test_termscript_infrastructure_alloc)
      set_tests_properties(termscript_infrastructure_alloc PROPERTIES TIMEOUT 20)
      add_executable(test_termscript_parser_alloc tests/test_termscript_parser_alloc.c)
      target_link_libraries(test_termscript_parser_alloc PRIVATE termscript_stdlib)
      target_link_options(test_termscript_parser_alloc PRIVATE
        -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc
        -Wl,--wrap=strdup -Wl,--wrap=free)
      add_test(NAME termscript_parser_alloc COMMAND test_termscript_parser_alloc)
      set_tests_properties(termscript_parser_alloc PROPERTIES TIMEOUT 30)
    endif()
    add_executable(test_termscript_posix_ipc tests/test_termscript_posix_ipc.c)
    target_link_libraries(test_termscript_posix_ipc PRIVATE termscript_stdlib)
    if(TS_HAVE_MQUEUE)
      target_compile_definitions(test_termscript_posix_ipc PRIVATE TS_TEST_HAVE_MQUEUE=1)
    endif()
    add_test(NAME termscript_posix_ipc COMMAND test_termscript_posix_ipc)
    set_tests_properties(termscript_posix_ipc PROPERTIES TIMEOUT 10)
    # Compile the real no-mq_open path independently, even on Linux hosts.
    add_executable(test_termscript_mqueue_unavailable
      tests/test_termscript_mqueue_unavailable.c termscript/stdlib/mqueue/mqueue.c)
    target_link_libraries(test_termscript_mqueue_unavailable PRIVATE termscript_stdlib)
    add_test(NAME termscript_mqueue_unavailable COMMAND test_termscript_mqueue_unavailable)
    add_executable(test_termscript_input tests/test_termscript_input.c)
    target_compile_definitions(test_termscript_input PRIVATE
      TS_TEST_FONT="${CMAKE_CURRENT_SOURCE_DIR}/third_party/harfbuzz/perf/fonts/Roboto-Regular.ttf")
    target_link_libraries(test_termscript_input PRIVATE termscript_stdlib)
    add_test(NAME termscript_input COMMAND test_termscript_input)
    add_test(NAME termscript_input_backends COMMAND ${CMAKE_COMMAND}
      -DDRIVER=$<TARGET_FILE:termscript-driver>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_input.tsc
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/input-backend-test
      -DNATIVE=${TERMSCRIPT_NATIVE} -DJITERATI=${TERMSCRIPT_JITERATI}
      "-DMARKER=input decoding OK"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_backends.cmake)
    set_tests_properties(termscript_input termscript_input_backends PROPERTIES TIMEOUT 30)
    set_tests_properties(termscript_input_backends PROPERTIES ENVIRONMENT
      "TS_TEST_FONT=${CMAKE_CURRENT_SOURCE_DIR}/third_party/harfbuzz/perf/fonts/Roboto-Regular.ttf")
    add_executable(test_termscript_crdt tests/test_termscript_crdt.c)
    target_link_libraries(test_termscript_crdt PRIVATE termscript_stdlib)
    add_test(NAME termscript_crdt COMMAND test_termscript_crdt)
    set_tests_properties(termscript_crdt PROPERTIES TIMEOUT 10)
    add_test(NAME termscript_crdt_backends COMMAND ${CMAKE_COMMAND}
      -DDRIVER=$<TARGET_FILE:termscript-driver>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_crdt.tsc
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/crdt-backend-test
      -DNATIVE=${TERMSCRIPT_NATIVE} -DJITERATI=${TERMSCRIPT_JITERATI}
      "-DMARKER=CRDT convergence OK"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_backends.cmake)
    set_tests_properties(termscript_crdt_backends PROPERTIES TIMEOUT 30)
    add_executable(test_termscript_new_stdlib tests/test_termscript_new_stdlib.c)
    target_link_libraries(test_termscript_new_stdlib PRIVATE termscript_stdlib)
    add_test(NAME termscript_new_stdlib COMMAND test_termscript_new_stdlib)
    add_executable(test_termscript_termart tests/test_termscript_termart.c)
    target_link_libraries(test_termscript_termart PRIVATE termscript_stdlib)
    add_test(NAME termscript_termart COMMAND test_termscript_termart)
    foreach(art_source IN ITEMS tests/termscript_termart.tsc termscript/examples/MyApp.ta termscript/examples/Welcome.ta)
      get_filename_component(art_name ${art_source} NAME_WE)
      add_test(NAME ${art_name}_backends COMMAND ${CMAKE_COMMAND}
        -DDRIVER=$<TARGET_FILE:termscript-driver>
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/${art_source}
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/${art_name}-backend-test
        -DNATIVE=${TERMSCRIPT_NATIVE} -DJITERATI=${TERMSCRIPT_JITERATI}
        "-DMARKER=[+][---]+[+]"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_backends.cmake)
    endforeach()
    add_executable(test_termscript_fs_view tests/test_termscript_fs_view.c)
    target_link_libraries(test_termscript_fs_view PRIVATE termscript_stdlib)
    add_test(NAME termscript_fs_view COMMAND test_termscript_fs_view)
    set_tests_properties(termscript_fs_view PROPERTIES TIMEOUT 10)
    add_test(NAME termscript_fs_view_backends COMMAND ${CMAKE_COMMAND}
      -DDRIVER=$<TARGET_FILE:termscript-driver>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_fs_view.tsc
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/fs-view-backend-test
      -DNATIVE=${TERMSCRIPT_NATIVE} -DJITERATI=${TERMSCRIPT_JITERATI}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_fs_view.cmake)
    set_tests_properties(termscript_fs_view_backends PROPERTIES TIMEOUT 30)
    add_test(NAME termscript_entry COMMAND ${CMAKE_COMMAND}
      -DDRIVER=$<TARGET_FILE:termscript-driver>
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/entry-test
      -DNATIVE=${TERMSCRIPT_NATIVE} -DJITERATI=${TERMSCRIPT_JITERATI}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_entry.cmake)
    add_test(NAME termscript_new_stdlib_backends COMMAND ${CMAKE_COMMAND}
      -DDRIVER=$<TARGET_FILE:termscript-driver>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_new_stdlib.tsc
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/new-stdlib-backend-test
      -DNATIVE=${TERMSCRIPT_NATIVE} -DJITERATI=${TERMSCRIPT_JITERATI}
      "-DMARKER=new stdlib OK"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_backends.cmake)
    add_test(NAME termscript_modern_backends COMMAND ${CMAKE_COMMAND}
      -DDRIVER=$<TARGET_FILE:termscript-driver>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_modern.tsc
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/modern-backend-test
      -DNATIVE=${TERMSCRIPT_NATIVE} -DJITERATI=${TERMSCRIPT_JITERATI}
      "-DMARKER=example-style core OK"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_backends.cmake)
    add_test(NAME termscript_browser_core_backends COMMAND ${CMAKE_COMMAND}
      -DDRIVER=$<TARGET_FILE:termscript-driver>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_browser_core.tsc
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/browser-core-test
      -DNATIVE=${TERMSCRIPT_NATIVE}
      -DJITERATI=${TERMSCRIPT_JITERATI}
      "-DMARKER=browser core OK"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_backends.cmake)
    set_tests_properties(termscript_browser_core_backends PROPERTIES TIMEOUT 60)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
      set(XW3M_TEST_FLAGS "")
      if(TERMSCRIPT_NATIVE)
        list(APPEND XW3M_TEST_FLAGS --native)
      endif()
      if(TERMSCRIPT_JITERATI)
        list(APPEND XW3M_TEST_FLAGS --jit)
      endif()
      if(TERMSCRIPT_HTTP_TLS)
        list(APPEND XW3M_TEST_FLAGS --tls)
      endif()
      add_test(NAME xw3m_offline COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_xw3m.py
        --driver $<TARGET_FILE:termscript-driver>
        --root ${CMAKE_CURRENT_SOURCE_DIR}
        --work ${CMAKE_CURRENT_BINARY_DIR}/xw3m-test ${XW3M_TEST_FLAGS})
      set_tests_properties(xw3m_offline PROPERTIES TIMEOUT 180)
    endif()
  endif()
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
  add_executable(test_termscript_sections tests/test_termscript_sections.c)
  target_link_libraries(test_termscript_sections PRIVATE termscript_standalone)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_termscript_sections PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME termscript_sections COMMAND test_termscript_sections)
  set_tests_properties(termscript_sections PROPERTIES TIMEOUT 30)
  foreach(kind IN ITEMS ok fail)
    set(unit ${CMAKE_CURRENT_BINARY_DIR}/section_fixture_${kind}.c)
    add_custom_command(OUTPUT ${unit}
      COMMAND $<TARGET_FILE:test_termscript_sections> --emit-${kind} ${unit}
      DEPENDS test_termscript_sections tests/test_termscript_sections.c
      VERBATIM)
    add_executable(test_termscript_sections_c_${kind} ${unit})
    target_link_libraries(test_termscript_sections_c_${kind} PRIVATE termscript_standalone)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(test_termscript_sections_c_${kind} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME termscript_sections_c_${kind} COMMAND test_termscript_sections_c_${kind})
  endforeach()
  add_test(NAME termscript_section_failures COMMAND ${CMAKE_COMMAND}
    -DDRIVER=$<TARGET_FILE:termscript-driver>
    -DWORK=${CMAKE_CURRENT_BINARY_DIR}/section-failure-test
    -DNATIVE=${TERMSCRIPT_NATIVE}
    -DJITERATI=${TERMSCRIPT_JITERATI}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_section_failures.cmake)
  set_tests_properties(termscript_section_failures PROPERTIES TIMEOUT 60)
  if(TARGET termscript_stdlib)
    add_test(NAME termscript_sections_backends COMMAND ${CMAKE_COMMAND}
      -DDRIVER=$<TARGET_FILE:termscript-driver>
      -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_sections_fixture.tsc
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/section-backend-test
      -DNATIVE=${TERMSCRIPT_NATIVE}
      -DJITERATI=${TERMSCRIPT_JITERATI}
      "-DMARKER=sections OK"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/termscript_backends.cmake)
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
