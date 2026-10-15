file(MAKE_DIRECTORY "${WORK}")
set(cases
  "G:Import(\"missing\") ~> R:ImportError($E->Msg) -> R:DieGracefully(:EXIT_FAILURE) => G:die(\"bad success branch\")\;"
  "G:div(1, 0) ~> G:puts($E->Code) -> G:die(\"secondary diagnostic\")\;"
  "G:puts(G:concat(\"owned\", G:div(1, 0)))\;"
  "config x := 42\; x = 1\;"
  "G:add(1, 2) => G:die(\"success observer failed\")\;"
  "G:puts((nil or false or 42) => G:puts($E->Value))\;"
  "true => G:puts((42 => G:puts($E->Value)))\;"
  ".section Program G:puts(\"early exit\")\; R:Exit(:EXIT_SUCCESS)\; G:die(\"unreachable\")\; .end"
  "lifetime value := \"managed\"\; G:die(\"cleanup on failure\")\;")
set(index 0)
foreach(source IN LISTS cases)
  math(EXPR index "${index} + 1")
  file(WRITE "${WORK}/case.tsc" "${source}")
  execute_process(COMMAND "${DRIVER}" "${WORK}/case.tsc"
    RESULT_VARIABLE expected_status OUTPUT_VARIABLE expected_out ERROR_VARIABLE expected_error)
  set(backends c)
  if(NATIVE)
    list(APPEND backends qbe)
  endif()
  foreach(backend IN LISTS backends)
    execute_process(COMMAND "${DRIVER}" --backend "${backend}" -o "${WORK}/program" "${WORK}/case.tsc"
      RESULT_VARIABLE compile_status ERROR_VARIABLE compile_error)
    if(NOT compile_status EQUAL 0)
      message(FATAL_ERROR "Case ${index} ${backend} compile: ${compile_error}")
    endif()
    execute_process(COMMAND "${WORK}/program"
      RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE error)
    if(NOT status STREQUAL expected_status OR NOT out STREQUAL expected_out OR NOT error STREQUAL expected_error)
      message(FATAL_ERROR "Case ${index} ${backend}: (${status}) ${out} ${error}; VM: (${expected_status}) ${expected_out} ${expected_error}")
    endif()
  endforeach()
  if(JITERATI)
    execute_process(COMMAND "${DRIVER}" --backend jiterati "${WORK}/case.tsc"
      RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE error)
    if(NOT status STREQUAL expected_status OR NOT out STREQUAL expected_out OR NOT error STREQUAL expected_error)
      message(FATAL_ERROR "Case ${index} Jiterati: (${status}) ${out} ${error}; VM: (${expected_status}) ${expected_out} ${expected_error}")
    endif()
  endif()
endforeach()
