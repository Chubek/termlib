# MoosedogHelpers.cmake -- CMake helpers for Moosedog-generated parsers.
#
#   find_package(moosedog) or add_subdirectory path provides the
#   `moosedog` executable target plus this function:
#
#     moosedog_add_grammar(TARGET <tgt> GRM <file.grm> BASENAME <Base>
#                          OUTPUT_DIR <dir> [MAKEFILE] [CMAKEFILE] [NO_REWRITE])
#
#   The function runs `moosedog` at build time and attaches the generated
#   <Base>.parser.c to <tgt> with libglr include/link handling left to the
#   caller (target_link_libraries(<tgt> PRIVATE libglr::libglr)).

find_program(MOOSEDOG_EXECUTABLE moosedog PATHS ${CMAKE_CURRENT_LIST_DIR}/../../bin NO_DEFAULT_PATH)
if(NOT MOOSEDOG_EXECUTABLE)
  find_program(MOOSEDOG_EXECUTABLE moosedog)
endif()

function(moosedog_add_grammar)
  set(opts MAKEFILE CMAKEFILE NO_REWRITE NO_ATN)
  set(one TARGET GRM BASENAME OUTPUT_DIR)
  set(multi EXTRA_ARGS)
  cmake_parse_arguments(MD "${opts}" "${one}" "${multi}" ${ARGN})
  if(NOT MD_TARGET OR NOT MD_GRM OR NOT MD_BASENAME OR NOT MD_OUTPUT_DIR)
    message(FATAL_ERROR "moosedog_add_grammar: TARGET/GRM/BASENAME/OUTPUT_DIR required")
  endif()
  if(NOT MOOSEDOG_EXECUTABLE)
    message(FATAL_ERROR "moosedog_add_grammar: moosedog executable not found")
  endif()
  set(args --output-dir ${MD_OUTPUT_DIR} --basename ${MD_BASENAME} ${MD_GRM})
  if(MD_MAKEFILE)
    list(APPEND args --emit-makefile)
  endif()
  if(MD_CMAKEFILE)
    list(APPEND args --emit-cmakefile)
  endif()
  if(MD_NO_REWRITE)
    list(APPEND args --no-rewrite)
  endif()
  if(MD_NO_ATN)
    list(APPEND args --no-atn)
  endif()
  list(APPEND args ${MD_EXTRA_ARGS})
  add_custom_command(
    OUTPUT ${MD_OUTPUT_DIR}/${MD_BASENAME}.parser.c
           ${MD_OUTPUT_DIR}/${MD_BASENAME}.parser.h
           ${MD_OUTPUT_DIR}/${MD_BASENAME}.ast.h
           ${MD_OUTPUT_DIR}/${MD_BASENAME}.lexer.h
    COMMAND ${MOOSEDOG_EXECUTABLE} ${args}
    DEPENDS ${MD_GRM} moosedog
    COMMENT "Moosedog: generating ${MD_BASENAME} parser"
    VERBATIM)
  target_sources(${MD_TARGET} PRIVATE ${MD_OUTPUT_DIR}/${MD_BASENAME}.parser.c)
  target_include_directories(${MD_TARGET} PRIVATE ${MD_OUTPUT_DIR})
endfunction()
