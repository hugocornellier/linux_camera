cmake_minimum_required(VERSION 3.10)

set(PROJECT_NAME "camera_desktop")
project(${PROJECT_NAME} LANGUAGES CXX)

set(PLUGIN_NAME "camera_desktop_plugin")

list(APPEND PLUGIN_SOURCES
  "camera_desktop_plugin.cc"
  "camera_texture.cc"
  "camera.cc"
  "device_enumerator.cc"
  "photo_handler.cc"
  "record_handler.cc"
)

add_library(${PLUGIN_NAME} SHARED
  ${PLUGIN_SOURCES}
)

apply_standard_settings(${PLUGIN_NAME})

set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)

target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(${PLUGIN_NAME} PRIVATE flutter)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::GTK)

# GStreamer dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED
  gstreamer-1.0
  gstreamer-app-1.0
  gstreamer-video-1.0
)
target_include_directories(${PLUGIN_NAME} PRIVATE ${GSTREAMER_INCLUDE_DIRS})
target_link_libraries(${PLUGIN_NAME} PRIVATE ${GSTREAMER_LIBRARIES})

set(camera_desktop_bundled_libraries
  ""
  PARENT_SCOPE
)
