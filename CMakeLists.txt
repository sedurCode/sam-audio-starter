cmake_minimum_required(VERSION 3.20)

# CMake port of build/makefile for the SHARC Audio Module (SAM, ADZS-SC589-MINI).
#
#   ARM Cortex-A5 (core0)  -> SAM-Audio-Starter-ARM.exe    (arm-none-eabi-gcc, native CMake target)
#   SHARC+ core1 (SHARC0)  -> SAM-Audio-Starter-SHARC0.dxe (cc21k, custom commands)
#   SHARC+ core2 (SHARC1)  -> SAM-Audio-Starter-SHARC1.dxe (cc21k, custom commands)
#   elfloader              -> SAM-Audio-Starter.ldr (app) and -Full.ldr (boot0+boot1+app)
#
# Configure with the ARM toolchain file (see CMakePresets.json and README-CMake.md):
#   cmake --preset release -DCCES_ROOT="C:/Analog Devices/CrossCore Embedded Studio 2.8.3"
#   cmake --build --preset release

if(NOT CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR
        "Use the CCES toolchain file:\n"
        "  cmake -S . -B out/release -G Ninja -DCMAKE_BUILD_TYPE=Release "
        "-DCMAKE_TOOLCHAIN_FILE=cmake/cces-arm-toolchain.cmake\n"
        "or simply: cmake --preset release")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Debug or Release" FORCE)
endif()

project(SAM-Audio-Starter LANGUAGES C CXX ASM)

set(PROJECT_TAG "SAM-Audio-Starter")
set(PROC        "${SAM_PROC}")
set(SI_REVISION "${SAM_SI_REVISION}")
set(SAM_ROOT    "${CMAKE_SOURCE_DIR}")
set(SAM_ASSETS  "${SAM_ROOT}/build")   # linker scripts, init code, bootloader LDRs

# Mirror the makefile: Debug = "-g" only, Release = "-O1".
set(SAM_ARM_OPTIMIZE_DEBUG     ""    CACHE STRING "ARM optimisation flags, Debug")
set(SAM_ARM_OPTIMIZE_RELEASE   "-O1" CACHE STRING "ARM optimisation flags, Release")
set(SAM_SHARC_OPTIMIZE_RELEASE "-O1 -Ov100 -Oa" CACHE STRING "SHARC optimisation flags, Release")
option(SAM_RELEASE_DEBUG_INFO "Keep -g in Release builds" OFF)

foreach(_lang C CXX ASM)
    set(CMAKE_${_lang}_FLAGS_DEBUG   "-g ${SAM_ARM_OPTIMIZE_DEBUG}")
    set(CMAKE_${_lang}_FLAGS_RELEASE "${SAM_ARM_OPTIMIZE_RELEASE}")
    if(SAM_RELEASE_DEBUG_INFO)
        set(CMAKE_${_lang}_FLAGS_RELEASE "-g ${CMAKE_${_lang}_FLAGS_RELEASE}")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# ARM core0
