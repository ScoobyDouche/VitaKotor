cmake_minimum_required(VERSION 3.2)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(KOTOR C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "KOTOR")
set(VITA_TITLEID  "KOTR00001")
set(VITA_VERSION  "01.00")
# ATTRIBUTE2=12 grants the extended-memory partition (~+109 MB + phycont).
# Without it the app only has the base user partition and a large newlib heap
# fails to allocate at CRT init -- which crashes in _sbrk_r before main().
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

# NOTE: This SDK (vitaGL/vitashark/mathneon) is built hardfp ("VFP registers"),
# so the loader is compiled hardfp to link. The Android armeabi-v7a ABI is
# softfp, so the RUNTIME port will need a softfp toolchain (rebuild vitaGL &
# deps with -mfloat-abi=softfp) to match float arguments across the .so
# boundary. That is a runtime-phase concern; this skeleton only needs to link.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -D_GNU_SOURCE -Wl,-q -Wall -O2 -fno-optimize-sibling-calls")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -std=c++11 -Wno-write-strings")

add_executable(KOTOR
  loader/main.c
  loader/so_util.c
  loader/dynlib.c
  loader/obb_index.c
  loader/loadscreen.c
  loader/jni_patch.c
  loader/audio_patch.c
  loader/audio_mp3.c
  loader/bink_patch.c
  loader/fs_patch.c
  loader/sdl_patch.c
  loader/input_patch.c
  loader/gl_patch.c
  loader/glsl_prep.c
  loader/crash.c
  loader/log.c
)

target_link_libraries(KOTOR
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  # real-code archives first, then the libs they depend on (stdc++, m) so the
  # linker resolves left-to-right without undefined std:: references
  SDL2
  vitaGL
  vitashark
  SceShaccCgExt
  mathneon
  # FreeType (KOTOR's font/text engine) + its decode deps, before stdc++/m
  freetype
  png16
  z
  bz2
  stdc++
  m
  taihen_stub
  kubridge_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceAudio_stub
  SceAudiodec_stub   # hardware MP3 decode for the FMOD backend (audio_mp3.c)
  SceAudioIn_stub
  SceCtrl_stub
  SceCommonDialog_stub
  SceDisplay_stub
  SceKernelDmacMgr_stub
  SceGxm_stub
  SceShaccCg_stub
  SceSysmodule_stub
  ScePower_stub
  SceTouch_stub
  SceVshBridge_stub
  # additional Sce stubs pulled in by Vita SDL2 (input/motion/ime/fios)
  SceHid_stub
  SceMotion_stub
  SceIme_stub
  SceFios2_stub
)

# Bundle our edited GLSL shader chunks into the VPK at app0:shaders/. The loader's
# SDL_RWFromFile hook serves these in preference to ux0:data/kotor/ so the shaders
# (which we edit for GXM's TEXCOORD budget) always travel with the loader and never
# depend on a manual card copy. Globbed from the (git-ignored) apk/assets input; if
# that dir is absent the list is empty and the loader just falls back to the card.
file(GLOB SHADER_SRCS
  "${CMAKE_SOURCE_DIR}/apk/assets/*.vert"
  "${CMAKE_SOURCE_DIR}/apk/assets/*.frag"
  "${CMAKE_SOURCE_DIR}/apk/assets/*.glsl")
set(SHADER_VPK_FILES "")
foreach(f ${SHADER_SRCS})
  get_filename_component(fn ${f} NAME)
  list(APPEND SHADER_VPK_FILES ${f} "shaders/${fn}")
endforeach()

# Bundle font metrics (.txi) into the VPK at app0:fonts/. Aspyr shipped the loose
# high-res font TGAs in override/ WITHOUT their companion .txi glyph metrics (the
# real metrics are embedded in the swpc_tex_gui.erf TPCs). Without them the game
# frees fontInfo -> null -> no GUI text. We extract the TXI text from those TPCs and
# ship them here; the AurResGet .txi-fallback hook + SDL_RWFromFile hook serve them.
#
# iosdialog.otf rides along in the same directory: libandroid_port loads it with
# FT_New_Face by bare filename, does NOT check the FT_Error, and null-derefs when
# it fails -- which is what crashed the first in-game dialogue (log96). The
# FT_New_Face shim in dynlib.c tries app0:fonts/ before the card.
file(GLOB FONT_TXI_SRCS
  "${CMAKE_SOURCE_DIR}/apk/assets/*.txi"
  "${CMAKE_SOURCE_DIR}/apk/assets/*.otf"
  "${CMAKE_SOURCE_DIR}/apk/assets/*.ttf")
set(FONT_VPK_FILES "")
foreach(f ${FONT_TXI_SRCS})
  get_filename_component(fn ${f} NAME)
  list(APPEND FONT_VPK_FILES ${f} "fonts/${fn}")
endforeach()

vita_create_self(eboot.bin KOTOR UNSAFE)
vita_create_vpk(KOTOR.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
       ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
       ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
       ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
       ${SHADER_VPK_FILES}
       ${FONT_VPK_FILES}
)

# Keep the last few builds (vpk + matching source tarball) under backups/ so a
# bad change can be undone without a rebuild. Never fails the build.
add_custom_command(TARGET KOTOR.vpk-vpk POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E env KEEP_BUILDS=4
          bash ${CMAKE_SOURCE_DIR}/tools/snapshot.sh || ${CMAKE_COMMAND} -E true
  COMMENT "Snapshotting build to backups/"
)
