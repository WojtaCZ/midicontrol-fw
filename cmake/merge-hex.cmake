# Sloučení Intel HEX souborů do jednoho (bootloader + aplikace) pro jedno nahrání přes SWD.
# Použití: cmake -DOUTPUT=<out.hex> -DINPUT1=<a.hex> -DINPUT2=<b.hex> [-DINPUT3=...] -P merge-hex.cmake
# Ze vstupů se vynechá záznam EOF (typ 01) a na konec se přidá jediný.
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
	file(STRINGS ${_in} _lines NO_HEX_CONVERSION) # jinak CMake HEX sám převede na binární data
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