# ---------------------------------------------------------------------------
set(ARM_SRC_DIRS
    ALL/src
    ALL/src/sae
    ARM
    ARM/src
    ARM/src/adi-drivers/rsi
    ARM/src/simple-drivers
    ARM/src/simple-services/adau1761
    ARM/src/simple-services/syslog
    ARM/src/simple-services/buffer-track
    ARM/src/simple-services/a2b-xml
    ARM/src/simple-services/adi-a2b-cmdlist
    ARM/src/simple-services/FreeRTOS-cpu-load
    ARM/src/simple-services/a2b-to-sport-cfg
    ARM/src/adi-drivers/ethernet/common
    ARM/src/adi-drivers/ethernet/gemac
    ARM/src/simple-services/adi-osal-minimal
    ARM/src/simple-services/uac2-cdc-soundcard
    ARM/src/simple-services/fs-dev
    ARM/src/simple-services/rtp-stream
    ARM/src/simple-services/vban-stream
    ARM/src/simple-services/wav-file
    ARM/src/simple-services/telnet
    ARM/src/oss-services/lwip/core
    ARM/src/oss-services/lwip/core/ipv4
    ARM/src/oss-services/lwip/netif
    ARM/src/oss-services/lwip/api
    ARM/src/oss-services/lwip/arch
    ARM/src/oss-services/lwip/apps/mdns
    ARM/src/oss-services/umm_malloc
    ARM/src/oss-services/shell
    ARM/src/oss-services/crc
    ARM/src/oss-services/xmodem
    ARM/src/oss-services/yxml
    ARM/src/oss-services/pa-ringbuffer
    ARM/src/oss-services/spiffs
    ARM/src/oss-services/libtelnet
    ARM/src/oss-services/kilo
    ARM/src/oss-services/getline
    ARM/src/oss-services/getopt
    ARM/src/oss-services/printf
    ARM/src/oss-services/c-timestamp
    ARM/src/oss-services/FatFs
    ARM/src/oss-services/FreeRTOS-ARM
    ARM/src/oss-services/FreeRTOS-ARM/portable/MemMang
    ARM/src/oss-services/FreeRTOS-ARM/portable/CCES/ARM_CA5
    ARM/src/oss-services/FreeRTOS-ARM/portable/GCC/ARM_CA9)

# NUPC convolution engine (C++11, see docs/NUPC-THREADING-PLAN.md) and the Ne10 FFT subset it needs.
option(SAM_NUPC "Build the NUPC convolution engine into the ARM application" ON)
set(SAM_NUPC_ARENA_MB 16 CACHE STRING "Size of the NUPC memory arena in DDR (MB)")
if(SAM_NUPC)
    list(APPEND ARM_SRC_DIRS ARM/src/nupc ARM/src/oss-services/ne10)
endif()

set(ARM_EXTRA_INCLUDES
    ALL/include
    ARM/include
    ARM/src/adi-drivers/ethernet/include
    ARM/src/oss-services/lwip
    ARM/src/oss-services/lwip/include
    ARM/src/oss-services/FreeRTOS-ARM/include)

# Same rule as the makefile: only *.c and *.S directly inside each listed directory (plus *.cpp, used by NUPC/Ne10).
set(ARM_SOURCES "")
foreach(_dir ${ARM_SRC_DIRS})
    file(GLOB _found CONFIGURE_DEPENDS
        "${SAM_ROOT}/${_dir}/*.c" "${SAM_ROOT}/${_dir}/*.S" "${SAM_ROOT}/${_dir}/*.cpp")
    list(APPEND ARM_SOURCES ${_found})
endforeach()

add_executable(sam_arm ${ARM_SOURCES})
set_target_properties(sam_arm PROPERTIES
    OUTPUT_NAME "${PROJECT_TAG}-ARM"
    SUFFIX ".exe")

foreach(_dir ${ARM_SRC_DIRS} ${ARM_EXTRA_INCLUDES})
    target_include_directories(sam_arm PRIVATE "${SAM_ROOT}/${_dir}")
endforeach()

# __ADI_FREERTOS : define ONLY when using the original ADI OSAL (not the OSS FreeRTOS port).
# SHARC_AUDIO_ENABLE : enable SHARC core audio processing.
target_compile_definitions(sam_arm PRIVATE
    CORE0 FREE_RTOS SAE_IPC __SAM_V1__ FEATURE_CPU_LOAD USB_CDC_STDIO SHARC_AUDIO_ENABLE)
# GCC 4.8 (CCES 2.8.x) defaults to gnu90; the starter needs C99+. GCC 10 (CCES 2.11+) defaults to gnu17.
if(CMAKE_C_COMPILER_VERSION VERSION_LESS 5)
    target_compile_options(sam_arm PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu99>)
endif()
target_compile_options(sam_arm PRIVATE
    -Wall -Wno-unused-but-set-variable -Wno-unused-function
    -gdwarf-2 -ffunction-sections -fdata-sections
    -madi-threads)

