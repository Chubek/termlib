# Termscript standard library — hybrid C native modules + .tsc companions.
#
# Layout:
#   <name>/<name>.c     native TS_Module (storage, parsing, syscalls)
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
  sched screen signal socket stream style syntax table termcap terminfo test text_search theme
  terminal termios toml tui utf8 verbatim vterm widget word_motion yaml
)

set(TS_STDLIB_SOURCES
  common/sbuf.c
  common/values.c
  common/list_store.c
  common/dom.c
  common/text.c
  common/grid.c
  common/html.c
  ts_stdlib.c
)
foreach (m IN LISTS TS_STDLIB_MODULES)
  list (APPEND TS_STDLIB_SOURCES ${m}/${m}.c)
endforeach ()

# Vendored third-party sources (warning-cleanliness of our own files
# is enforced below; vendored files compile with -w).
set(TS_STDLIB_TOML_C ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/tomlc99/toml.c)
set(TS_STDLIB_ISOCLINE_C ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/isocline/src/isocline.c)
set(TS_STDLIB_TOML_INC ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/tomlc99)
set(TS_STDLIB_ISOCLINE_INC ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/isocline/include)

if(NOT EXISTS "${TS_STDLIB_TOML_C}" OR NOT EXISTS "${TS_STDLIB_ISOCLINE_C}")
  message(FATAL_ERROR "Termscript stdlib requires the bundled tomlc99/isocline sources")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(TS_UTF8PROC REQUIRED IMPORTED_TARGET libutf8proc)
pkg_check_modules(TS_FRIBIDI REQUIRED IMPORTED_TARGET fribidi)
pkg_check_modules(TS_FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(TS_FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(TS_HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)

set(LEXBOR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LEXBOR_INSTALL_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(../../third_party/lexbor ${CMAKE_CURRENT_BINARY_DIR}/lexbor)

add_library(termscript_stdlib STATIC
  ${TS_STDLIB_SOURCES}
  ${TS_STDLIB_TOML_C}
  ${TS_STDLIB_ISOCLINE_C}
)
add_library(termscript::stdlib ALIAS termscript_stdlib)

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
target_link_libraries(termscript_stdlib PRIVATE Threads::Threads)
target_link_libraries(termscript_stdlib PRIVATE lexbor_static
  PkgConfig::TS_UTF8PROC PkgConfig::TS_FRIBIDI PkgConfig::TS_FONTCONFIG
  PkgConfig::TS_FREETYPE PkgConfig::TS_HARFBUZZ m)

# The driver emits and links C in a separate compiler process. Export the
# native dependency closure for that process and for installed pkg-config.
set(TS_DRIVER_NATIVE_FLAGS "-pthread")
set(TS_DRIVER_STATIC_FLAGS "-pthread")
foreach(pkg IN ITEMS TS_UTF8PROC TS_FRIBIDI TS_FONTCONFIG TS_FREETYPE TS_HARFBUZZ)
  foreach(dir IN LISTS ${pkg}_LIBRARY_DIRS)
    list(APPEND TS_DRIVER_NATIVE_FLAGS "-L${dir}")
  endforeach()
  foreach(lib IN LISTS ${pkg}_LIBRARIES)
    list(APPEND TS_DRIVER_NATIVE_FLAGS "-l${lib}")
  endforeach()
  list(APPEND TS_DRIVER_NATIVE_FLAGS ${${pkg}_LDFLAGS_OTHER})
  list(APPEND TS_DRIVER_STATIC_FLAGS ${${pkg}_STATIC_LDFLAGS})
endforeach()
list(APPEND TS_DRIVER_NATIVE_FLAGS -lm)
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

set_source_files_properties(${TS_STDLIB_TOML_C} ${TS_STDLIB_ISOCLINE_C}
  PROPERTIES COMPILE_OPTIONS "-w")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(termscript_stdlib PRIVATE -Wall -Wextra)
endif()

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
  )
  install(FILES ${m}/${m}.tsc
    DESTINATION ${CMAKE_INSTALL_DATADIR}/termscript/stdlib/${m}
  )
endforeach ()

install(FILES buffer/allocator.h buffer/dommemtk_adapter.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/termscript/stdlib/buffer)
