# Keep QBE's process-global compiler state in a separate executable.
file(GLOB qbe_common "${PROJECT_SOURCE_DIR}/third_party/qbe/*.c")
file(GLOB qbe_targets "${PROJECT_SOURCE_DIR}/third_party/qbe/amd64/*.c"
  "${PROJECT_SOURCE_DIR}/third_party/qbe/arm64/*.c"
  "${PROJECT_SOURCE_DIR}/third_party/qbe/rv64/*.c")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/qbe-config/config.h" "#define Deftgt T_amd64_sysv\n")
add_executable(termscript-qbe ${qbe_common} ${qbe_targets})
target_include_directories(termscript-qbe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/qbe-config")
install(TARGETS termscript-qbe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
function(ts_add_keystone)
  set(BUILD_SHARED_LIBS OFF)
  set(BUILD_LIBS_ONLY ON CACHE BOOL "Build Keystone library only")
  set(LLVM_TARGETS_TO_BUILD X86 CACHE STRING "Keystone targets")
  add_subdirectory(third_party/keystone EXCLUDE_FROM_ALL)
endfunction()
ts_add_keystone()
target_sources(termscript-driver PRIVATE termscript/backends/native.c)
target_include_directories(termscript-driver PRIVATE third_party/keystone/include)
target_link_libraries(termscript-driver PRIVATE keystone)
target_compile_definitions(termscript-driver PRIVATE
  TS_HAVE_NATIVE=1 TS_QBE_BUILD="$<TARGET_FILE:termscript-qbe>"
  TS_QBE_INSTALL="${CMAKE_INSTALL_FULL_BINDIR}/termscript-qbe")
add_dependencies(termscript-driver termscript-qbe)
