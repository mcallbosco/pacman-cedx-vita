cmake_minimum_required(VERSION 3.14)

if (NOT DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Please define VITASDK env var to point to your SDK path or set CMAKE_TOOLCHAIN_FILE.")
endif ()

set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Toolchain file")

# We need to set linker flags BEFORE project() so CMake's compiler test passes.
# The softfp ABI objects conflict with hardfp crt0.o, but this is expected
# for .so loader ports and safe to suppress.
set(CMAKE_EXE_LINKER_FLAGS "-Wl,-z,nocopyreloc,--no-warn-mismatch,--noinhibit-exec" CACHE STRING "" FORCE)

project(pacmancedx C CXX)

include("${VITASDK}/share/vita.cmake" REQUIRED)

# Vita application configuration
set(VITA_APP_NAME "PAC-MAN CE DX")
set(VITA_TITLEID "PCMN00001")
set(VITA_VPKNAME "pacmancedx")
set(VITA_VERSION "01.00")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

# Game-specific definitions
# DATA_PATH:         game data copied to the memory card (ux0:) — not bundled
#                    in the VPK so VitaShell's 30+ minute install step can be
#                    skipped. The website's "split install" download gives
#                    users a zip that drops straight into ux0:data/pacmancedx/.
# SAVE_PATH:         writable save data on memory card
# SHADER_CACHE_PATH: writable shader cache (must be on ux0:, app0: is read-only)
set(DATA_PATH "ux0:data/pacmancedx/" CACHE STRING "Path to read-only game data (with trailing /)")
set(DATA_PATH_NOSLASH "ux0:data/pacmancedx" CACHE STRING "Path to read-only game data (no trailing /)")
set(WRITABLE_PATH "ux0:data/pacmancedx/" CACHE STRING "Writable state root for config/log (with trailing /)")
set(SAVE_PATH "${WRITABLE_PATH}files/" CACHE STRING "Path to writable save data (with trailing /)")
set(SHADER_CACHE_PATH "${WRITABLE_PATH}gxp/" CACHE STRING "Path to shader cache (with trailing /)")
set(SHADER_CACHE_PATH_NOSLASH "${WRITABLE_PATH}gxp" CACHE STRING "Path to shader cache (no trailing /)")
set(SO_PATH "${DATA_PATH}lib/armeabi-v7a/libPacmanCE.so" CACHE STRING "Path to main .so")
set(FMOD_SO_PATH "${DATA_PATH}lib/armeabi-v7a/libfmod.so" CACHE STRING "Path to FMOD .so")
add_definitions(-DDATA_PATH="${DATA_PATH}"
                -DDATA_PATH_NOSLASH="${DATA_PATH_NOSLASH}"
                -DWRITABLE_PATH="${WRITABLE_PATH}"
                -DSAVE_PATH="${SAVE_PATH}"
                -DSHADER_CACHE_PATH="${SHADER_CACHE_PATH}"
                -DSHADER_CACHE_PATH_NOSLASH="${SHADER_CACHE_PATH_NOSLASH}"
                -DSO_PATH="${SO_PATH}"
                -DFMOD_SO_PATH="${FMOD_SO_PATH}")

# Optional: when set to a directory, recursively bundle its contents into the
# VPK at data/<relpath>. The directory should contain:
#     lib/armeabi-v7a/libPacmanCE.so
#     lib/armeabi-v7a/libfmod.so
#     assets/...
# (merge APK assets/ and OBB assets/). Leave empty for a lightweight "loader only"
# VPK that the docs/ web builder injects game data into client-side.
set(BUNDLE_GAME_DATA_DIR "" CACHE PATH "Directory to bundle into the VPK at data/")

option(USE_SCELIBC_IO "Use SceLibcBridge for IO and some other std functions" ON)
if (USE_SCELIBC_IO)
  add_definitions(-DUSE_SCELIBC_IO)
endif()

option(ENABLE_RUNTIME_LOGS "Enable verbose soloader runtime logging (DEBUG_SOLOADER)" OFF)
option(ENABLE_GL_DEBUG_HOOKS "Enable heavy OpenGL debug hooks/instrumentation (DEBUG_OPENGL)" OFF)
option(ENABLE_FALSOJNI_VERBOSE "Enable verbose FalsoJNI logging (FALSOJNI_DEBUGLEVEL=0)" OFF)
option(ENABLE_AUDIO_LOGS "Enable FMOD/BGM/audio diagnostics logging" OFF)
option(ENABLE_IO_PROFILING "Enable per-call I/O profiling instrumentation (adds ~150-200ms to cold start)" OFF)

set(VITA_MSAA_MODE "OFF" CACHE STRING "MSAA mode for vitaGL (OFF, 2X, 4X)")
if (${VITA_MSAA_MODE} STREQUAL "OFF")
  add_definitions(-DVITA_MSAA_MODE=SCE_GXM_MULTISAMPLE_NONE)
elseif (${VITA_MSAA_MODE} STREQUAL "2X")
  add_definitions(-DVITA_MSAA_MODE=SCE_GXM_MULTISAMPLE_2X)
elseif (${VITA_MSAA_MODE} STREQUAL "4X")
  add_definitions(-DVITA_MSAA_MODE=SCE_GXM_MULTISAMPLE_4X)
else()
  message(FATAL_ERROR "VITA_MSAA_MODE option can only be one of: 'OFF', '2X', '4X'")
endif()

set(SHADER_FORMAT "GLSL" CACHE STRING "Preferred shader format (one of 'GLSL', 'CG', 'GXP')")
if (${SHADER_FORMAT} STREQUAL "GLSL")
  add_definitions(-DUSE_GLSL_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "CG")
  add_definitions(-DUSE_CG_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "GXP")
  add_definitions(-DUSE_GXP_SHADERS)
else()
  message(FATAL_ERROR "SHADER_FORMAT option can only be one of: 'GLSL', 'CG', 'GXP'")
endif()

set(valid_dump_formats GLSL CG)
option(DUMP_COMPILED_SHADERS "When using GLSL or CG shader format, cache compiled shaders on disk" OFF)
if (DUMP_COMPILED_SHADERS)
  if (SHADER_FORMAT IN_LIST valid_dump_formats)
    add_definitions(-DDUMP_COMPILED_SHADERS)
  else()
    message(WARNING "DUMP_COMPILED_SHADERS has no effect for the chosen shader format ${SHADER_FORMAT}")
  endif()
endif()

# Debug instrumentation/logging is explicitly opt-in to allow playable debug builds.
if (ENABLE_RUNTIME_LOGS)
  add_definitions(-DDEBUG_SOLOADER)
endif()

if (ENABLE_GL_DEBUG_HOOKS)
  add_definitions(-DDEBUG_OPENGL)
endif()

if (ENABLE_FALSOJNI_VERBOSE)
  add_definitions(-DFALSOJNI_DEBUGLEVEL=0)
endif()

if (ENABLE_AUDIO_LOGS)
  add_definitions(-DENABLE_AUDIO_LOGS)
endif()

if (ENABLE_IO_PROFILING)
  add_definitions(-DENABLE_IO_PROFILING)
endif()

# makes sincos, sincosf, etc. visible
add_definitions(-D_GNU_SOURCE -D__POSIX_VISIBLE=999999)

# IMPORTANT: -mfloat-abi=softfp is required for compatibility with Android .so files.
# --no-warn-mismatch and --noinhibit-exec suppress VFP ABI mismatch between
# our softfp code and the hardfp VitaSDK system libs (crt0, etc).
# This is the standard approach used by .so loader ports.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q,--allow-multiple-definition,--no-warn-mismatch,--noinhibit-exec -mfloat-abi=softfp -std=gnu11 -Wno-deprecated")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wl,-q,--allow-multiple-definition,--no-warn-mismatch,--noinhibit-exec -mfloat-abi=softfp -std=gnu++20 -Wno-write-strings -Wno-psabi")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O1 -g3")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O1 -g3")
else ()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g0 -ffast-math")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -g0 -ffast-math")
endif()

