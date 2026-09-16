cmake_minimum_required(VERSION 3.24)

add_library(PDTetPhysics STATIC
  PDDeformer/src/Add_Force.cpp
  PDDeformer/src/GridDeformerTet.cpp
  PDDeformer/src/ReshapeDataStructure.cpp
  PDDeformer/src/SchurSolver.cpp
  PDDeformer/src/PardisoWrapper.cpp
  src/PDTetSolver.cpp
  src/MergedLevelSet.cpp)
add_library(PD::TetPhysics ALIAS PDTetPhysics)

set_target_properties(PDTetPhysics PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(PDTetPhysics
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/PDDeformer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../simd-numeric-kernels-new
    ${CMAKE_CURRENT_SOURCE_DIR}/../simd-numeric-kernels-new/Common
    ${CMAKE_CURRENT_SOURCE_DIR}/../PhysBAM_subset/Public_Library
    ${CMAKE_CURRENT_SOURCE_DIR}/../PhysBAM_subset/Common_Libraries
    ${CMAKE_CURRENT_SOURCE_DIR}/../external/eigen)

target_compile_definitions(PDTetPhysics
  PUBLIC
    ENABLE_AVX_INSTRUCTION_SET
    COMPILE_ID_TYPES_AS_INT
    COMPILE_WITHOUT_DYADIC_SUPPORT
    COMPILE_WITHOUT_RLE_SUPPORT
    COMPILE_WITHOUT_ZLIB_SUPPORT)

target_link_libraries(PDTetPhysics
  PUBLIC
    PhysBAM_subset
    port::simd
    port::tasking
    port::la_dense
    port::sparse)
