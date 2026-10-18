set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_COLOR_MAKEFILE FALSE)
set(CMAKE_COLOR_DIAGNOSTICS TRUE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE "STATIC_LIBRARY")

cmake_minimum_required(VERSION 3.10.0)

# Set the project name, otherwise it is based on the root folder name
#set (PROJECT "yournamehere")

# Set the device type
set(DEVICE STM32G431xx)

add_compile_definitions(DEVICE=${DEVICE})

#set(TOOLCHAIN_PREFIX "C:/Program Files (x86)/GNU Arm Embedded Toolchain/10/")

# Replace with appropriate linker script
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/STM32G431CBTx_FLASH.ld)

# List the source files to be compiled
set(SOURCE_BASE ${CMAKE_SOURCE_DIR}/src)
set(SOURCES
	${SOURCE_BASE}/main.cpp
	${SOURCE_BASE}/base.cpp
	${SOURCE_BASE}/display.cpp
	${SOURCE_BASE}/menu.cpp
	${SOURCE_BASE}/bluetooth.cpp
	${SOURCE_BASE}/led.cpp
	${SOURCE_BASE}/midi.cpp
	${SOURCE_BASE}/oled.cpp
	#${SOURCE_BASE}/oled_fonts.cpp
	${SOURCE_BASE}/usb.cpp
	${SOURCE_BASE}/usb_descriptors.c
	${SOURCE_BASE}/debug.cpp
	${SOURCE_BASE}/settings.cpp
	${SOURCE_BASE}/router.cpp


	${CMAKE_SOURCE_DIR}/lib/gfx/src/icons.cpp
	${CMAKE_SOURCE_DIR}/lib/gfx/src/font_7x10.cpp
	${CMAKE_SOURCE_DIR}/lib/gfx/src/font_11x18.cpp

	${CMAKE_SOURCE_DIR}/lib/ui/src/menu_screen.cpp
	${CMAKE_SOURCE_DIR}/lib/ui/src/splash_screen.cpp
	${CMAKE_SOURCE_DIR}/lib/ui/src/paragraph_screen.cpp
	${CMAKE_SOURCE_DIR}/lib/ui/src/number_screen.cpp
	${CMAKE_SOURCE_DIR}/lib/ui/src/confirm_screen.cpp
	${CMAKE_SOURCE_DIR}/lib/ui/src/navigator.cpp

	${CMAKE_SOURCE_DIR}/lib/protocol/src/crc8.cpp
	${CMAKE_SOURCE_DIR}/lib/protocol/src/codec.cpp
	${CMAKE_SOURCE_DIR}/lib/protocol/src/parser.cpp

	# common
	${CMAKE_SOURCE_DIR}/lib/tinyusb/src/tusb.c
	${CMAKE_SOURCE_DIR}/lib/tinyusb/src/common/tusb_fifo.c
	# device
	${CMAKE_SOURCE_DIR}/lib/tinyusb/src/device/usbd.c
	${CMAKE_SOURCE_DIR}/lib/tinyusb/src/device/usbd_control.c
	${CMAKE_SOURCE_DIR}/lib/tinyusb/src/class/cdc/cdc_device.c
	${CMAKE_SOURCE_DIR}/lib/tinyusb/src/class/midi/midi_device.c
	${CMAKE_SOURCE_DIR}/lib/tinyusb/src/portable/st/stm32_fsdev/dcd_stm32_fsdev.c

	startup/startup_stm32g431xx.s

)



#
# User setup ends here
#



# If the project name is not set, base it on the root folder name
if (NOT DEFINED PROJECT)
	get_filename_component(PROJECT ${CMAKE_CURRENT_SOURCE_DIR} NAME)
	string(REPLACE " " "_" PROJECT ${PROJECT})
endif (NOT DEFINED PROJECT)

project(${PROJECT} LANGUAGES CXX C ASM VERSION 0.1.0)