add_executable(${CMAKE_PROJECT_NAME}
               source/main.c
               source/dynlib.c
               source/java.c
               source/patch.c
               source/reimpl/bits/_ctype.c
               source/reimpl/controls.c
               source/reimpl/controls.h
               source/reimpl/egl.c
               source/reimpl/errno.c
               source/reimpl/io.c
               source/reimpl/log.c
               source/reimpl/mem.c
               source/reimpl/pthr.c
               source/reimpl/sys.c
               source/reimpl/time64.c
               source/reimpl/kubridge_shim.c
               source/reimpl/asset_manager.cpp
               source/utils/dialog.c
               source/utils/glutil.c
               source/utils/init.c
               source/utils/logger.c
               source/utils/pgxt.c
               source/utils/preloader.c
               source/utils/settings.c
               source/utils/utils.c
               lib/falso_jni/FalsoJNI.c
               lib/falso_jni/FalsoJNI_ImplBridge.c
               lib/falso_jni/FalsoJNI_Logger.c
               lib/falso_jni/converter.c
               lib/sha1/sha1.c
               lib/fios/fios.c
               lib/so_util/so_util.c)

# ---------------------------------------------------------------------------
# vitaGL — built from patched source in lib/vitaGL
# ---------------------------------------------------------------------------
set(VITAGL_DIR "${CMAKE_SOURCE_DIR}/lib/vitaGL")
set(VITAGL_LIB "${VITAGL_DIR}/libvitaGL.a")

