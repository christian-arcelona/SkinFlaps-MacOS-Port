cmake_minimum_required(VERSION 3.24)

# All sources listed here are the pristine vendor files. The top-level
# CMakeLists swaps the macOS overlay sources onto this target.
set(SKINFLAPS_SOURCES
  src/FacialFlapsGui.cpp
  src/bccTetScene.cpp
  src/deepCut.cpp
  src/fence.cpp
  src/hooks.cpp
  src/json.cpp
  src/main.cpp
  src/remapTetPhysics.cpp
  src/skinCutUndermineTets.cpp
  src/surgicalActions.cpp
  src/sutures.cpp
  src/tetCollisions.cpp
  src/tetSubset.cpp
  src/vnBccTetCutter_tbb.cpp
  src/vnBccTetrahedra.cpp)

if(APPLE)
  set(_skinflaps_icon "${CMAKE_CURRENT_SOURCE_DIR}/../installer/resources/SkinFlaps.icns")
  set_source_files_properties("${_skinflaps_icon}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
  add_executable(SkinFlaps MACOSX_BUNDLE ${SKINFLAPS_SOURCES} "${_skinflaps_icon}")
  set_target_properties(SkinFlaps PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "SkinFlaps"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.christian-arcelona.skinflaps"
    MACOSX_BUNDLE_ICON_FILE "SkinFlaps"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.1.6"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.1.6")
else()
  add_executable(SkinFlaps ${SKINFLAPS_SOURCES})
endif()

target_include_directories(SkinFlaps PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/../gl3wGraphics
  ${CMAKE_CURRENT_SOURCE_DIR}/../imgui_glfw_nfd_lib
  ${CMAKE_CURRENT_SOURCE_DIR}/src/CDT/include)

target_compile_definitions(SkinFlaps PRIVATE IMGUI_IMPL_OPENGL_LOADER_GL3W)

target_link_libraries(SkinFlaps PRIVATE
  gl3wGraphics
  imgui_glfw_ndf
  PD::TetPhysics
  port::simd
  port::tasking
  port::la_dense
  port::sparse)

if(APPLE)
  find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
  find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
  find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
  find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
  target_link_libraries(SkinFlaps PRIVATE ${COCOA_FRAMEWORK} ${IOKIT_FRAMEWORK} ${COREVIDEO_FRAMEWORK} ${OPENGL_FRAMEWORK})
endif()
