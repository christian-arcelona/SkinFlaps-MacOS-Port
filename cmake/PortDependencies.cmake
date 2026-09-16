# Backend resolution for the macOS port: SIMDe (SIMD translation), Apple
# Accelerate (dense BLAS/LAPACK), SuiteSparse CHOLMOD (sparse Cholesky),
# oneTBB (tasking). CHOLMOD and TBB come from Homebrew.

include_guard(GLOBAL)

add_library(port_simd INTERFACE)
target_include_directories(port_simd INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/external/simde")
target_compile_definitions(port_simd INTERFACE SIMDE_ENABLE_NATIVE_ALIASES)
add_library(port::simd ALIAS port_simd)

find_library(ACCELERATE_FRAMEWORK Accelerate REQUIRED)
add_library(port_dense INTERFACE)
target_link_libraries(port_dense INTERFACE "${ACCELERATE_FRAMEWORK}")
add_library(port::la_dense ALIAS port_dense)

find_package(TBB CONFIG REQUIRED COMPONENTS tbb)
add_library(port_tasking INTERFACE)
target_link_libraries(port_tasking INTERFACE TBB::tbb)
add_library(port::tasking ALIAS port_tasking)

find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(CHOLMOD IMPORTED_TARGET cholmod)
endif()
if(CHOLMOD_FOUND)
  add_library(port_sparse INTERFACE)
  target_include_directories(port_sparse INTERFACE ${CHOLMOD_INCLUDE_DIRS})
  target_link_libraries(port_sparse INTERFACE PkgConfig::CHOLMOD port::la_dense port::tasking)
else()
  find_path(CHOLMOD_INCLUDE_DIR NAMES cholmod.h PATH_SUFFIXES suitesparse)
  find_library(CHOLMOD_LIB NAMES cholmod)
  if(NOT CHOLMOD_INCLUDE_DIR OR NOT CHOLMOD_LIB)
    message(FATAL_ERROR "CHOLMOD not found: brew install suite-sparse (or provide pkg-config 'cholmod').")
  endif()
  add_library(port_sparse INTERFACE)
  target_include_directories(port_sparse INTERFACE ${CHOLMOD_INCLUDE_DIR})
  target_link_libraries(port_sparse INTERFACE ${CHOLMOD_LIB} port::la_dense port::tasking)
endif()
add_library(port::sparse ALIAS port_sparse)
