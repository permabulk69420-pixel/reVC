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
set(REVC_VENDOR_DIR "${REVC_GAME_DIR}/vendor")

if(NOT IS_DIRECTORY "${REVC_GAME_DIR}" OR NOT EXISTS "${REVC_GAME_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "reVC source tree not found at ${REVC_GAME_DIR}")
endif()

if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "Quest builds support arm64-v8a only (got ${ANDROID_ABI})")
endif()

set(SDL2_INCLUDE_DIR "${REVC_VENDOR_DIR}/sdl2/include")
set(SDL2_LIBRARY "${REVC_VENDOR_DIR}/sdl2/libs/Android/${ANDROID_ABI}/libSDL2.so")
set(OPENAL_INCLUDE_DIR "${REVC_VENDOR_DIR}/openal-soft/include" CACHE PATH "Android OpenAL headers" FORCE)
set(OPENAL_LIBRARY "${REVC_VENDOR_DIR}/openal-soft/libs/Android/${ANDROID_ABI}/libopenal.so" CACHE FILEPATH "Android OpenAL library" FORCE)
set(mpg123_INCLUDE_DIR "${REVC_VENDOR_DIR}/mpg123/include" CACHE PATH "Android mpg123 headers" FORCE)
set(mpg123_LIBRARIES "${REVC_VENDOR_DIR}/mpg123/lib/Android/${ANDROID_ABI}/libmpg123.so" CACHE FILEPATH "Android mpg123 library" FORCE)

foreach(required_file
        "${SDL2_LIBRARY}"
        "${OPENAL_LIBRARY}"
        "${mpg123_LIBRARIES}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Required Android dependency not found: ${required_file}")
    endif()
endforeach()

if(NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 SHARED IMPORTED GLOBAL)
    set_target_properties(SDL2::SDL2 PROPERTIES
            IMPORTED_LOCATION "${SDL2_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${SDL2_INCLUDE_DIR}")
endif()

find_package(OpenXR REQUIRED CONFIG)

# Preserve the known-good Android renderer exactly as it was configured.
set(LIBRW_PLATFORM "GL3" CACHE STRING "librw platform" FORCE)
set(LIBRW_GL3_GFXLIB "SDL2" CACHE STRING "librw window/input backend" FORCE)
set(LIBRW_TOOLS OFF CACHE BOOL "Do not build desktop librw tools" FORCE)
set(LIBRW_EXAMPLES OFF CACHE BOOL "Do not build desktop librw examples" FORCE)
set(LIBRW_INSTALL OFF CACHE BOOL "Do not install librw" FORCE)
set(REVC_AUDIO "OAL" CACHE STRING "Use OpenAL audio backend" FORCE)
set(REVC_VENDORED_LIBRW ON CACHE BOOL "Use vendored librw" FORCE)
set(REVC_INSTALL OFF CACHE BOOL "Do not create desktop install targets" FORCE)

add_subdirectory("${REVC_GAME_DIR}" "${CMAKE_BINARY_DIR}/revc_game")

# Native per-eye stereo: the SDL/EGL/OpenXR ownership and two-pass eye replay
# are preserved, and each eye is now submitted as an XrCompositionLayerProjection
# view by QuestOpenXRStereo. The quad-layer screen baseline and the stereo-replay
# disable were diagnostic scaffolding and have been removed.
target_compile_definitions(reVC PRIVATE
        ANDROID_x32=0
        XR_USE_PLATFORM_ANDROID
        XR_USE_GRAPHICS_API_OPENGL_ES)
target_link_libraries(reVC PRIVATE
        OpenXR::openxr_loader
        android
        log
        EGL
        GLESv3)
# Only symbols whose references genuinely cross object files are wrapped here.
#
# The render-sequence functions in core/main.cpp used to be listed too, but
# --wrap cannot reach them: each one is defined in main.cpp and called from
# Idle()/FrontendIdle() in main.cpp, so the compiler binds those calls to the
# local definition and the linker never sees a reference to redirect. Those ten
# wrappers silently never ran, which is why the right-eye replay never happened.
# They are now ordinary calls into QuestGameHooks from inside the function
# bodies. Do not add a --wrap entry for anything defined in core/main.cpp.
target_link_options(reVC PRIVATE
        "-Wl,--wrap=RsEventHandler"
        "-Wl,--wrap=psCameraBeginUpdate"
        "-Wl,--wrap=psCameraShowRaster"
        "-Wl,--wrap=glBlitFramebuffer"
        "-Wl,--wrap=_ZN11QuestOpenXR10InitializeEv"
        "-Wl,--wrap=_ZN4CPad10UpdatePadsEv"
        "-Wl,--wrap=_ZN9CRenderer19ConstructRenderListEv"
        "-Wl,--wrap=_ZN9CRenderer9PreRenderEv")
