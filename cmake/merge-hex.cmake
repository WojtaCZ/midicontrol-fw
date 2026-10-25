# Merges Intel HEX files into one (bootloader + application) for a single flash over SWD.
# Usage: cmake -DOUTPUT=<out.hex> -DINPUT1=<a.hex> -DINPUT2=<b.hex> [-DINPUT3=...] -P merge-hex.cmake
# The EOF record (type 01) is dropped from the inputs and a single one is appended at the end.
set(INPUTS "")
foreach(_i RANGE 1 4)
	if(DEFINED INPUT${_i})
		list(APPEND INPUTS ${INPUT${_i}})
	endif()
endforeach()
if(NOT OUTPUT OR NOT INPUTS)
	message(FATAL_ERROR "merge-hex: chybí OUTPUT nebo INPUT1..")
endif()

set(_merged "")
foreach(_in IN LISTS INPUTS)
	if(NOT EXISTS ${_in})
		message(FATAL_ERROR "merge-hex: soubor ${_in} neexistuje")
	endif()
	file(STRINGS ${_in} _lines NO_HEX_CONVERSION) # otherwise CMake would convert the HEX to binary data by itself
	foreach(_line IN LISTS _lines)
		string(STRIP "${_line}" _line)
		if(_line STREQUAL "" OR _line MATCHES "^:00000001")
			continue()
		endif()
		string(APPEND _merged "${_line}\n")
	endforeach()
endforeach()
string(APPEND _merged ":00000001FF\n")
file(WRITE ${OUTPUT} "${_merged}")
