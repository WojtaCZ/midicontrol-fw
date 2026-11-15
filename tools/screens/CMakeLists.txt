# Screen export tool: the base's GUI code built for the host with stubbed hardware.
# Run tools/screens/export.sh instead of using this directly.
cmake_minimum_required(VERSION 3.20)
project(midicontrol-screens LANGUAGES CXX)

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# lib/ui, lib/gfx, lib/protocol are git submodules; in a fresh worktree they are empty. FW_LIB_DIR points to a
# checkout whose lib/ holds them (read only use; the midicontrol-fw main checkout has local lib/ui, lib/menu changes).
if(NOT FW_LIB_DIR)
	if(DEFINED ENV{FW_LIB_DIR})
		set(FW_LIB_DIR $ENV{FW_LIB_DIR})
	else()
		set(FW_LIB_DIR ${FW}/lib)
	endif()
endif()
if(NOT EXISTS ${FW_LIB_DIR}/ui/src/list.cpp)
	message(FATAL_ERROR "lib/ui is empty: set FW_LIB_DIR (env or -D) to a lib/ directory with the submodules")
endif()
message(STATUS "FW_LIB_DIR = ${FW_LIB_DIR}")

set(GUI_SOURCES
	host.cpp
	${FW}/src/logic/ble_status.cpp
	${FW}/src/logic/now_playing.cpp
	${FW_LIB_DIR}/ui/src/confirm_screen.cpp
	${FW_LIB_DIR}/ui/src/list.cpp
	${FW_LIB_DIR}/ui/src/menu_screen.cpp
	${FW_LIB_DIR}/ui/src/navigator.cpp
	${FW_LIB_DIR}/ui/src/number_screen.cpp
	${FW_LIB_DIR}/ui/src/paragraph_screen.cpp
	${FW_LIB_DIR}/ui/src/progress.cpp
	${FW_LIB_DIR}/ui/src/splash_screen.cpp
	${FW_LIB_DIR}/gfx/src/font_7x10.cpp
	${FW_LIB_DIR}/gfx/src/font_11x18.cpp
	${FW_LIB_DIR}/gfx/src/font_16x26.cpp
	${FW_LIB_DIR}/gfx/src/icons.cpp)

add_executable(screens main.cpp ${GUI_SOURCES} ${FW}/src/menu.cpp)
# sim.cpp includes src/menu.cpp itself (one translation unit, to reach the screen objects)
add_executable(sim sim.cpp ${GUI_SOURCES})

# stubs/ first: it replaces the hardware headers (oled, base, bluetooth, git, systick, scheduler)
foreach(t screens sim)
target_include_directories(${t} PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/stubs
	${CMAKE_CURRENT_SOURCE_DIR}
	${FW}/inc
	${FW_LIB_DIR}
	${FW_LIB_DIR}/ui/inc
	${FW_LIB_DIR}/gfx/inc
	${FW_LIB_DIR}/protocol/inc)
set_target_properties(${t} PROPERTIES CXX_STANDARD 23)
target_compile_options(${t} PRIVATE -Wall -Wno-unused-function -Wno-unused-variable)
endforeach()