# Set a default build type if none was specified
set(default_build_type "Debug")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	message(STATUS "Setting build type to '${default_build_type}' as none was specified.")
	set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE
			STRING "Choose the type of build." FORCE)
	# Set the possible values of build type for cmake-gui
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
		"Debug" "Release")
endif()

add_executable(${PROJECT_NAME}.elf ${SOURCES})

set_target_properties(${PROJECT_NAME}.elf PROPERTIES CXX_STANDARD 23)
set_target_properties(${PROJECT_NAME}.elf PROPERTIES C_STANDARD 11)
#set_target_properties(${PROJECT_NAME}.elf PROPERTIES COMPILE_FLAGS "-save-temps") 


set(W_ERRORS "-Werror=switch -Werror=return-type -Werror=stringop-overflow -Werror=parentheses -Wfatal-errors" CACHE INTERNAL "Warning to error promotion")
set(W_IGNORED "-Wno-unused-function " CACHE INTERNAL "Supressed warnings")
set(VALIDATION_OPTS "${W_ERRORS} ${W_IGNORED} -Wall -Wextra -Wundef -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wnull-dereference -Wcast-align -Wvla -Wmissing-format-attribute -Wuninitialized -Winit-self -Wdouble-promotion -Wstrict-aliasing -Wno-unused-parameter -pedantic" CACHE INTERNAL "Sources validation options")
set(OTHER_C "-Werror=int-conversion -Werror=incompatible-pointer-types -Werror=implicit-function-declaration" CACHE INTERNAL "Other C specific options")
set(OTHER_CPP "-fno-use-cxa-atexit -Wint-to-pointer-cast -Wno-old-style-cast -Wno-volatile -Weffc++ -Wno-register -fno-exceptions -fno-threadsafe-statics" CACHE INTERNAL "Other CPP specific options")
set(OPTIMIZATIONS "-funsigned-char -funsigned-bitfields -fdiagnostics-color=always -fstack-protector-strong -finline-small-functions -findirect-inlining -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables" CACHE INTERNAL "Optimizations options")
set(LIBS "" CACHE INTERNAL "which libs to use")

set(CMAKE_C_FLAGS     "${CMAKE_C_FLAGS} ${VALIDATION_OPTS} ${OPTIMIZATIONS} ${LIBS} ${OTHER_C} -D${DEVICE}" CACHE INTERNAL "(Project) C Compiler options")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${VALIDATION_OPTS} ${OPTIMIZATIONS} ${LIBS} ${OTHER_CPP} -D${DEVICE}" CACHE INTERNAL "(Project) C++ Compiler options")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -D${DEVICE}" CACHE INTERNAL "(Project) ASM Compiler options")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -D${DEVICE} -lm -ffreestanding -fdiagnostics-color=always -Wl,--print-memory-usage " CACHE INTERNAL "(Project) Project linker options")
#-Wl,--wrap,printf -Wl,--wrap,snprintf -Wl,--wrap,vsnprintf


target_include_directories(${PROJECT_NAME}.elf PRIVATE 
${SOURCE_BASE}
${CMAKE_SOURCE_DIR}/inc/
${CMAKE_SOURCE_DIR}/lib/
${CMAKE_SOURCE_DIR}/lib/CMSIS_5/CMSIS/Core/Include
${CMAKE_SOURCE_DIR}/lib/cmsis-device-g4/Include
${CMAKE_SOURCE_DIR}/lib/tinyusb/src/
${CMAKE_SOURCE_DIR}/lib/stmcpp
${CMAKE_SOURCE_DIR}/lib/gfx/inc/
${CMAKE_SOURCE_DIR}/lib/ui/inc/
${CMAKE_SOURCE_DIR}/lib/protocol/inc/

)

# Select the device definition
if (DEVICE MATCHES ^STM32H7)
	include(${CMAKE_SOURCE_DIR}/cmake/stm32h7.cmake)
elseif (DEVICE MATCHES ^STM32G4)
	include(${CMAKE_SOURCE_DIR}/cmake/stm32g4.cmake)
