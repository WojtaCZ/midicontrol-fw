
#ifndef BASE_H
#define BASE_H

#include "global.hpp"

namespace Base{
    void init(void);

    class CurrentSource{
        private:
            bool enabled = false;
        public:
            bool isEnabled();
            void enable();
            void disable();
    };

    class Encoder{
        private:
            uint16_t gpioState, gpioStateOld = 0xffff;
            uint8_t readCode = 0, storedCode = 0;
            int pos = 0;
            bool pressed;
        public:
            void process();

    };

}


#endif
