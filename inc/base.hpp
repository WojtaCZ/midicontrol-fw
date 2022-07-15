
#ifndef BASE_H
#define BASE_H

#include <stm32/gpio.h>
#include <string>

namespace Base{
	
    using namespace std;

    static string DEVICE_NAME = "MIDIControl";
    static string DEVICE_TYPE = "BASE";
    static string FW_VERSION = "2.0.0";

    void init(void);
	void dfuCheck();
	void wdtStart();

    namespace CurrentSource{
		
        bool isEnabled();
        void enable();
        void disable();
        void toggle(void);
    };

    namespace Encoder{
        void process();
    };
}

namespace GPIO {

	enum { PORTA = GPIOA, PORTB = GPIOB, PORTC = GPIOC, PORTD = GPIOD, PORTE = GPIOE };

	enum {
		PIN0  = GPIO0,
		PIN1  = GPIO1,
		PIN2  = GPIO2,
		PIN3  = GPIO3,
		PIN4  = GPIO4,
		PIN5  = GPIO5,
		PIN6  = GPIO6,
		PIN7  = GPIO7,
		PIN8  = GPIO8,
		PIN9  = GPIO9,
		PIN10 = GPIO10,
		PIN11 = GPIO11,
		PIN12 = GPIO12,
		PIN13 = GPIO13,
		PIN14 = GPIO14,
		PIN15 = GPIO15
	};
}; 

#endif