elseif (DEVICE MATCHES ^STM32F0)
	include(${CMAKE_SOURCE_DIR}/cmake/stm32f0.cmake)	
elseif (DEVICE MATCHES ^STM32F1)
	include(${CMAKE_SOURCE_DIR}/cmake/stm32f1.cmake)
elseif (DEVICE MATCHES ^STM32F4)
	include(${CMAKE_SOURCE_DIR}/cmake/stm32f4.cmake)
else ()
	message(FATAL_ERROR "Could not find a suitable device cmake definition for the specified device type!")
endif ()

set(HEX_FILE ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.hex)
set(BIN_FILE ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.bin)

add_custom_command(
	POST_BUILD
	TARGET ${PROJECT_NAME}.elf
	COMMAND ${CMAKE_OBJCOPY} -Oihex $<TARGET_FILE:${PROJECT_NAME}.elf> ${HEX_FILE}
	COMMAND ${CMAKE_OBJCOPY} -Obinary $<TARGET_FILE:${PROJECT_NAME}.elf> ${BIN_FILE}
	COMMENT "Building ${HEX_FILE} and ${BIN_FILE}"
)


# Obraz aplikace s trailerem pro bootloader (lib/bootloader/README.md, "Image format"):
#   <name>-update.bin  pro PC updater (FW_UPDATE_*)
#   <name>-update.hex  na 0x08008000, vyplněno 0xFF do 0x0801F800 (SWD) — starý trailer v flash
#                      by jinak bootloader odmítl spustit aplikaci
include(${CMAKE_SOURCE_DIR}/lib/bootloader/cmake/bootloader.cmake)
bootlib_add_image(${PROJECT_NAME}.elf BIN ${BIN_FILE} BOARD base VERSION ${PROJECT_VERSION} APP_BASE 0x08008000 APP_END 0x0801F800)
set(APP_UPDATE_HEX ${CMAKE_BINARY_DIR}/${PROJECT_NAME}-update.hex)

# Bootloader (bootloader/, 0x08000000, 32 KB) má vlastní CMake projekt a vlastní příznaky,
# proto se staví jako ExternalProject se stejným toolchainem. Výsledkem je i sloučený HEX
# (bootloader + aplikace) pro jedno nahrání přes SWD.
option(BUILD_BOOTLOADER "Build bootloader/ alongside the app and produce a merged hex" ON)
if(BUILD_BOOTLOADER)
	include(ExternalProject)
	set(BOOTLOADER_BINARY_DIR ${CMAKE_BINARY_DIR}/bootloader)
	set(BOOTLOADER_HEX ${BOOTLOADER_BINARY_DIR}/base-bootloader.hex)
	ExternalProject_Add(bootloader
		SOURCE_DIR ${CMAKE_SOURCE_DIR}/bootloader
		BINARY_DIR ${BOOTLOADER_BINARY_DIR}
		CMAKE_ARGS
			-DCMAKE_TOOLCHAIN_FILE=${CMAKE_SOURCE_DIR}/cmake/toolchain-arm-none-eabi.cmake
			-DCMAKE_BUILD_TYPE=Release
		BUILD_ALWAYS ON
		INSTALL_COMMAND ""
		BUILD_BYPRODUCTS ${BOOTLOADER_HEX}
	)

	set(MERGED_HEX ${CMAKE_BINARY_DIR}/${PROJECT_NAME}-full.hex)
	add_custom_target(merged_hex ALL
		COMMAND ${CMAKE_COMMAND} -DOUTPUT=${MERGED_HEX} -DINPUT1=${BOOTLOADER_HEX} -DINPUT2=${APP_UPDATE_HEX} -P ${CMAKE_SOURCE_DIR}/cmake/merge-hex.cmake
		BYPRODUCTS ${MERGED_HEX}
		COMMENT "Merging bootloader + app into ${MERGED_HEX}"
	)
	add_dependencies(merged_hex bootloader ${PROJECT_NAME}.elf)
endif()

include(${CMAKE_SOURCE_DIR}/cmake/git.cmake)
