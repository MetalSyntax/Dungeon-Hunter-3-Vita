cmake_minimum_required(VERSION 3.14)

if (NOT DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Please define VITASDK env var to point to your SDK path or set CMAKE_TOOLCHAIN_FILE.")
endif ()

set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Toolchain file")

project(dungeonhunter3 C CXX)

include("${VITASDK}/share/vita.cmake" REQUIRED)

# Vita application configuration
set(VITA_APP_NAME "Dungeon Hunter 3")
set(VITA_TITLEID "PSVDH0003")
set(VITA_VPKNAME "dungeonhunter3")
set(VITA_VERSION "01.00")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

# Optional: used for "send" and "dump" targets
set(PSVITAIP "192.168.1.100" CACHE STRING "PSVita IP (for FTP access; override with -DPSVITAIP=...)")
set(PARSECOREPATH "$ENV{PARSECOREPATH}" CACHE STRING "vita-parse-core main.py")

# Game-specific definitions
set(DATA_PATH "ux0:data/dungeonhunter3/" CACHE STRING "Path to data (with trailing /)")
set(SO_PATH "${DATA_PATH}libDH3.so" CACHE STRING "Path to .so")
add_definitions(-DDATA_PATH="${DATA_PATH}" -DSO_PATH="${SO_PATH}")

option(USE_SCELIBC_IO "Use SceLibcBridge for IO and some other std functions" ON)
if (USE_SCELIBC_IO)
  add_definitions(-DUSE_SCELIBC_IO)
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
option(DUMP_COMPILED_SHADERS "When using GLSL or CG shader format, cache compiled shaders on disk" ON)
# 2026-10-02: forced OFF. With VGL_MODE_POSTPONED vitaGL only compiles in
# glLinkProgram, so glutil.c's vglGetShaderBinary-after-glCompileShader cache
# never got a binary (ux0:data/dungeonhunter3/gxp/ was never created). The
# vendored vitaGL's HAVE_SHADER_CACHE (ux0:data/shader_cache/PSVDH0003/)
# caches at the right point. The toolkit passes -DDUMP_COMPILED_SHADERS=ON,
# hence FORCE.
set(DUMP_COMPILED_SHADERS OFF CACHE BOOL "" FORCE)
if (DUMP_COMPILED_SHADERS)
  if (SHADER_FORMAT IN_LIST valid_dump_formats)
    add_definitions(-DDUMP_COMPILED_SHADERS)
  else()
    message(WARNING "DUMP_COMPILED_SHADERS has no effect for the chosen shader format ${SHADER_FORMAT}")
  endif()
endif()

# Unique debug definition for loader's logging to not interfere with anything
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_definitions(-DDEBUG_SOLOADER)
  # Uncomment this for verbose FalsoJNI logging:
  #add_definitions(-DFALSOJNI_DEBUGLEVEL=0)
endif()

# makes sincos, sincosf, etc. visible
add_definitions(-D_GNU_SOURCE -D__POSIX_VISIBLE=999999)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -O3 -g -ffast-math -mfloat-abi=softfp -Wno-deprecated")
# 2026-10-01: system vitaGL (vdpm) also exports eglQueryString, colliding with
# this template's source/reimpl/egl.c fake. DH2 never hits it because it builds
# vitaGL from source (lib/vitagl) with its own flags. Ours (executable objs)
# must win so the engine's egl calls keep hitting the reimpl fakes -- same
# behavior the template assumes. First hardware log must confirm egl works;
# long-term fix is vendoring vitaGL like DH2 (see port_progress.md Fase 6).
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--allow-multiple-definition")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -std=gnu++20 -Wno-write-strings -Wno-psabi")

add_executable(${CMAKE_PROJECT_NAME}
               source/main.c
               source/video.cpp
               source/dynlib.c
               source/java.c
               source/patch.c
               source/reimpl/bits/_ctype.c
               source/reimpl/basic_file.c
               source/reimpl/pak.c
               source/reimpl/vfile.c
               source/reimpl/egl.c
               source/reimpl/errno.c
               source/reimpl/fsbuf.c
               source/reimpl/io.c
               source/reimpl/log.c
               source/reimpl/material.c
               source/reimpl/mem.c
               source/reimpl/pthr.c
               source/reimpl/sys.c
               source/reimpl/time64.c
               source/reimpl/asset_manager.cpp
               source/utils/dialog.c
               source/utils/glutil.c
               source/utils/init.c
               source/utils/logger.c
               source/utils/settings.c
               source/utils/utils.c
               lib/falso_jni/FalsoJNI.c
               lib/falso_jni/FalsoJNI_ImplBridge.c
               lib/falso_jni/FalsoJNI_Logger.c
               lib/sha1/sha1.c
               lib/fios/fios.c
               lib/so_util/so_util.c)

add_subdirectory(lib/libc_bridge)
add_dependencies(${CMAKE_PROJECT_NAME} SceLibcBridge)

target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM
                           PUBLIC ${CMAKE_SOURCE_DIR}/lib
                           )

target_include_directories(${CMAKE_PROJECT_NAME}
                           PUBLIC ${CMAKE_SOURCE_DIR}/source
                           )

