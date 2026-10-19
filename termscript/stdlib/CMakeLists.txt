# Termscript standard library — hybrid C native modules + .tsc companions.
#
# Layout:
#   <name>/<name>.c     native TS_Module (shell uses C++20)
#   <name>/<name>.h     module declaration (installed)
#   <name>/<name>.tsc   Termscript companion, loaded via G:import (installed)
#   common/             shared sbuf/args/list-store/DOM helpers (private)
#   gen/                Q-generated vectors (committed; optionally renewed)
#   q/tsvec.q           domqlib Q template behind gen/
#   ts_stdlib.[hc]      registry + default search dir (installed)
#
# Third-party code compiled in:
#   third_party/tomlc99/toml.c      TOML parser behind std.toml
#   third_party/isocline/src/isocline.c  line editing behind std.readline
#   third_party/lexbor              HTML, CSS parsing and selector matching

option(DOMWEAVE_BUILD_TERMSCRIPT_STDLIB "Build the Termscript stdlib" ON)

if(NOT DOMWEAVE_BUILD_TERMSCRIPT_STDLIB)
  return ()
endif()

set(TS_STDLIB_MODULES
  array autocomp baudrate bidi buffer cell codec color css csv draw edit_history env exec expect font fsm glob html http io
  ipc json key layout list log map menu notify panel pipe pty prompt prompt_string readline regex rules
  sched screen shell signal socket stream style syntax table termcap terminfo test text_search theme
  terminal termios toml tui utf8 verbatim vterm widget word_motion yaml drm poll x11 compositor emulator
  gapbuffer piecetable memory persian_script set stdio datagram procfs sysfs crdt
  input font_shape shm mqueue grammar peg evdev uvdev wayland index multiplex protocols
)

set(TS_STDLIB_COMMON_SOURCES
  common/sbuf.c
  common/values.c
  common/list_store.c
  common/dom.c
  common/text.c
  common/grid.c
  common/html.c
  common/fs_view.c
)
set(TS_STDLIB_SOURCES ${TS_STDLIB_COMMON_SOURCES} ts_stdlib.c http/fetch.cpp)
foreach (m IN LISTS TS_STDLIB_MODULES)
  if(m STREQUAL "shell")
    list(APPEND TS_STDLIB_SOURCES ${m}/${m}.cpp)
  else()
    list(APPEND TS_STDLIB_SOURCES ${m}/${m}.c)
  endif()
  if(m STREQUAL "shell")
    set(module_source "${m}/${m}.cpp")
  else()
    set(module_source "${m}/${m}.c")
  endif()
  foreach(required IN ITEMS "${module_source}" "${m}/${m}.h" "${m}/${m}.tsc")
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${required}")
      message(FATAL_ERROR "Shipped Termscript module is missing ${required}")
    endif()
    file(SIZE "${CMAKE_CURRENT_SOURCE_DIR}/${required}" required_size)
    if(required_size EQUAL 0)
      message(FATAL_ERROR "Shipped Termscript module has an empty placeholder: ${required}")
    endif()
  endforeach()
endforeach ()

# Aliases have real descriptors in font.c/css.c, not separate .c files.
foreach(m IN ITEMS font_discovery stylesheet)
  foreach(extension IN ITEMS h tsc)
    set(required "${CMAKE_CURRENT_SOURCE_DIR}/${m}/${m}.${extension}")
    if(NOT EXISTS "${required}")
      message(FATAL_ERROR "Shipped Termscript alias is missing ${required}")
    endif()
    file(SIZE "${required}" required_size)
    if(required_size EQUAL 0)
      message(FATAL_ERROR "Shipped Termscript alias has an empty placeholder: ${required}")
    endif()
  endforeach()
endforeach()

# Vendored third-party sources (warning-cleanliness of our own files
# is enforced below; vendored files compile with -w).
set(TS_STDLIB_TOML_C ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/tomlc99/toml.c)
set(TS_STDLIB_ISOCLINE_C ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/isocline/src/isocline.c)
set(TS_STDLIB_TOML_INC ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/tomlc99)
set(TS_STDLIB_ISOCLINE_INC ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/isocline/include)
set(TS_STDLIB_LIBSHELL ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/libshell)
set(TS_STDLIB_SHELL_PARSER
  ${TS_STDLIB_LIBSHELL}/cli/lsh-lex.cpp
  ${TS_STDLIB_LIBSHELL}/cli/libsh-parse.cpp)

