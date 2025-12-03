
#ifndef BASE_H
#define BASE_H

#include <string>
#include <stm32g431xx.h>

namespace base{
	
    using namespace std;

    static string DEVICE_NAME = "MIDIControl";
    static string DEVICE_TYPE = "BASE";
    static string FW_VERSION = "2.0.0";

    void init(void);
	void dfuCheck();
	void wdtStart();

    namespace current{
		
        bool isEnabled();
        void enable();
        void disable();
        void toggle(void);
        void set(bool value);
    };

    namespace Encoder{
        void process();
    };
}

#endif