# Build flags matching what the port needs
set(VITAGL_MAKE_FLAGS
    HAVE_GLSL_SUPPORT=1
    HAVE_SHADER_CACHE=1
    SOFTFP_ABI=1
)

add_custom_command(
    OUTPUT ${VITAGL_LIB}
    COMMAND make -j$$(nproc) ${VITAGL_MAKE_FLAGS}
    WORKING_DIRECTORY ${VITAGL_DIR}
    COMMENT "Building vitaGL from source (GLSL + softfp)"
)
add_custom_target(vitaGL_build DEPENDS ${VITAGL_LIB})

# Import the built static library
add_library(vitaGL_lib STATIC IMPORTED)
set_target_properties(vitaGL_lib PROPERTIES IMPORTED_LOCATION ${VITAGL_LIB})
add_dependencies(vitaGL_lib vitaGL_build)

add_subdirectory(lib/libc_bridge)
add_dependencies(${CMAKE_PROJECT_NAME} SceLibcBridge vitaGL_build)

target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM
                           PUBLIC ${CMAKE_SOURCE_DIR}/lib
                           PUBLIC ${CMAKE_SOURCE_DIR}/lib/vitaGL/source
                           )

target_include_directories(${CMAKE_PROJECT_NAME}
                           PUBLIC ${CMAKE_SOURCE_DIR}/source
                           )

target_link_libraries(${CMAKE_PROJECT_NAME}
                      -Wl,--whole-archive pthread -Wl,--no-whole-archive
                      OpenSLES
                      sndfile
                      mp3lame
                      mpg123
                      opus
                      vorbis
                      ogg
                      vorbisenc
                      vorbisfile
                      FLAC
                      stdc++
                      vitaGL_lib
                      vitashark
                      kubridge_stub
                      SceShaccCgExt
                      taihen_stub
                      SceShaccCg_stub
                      z
                      m
                      ScePgf_stub
                      mathneon
                      ${CMAKE_BINARY_DIR}/lib/libc_bridge/stubs/libSceLibcBridge_stub.a
                      SceHid_stub
                      SceTouch_stub
                      SceMotion_stub
                      SceAppMgr_stub
                      SceAppUtil_stub
                      SceAudio_stub
                      SceAudioIn_stub
                      SceCommonDialog_stub
                      SceDisplay_stub
                      SceKernelDmacMgr_stub
                      SceFios2_stub
                      SceCtrl_stub
                      SceGxm_stub
                      ScePower_stub
                      SceTouch_stub
                      SceVshBridge_stub
                      )

vita_create_self(eboot.bin ${CMAKE_PROJECT_NAME} UNSAFE NOASLR)