if(SAM_NUPC)
    # BUFFER_SIZE_SAMPLES must equal SYSTEM_BLOCK_SIZE (ARM/src/context.h).
    # NE10_* select the NEON-intrinsics float32 FFT; NE10_DSP_RFFT_SCALING makes c2r apply 1/N, which
    # nupc/Convolution.h relies on. NE10_USE_NUPC_ARENA routes Ne10's allocations to the NUPC arena.
    math(EXPR _arena_bytes "${SAM_NUPC_ARENA_MB} * 1024 * 1024")
    target_compile_definitions(sam_arm PRIVATE
        SAM_NUPC NUPC_BARE_METAL BUFFER_SIZE_SAMPLES=32 NUPC_ARENA_BYTES=${_arena_bytes}u
        NE10_ENABLE_DSP NE10_UNROLL_LEVEL=1 NE10_DSP_RFFT_SCALING NE10_USE_NUPC_ARENA)
    # No exceptions/RTTI/thread-safe statics: bare-metal C++ without OS support code.
    target_compile_options(sam_arm PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++11>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
endif()

if(SAM_NUPC)
    # Same script, but the 16 KB supervisor/interrupt stack lives in DDR (L2 is full).
    set(ARM_LD_SCRIPT "${SAM_ASSETS}/ARM-${PROJECT_TAG}-nupc.ld")
else()
    set(ARM_LD_SCRIPT "${SAM_ASSETS}/ARM-${PROJECT_TAG}.ld")
endif()
set(ARM_STATIC_LIB "${SAM_ROOT}/ARM/lib/cld_sc58x_audio_2_0_w_cdc_lib_Core0.a")
set_property(TARGET sam_arm APPEND PROPERTY LINK_DEPENDS "${ARM_LD_SCRIPT}" "${ARM_STATIC_LIB}")

# _dev_open/_dev_close wrapping is required by the fs-dev layer.
# To debug the heap also add: -Wl,--wrap=_malloc_r -Wl,--wrap=_calloc_r -Wl,--wrap=_realloc_r -Wl,--wrap=_free_r
target_link_options(sam_arm PRIVATE
    "-T${ARM_LD_SCRIPT}"
    -Wl,--wrap=_dev_open -Wl,--wrap=_dev_close
    "-Wl,-Map=${CMAKE_BINARY_DIR}/${PROJECT_TAG}-ARM.map"
    -Wl,--gc-sections
    -madi-threads -mdebug-libs)
# Library order matters for GNU ld: objects first, then the CDC library, then libm.
target_link_libraries(sam_arm PRIVATE "${ARM_STATIC_LIB}" m)

# ---------------------------------------------------------------------------
# SHARC+ cores (cc21k / easm21k have no CMake language support -> custom commands)
# ---------------------------------------------------------------------------
find_program(CC21K     cc21k     REQUIRED)
find_program(EASM21K   easm21k   REQUIRED)
find_program(ELFLOADER elfloader REQUIRED)

# The ADI SHARC tools locate their sub-tools (SHARC/etc/compiler_ba.exe ...) via PATH, and the
# PATH set by the toolchain file only exists at configure time, so re-establish it for every
# SHARC/loader command at build time.
get_filename_component(CCES_TOOLS_DIR "${CC21K}" DIRECTORY)
if(CMAKE_HOST_WIN32)
    set(_sep ";")
else()
    set(_sep ":")
endif()
# PATH itself contains ';' on Windows, so escape it or CMake splits it into list elements.
set(_tool_path "${CCES_TOOLS_DIR}${_sep}$ENV{PATH}")
string(REPLACE ";" "\\;" _tool_path "${_tool_path}")
set(SAM_TOOL_ENV ${CMAKE_COMMAND} -E env "PATH=${_tool_path}")

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    separate_arguments(SHARC_OPT_FLAGS NATIVE_COMMAND "${SAM_SHARC_OPTIMIZE_RELEASE}")
    if(SAM_RELEASE_DEBUG_INFO)
        list(PREPEND SHARC_OPT_FLAGS -g)
    endif()
else()
    set(SHARC_OPT_FLAGS -g)
endif()

# sharc_core(<name> <core-define> <src-dir>...)
#   name        SHARC0 / SHARC1 (directory, ldf and output names)
#   core-define CORE1 / CORE2 (what the ADI sources expect)
function(sharc_core NAME CORE_DEFINE)
    set(srcdirs ${ARGN})
    set(objdir "${CMAKE_BINARY_DIR}/${NAME}")
    set(incs "-I${SAM_ROOT}/ALL/include" "-I${SAM_ROOT}/${NAME}/include")
    foreach(d ${srcdirs})
        list(APPEND incs "-I${SAM_ROOT}/${d}")
    endforeach()

    set(common -proc ${PROC} -si-revision ${SI_REVISION} -D${CORE_DEFINE} -swc -char-size-8)
    set(cflags ${SHARC_OPT_FLAGS} ${common} ${incs}
        -structs-do-not-overlap -no-const-strings -no-multiline -warn-protos -double-size-32)
    # easm21k only gets -g (never the optimiser flags), as in the makefile
    if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT SAM_RELEASE_DEBUG_INFO)
        set(aflags ${common})
    else()
        set(aflags -g ${common})
    endif()

    set(objs "")
    foreach(d ${srcdirs})
        file(GLOB csrc CONFIGURE_DEPENDS "${SAM_ROOT}/${d}/*.c")
        file(GLOB ssrc CONFIGURE_DEPENDS "${SAM_ROOT}/${d}/*.s")
        foreach(src ${csrc} ${ssrc})
            file(RELATIVE_PATH rel "${SAM_ROOT}" "${src}")
            set(obj "${objdir}/${rel}.doj")
            get_filename_component(objsubdir "${obj}" DIRECTORY)
            get_filename_component(ext "${src}" EXT)
            if(ext STREQUAL ".c")
                add_custom_command(OUTPUT "${obj}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${objsubdir}"
                    COMMAND ${SAM_TOOL_ENV} "${CC21K}" ${cflags} "-I${SAM_ROOT}/${d}"
                            -gnu-style-dependencies -MD -Mo "${obj}.d" -o "${obj}" -c "${src}"
                    DEPENDS "${src}"
                    DEPFILE "${obj}.d"
                    COMMENT "cc21k ${rel}"
                    VERBATIM)
            else()
                add_custom_command(OUTPUT "${obj}"
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${objsubdir}"
                    COMMAND ${SAM_TOOL_ENV} "${EASM21K}" ${aflags}
                            -gnu-style-dependencies -MM -Mo "${obj}.d" -o "${obj}" "${src}"
                    DEPENDS "${src}"
                    DEPFILE "${obj}.d"
                    COMMENT "easm21k ${rel}"
                    VERBATIM)
            endif()
            list(APPEND objs "${obj}")
        endforeach()
    endforeach()

    set(ldf "${SAM_ASSETS}/${NAME}-${PROJECT_TAG}.ldf")
    set(dxe "${CMAKE_BINARY_DIR}/${PROJECT_TAG}-${NAME}.dxe")

    # SHARC0 passes "-flags-link -e -no-mem", SHARC1 "-no-mem ... -flags-link -e"; equivalent.
    add_custom_command(OUTPUT "${dxe}"
        COMMAND ${SAM_TOOL_ENV} "${CC21K}" -proc ${PROC} -si-revision ${SI_REVISION}
                "-T${ldf}" -flags-link -e -no-mem
                -map "${CMAKE_BINARY_DIR}/linker_log_${NAME}.xml"
                -flags-link -ip -flags-link -MD${CORE_DEFINE},-MDOTHERCORE,-MDDEBUG
                -o "${dxe}" ${objs}
        COMMAND ${CMAKE_COMMAND} -E rm -f "${dxe}.doj" "${dxe}.asm"
        DEPENDS ${objs} "${ldf}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Linking ${NAME} -> ${PROJECT_TAG}-${NAME}.dxe"
        VERBATIM)

    string(TOLOWER "sam_${NAME}" tgt)
    add_custom_target(${tgt} DEPENDS "${dxe}")
    set(${NAME}_DXE "${dxe}" PARENT_SCOPE)