if(NOT EXISTS "${TS_STDLIB_TOML_C}" OR NOT EXISTS "${TS_STDLIB_ISOCLINE_C}")
  message(FATAL_ERROR "Termscript stdlib requires the bundled tomlc99/isocline sources")
endif()
if(NOT EXISTS "${TS_STDLIB_LIBSHELL}/include/LibShell-Posix.hpp"
   OR NOT EXISTS "${TS_STDLIB_LIBSHELL}/cli/libsh-parse.cpp")
  message(FATAL_ERROR "Termscript std.shell requires the bundled libshell sources")
endif()

find_package(PkgConfig REQUIRED)
include(CheckSymbolExists)
include(CheckLibraryExists)
include(CMakePushCheckState)
# Linux before glibc 2.34 keeps POSIX IPC in librt. Newer libc and BSD systems
# may provide it directly. The mqueue adapter reports unavailable explicitly on
# platforms without mq_open; no queue operation succeeds as a placeholder.
cmake_push_check_state(RESET)
check_library_exists(rt mq_open "" TS_IPC_NEEDS_RT)
if(TS_IPC_NEEDS_RT)
  set(CMAKE_REQUIRED_LIBRARIES rt)
endif()
check_symbol_exists(mq_open "mqueue.h" TS_HAVE_MQUEUE)
cmake_pop_check_state()
pkg_check_modules(TS_UTF8PROC REQUIRED IMPORTED_TARGET libutf8proc)
pkg_check_modules(TS_FRIBIDI REQUIRED IMPORTED_TARGET fribidi)
pkg_check_modules(TS_FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(TS_FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(TS_HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
option(TERMSCRIPT_X11 "Enable the Xlib window backend" ON)
set(TS_EXTRA_PKGS "")
set(TS_STDLIB_EXTRA_REQUIRES "")
option(TERMSCRIPT_HTTP_TLS "Enable verified HTTPS in std.http.fetch" ON)
if(TERMSCRIPT_HTTP_TLS)
  pkg_check_modules(TS_OPENSSL REQUIRED IMPORTED_TARGET openssl>=3.0)
  list(APPEND TS_EXTRA_PKGS TS_OPENSSL)
  string(APPEND TS_STDLIB_EXTRA_REQUIRES " openssl >= 3.0")
endif()
if(TERMSCRIPT_X11)
  pkg_check_modules(TS_X11 QUIET IMPORTED_TARGET x11)
  if(TS_X11_FOUND)
    list(APPEND TS_EXTRA_PKGS TS_X11)
    string(APPEND TS_STDLIB_EXTRA_REQUIRES " x11")
  else()
    message(STATUS "Xlib not found: std.x11.available will return false")
  endif()
endif()
set(TS_DRM_DEFAULT OFF)
option(TERMSCRIPT_WAYLAND "Enable optional Wayland registry/connection support" ON)
if(TERMSCRIPT_WAYLAND)
  # 1.23 provides the bounded connection-buffer API used by this adapter.
  pkg_check_modules(TS_WAYLAND QUIET IMPORTED_TARGET wayland-client>=1.23)
  if(TS_WAYLAND_FOUND)
    list(APPEND TS_EXTRA_PKGS TS_WAYLAND)
    string(APPEND TS_STDLIB_EXTRA_REQUIRES " wayland-client >= 1.23")
    if(BUILD_TESTING)
      pkg_check_modules(TS_WAYLAND_SERVER QUIET IMPORTED_TARGET GLOBAL wayland-server)
    endif()
  else()
    message(STATUS "Wayland client >= 1.23 not found: std.wayland.available will return false")
  endif()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(TS_DRM_DEFAULT ON)
endif()
option(TERMSCRIPT_DRM "Enable bundled libdrm discovery (Linux)" ${TS_DRM_DEFAULT})
if(TERMSCRIPT_DRM)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The bundled Termscript DRM backend requires Linux")
  endif()
  set(TS_DRM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/libdrm)
  set(TS_DRM_SOURCES "")
  foreach(source IN ITEMS xf86drm.c xf86drmHash.c xf86drmRandom.c xf86drmSL.c xf86drmMode.c)
    list(APPEND TS_DRM_SOURCES ${TS_DRM_ROOT}/${source})
  endforeach()
  # Put the core objects in the stdlib archive so generated programs have
  # the same dependency closure as embeds, without a system libdrm dependency.
  add_library(termscript_drm_core OBJECT ${TS_DRM_SOURCES})
  target_include_directories(termscript_drm_core PRIVATE ${TS_DRM_ROOT} ${TS_DRM_ROOT}/include/drm)
  target_compile_definitions(termscript_drm_core PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64 MAJOR_IN_SYSMACROS=1)
endif()

set(LEXBOR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LEXBOR_INSTALL_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(../../third_party/lexbor ${CMAKE_CURRENT_BINARY_DIR}/lexbor)

add_library(termscript_stdlib STATIC
  $<TARGET_OBJECTS:termscript_crdt_core>
  ${TS_STDLIB_SOURCES}
  ${TS_STDLIB_TOML_C}
  ${TS_STDLIB_ISOCLINE_C}
  ${TS_STDLIB_SHELL_PARSER}
)
add_library(termscript::stdlib ALIAS termscript_stdlib)
if(TERMSCRIPT_HTTP_TLS)
  target_compile_definitions(termscript_stdlib PRIVATE TS_STD_HTTP_TLS=1)
  target_link_libraries(termscript_stdlib PRIVATE PkgConfig::TS_OPENSSL)
endif()
if(TERMSCRIPT_WAYLAND AND TS_WAYLAND_FOUND)
  target_compile_definitions(termscript_stdlib PRIVATE TS_STD_HAVE_WAYLAND=1)
  target_link_libraries(termscript_stdlib PRIVATE PkgConfig::TS_WAYLAND)
endif()
if(TS_HAVE_MQUEUE)
  target_compile_definitions(termscript_stdlib PRIVATE TS_STD_HAVE_MQUEUE=1)
endif()
set(TS_STDLIB_IPC_LIBS "")
if(TS_IPC_NEEDS_RT)
  target_link_libraries(termscript_stdlib PRIVATE rt)
  set(TS_STDLIB_IPC_LIBS " -lrt")
endif()
if(TERMSCRIPT_X11 AND TS_X11_FOUND)
  target_compile_definitions(termscript_stdlib PRIVATE TS_STD_HAVE_X11=1)
  target_link_libraries(termscript_stdlib PRIVATE PkgConfig::TS_X11)
endif()
if(TERMSCRIPT_DRM)
  target_sources(termscript_stdlib PRIVATE $<TARGET_OBJECTS:termscript_drm_core>)
  target_compile_definitions(termscript_stdlib PRIVATE TS_STD_HAVE_DRM=1)
  target_include_directories(termscript_stdlib SYSTEM PRIVATE ${TS_DRM_ROOT} ${TS_DRM_ROOT}/include/drm)
endif()
target_compile_features(termscript_stdlib PRIVATE cxx_std_20)

target_include_directories(termscript_stdlib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../..>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(termscript_stdlib PRIVATE
  ${TS_STDLIB_TOML_INC}
  ${TS_STDLIB_ISOCLINE_INC}
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_include_directories(termscript_stdlib SYSTEM PRIVATE
  ${TS_STDLIB_LIBSHELL}/include
  ${TS_STDLIB_LIBSHELL}/cli)
# Reuse isocline's MIT-licensed CSS color-name data, rather than maintaining
# a second palette. Only RGB entries (standard CSS names) are selected.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/isocline/src/bbcode_colors.c"
  TS_CSS_COLORS REGEX "IC_RGB\\(0x[0-9a-f]+\\)")
set(TS_CSS_COLOR_TABLE "/* Generated from isocline bbcode_colors.c (MIT, Daan Leijen). */\n")
foreach(entry IN LISTS TS_CSS_COLORS)
  string(REGEX REPLACE "IC_RGB\\((0x[0-9a-f]+)\\)" "\\1" entry "${entry}")
  string(APPEND TS_CSS_COLOR_TABLE "${entry}\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/css_named_colors.h" "${TS_CSS_COLOR_TABLE}")
target_link_libraries(termscript_stdlib PUBLIC termscript_standalone)
target_link_libraries(termscript_stdlib PRIVATE novoparse_runtime)
target_link_libraries(termscript_stdlib PRIVATE Threads::Threads)
target_link_libraries(termscript_stdlib PRIVATE lexbor_static
  PkgConfig::TS_UTF8PROC PkgConfig::TS_FRIBIDI PkgConfig::TS_FONTCONFIG
  PkgConfig::TS_FREETYPE PkgConfig::TS_HARFBUZZ m)

# The driver emits and links C in a separate compiler process. Export the
# native dependency closure for that process and for installed pkg-config.
set(TS_DRIVER_NATIVE_FLAGS "-pthread")
set(TS_DRIVER_STATIC_FLAGS "-pthread")
if(TS_IPC_NEEDS_RT)
  list(APPEND TS_DRIVER_NATIVE_FLAGS -lrt)
  list(APPEND TS_DRIVER_STATIC_FLAGS -lrt)
endif()
foreach(pkg IN ITEMS TS_UTF8PROC TS_FRIBIDI TS_FONTCONFIG TS_FREETYPE TS_HARFBUZZ ${TS_EXTRA_PKGS})
  foreach(dir IN LISTS ${pkg}_LIBRARY_DIRS)
    list(APPEND TS_DRIVER_NATIVE_FLAGS "-L${dir}")
  endforeach()
  foreach(lib IN LISTS ${pkg}_LIBRARIES)
    list(APPEND TS_DRIVER_NATIVE_FLAGS "-l${lib}")
  endforeach()
  list(APPEND TS_DRIVER_NATIVE_FLAGS ${${pkg}_LDFLAGS_OTHER})
  list(APPEND TS_DRIVER_STATIC_FLAGS ${${pkg}_STATIC_LDFLAGS})
endforeach()
# Generated units remain C; their separate C linker needs the C++ runtime
# selected by this build's C++ compiler (libstdc++ or libc++).
set(TS_STDLIB_CXX_LIBS "")
foreach(lib IN LISTS CMAKE_CXX_IMPLICIT_LINK_LIBRARIES)
  if(lib MATCHES "^(stdc\\+\\+|c\\+\\+|c\\+\\+abi)$")
    list(APPEND TS_DRIVER_NATIVE_FLAGS "-l${lib}")
    list(APPEND TS_DRIVER_STATIC_FLAGS "-l${lib}")
    string(APPEND TS_STDLIB_CXX_LIBS " -l${lib}")
  endif()
endforeach()
list(APPEND TS_DRIVER_NATIVE_FLAGS -lm)
list(APPEND TS_DRIVER_STATIC_FLAGS -lm)
set(TERMSCRIPT_NATIVE_FLAGS ${TS_DRIVER_NATIVE_FLAGS} PARENT_SCOPE)
set(TERMSCRIPT_STATIC_FLAGS ${TS_DRIVER_STATIC_FLAGS} PARENT_SCOPE)

configure_file(termscript_stdlib.pc.in termscript_stdlib.pc @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/termscript_stdlib.pc"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# Installed companions live under <datadir>/termscript/stdlib; bake
# that path in so G:import works out of the box after install.
# _GNU_SOURCE like libtermlib's dt_internal.h (pty, signals, getline).
target_compile_definitions(termscript_stdlib PRIVATE
  TERMSCRIPT_STDLIB_DIR="${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/termscript/stdlib"
  TERMSCRIPT_STDLIB_SRC="${CMAKE_CURRENT_SOURCE_DIR}"
  TS_STD_HAVE_ISOCLINE=1
  _GNU_SOURCE
)

set_source_files_properties(${TS_STDLIB_TOML_C} ${TS_STDLIB_ISOCLINE_C} ${TS_STDLIB_SHELL_PARSER}
  PROPERTIES COMPILE_OPTIONS "-w")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(termscript_stdlib PRIVATE -Wall -Wextra)
endif()

# Real per-module adapter archives used by the Termakfiles. They deliberately
# share the aggregate library's verified feature/dependency configuration.
# Archives are optional, excluded from ordinary builds/default installs, and
# require the full stdlib/runtime dependency closure when linked by a consumer.
function(ts_stdlib_adapter_archive name)
  set(adapter "termscript_std_${name}")
  add_library(${adapter} STATIC EXCLUDE_FROM_ALL ${ARGN})
  target_compile_features(${adapter} PRIVATE cxx_std_20)
  target_include_directories(${adapter} PRIVATE
    $<TARGET_PROPERTY:termscript_stdlib,INCLUDE_DIRECTORIES>)
  target_compile_definitions(${adapter} PRIVATE
    $<TARGET_PROPERTY:termscript_stdlib,COMPILE_DEFINITIONS>)
  target_compile_options(${adapter} PRIVATE
    $<TARGET_PROPERTY:termscript_stdlib,COMPILE_OPTIONS>)
  target_link_libraries(${adapter} PRIVATE termscript_stdlib
    $<TARGET_PROPERTY:termscript_stdlib,LINK_LIBRARIES>)
  install(TARGETS ${adapter} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT "termscript-module-${name}" EXCLUDE_FROM_ALL)
endfunction()
foreach(m IN LISTS TS_STDLIB_MODULES ITEMS font_discovery stylesheet)
  if(m STREQUAL "font_discovery")
    set(adapter_source font/font.c)
  elseif(m STREQUAL "stylesheet")
    set(adapter_source css/css.c)
  elseif(m STREQUAL "shell")
    set(adapter_source shell/shell.cpp)
  else()
    set(adapter_source "${m}/${m}.c")
  endif()
  if(m STREQUAL "http")
    ts_stdlib_adapter_archive(${m} ${adapter_source} http/fetch.cpp)
  else()
    ts_stdlib_adapter_archive(${m} ${adapter_source})
  endif()
endforeach()
ts_stdlib_adapter_archive(common ${TS_STDLIB_COMMON_SOURCES})

# ── Q regeneration (optional) ─────────────────────────────────────
# Renews gen/ts_valvec.h + gen/ts_strvec.h from q/tsvec.q through
# domqlib's Q (sourceless .pyc driven by q/run_qc.py).  Runs only when
# python3 + tkinter + the Q bytecode cache are all present; otherwise
# the committed gen/ outputs are used as-is.
find_package(Python3 COMPONENTS Interpreter QUIET)
set(TS_Q_CACHE ${CMAKE_CURRENT_SOURCE_DIR}/../../domlibs/domqlib/q/__pycache__)
if(Python3_FOUND AND EXISTS "${TS_Q_CACHE}")
  execute_process(
    COMMAND ${Python3_EXECUTABLE} -c "import tkinter"
    RESULT_VARIABLE TS_TK_OK
    OUTPUT_QUIET ERROR_QUIET
  )
  if(TS_TK_OK EQUAL 0)
    add_custom_command(
      OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/gen/ts_valvec.h
             ${CMAKE_CURRENT_SOURCE_DIR}/gen/ts_strvec.h
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/q/run_qc.py
              --qcache ${TS_Q_CACHE}
              --q ${CMAKE_CURRENT_SOURCE_DIR}/q/tsvec.q
              --out ${CMAKE_CURRENT_SOURCE_DIR}/gen
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/q/tsvec.q
              ${CMAKE_CURRENT_SOURCE_DIR}/q/run_qc.py
      COMMENT "Regenerating Termscript stdlib vectors (domqlib Q)"
      VERBATIM
    )
    add_custom_target(termscript_stdlib_qgen DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/gen/ts_valvec.h
      ${CMAKE_CURRENT_SOURCE_DIR}/gen/ts_strvec.h
    )
    add_dependencies(termscript_stdlib termscript_stdlib_qgen)
  endif()
endif()

# ── install ───────────────────────────────────────────────────────
install(TARGETS termscript_stdlib
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES ts_stdlib.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/termscript/stdlib
)
install(FILES common/ts_std_common.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/termscript/stdlib/common
  COMPONENT termscript-module-common
)
install(FILES gen/ts_valvec.h gen/ts_strvec.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/termscript/stdlib/gen
)
install(FILES q/tsvec.q q/run_qc.py
  DESTINATION ${CMAKE_INSTALL_DATADIR}/termlib/stdlib-q
)
foreach (m IN LISTS TS_STDLIB_MODULES ITEMS stylesheet font_discovery)
  install(FILES ${m}/${m}.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/termscript/stdlib/${m}
    COMPONENT "termscript-module-${m}"
  )
  install(FILES ${m}/${m}.tsc
    DESTINATION ${CMAKE_INSTALL_DATADIR}/termscript/stdlib/${m}
    COMPONENT "termscript-module-${m}"
  )
endforeach ()

install(FILES buffer/allocator.h buffer/dommemtk_adapter.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/termscript/stdlib/buffer)
install(FILES shell/README.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/stdlib/shell)
install(FILES http/README.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/stdlib/http)
install(FILES emulator-backends.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/stdlib)
install(FILES new-modules.md parser-modules.md device-modules.md infrastructure-modules.md MANIFEST.yaml DESTINATION ${CMAKE_INSTALL_DOCDIR}/stdlib)
install(FILES x11/window.tsc DESTINATION ${CMAKE_INSTALL_DOCDIR}/stdlib/x11)
install(FILES drm/discover.tsc DESTINATION ${CMAKE_INSTALL_DOCDIR}/stdlib/drm)
install(FILES compositor/README.md DESTINATION ${CMAKE_INSTALL_DOCDIR}/stdlib/compositor)
install(FILES emulator/README.md emulator/terminal.tsc emulator/headless.tsc
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/stdlib/emulator)
