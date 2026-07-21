cmake_minimum_required(VERSION 3.22.1 FATAL_ERROR)

set(REVC_PROJECT_NAME "reVC")
set(REVC_VERSION 0.0.1)
set(REVC_DESCRIPTION "reverted ${REVC_PROJECT_NAME}")

project(${REVC_PROJECT_NAME}
        VERSION ${REVC_VERSION}
        LANGUAGES C CXX
        DESCRIPTION ${REVC_DESCRIPTION}
)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(REVC_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)
set(REVC_GAME_DIR "${REVC_ROOT_DIR}/../../../")

if(NOT IS_DIRECTORY "${REVC_GAME_DIR}" OR NOT EXISTS "${REVC_GAME_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "reVC source tree not found at ${REVC_GAME_DIR}")
endif()

# Configure librw/reVC for the Android OpenGL ES + SDL2 path before the
# real project is added. These are cache variables because the vendored
# subprojects read them during configure.
set(LIBRW_PLATFORM_GL3 ON CACHE BOOL "Use librw GL3/GLES renderer" FORCE)
set(LIBRW_GL3_GFXLIB "SDL2" CACHE STRING "Use SDL2 window/input backend" FORCE)
set(REVC_AUDIO "OAL" CACHE STRING "Use OpenAL audio backend" FORCE)
set(REVC_VENDORED_LIBRW ON CACHE BOOL "Use vendored librw" FORCE)
set(REVC_INSTALL OFF CACHE BOOL "Do not create desktop install targets" FORCE)

# This is the missing connection in the old placeholder launcher: it makes
# Gradle/CMake compile the actual reVC engine and package libreVC.so.
add_subdirectory("${REVC_GAME_DIR}" "${CMAKE_BINARY_DIR}/revc_game")