# ---------------------------------------------------------------------------
# vitaGL (clean/unpatched) — for the configurator + imgui
# Built from source (hardfp) to ensure legacy fixed-function pipeline support.
# ---------------------------------------------------------------------------
set(VITAGL_CLEAN_DIR "${CMAKE_SOURCE_DIR}/lib/vitaGL_clean")
set(VITAGL_CLEAN_LIB "${VITAGL_CLEAN_DIR}/libvitaGL.a")

add_custom_command(
    OUTPUT ${VITAGL_CLEAN_LIB}
    COMMAND make -j$$(nproc) HAVE_GLSL_SUPPORT=1
    WORKING_DIRECTORY ${VITAGL_CLEAN_DIR}
    COMMENT "Building vitaGL (clean, hardfp) from source"
)
add_custom_target(vitaGL_clean_build DEPENDS ${VITAGL_CLEAN_LIB})

add_library(vitaGL_clean_lib STATIC IMPORTED)
set_target_properties(vitaGL_clean_lib PROPERTIES IMPORTED_LOCATION ${VITAGL_CLEAN_LIB})
add_dependencies(vitaGL_clean_lib vitaGL_clean_build)

# Configurator - standalone settings app launched from LiveArea (imgui)
# The configurator does NOT load Android .so files, so it uses the standard
# hardfp ABI to match the SDK's pre-built imgui and system libraries.
add_executable(configurator source/configurator.cpp)
target_compile_options(configurator PRIVATE -mfloat-abi=hard)
target_link_libraries(configurator
                      vita2d
                      vitaGL_clean_lib
                      vitashark
                      SceShaccCgExt
                      taihen_stub
                      SceShaccCg_stub
                      SceDisplay_stub
                      SceCtrl_stub
                      SceGxm_stub
                      SceCommonDialog_stub
                      SceAppUtil_stub
                      SceAppMgr_stub
                      SceKernelDmacMgr_stub
                      ScePower_stub
                      SceHid_stub
                      SceTouch_stub
                      SceSysmodule_stub
                      ScePgf_stub
                      stdc++
                      z
                      m
                      mathneon
                      )
vita_create_self(configurator.bin configurator UNSAFE NOASLR)

# VPK contents. By default a lightweight loader-only VPK is produced — the
# docs/ website then injects game data into the base VPK client-side before
# serving it to the end user. Set -DBUNDLE_GAME_DATA_DIR=/path/to/merged to
# bundle game files at build time instead.
#
# Either way, at runtime the loader reads game data from DATA_PATH (app0:data/)
# and writable state from SAVE_PATH / SHADER_CACHE_PATH (ux0:data/pacmancedx/).

set(VPK_EXTRA_FILES "")
if(BUNDLE_GAME_DATA_DIR AND EXISTS "${BUNDLE_GAME_DATA_DIR}")
    message(STATUS "Bundling game data from ${BUNDLE_GAME_DATA_DIR} into VPK at data/")
    file(GLOB_RECURSE _bundle_files
         LIST_DIRECTORIES FALSE
         RELATIVE "${BUNDLE_GAME_DATA_DIR}"
         "${BUNDLE_GAME_DATA_DIR}/*")
    foreach(f ${_bundle_files})
        list(APPEND VPK_EXTRA_FILES "${BUNDLE_GAME_DATA_DIR}/${f}" "data/${f}")
    endforeach()
    list(LENGTH _bundle_files _bundle_count)
    message(STATUS "  bundled ${_bundle_count} files")
endif()

vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
                VERSION ${VITA_VERSION}
                NAME ${VITA_APP_NAME}
                FILE ${CMAKE_SOURCE_DIR}/extras/cpuinfo cpuinfo
                ${CMAKE_SOURCE_DIR}/extras/meminfo meminfo
                ${CMAKE_SOURCE_DIR}/extras/urandom urandom
                ${CMAKE_BINARY_DIR}/configurator.bin configurator.bin
                ${CMAKE_SOURCE_DIR}/extras/livearea/icon0.png sce_sys/icon0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/pic0.png sce_sys/pic0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/startup.png sce_sys/livearea/contents/startup.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/bg0.png sce_sys/livearea/contents/bg0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/template.xml sce_sys/livearea/contents/template.xml
                ${VPK_EXTRA_FILES}
                )
