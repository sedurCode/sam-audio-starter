# Toolchain file: ADI CCES arm-none-eabi GCC for the ADSP-SC589 Cortex-A5 (bare metal).
#
# Locating the compiler (first match wins):
#   1. -DCCES_ROOT=<path to CrossCore Embedded Studio install>
#   2. the CCES_ROOT environment variable
#   3. arm-none-eabi-gcc already on PATH (e.g. after sourcing env.sh)
#
# Example: cmake --preset release -DCCES_ROOT="C:/Analog Devices/CrossCore Embedded Studio 2.8.3"

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT CCES_ROOT AND DEFINED ENV{CCES_ROOT})
    set(CCES_ROOT "$ENV{CCES_ROOT}")
endif()

if(CCES_ROOT)
    file(TO_CMAKE_PATH "${CCES_ROOT}" CCES_ROOT)
    set(_cces_arm_bin "${CCES_ROOT}/ARM/arm-none-eabi/bin")
    # cc21k / easm21k / elfloader live in the CCES root itself.
    set(ENV{PATH} "${CCES_ROOT};${_cces_arm_bin};$ENV{PATH}")
    set(_hint HINTS "${_cces_arm_bin}")
endif()

find_program(CMAKE_C_COMPILER   arm-none-eabi-gcc ${_hint} REQUIRED)
find_program(CMAKE_CXX_COMPILER arm-none-eabi-g++ ${_hint} REQUIRED)
find_program(CMAKE_ASM_COMPILER arm-none-eabi-gcc ${_hint} REQUIRED)
find_program(CMAKE_AR           arm-none-eabi-ar  ${_hint})
find_program(CMAKE_OBJCOPY      arm-none-eabi-objcopy ${_hint})

# Processor selection must be on every compile (including CMake's compiler check).
set(SAM_PROC "ADSP-SC589" CACHE STRING "ADI processor passed to -mproc")
set(SAM_SI_REVISION "1.0" CACHE STRING "Silicon revision passed to -msi-revision")
set(_proc_flags "-mproc=${SAM_PROC} -msi-revision=${SAM_SI_REVISION}")
set(CMAKE_C_FLAGS_INIT   "${_proc_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_proc_flags}")
set(CMAKE_ASM_FLAGS_INIT "${_proc_flags}")

# The compiler check only needs to produce an object file (no linker script yet).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_EXECUTABLE_SUFFIX ".exe")
