file(MAKE_DIRECTORY "${WORK}")
set(cases
  "i = 0\; while G:lt i 20 do i = G:add i 1\; if G:eq i 10 do G:puts \"ten\"\; else G:puts i\; end end G:puts 9223372036854775807\; G:puts -123456789012345\; const s = nil or false or \"yes\"\; G:puts s\; const b = true or G:die \"bad short circuit\"\; G:puts b\;"
  "G:puts \"discarded on error\"\; G:die \"expected failure\"\;"
  "G:puts missing\;"
  "const c = \"owned\"\; c = \"reassign\"\;"
  "G:puts \"owned argument\" missing\;"
  "while true do end"
  "")
set(index 0)
foreach(source IN LISTS cases)
  math(EXPR index "${index} + 1")
  file(WRITE "${WORK}/case.tsc" "${source}")
  execute_process(COMMAND "${DRIVER}" "${WORK}/case.tsc"
    RESULT_VARIABLE vm_status OUTPUT_VARIABLE vm ERROR_VARIABLE vm_error)
  execute_process(COMMAND "${DRIVER}" --backend qbe -o "${WORK}/program" "${WORK}/case.tsc"
    RESULT_VARIABLE status ERROR_VARIABLE error)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "Native compile case ${index}: ${error}\n${source}")
  endif()
  execute_process(COMMAND "${WORK}/program"
    RESULT_VARIABLE native_status OUTPUT_VARIABLE native ERROR_VARIABLE native_error)
  if(NOT vm_status STREQUAL native_status OR NOT vm STREQUAL native OR NOT vm_error STREQUAL native_error)
    message(FATAL_ERROR "Case ${index}: VM (${vm_status}) ${vm} ${vm_error}; native (${native_status}) ${native} ${native_error}")
  endif()
endforeach()
file(WRITE "${WORK}/bad.tsc" "const = ;")
execute_process(COMMAND "${DRIVER}" --backend qbe -c "${WORK}/bad.tsc" RESULT_VARIABLE status)
if(status EQUAL 0)
  message(FATAL_ERROR "Malformed source accepted")
endif()
