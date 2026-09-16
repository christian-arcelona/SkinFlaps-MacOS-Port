cmake_minimum_required(VERSION 3.24)

project(SkinFlaps LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PortDependencies)

# Floating-point contraction and fast-math stay off in every configuration:
# the projective-dynamics iteration sits near its stability edge on post-cut
# topologies and is sensitive to FP reordering.
add_compile_options(-ffp-contract=off -fno-fast-math)

add_subdirectory(PhysBAM_subset)
add_subdirectory(PDTetPhysics)
add_subdirectory(imgui_glfw_nfd_lib)
add_subdirectory(gl3wGraphics)
add_subdirectory(SkinFlaps)

# Source-swap overlay: SkinFlaps/src/ stays byte-identical to the upstream
# Windows tree (vendor/original_windows); every macOS-specific change lives in
# overlays/ and is substituted here at configure time. This keeps the vendor
# tree mergeable as upstream evolves and confines the whole port to one
# reviewable directory.
if(NOT WIN32)
  set(_overlay "${CMAKE_SOURCE_DIR}/overlays")
  set(_overlay_src "${_overlay}/skinflaps_src")
  get_target_property(_sf_sources SkinFlaps SOURCES)
  list(REMOVE_ITEM _sf_sources
    "src/vnBccTetCutter_tbb.cpp"
    "src/FacialFlapsGui.cpp"
    "src/json.cpp"
    "src/remapTetPhysics.cpp"
    "src/main.cpp"
    "src/surgicalActions.cpp"
    "src/deepCut.cpp"
    "src/hooks.cpp"
    "src/sutures.cpp"
    "src/fence.cpp"
    "src/skinCutUndermineTets.cpp"
    "src/bccTetScene.cpp"
    "src/tetCollisions.cpp")
  list(APPEND _sf_sources
    "${_overlay}/vnBccTetCutter_tbb_deterministic.cpp"
    "${_overlay_src}/FacialFlapsGui.cpp"
    "${_overlay_src}/json.cpp"
    "${_overlay_src}/remapTetPhysics.cpp"
    "${_overlay_src}/main.cpp"
    "${_overlay_src}/surgicalActions.cpp"
    "${_overlay_src}/deepCut.cpp"
    "${_overlay_src}/hooks.cpp"
    "${_overlay_src}/sutures.cpp"
    "${_overlay_src}/fence.cpp"
    "${_overlay_src}/skinCutUndermineTets.cpp"
    "${_overlay_src}/bccTetScene.cpp"
    "${_overlay_src}/tetCollisions.cpp")
  set_target_properties(SkinFlaps PROPERTIES SOURCES "${_sf_sources}")
  # Overlay translation units resolve the overlay headers (FacialFlapsGui.h,
  # deepCut.h, bccTetScene.h and the others in overlays/skinflaps_src/) ahead
  # of the vendor copies; the vendor translation units keep their own headers
  # by the same-directory rule, and the overlay headers preserve every class
  # layout so both compile against one binary interface.
  target_include_directories(SkinFlaps BEFORE PRIVATE "${_overlay_src}")

  # gl3wGraphics overlay: the macOS HiDPI/int-coordinate changes live in
  # overlays/gl3wGraphics/ and the gl3wGraphics/ directory stays byte-identical
  # to the upstream tree (CMakeLists.txt excepted — build files are port-owned).
  # lines/sceneNode/shapes are byte-identical copies: they include the overlaid
  # gl3wGraphics.h by quoted include, and the same-directory rule must resolve
  # every translation unit of the library against one header.
  set(_gl3w_overlay "${_overlay}/gl3wGraphics")
  get_target_property(_gl3w_sources gl3wGraphics SOURCES)
  list(REMOVE_ITEM _gl3w_sources
    gl3wGraphics.cpp lightsShaders.cpp materialTriangles.cpp
    staticTriangle.cpp surgGraphics.cpp textures.cpp trackball.cpp
    lines.cpp sceneNode.cpp shapes.cpp)
  list(APPEND _gl3w_sources
    "${_gl3w_overlay}/gl3wGraphics.cpp"
    "${_gl3w_overlay}/lightsShaders.cpp"
    "${_gl3w_overlay}/materialTriangles.cpp"
    "${_gl3w_overlay}/staticTriangle.cpp"
    "${_gl3w_overlay}/surgGraphics.cpp"
    "${_gl3w_overlay}/textures.cpp"
    "${_gl3w_overlay}/trackball.cpp"
    "${_gl3w_overlay}/lines.cpp"
    "${_gl3w_overlay}/sceneNode.cpp"
    "${_gl3w_overlay}/shapes.cpp")
  set_target_properties(gl3wGraphics PROPERTIES SOURCES "${_gl3w_sources}")
  # The overlay gl3wGraphics.h / textures.h must shadow the vendor copies for
  # the library itself and for every consumer.
  target_include_directories(gl3wGraphics BEFORE PUBLIC
    "$<BUILD_INTERFACE:${_gl3w_overlay}>")
  target_include_directories(SkinFlaps BEFORE PRIVATE "${_gl3w_overlay}")
endif()
