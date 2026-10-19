file(MAKE_DIRECTORY "${WORK}")
set(modes vm c)
if(NATIVE)
  list(APPEND modes qbe)
endif()
if(JITERATI)
  list(APPEND modes jiterati)
endif()
set(sources
  "G:puts(1.0 / 0.0)"
  "G:puts(1e308 * 1e308)"
  "G:puts(int(9223372036854775808.0))"
  "G:puts(1.0 << 1)"
  "G:puts(float(\"nan\"))")
set(markers "division by zero" "floating-point overflow" "int64 range"
  "integer operands required" "finite decimal number required")
foreach(index RANGE 0 4)
  list(GET sources ${index} source)
  list(GET markers ${index} marker)
  set(path "${WORK}/failure-${index}.tsc")
  file(WRITE "${path}" "${source};\n")
  foreach(mode IN LISTS modes)
    if(mode STREQUAL "c" OR mode STREQUAL "qbe")
      set(executable "${WORK}/failure-${index}-${mode}")
      execute_process(COMMAND ${CMAKE_COMMAND} -E env "TMPDIR=${WORK}"
        "${DRIVER}" --backend ${mode} -o "${executable}" "${path}"
        RESULT_VARIABLE status ERROR_VARIABLE error)
      if(NOT status EQUAL 0)
        message(FATAL_ERROR "${mode} failed to compile runtime-failure fixture: ${error}")
      endif()
      execute_process(COMMAND "${executable}" RESULT_VARIABLE status
        OUTPUT_VARIABLE output ERROR_VARIABLE error)
    elseif(mode STREQUAL "vm")
      execute_process(COMMAND "${DRIVER}" "${path}" RESULT_VARIABLE status
        OUTPUT_VARIABLE output ERROR_VARIABLE error)
    else()
      execute_process(COMMAND "${DRIVER}" --backend ${mode} "${path}"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    endif()
    if(status EQUAL 0 OR NOT output STREQUAL "" OR NOT error MATCHES "${marker}"
        OR error MATCHES "Sanitizer|runtime error:")
      message(FATAL_ERROR "${mode}/${index} incorrect failure (${status}): ${output}\n${error}")
    endif()
  endforeach()
endforeach()
file(WRITE "${WORK}/invalid.tsc" "G:puts(1e309);\n")
foreach(mode IN LISTS modes)
  if(mode STREQUAL "c" OR mode STREQUAL "qbe")
    set(options --backend ${mode} -c -o "${WORK}/invalid-${mode}.txt")
  elseif(mode STREQUAL "jiterati")
    set(options --backend jiterati)
  else()
    set(options)
  endif()
  execute_process(COMMAND "${DRIVER}" ${options} "${WORK}/invalid.tsc"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(status EQUAL 0 OR NOT output STREQUAL "" OR NOT error MATCHES "out of range"
      OR error MATCHES "Sanitizer|runtime error:")
    message(FATAL_ERROR "${mode} accepted invalid floating literal: ${error}")
  endif()
endforeach()
