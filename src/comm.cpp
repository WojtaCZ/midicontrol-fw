#include "comm.hpp"
#include "base.hpp"
#include "menu.hpp"
#include "display.hpp"

#include <cstdlib>
#include <tinyusb/src/device/usbd.h>
#include <tinyusb/src/class/cdc/cdc_device.h>


namespace communication {
	
	bool processBinaryCommand(command cmd, direction dir, uint16_t index, uint16_t value, char * params, uint32_t * length){
        switch (cmd){
			case command::BASE_STATUS:
				if(dir == direction::GET){
					return true;
				} else return false;

				break;
			
			case command::BASE_UID:
                if (dir == direction::GET) {
                    memcpy(params, (uint32_t *)(UID_BASE), 12);
                    *length = 12;
                    return true;
                } else return false;

                break;

			case command::BASE_CURRENT:
				if(dir == direction::GET){
					params[0] = static_cast<uint8_t>(base::current::isEnabled());
					*length = 1;
					return true;
				} else {
					base::current::set(static_cast<bool>(value));
					return true;
				}

			case command::DISPLAY_STATUS:
				if(dir == direction::GET){
					params[0] = static_cast<uint8_t>(display::isConnected());
					*length = 1;
					return true;
				} else return false;

			case command::DISPLAY_SONG:
				if(dir == direction::GET){
					uint16_t song = display::getSong();
					params[0] = song & 0xff;
					params[1] = (song >> 8) & 0xff;
					*length = 2;
					return true;
				} else {
					display::setSong(value, 1);
					return true;
				}

			case command::DISPLAY_VERSE:
				if(dir == direction::GET){	
					params[0] = display::getVerse();
					*length = 1;
					return true;
				} else {
					display::setVerse(static_cast<uint8_t>(value), 1);
					return true;
				}

			case command::DISPLAY_LETTER:
				if(dir == direction::GET){
					params[0] = static_cast<uint8_t>(display::getLetter());
					*length = 1;
					return true;
				} else {
					display::setLetter(static_cast<char>(value), 1);
					return true;
				}
			
			case command::DISPLAY_LED:
				if(dir == direction::GET){
					params[0] = static_cast<uint8_t>(display::getLed());
					*length = 1;
					return true;
				} else {
					display::setLed(static_cast<display::ledColor>(value));
					return true;
				}

				break;
			
			default:
			case command::UNKNOWN:
				return false;
				break;


		}

	}
}