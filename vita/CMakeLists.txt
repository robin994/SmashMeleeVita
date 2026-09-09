cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Vita toolchain")
endif()
project(SmashMeleeVita C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
set(CMAKE_C_STANDARD 11)
add_library(melee_portable OBJECT ../src/sysdolphin/baselib/random.c
  ../extern/dolphin/src/dolphin/pad/PadClamp.c)
target_include_directories(melee_portable PRIVATE ../src ../extern/dolphin/include)
target_compile_options(melee_portable PRIVATE -O2 -fno-short-enums)
add_library(melee_assets STATIC hsd_data.c hsd_anim.c hsd_scene.c hsd_matanim.c hsd_native.c gx_texture.c asset_file.c)
target_include_directories(melee_assets PRIVATE ../src ../extern/dolphin/include)
target_compile_options(melee_assets PRIVATE -Wall -Wextra -Werror -O2 -fno-short-enums)
add_library(melee_hsd_runtime STATIC
  hsd_runtime_platform.c hsd_runtime_probe.c gx_capture_vita.c
  ../src/sysdolphin/baselib/memory.c
  ../src/sysdolphin/baselib/objalloc.c
  ../src/sysdolphin/baselib/class.c
  ../src/sysdolphin/baselib/archive.c
  ../src/sysdolphin/baselib/hash.c
  ../src/sysdolphin/baselib/object.c
  ../src/sysdolphin/baselib/initialize.c
  ../src/sysdolphin/baselib/video.c
  ../src/sysdolphin/baselib/state.c
  ../src/sysdolphin/baselib/tev.c
  ../src/sysdolphin/baselib/lobj.c
  ../src/sysdolphin/baselib/list.c
  ../src/sysdolphin/baselib/controller.c
  ../src/sysdolphin/baselib/rumble.c
  ../src/sysdolphin/baselib/aobj.c
  ../src/sysdolphin/baselib/fobj.c
  ../src/sysdolphin/baselib/mtx.c
  ../src/sysdolphin/baselib/robj.c
  ../src/sysdolphin/baselib/shadow.c
  ../src/sysdolphin/baselib/displayfunc.c
  ../extern/dolphin/src/dolphin/gx/GXFrameBuf.c
  ../src/sysdolphin/baselib/id.c
  ../src/sysdolphin/baselib/jobj.c
  ../src/sysdolphin/baselib/dobj.c
  ../src/sysdolphin/baselib/mobj.c
  ../src/sysdolphin/baselib/pobj.c
  ../src/sysdolphin/baselib/tobj.c)
target_include_directories(melee_hsd_runtime PRIVATE ../src ../extern/dolphin/include)
target_compile_definitions(melee_hsd_runtime PRIVATE MELEE_VITA_PLATFORM=1 MELEE_VITA_HSD_LOAD_ONLY=1 MELEE_VITA_HSD_GX_CAPTURE=1 MELEE_VITA_BIG_ENDIAN_ARCHIVE=1 MELEE_VITA_INIT_PROBE=1)
# GameCube HSD descriptors store GX enum fields as 32-bit words. ARM EABI's
# default variable-size enum layout corrupts those structures, so only the
# upstream HSD runtime island is compiled with the original 32-bit enum ABI.
# Its boundary to the Vita/viewer code is pointers plus fixed-width counters.
target_compile_options(melee_hsd_runtime PRIVATE -O2 -ffunction-sections -fdata-sections -fno-short-enums)

add_library(melee_game_boot STATIC
  gc_runtime_vita.c gx_boot_vita.c audio_boot_vita.c ax_vita.c gmopening_state_vita.c
  ../src/melee/gm/gmmain.c
  ../src/melee/gm/gm_1A3F.c
  ../src/melee/gm/gmboot.c
  ../src/melee/db/dbinit.c
  ../src/melee/lb/lb_0195.c
  ../src/melee/lb/lbmemory.c
  ../src/melee/lb/lbheap.c
  ../src/melee/lb/lbdvd.c
  ../src/melee/lb/lbarq.c
  ../src/melee/lb/lbcardnew.c
  ../src/melee/lb/lbcardgame.c
  ../src/melee/lb/lbsnap.c
  ../src/melee/lb/lbtime.c
  ../src/melee/lb/lbmthp.c
  ../src/melee/gm/gmmain_lib.c
  ../src/melee/lb/lbfile.c
  ../src/melee/lb/lbarchive.c
  ../src/melee/lb/lbaudio_ax.c
  ../src/sysdolphin/baselib/axdriver.c
  ../src/sysdolphin/baselib/synth.c
  ../src/sysdolphin/baselib/sislib.c)
target_include_directories(melee_game_boot PRIVATE . ../src ../extern/dolphin/include)
target_compile_definitions(melee_game_boot PRIVATE MELEE_VITA_PLATFORM=1 MELEE_VITA_BOOT_PROBE=1 MELEE_VITA_SYNC_DVD=1 MELEE_VITA_AUDIO_BOOT_PROBE=1)
target_compile_options(melee_game_boot PRIVATE -O2 -ffunction-sections -fdata-sections -fno-short-enums)

add_executable(melee_vita main.c pad_vita.c asset_viewer.c gx_replay_vita.c $<TARGET_OBJECTS:melee_portable>)
target_include_directories(melee_vita PRIVATE ../src ../extern/dolphin/include)
target_compile_definitions(melee_vita PRIVATE MELEE_VITA_PLATFORM=1 MELEE_VITA_INIT_PROBE=1)
target_compile_options(melee_vita PRIVATE -Wall -Wextra -Werror -O2 -fno-short-enums)
target_link_options(melee_vita PRIVATE "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/melee_vita.map" -Wl,--gc-sections -Wl,--wrap=__assert)
target_link_libraries(melee_vita melee_assets melee_game_boot melee_hsd_runtime vita2d SceGxm_stub SceDisplay_stub SceCtrl_stub
  SceSysmodule_stub ScePgf_stub SceCommonDialog_stub SceAppMgr_stub SceTouch_stub m)
vita_create_self(eboot.bin melee_vita UNSAFE)
vita_create_vpk(SmashMeleeVita-assets.vpk SMEL00001 eboot.bin
  VERSION 00.17 NAME "Melee Vita Runtime")
