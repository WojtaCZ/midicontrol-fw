# Screen export tool: the base's GUI code built for the host with stubbed hardware.
# Run tools/screens/export.sh instead of using this directly.
cmake_minimum_required(VERSION 3.20)
project(midicontrol-screens LANGUAGES CXX)

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(screens
	main.cpp
	host.cpp
	${FW}/src/menu.cpp
	${FW}/lib/ui/src/confirm_screen.cpp
	${FW}/lib/ui/src/list.cpp
	${FW}/lib/ui/src/menu_screen.cpp
	${FW}/lib/ui/src/navigator.cpp
	${FW}/lib/ui/src/number_screen.cpp
	${FW}/lib/ui/src/paragraph_screen.cpp
	${FW}/lib/ui/src/progress.cpp
	${FW}/lib/ui/src/splash_screen.cpp
	${FW}/lib/gfx/src/font_7x10.cpp
	${FW}/lib/gfx/src/font_11x18.cpp
	${FW}/lib/gfx/src/font_16x26.cpp
	${FW}/lib/gfx/src/icons.cpp)

# stubs/ first: it replaces the hardware headers (oled, base, bluetooth, git, systick, scheduler)
target_include_directories(screens PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/stubs
	${CMAKE_CURRENT_SOURCE_DIR}
	${FW}/inc
	${FW}/lib
	${FW}/lib/ui/inc
	${FW}/lib/gfx/inc
	${FW}/lib/protocol/inc)
set_target_properties(screens PROPERTIES CXX_STANDARD 23)
target_compile_options(screens PRIVATE -Wall -Wno-unused-function -Wno-unused-variable)
