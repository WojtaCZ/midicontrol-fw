set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")

cmake_minimum_required(VERSION 3.10.0)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

set(DEVICE STM32G431xx)
add_compile_definitions(DEVICE=${DEVICE})

# Application region the bootloader hands control to (see STM32G431CBTx_FLASH.ld).
add_compile_definitions(BOOT_APP_BASE=0x08008000 BOOT_APP_END=0x0801F800 BOOT_PAGE_SIZE=2048)

set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/STM32G431CBTx_BOOT.ld)

include(${REPO_ROOT}/lib/bootloader/cmake/bootloader.cmake)

set(SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/usb_descriptors.c

	# Shared engine + the FW_UPDATE protocol receiver (writes internal flash).
	${BOOTLIB_COMMON_SOURCES}

	# Protocol codec/parser.
	${REPO_ROOT}/lib/protocol/src/crc8.cpp
	${REPO_ROOT}/lib/protocol/src/codec.cpp
	${REPO_ROOT}/lib/protocol/src/parser.cpp

	# TinyUSB (CDC only).
	${REPO_ROOT}/lib/tinyusb/src/tusb.c
	${REPO_ROOT}/lib/tinyusb/src/common/tusb_fifo.c
	${REPO_ROOT}/lib/tinyusb/src/device/usbd.c
	${REPO_ROOT}/lib/tinyusb/src/device/usbd_control.c
	${REPO_ROOT}/lib/tinyusb/src/class/cdc/cdc_device.c
	${REPO_ROOT}/lib/tinyusb/src/portable/st/stm32_fsdev/dcd_stm32_fsdev.c

	${REPO_ROOT}/startup/startup_stm32g431xx.s
)

project(base-bootloader LANGUAGES CXX C ASM VERSION 1.0.0)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
endif()

add_executable(${PROJECT_NAME}.elf ${SOURCES})
set_target_properties(${PROJECT_NAME}.elf PROPERTIES CXX_STANDARD 23 C_STANDARD 11)

set(OTHER_CPP "-fno-use-cxa-atexit -Wno-old-style-cast -Wno-volatile -Wno-register -fno-exceptions -fno-threadsafe-statics" CACHE INTERNAL "")
set(OPTIMIZATIONS "-Os -funsigned-char -funsigned-bitfields -ffunction-sections -fdata-sections -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables" CACHE INTERNAL "")
# The toolchain file already supplies --specs=nano.specs/--specs=nosys.specs in
# the linker flags; do not repeat them here (it triggers a duplicate-spec error).
set(LIBS "" CACHE INTERNAL "")

set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${OPTIMIZATIONS} ${LIBS} -D${DEVICE}" CACHE INTERNAL "")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OPTIMIZATIONS} ${LIBS} ${OTHER_CPP} -D${DEVICE}" CACHE INTERNAL "")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -D${DEVICE}" CACHE INTERNAL "")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -D${DEVICE} -lm -ffreestanding -Wl,--gc-sections -Wl,--print-memory-usage" CACHE INTERNAL "")

target_include_directories(${PROJECT_NAME}.elf PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/inc        # bootloader's CDC-only tusb_config.h (must precede app inc)
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${BOOTLIB_INC}
	${REPO_ROOT}/lib
	${REPO_ROOT}/lib/CMSIS_5/CMSIS/Core/Include
	${REPO_ROOT}/lib/cmsis-device-g4/Include
	${REPO_ROOT}/lib/tinyusb/src
	${REPO_ROOT}/lib/stmcpp
	${REPO_ROOT}/lib/protocol/inc
)

include(${REPO_ROOT}/cmake/linker.cmake)
include(${REPO_ROOT}/cmake/stm32g4.cmake)

set(HEX_FILE ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.hex)
set(BIN_FILE ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.bin)
add_custom_command(
	POST_BUILD
	TARGET ${PROJECT_NAME}.elf
	COMMAND ${CMAKE_OBJCOPY} -Oihex $<TARGET_FILE:${PROJECT_NAME}.elf> ${HEX_FILE}
	COMMAND ${CMAKE_OBJCOPY} -Obinary $<TARGET_FILE:${PROJECT_NAME}.elf> ${BIN_FILE}
	COMMENT "Building ${HEX_FILE} and ${BIN_FILE}"
)
