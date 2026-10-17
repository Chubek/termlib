# Optional entry point: cmake -S termscript -B build. The parent owns dependency
# setup, generated NovoParse descriptors, install rules and conformance tests.
cmake_minimum_required(VERSION 3.15)
project(TermscriptBootstrap LANGUAGES C CXX)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_CURRENT_BINARY_DIR}/termlib")
