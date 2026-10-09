add_library(glrdisambstd STATIC
    standard.c
    tree_score.c
    solver.cpp
    ../third_party/satie/src/SatieMemory.cpp
    attribute_grammar.c
    bounded_ambguity_checking_sat.c
    bounded_ambguity_checking_smt.c
    case_sensitivity.c
    counter_example_guided.c
    indentation_and_layout.c
    island_parsing.c
    layout_sensitive_disamb.c
    lexical.c
    longest_match.c
    name_resolution.c
    post_parse_filtering.c
    prefer_avoid_filters.c
    scannerless_priorities.c
    associativity.c
    dynamic_programming.c
    precedence.c
    predicate.c
    probability.c
    semantic.c
)

target_link_libraries(glrdisambstd PUBLIC libglr PRIVATE m)
add_library(libglr::glrdisambstd ALIAS glrdisambstd)
set_target_properties(glrdisambstd PROPERTIES POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
# Satie's CDCL and IDL engines use headers plus the memory resource. Avoid building unrelated
# frontends/plugins or inheriting their global build options.
target_include_directories(glrdisambstd SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/satie/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/satie/third_party/memtkx/include)

target_include_directories(glrdisambstd
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
        $<INSTALL_INTERFACE:include>
)

target_compile_options(glrdisambstd PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3>
)
if(ENABLE_TEST_SANITIZERS)
    target_compile_options(glrdisambstd PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

install(TARGETS glrdisambstd
    EXPORT libglr-exports
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES ../include/glr/disambstd.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/glr)
install(FILES README.md DESTINATION share/doc/libglr/disambstd)
install(FILES ../third_party/satie/LICENSE
    DESTINATION share/doc/libglr/licenses RENAME satie-LICENSE)
