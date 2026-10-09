cmake_minimum_required(VERSION 3.15)

# Aurocks: `.g` grammar frontend lowering to GLRpp/libglr.
add_library(aurocks STATIC Aurocks.cpp)
target_include_directories(aurocks PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)
target_link_libraries(aurocks PUBLIC GLRpp libglr)
target_compile_features(aurocks PUBLIC cxx_std_20)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aurocks PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Command-line driver.
get_filename_component(AUROCKS_GLRPP_ABS "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
add_executable(aurocks_cli cli.cpp)
target_link_libraries(aurocks_cli PRIVATE aurocks)
set_target_properties(aurocks_cli PROPERTIES OUTPUT_NAME aurocks)
target_compile_definitions(aurocks_cli PRIVATE
    AUROCKS_VERSION="1.0.0"
    AUROCKS_GLRPP_DIR="${AUROCKS_GLRPP_ABS}"
    AUROCKS_LIBGLR_INCLUDE_DIRS="-I${CMAKE_SOURCE_DIR}/include -I${CMAKE_BINARY_DIR}/include"
    AUROCKS_LIBGLR_LINK="$<TARGET_FILE:libglr> $<TARGET_FILE:sfsexp> -pthread"
)

# Unit tests.
if(BUILD_TESTING AND Catch2_FOUND)
    if(TARGET Catch2::Catch2WithMain)
        add_executable(aurocks_test tests/test_aurocks.cpp)
        target_link_libraries(aurocks_test PRIVATE aurocks Catch2::Catch2WithMain)
        target_compile_features(aurocks_test PRIVATE cxx_std_20)
        target_compile_definitions(aurocks_test PRIVATE
            AUROCKS_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        )
        add_test(NAME aurocks_test COMMAND aurocks_test)
    endif()
endif()

# End-to-end: generate the JSON parser from examples/JSON.g at build time,
# compile it against this tree, and exercise the demo on sample documents.
if(BUILD_TESTING AND TARGET aurocks_cli)
    set(AUROCKS_JSON_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${AUROCKS_JSON_GEN_DIR}")
    add_custom_command(
        OUTPUT
            "${AUROCKS_JSON_GEN_DIR}/JSONParser.cpp"
            "${AUROCKS_JSON_GEN_DIR}/JSONParserRuntime.hpp"
            "${AUROCKS_JSON_GEN_DIR}/JSONParserAST.hpp"
            "${AUROCKS_JSON_GEN_DIR}/Makefile"
        COMMAND aurocks_cli --output-dir "${AUROCKS_JSON_GEN_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/JSON.g"
        DEPENDS aurocks_cli "${CMAKE_CURRENT_SOURCE_DIR}/examples/JSON.g"
        COMMENT "Aurocks: generating JSON parser from JSON.g"
        VERBATIM
    )
    add_executable(aurocks_json_demo
        "${AUROCKS_JSON_GEN_DIR}/JSONParser.cpp"
    )
    target_include_directories(aurocks_json_demo PRIVATE "${AUROCKS_JSON_GEN_DIR}")
    target_compile_definitions(aurocks_json_demo PRIVATE AUROCKS_DEMO_MAIN)
    target_link_libraries(aurocks_json_demo PRIVATE aurocks)
    add_test(NAME aurocks_json_demo_ok
        COMMAND aurocks_json_demo "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/ok.json"
    )
    add_test(NAME aurocks_json_demo_bad
        COMMAND aurocks_json_demo "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/bad.json"
    )
    set_tests_properties(aurocks_json_demo_bad PROPERTIES WILL_FAIL TRUE)
endif()

# Install with the GLRpp headers (no export set: the parent GLRpp target
# itself installs as files, so aurocks follows the same pattern).
install(FILES Aurocks.hpp DESTINATION include/GLRpp/aurocks)
install(TARGETS aurocks
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(TARGETS aurocks_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
