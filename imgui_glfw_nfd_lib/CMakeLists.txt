cmake_minimum_required (VERSION 3.15)

project(imgui_glfw_nfd_lib VERSION 0.1 LANGUAGES CXX C)

add_library(imgui_glfw_ndf
  extLibs/gl3w/GL/gl3w.c
  imgui.cpp
  imgui_demo.cpp
  imgui_draw.cpp
  imgui_tables.cpp
  imgui_impl_glfw.cpp
  imgui_impl_opengl3.cpp
  imgui_widgets.cpp
  ImGuiFileDialog.cpp)

# Use gl3w as OpenGL loader for ImGui backend
target_compile_definitions(imgui_glfw_ndf PUBLIC IMGUI_IMPL_OPENGL_LOADER_GL3W)

find_package(OpenGL REQUIRED)

# Prefer CMake config package (Homebrew) for glfw on macOS
set(_linked_glfw OFF)
if(APPLE)
  find_package(glfw3 CONFIG QUIET)
  if(TARGET glfw)
    target_link_libraries(imgui_glfw_ndf PUBLIC glfw)
    set(_linked_glfw ON)
  endif()
endif()

if(NOT _linked_glfw)
  # Try generic config
  find_package(glfw3 CONFIG QUIET)
  if(TARGET glfw)
    target_link_libraries(imgui_glfw_ndf PUBLIC glfw)
    set(_linked_glfw ON)
  endif()
endif()

if(NOT _linked_glfw)
  # Fallback to pkg-config (glfw or glfw3)
  find_package(PkgConfig)
  if(PkgConfig_FOUND)
    pkg_check_modules(GLFW3 QUIET glfw)
    if(NOT GLFW3_FOUND)
      pkg_check_modules(GLFW3 QUIET glfw3)
    endif()
    if(GLFW3_FOUND)
      target_include_directories(imgui_glfw_ndf PUBLIC ${GLFW3_INCLUDE_DIRS})
      target_link_libraries(imgui_glfw_ndf PUBLIC ${GLFW3_LIBRARIES})
      set(_linked_glfw ON)
    endif()
  endif()
endif()

if(NOT _linked_glfw)
  message(FATAL_ERROR "glfw not found: install via package manager (e.g., brew install glfw).")
endif()

target_link_libraries(imgui_glfw_ndf PUBLIC OpenGL::GL)

target_include_directories(imgui_glfw_ndf PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/extLibs/gl3w>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/>
  $<INSTALL_INTERFACE:include>)

if(APPLE)
  find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
  find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
  find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
  target_link_libraries(imgui_glfw_ndf PUBLIC ${COCOA_FRAMEWORK} ${IOKIT_FRAMEWORK} ${COREVIDEO_FRAMEWORK})
endif()
