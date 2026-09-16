cmake_minimum_required(VERSION 3.24)

# The subset of PhysBAM sources the port compiles: small matrix/rotation
# kernels plus the level-set machinery behind fixed-collision surfaces.
set(PHYSBAM_SUBSET_SOURCES
  Public_Library/PhysBAM_Tools/Matrices/MATRIX_3X3.cpp
  Public_Library/PhysBAM_Tools/Matrices/SYMMETRIC_MATRIX_3X3.cpp
  Public_Library/PhysBAM_Tools/Matrices/ROTATION.cpp
  shims/DebugStubs.cpp
  shims/LogNoop.cpp
)

file(GLOB PHYSBAM_LEVELSET_GEOMETRY_SOURCES
    Public_Library/PhysBAM_Geometry/Basic_Geometry/*.cpp
    Public_Library/PhysBAM_Geometry/Basic_Geometry_Intersections/*.cpp
    Public_Library/PhysBAM_Geometry/Collisions/*.cpp
    Public_Library/PhysBAM_Geometry/Collisions_And_Grids/*.cpp
    Public_Library/PhysBAM_Geometry/Collision_Detection/*.cpp
    Public_Library/PhysBAM_Geometry/Continuous_Collision_Detection/*.cpp
    Public_Library/PhysBAM_Geometry/Geometry_Particles/*.cpp
    Public_Library/PhysBAM_Geometry/Grids_Uniform_Advection_Collidable/*.cpp
    Public_Library/PhysBAM_Geometry/Grids_Uniform_Collisions/*.cpp
    Public_Library/PhysBAM_Geometry/Grids_Uniform_Level_Sets/*.cpp
    Public_Library/PhysBAM_Geometry/Grids_Uniform_Interpolation_Collidable/*.cpp
    Public_Library/PhysBAM_Geometry/Implicit_Objects/*.cpp
    Public_Library/PhysBAM_Geometry/Topology/*.cpp
    Public_Library/PhysBAM_Geometry/Topology_Based_Geometry/*.cpp
    Public_Library/PhysBAM_Geometry/Topology_Based_Geometry_Intersections/*.cpp
    Public_Library/PhysBAM_Geometry/Level_Sets/*.cpp
    Public_Library/PhysBAM_Geometry/Registry/*.cpp
    Public_Library/PhysBAM_Geometry/Solids_Geometry/*.cpp
    Public_Library/PhysBAM_Geometry/Solids_Geometry_Evolution/*.cpp
    Public_Library/PhysBAM_Geometry/Spatial_Acceleration/*.cpp
  )
  file(GLOB PHYSBAM_LEVELSET_TOOL_SOURCES
    Public_Library/PhysBAM_Tools/Arrays/*.cpp
    Public_Library/PhysBAM_Tools/Data_Structures/*.cpp
    Public_Library/PhysBAM_Tools/Grids_Uniform/*.cpp
    Public_Library/PhysBAM_Tools/Grids_Uniform_Arrays/*.cpp
    Public_Library/PhysBAM_Tools/Grids_Uniform_Boundaries/*.cpp
    Public_Library/PhysBAM_Tools/Grids_Uniform_Interpolation/*.cpp
    Public_Library/PhysBAM_Tools/Interpolation/*.cpp
    Public_Library/PhysBAM_Tools/Nonlinear_Equations/*.cpp
    Public_Library/PhysBAM_Tools/Parallel_Computation/*.cpp
    Public_Library/PhysBAM_Tools/Polynomials/*.cpp
    Public_Library/PhysBAM_Tools/Random_Numbers/*.cpp
    Public_Library/PhysBAM_Tools/Vectors/*.cpp
)
list(APPEND PHYSBAM_SUBSET_SOURCES
    shims/ReadWriteStructureRegistry.cpp
    Public_Library/PhysBAM_Tools/Grids_Uniform/GRID.cpp
    Public_Library/PhysBAM_Tools/Parsing/STRING_UTILITIES.cpp
    Public_Library/PhysBAM_Tools/Read_Write/Utilities/FILE_UTILITIES.cpp
    Public_Library/PhysBAM_Tools/Utilities/EXCEPTIONS.cpp
    Public_Library/PhysBAM_Tools/Utilities/TIMER.cpp
    Public_Library/PhysBAM_Geometry/Grids_Uniform_Computations/LEVELSET_MAKER_UNIFORM.cpp
    Public_Library/PhysBAM_Geometry/Grids_Uniform_Computations/RIGID_GEOMETRY_RASTERIZATION_UNIFORM.cpp
    Public_Library/PhysBAM_Geometry/Implicit_Objects_Uniform/LEVELSET_IMPLICIT_OBJECT.cpp
    ${PHYSBAM_LEVELSET_GEOMETRY_SOURCES}
    ${PHYSBAM_LEVELSET_TOOL_SOURCES}
)
list(REMOVE_DUPLICATES PHYSBAM_SUBSET_SOURCES)

add_library(PhysBAM_subset STATIC ${PHYSBAM_SUBSET_SOURCES})
add_library(PhysBAM::subset ALIAS PhysBAM_subset)
set_target_properties(PhysBAM_subset PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS ON)

target_include_directories(PhysBAM_subset
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Public_Library>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Common_Libraries>
    $<INSTALL_INTERFACE:include/PhysBAM_subset/Public_Library>
    $<INSTALL_INTERFACE:include/PhysBAM_subset/Common_Libraries>)

target_compile_definitions(PhysBAM_subset
  PUBLIC
    COMPILE_ID_TYPES_AS_INT
    COMPILE_WITHOUT_DYADIC_SUPPORT
    COMPILE_WITHOUT_RLE_SUPPORT
    COMPILE_WITHOUT_ZLIB_SUPPORT
    _USE_MATH_DEFINES
    PHYSBAM_DISABLE_LOG_REAL)