endfunction()

sharc_core(SHARC0 CORE1 ALL ALL/src/sae SHARC0 SHARC0/src SHARC0/startup_ldf)
sharc_core(SHARC1 CORE2 ALL ALL/src/sae SHARC1 SHARC1/src SHARC1/startup_ldf)

# ---------------------------------------------------------------------------
# Loader images
# ---------------------------------------------------------------------------
set(ARM_EXE   "$<TARGET_FILE:sam_arm>")
set(INIT_CODE "${SAM_ASSETS}/ezkitSC589_initcode_core0_v10")
set(LDR_APP   "${CMAKE_BINARY_DIR}/${PROJECT_TAG}.ldr")
set(LDR_FULL  "${CMAKE_BINARY_DIR}/${PROJECT_TAG}-Full.ldr")

add_custom_command(OUTPUT "${LDR_APP}"
    COMMAND ${SAM_TOOL_ENV} "${ELFLOADER}" -proc ${PROC}
            -core0=${ARM_EXE} -init "${INIT_CODE}"
            -core1=${SHARC0_DXE} -core2=${SHARC1_DXE}
            -NoFinalTag=${ARM_EXE} -NoFinalTag=${SHARC0_DXE}
            -b SPI -f BINARY -Width 8 -bcode 0x0C -verbose
            -o "${LDR_APP}"
    DEPENDS sam_arm "${SHARC0_DXE}" "${SHARC1_DXE}"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Building App LDR"
    VERBATIM)
