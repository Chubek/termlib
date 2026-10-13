cmake_minimum_required(VERSION 3.24)

# Also usable independently: cmake -S DSLtk/tests -B build/dsltk-tests.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(DSLtkEnumTests LANGUAGES CXX)
    enable_testing()
    add_library(DSLtk INTERFACE)
    target_include_directories(DSLtk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_features(DSLtk INTERFACE cxx_std_20)
endif()

add_executable(dsltk_enum_tests test_enum_reflection.cpp enum_odr.cpp)
target_link_libraries(dsltk_enum_tests PRIVATE DSLtk)
add_test(NAME dsltk_enum_reflection COMMAND dsltk_enum_tests)

add_executable(dsltk_enum_config_tests test_enum_config.cpp)
target_link_libraries(dsltk_enum_config_tests PRIVATE DSLtk)
add_test(NAME dsltk_enum_configuration COMMAND dsltk_enum_config_tests)

add_executable(dsltk_enum_alias_tests test_enum_aliases.cpp)
target_link_libraries(dsltk_enum_alias_tests PRIVATE DSLtk)
add_test(NAME dsltk_enum_aliases COMMAND dsltk_enum_alias_tests)

add_executable(dsltk_enum_format_tests test_enum_format.cpp)
target_link_libraries(dsltk_enum_format_tests PRIVATE DSLtk)
add_test(NAME dsltk_enum_format COMMAND dsltk_enum_format_tests)

find_package(fmt CONFIG QUIET)
if(TARGET fmt::fmt)
    add_executable(dsltk_enum_fmt_tests test_enum_format.cpp)
    target_compile_definitions(dsltk_enum_fmt_tests PRIVATE DSLTK_TEST_FMT)
    target_link_libraries(dsltk_enum_fmt_tests PRIVATE DSLtk fmt::fmt)
    add_test(NAME dsltk_enum_fmt COMMAND dsltk_enum_fmt_tests)
endif()
