find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

function(generate_fixture name profile extension)
    set(output "${GENERATED_DIR}/${name}${extension}")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${PROJECT_BINARY_DIR}/novoparse" generate "${PROJECT_SOURCE_DIR}/examples/${name}.ng"
            --profile "${profile}" -o "${output}"
        DEPENDS novoparse_cli "${PROJECT_BINARY_DIR}/novoparse" "${PROJECT_SOURCE_DIR}/examples/${name}.ng"
        VERBATIM)
    set(GENERATED_FIXTURES ${GENERATED_FIXTURES} "${output}" PARENT_SCOPE)
endfunction()
generate_fixture(JSON C .h)
generate_fixture(Arithmetic C .h)
generate_fixture(Arithmetic "C++" .hpp)
generate_fixture(Arithmetic D .d)
add_custom_target(novoparse_test_fixtures DEPENDS ${GENERATED_FIXTURES})

add_executable(novoparse_c_tests runtime.c)
add_executable(novoparse_cpp_tests runtime.cpp)
target_compile_features(novoparse_cpp_tests PRIVATE cxx_std_11)
foreach(target novoparse_c_tests novoparse_cpp_tests)
    add_dependencies(${target} novoparse_test_fixtures)
    target_include_directories(${target} PRIVATE "${GENERATED_DIR}")
    target_link_libraries(${target} PRIVATE novoparse_runtime)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    if(ENABLE_TEST_SANITIZERS)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
endforeach()
add_test(NAME c_runtime COMMAND novoparse_c_tests)
add_test(NAME cpp_runtime COMMAND novoparse_cpp_tests)

set(FRONTEND_SOURCES ${NOVOPARSE_SOURCES})
list(REMOVE_ITEM FRONTEND_SOURCES "${PROJECT_SOURCE_DIR}/source/app.d")
set(FRONTEND_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/frontend_tests")
set(RUNTIME_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/d_runtime_tests")
set(FRONTEND_FLAGS -unittest -main "-of=${FRONTEND_OUTPUT}")
set(RUNTIME_FLAGS "-of=${RUNTIME_OUTPUT}" "-L-L${PROJECT_BINARY_DIR}" -L-lnovoparse "-L-rpath=${PROJECT_BINARY_DIR}")
if(D_COMPILER_NAME MATCHES "gdc")
    set(FRONTEND_FLAGS -funittest -fmain -o "${FRONTEND_OUTPUT}")
    set(RUNTIME_FLAGS -o "${RUNTIME_OUTPUT}" "-L${PROJECT_BINARY_DIR}" -lnovoparse "-Wl,-rpath,${PROJECT_BINARY_DIR}")
endif()
add_custom_command(OUTPUT "${FRONTEND_OUTPUT}"
    COMMAND "${NOVOPARSE_D_COMPILER}" ${FRONTEND_FLAGS} "-I${PROJECT_SOURCE_DIR}/source" "-J${PROJECT_SOURCE_DIR}/profiles"
        ${FRONTEND_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/frontend.d"
    DEPENDS ${FRONTEND_SOURCES} "frontend.d" ${NOVOPARSE_PROFILES} VERBATIM)
add_custom_command(OUTPUT "${RUNTIME_OUTPUT}"
    COMMAND "${NOVOPARSE_D_COMPILER}" ${RUNTIME_FLAGS} "-I${PROJECT_SOURCE_DIR}/include" "-I${GENERATED_DIR}"
        "${PROJECT_SOURCE_DIR}/runtime/NovoParse.d" "${GENERATED_DIR}/Arithmetic.d" "${CMAKE_CURRENT_SOURCE_DIR}/runtime.d"
    DEPENDS novoparse_runtime novoparse_test_fixtures "${GENERATED_DIR}/Arithmetic.d"
        "${PROJECT_SOURCE_DIR}/runtime/NovoParse.d" "${PROJECT_SOURCE_DIR}/include/NovoParse.di" "runtime.d" VERBATIM)
add_custom_target(novoparse_d_tests ALL DEPENDS "${FRONTEND_OUTPUT}" "${RUNTIME_OUTPUT}")
add_test(NAME frontend COMMAND "${FRONTEND_OUTPUT}")
add_test(NAME d_runtime COMMAND "${RUNTIME_OUTPUT}")
add_test(NAME end_to_end COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_end_to_end.py"
    --cli "${PROJECT_BINARY_DIR}/novoparse" --library "$<TARGET_FILE:novoparse_runtime>"
    --workdir "${CMAKE_CURRENT_BINARY_DIR}")