# vitaGL vendorizado (lib/vitagl, copiado del port DH2 que corre en hardware;
# ver lib/vitagl/VENDORED.md). Su Makefile se llama vitagl.mk porque el
# .gitignore ignora "Makefile" y el toolkit copia el arbol respetandolo. Por que no el libvitaGL.a del vitasdk: viene
# sin logs ni shader cache, y un shader que no compila deja prog=NULL y
# crashea glLinkProgram (log_006). Parche local [DH3] en compile_shader.
# Flags = los base de DH2 (probados en hardware, Fase 23: 5-6 -> 20-25 FPS)
# + HAVE_SHARK_LOG para capturar el error de shacccg.
#   SOFTFP_ABI=1: libDH3.so es softfp (glUniform*f con floats en r0-r3).
#   Sin DEPTH_STENCIL_HACK/PRIMITIVES_SPEEDHACK: GameSWF usa stencil y lineas.
#   SAFE_ETC1=1: sin ETC1 por hardware (texturas rotas, log_015); gl_init()
#   pide recompresion a DXT1 para no multiplicar x6 la memoria de texturas.
#   Parche [DH3] en custom_shaders.c (dh3_attribs_interleaved): la deteccion
#   de atributos intercalados revisa TODOS, no solo los dos primeros (heroe
#   con skinning por CPU sin textura, log_022..026). Reemplaza SAFE_DRAW=1,
#   que tambien lo arreglaba pero bajaba los FPS (log_027).
set(VITAGL_MAKE_FLAGS_STR "SOFTFP_ABI=1 NO_DEBUG=1 HAVE_SHADER_CACHE=1 HAVE_SHARK_LOG=1 NO_SPLASHSCREEN=1 HAVE_GLSL_UBOS=1 SAMPLERS_SPEEDHACK=1 MATH_SPEEDHACK=1 CIRCULAR_POOL_SPEEDHACK=1 NO_TEX_COMBINER=1 DRAW_SPEEDHACK=2 SAFE_ETC1=1")
separate_arguments(VITAGL_MAKE_FLAGS UNIX_COMMAND "${VITAGL_MAKE_FLAGS_STR}")
set(VITAGL_STAMP "${CMAKE_BINARY_DIR}/vitagl.stamp")
include(ProcessorCount)
ProcessorCount(NCPU)
add_custom_target(vitaGL_lib
  COMMAND bash "${CMAKE_SOURCE_DIR}/lib/vitagl/build_vitagl.sh" "${VITAGL_MAKE_FLAGS_STR}" "${VITAGL_STAMP}" "${CMAKE_SOURCE_DIR}/lib/vitagl"
  COMMAND make -j${NCPU} -C "${CMAKE_SOURCE_DIR}/lib/vitagl" -f vitagl.mk ${VITAGL_MAKE_FLAGS}
  BYPRODUCTS "${CMAKE_SOURCE_DIR}/lib/vitagl/libvitaGL.a"
  COMMENT "Building vendored vitaGL..."
)
add_dependencies(${CMAKE_PROJECT_NAME} vitaGL_lib)
# vitaGL.h del arbol vendorizado antes que el del sistema
target_include_directories(${CMAKE_PROJECT_NAME} BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/lib/vitagl/source)

target_link_libraries(${CMAKE_PROJECT_NAME}
                      -Wl,--whole-archive pthread -Wl,--no-whole-archive
                      stdc++
                      ${CMAKE_SOURCE_DIR}/lib/vitagl/libvitaGL.a
                      vitashark
                      SceShaccCgExt
                      taihen_stub
                      SceShaccCg_stub
                      z
                      m
                      ScePgf_stub
                      mathneon
                      kubridge_stub
                      ${CMAKE_BINARY_DIR}/lib/libc_bridge/stubs/libSceLibcBridge_stub.a
                      SceHid_stub
                      SceTouch_stub
                      SceMotion_stub
                      SceAppMgr_stub
                      SceAppUtil_stub
                      SceAudio_stub
                      SceAvPlayer_stub
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
vita_create_vpk(${VITA_VPKNAME}.vpk ${VITA_TITLEID} eboot.bin
                VERSION ${VITA_VERSION}
                NAME ${VITA_APP_NAME}
                FILE ${CMAKE_SOURCE_DIR}/extras/cpuinfo cpuinfo
                ${CMAKE_SOURCE_DIR}/extras/meminfo meminfo
                ${CMAKE_SOURCE_DIR}/extras/livearea/icon0.png sce_sys/icon0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/pic0.png sce_sys/pic0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/startup.png sce_sys/livearea/contents/startup.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/bg0.png sce_sys/livearea/contents/bg0.png
                ${CMAKE_SOURCE_DIR}/extras/livearea/template.xml sce_sys/livearea/contents/template.xml
                )

add_custom_target(send
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ${VITA_TITLEID} | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}.vpk-vpk
                  )

add_custom_target(send_kvdb
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ux0:/app/${VITA_TITLEID}/eboot.bin | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}.vpk-vpk
                  )

add_custom_target(dump
                  COMMAND ${CMAKE_SOURCE_DIR}/extras/scripts/get_dump.sh ${PSVITAIP}
                  COMMAND python3 ${PARSECOREPATH} coredump ${CMAKE_PROJECT_NAME}
                  )

add_custom_target(reboot
                  COMMAND echo reboot | nc ${PSVITAIP} 1338
                  )