add_custom_target(sam_ldr ALL DEPENDS "${LDR_APP}")

# Full flash image: bootloader0 | pad to 64K | bootloader1 | pad to 256K | app.
# Needs a POSIX shell with cat/truncate (Git bash / MSYS2), as the makefile does.
find_program(SAM_SH       sh)
find_program(SAM_CAT      cat)
find_program(SAM_TRUNCATE truncate)
if(SAM_SH AND SAM_CAT AND SAM_TRUNCATE)
    set(BOOT1_OFFSET_SIZE 64K)
    set(APP_OFFSET_SIZE   256K)
    set(_a "${SAM_ASSETS}")
    set(_script
        "cat '${_a}/bare-metal-bootloader0.ldr' '${_a}/FF_Pad_256KB.ldr' > '${LDR_FULL}'"
        "truncate -s ${BOOT1_OFFSET_SIZE} '${LDR_FULL}'"
        "cat '${_a}/bare-metal-bootloader1.ldr' '${_a}/FF_Pad_256KB.ldr' >> '${LDR_FULL}'"
        "truncate -s ${APP_OFFSET_SIZE} '${LDR_FULL}'"
        "cat '${LDR_APP}' >> '${LDR_FULL}'")
    list(JOIN _script " && " _script)
    add_custom_command(OUTPUT "${LDR_FULL}"
        COMMAND "${SAM_SH}" -c "${_script}"
        DEPENDS "${LDR_APP}"
        COMMENT "Building Full LDR: Boot0 + Boot1 + App"
        VERBATIM)
    add_custom_target(sam_full_ldr DEPENDS "${LDR_FULL}")
else()
    message(STATUS "sh/cat/truncate not found: target sam_full_ldr disabled")
endif()

# CCES 2.8.x (GCC 4.8) has no generic <sru.h>; provide a shim that maps to sruSC589.h.
if(CMAKE_C_COMPILER_VERSION VERSION_LESS 5)
    target_include_directories(sam_arm PRIVATE "${SAM_ROOT}/cmake/compat")
endif()
# Old newlib (CCES 2.8.x) lacks the __htons/__ntohs/__htonl/__ntohl byte-swap macros; force-include a shim.
if(CMAKE_C_COMPILER_VERSION VERSION_LESS 5)
    target_compile_options(sam_arm PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-include> $<$<COMPILE_LANGUAGE:C>:${SAM_ROOT}/cmake/compat/sam_compat_endian.h>)
endif()
