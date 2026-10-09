cmake_minimum_required(VERSION 3.16)
project(moosedog VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---- libmoosedog: the .grm frontend as a reusable static library ----
set(MOOSEDOG_SOURCES
    parser/moosedog_parser.c
    lexer/moosedog_lexer.c
    entrypoint/moosedog_entrypoint.c
    entrypoint/Moosedog.c
    emitter/moosedog_emitter.c
    rewrite/moosedog_rewrite.c
    disamb/moosedog_disamb.c
    plugin/moosedog_plugin.c
    confuse/moosedog_confuse.c
    lua/moosedog_lua.c
    stdext/moosedog_stdext.c
    stdplugin/ebnf-frontend/EBNF-Frontend.c
)

add_library(moosedog_lib STATIC ${MOOSEDOG_SOURCES})
target_include_directories(moosedog_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/parser>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lexer>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/entrypoint>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/emitter>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/rewrite>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/disamb>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/plugin>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/confuse>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lua>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/stdext>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/stdplugin/ebnf-frontend>
)
target_compile_options(moosedog_lib PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(moosedog_lib PUBLIC ${CMAKE_DL_LIBS})
set_target_properties(moosedog_lib PROPERTIES OUTPUT_NAME moosedog)

# ---- moosedog executable ----
add_executable(moosedog bin/moosedog_main.c)
target_link_libraries(moosedog PRIVATE moosedog_lib)
target_compile_options(moosedog PRIVATE -Wall -Wextra -Wpedantic)

# ---- EBNF frontend as a loadable plugin (also linked statically) ----
add_library(moosedog_ebnf_plugin SHARED stdplugin/ebnf-frontend/EBNF-Frontend.c)
target_include_directories(moosedog_ebnf_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/stdplugin/ebnf-frontend)
set_target_properties(moosedog_ebnf_plugin PROPERTIES
    OUTPUT_NAME moosedog_ebnf
    POSITION_INDEPENDENT_CODE ON)

# ---- install ----
include(GNUInstallDirs)
install(TARGETS moosedog moosedog_lib moosedog_ebnf_plugin
    EXPORT moosedog-exports
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/Moosedog.h include/Moosedog-Plugin.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT moosedog-exports
    FILE moosedog-targets.cmake
    NAMESPACE moosedog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/moosedog)
install(FILES cmake/MoosedogHelpers.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/moosedog)
install(DIRECTORY gpp-prelude/ DESTINATION share/moosedog/gpp-prelude
    FILES_MATCHING PATTERN "*.gpp")
install(DIRECTORY lua-prelude/ DESTINATION share/moosedog/lua-prelude
    FILES_MATCHING PATTERN "*.lua")
install(DIRECTORY confuse-prelude/ DESTINATION share/moosedog/confuse-prelude)
install(DIRECTORY libsh-prelude/ DESTINATION share/moosedog/libsh-prelude)
install(DIRECTORY templates/ DESTINATION share/moosedog/templates)
install(DIRECTORY examples/ DESTINATION share/moosedog/examples)

# ---- self tests: parse + emit + compile the bundled examples ----
enable_testing()

add_test(NAME moosedog-check-json
    COMMAND moosedog --check ${CMAKE_CURRENT_SOURCE_DIR}/examples/JSON/JSON.grm)
add_test(NAME moosedog-check-ansic
    COMMAND moosedog --check ${CMAKE_CURRENT_SOURCE_DIR}/examples/ANSI-C/ANSI-C.grm)

# Full generation into the build tree (exercises emitter + Moosedog.h copy).
add_test(NAME moosedog-emit-json
    COMMAND moosedog --output-dir ${CMAKE_CURRENT_BINARY_DIR}/gen-json
        --basename JSON --emit-makefile --emit-cmakefile
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/JSON/JSON.grm)
add_test(NAME moosedog-emit-ansic
    COMMAND moosedog --output-dir ${CMAKE_CURRENT_BINARY_DIR}/gen-ansic
        --basename ANSI-C --no-atn
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/ANSI-C/ANSI-C.grm)

# EBNF frontend translation.
add_test(NAME moosedog-check-ebnf
    COMMAND moosedog --check ${CMAKE_CURRENT_SOURCE_DIR}/tests/mini.ebnf)

# CLI overrides: valid combination passes.
add_test(NAME moosedog-overrides
    COMMAND moosedog --check --no-atn --lookahead-depth 4 --max-errors 3
        --no-cache --no-strict-unicode
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/JSON/JSON.grm)

# Negative fixtures must fail validation.
add_test(NAME moosedog-reject-start-rule
    COMMAND moosedog --check ${CMAKE_CURRENT_SOURCE_DIR}/tests/bad-start-rule.grm)
add_test(NAME moosedog-reject-helper
    COMMAND moosedog --check ${CMAKE_CURRENT_SOURCE_DIR}/tests/bad-helper.grm)
add_test(NAME moosedog-reject-depth
    COMMAND moosedog --check --lookahead-depth 99
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/JSON/JSON.grm)
set_tests_properties(moosedog-reject-start-rule moosedog-reject-helper
    moosedog-reject-depth PROPERTIES WILL_FAIL TRUE)

# Fuzz smoke: garbage inputs must error, never crash.
add_test(NAME moosedog-fuzz-smoke
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz-smoke.sh
        $<TARGET_FILE:moosedog> ${CMAKE_CURRENT_SOURCE_DIR} 60)

set_tests_properties(moosedog-check-json moosedog-check-ansic
    moosedog-emit-json moosedog-emit-ansic moosedog-check-ebnf
    moosedog-overrides moosedog-reject-start-rule moosedog-reject-helper
    moosedog-reject-depth moosedog-fuzz-smoke PROPERTIES LABELS "moosedog")

# ---- build-time test: generated code compiles warning-clean and runs ----
# Regenerate the JSON parser at build time so the tree always proves the
# emitter output compiles (-Werror) and links against libglr.
set(MOOSEDOG_GEN_TEST_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen-test)
add_custom_command(
    OUTPUT ${MOOSEDOG_GEN_TEST_DIR}/JSON.parser.c
           ${MOOSEDOG_GEN_TEST_DIR}/JSON.parser.h
           ${MOOSEDOG_GEN_TEST_DIR}/JSON.ast.h
           ${MOOSEDOG_GEN_TEST_DIR}/JSON.lexer.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${MOOSEDOG_GEN_TEST_DIR}
    COMMAND moosedog --output-dir ${MOOSEDOG_GEN_TEST_DIR} --basename JSON
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/JSON/JSON.grm
    DEPENDS moosedog
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/JSON/JSON.grm
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/JSON/Support.gpp
    COMMENT "Moosedog: regenerating JSON parser for smoke test"
    VERBATIM)

add_library(moosedog_gen_json STATIC ${MOOSEDOG_GEN_TEST_DIR}/JSON.parser.c)
target_include_directories(moosedog_gen_json PUBLIC
    ${MOOSEDOG_GEN_TEST_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(moosedog_gen_json PUBLIC libglr)
target_compile_options(moosedog_gen_json PRIVATE -Wall -Wextra -Werror)

add_executable(moosedog_smoke tests/smoke.c)
target_include_directories(moosedog_smoke PRIVATE
    ${MOOSEDOG_GEN_TEST_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/parser
    ${CMAKE_CURRENT_SOURCE_DIR}/lexer
    ${CMAKE_CURRENT_SOURCE_DIR}/entrypoint
    ${CMAKE_CURRENT_SOURCE_DIR}/emitter
    ${CMAKE_CURRENT_SOURCE_DIR}/rewrite
    ${CMAKE_CURRENT_SOURCE_DIR}/disamb
    ${CMAKE_CURRENT_SOURCE_DIR}/plugin
    ${CMAKE_CURRENT_SOURCE_DIR}/confuse
    ${CMAKE_CURRENT_SOURCE_DIR}/lua
    ${CMAKE_CURRENT_SOURCE_DIR}/stdext
    ${CMAKE_CURRENT_SOURCE_DIR}/stdplugin/ebnf-frontend)
target_compile_definitions(moosedog_smoke PRIVATE
    MOOSEDOG_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(moosedog_smoke PRIVATE -Wall -Wextra -Werror)
target_link_libraries(moosedog_smoke PRIVATE
    moosedog_lib moosedog_gen_json libglr)
add_test(NAME moosedog-smoke COMMAND moosedog_smoke)
set_tests_properties(moosedog-smoke PROPERTIES LABELS "moosedog")
