#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "base.hpp"

#include <cstdint>

class Scheduler{
    private:
        //Interval in ms
        int interval;
        //Callback to a function
        void (*callback)();
        //Flags
        uint16_t flags;
        //Counter
        int counter;
    public:
        //Scheduler constructor
        Scheduler(int interval, void (*callback)(), uint16_t flags = 0, int counter = 0) : interval(interval), callback(callback), flags(flags), counter(counter){};
        void increment();
        void dispatch();
        uint16_t setFlags(uint16_t flags);
        uint16_t enableFlags(uint16_t flags);
        uint16_t disableFlags(uint16_t flags);
        void reset();
        void pause();
        void resume();
        bool isReady();
        bool isActive();
        bool isCompleted();
        void setInterval(int interval);

        enum{
            //Indicates that the scheduler is running
            ACTIVE      = 0x0001,
            //Indicates that the scheduler has finished
            READY       = 0x0002,
            //Enables the scheduler to run repeatedly
            PERIODICAL  = 0x0004,
            //In case of periodical, indicates that the scheduler has been ready at least once
            COMPLETED    = 0x0008,
            //Dispathches the scheduler on increment
            DISPATCH_ON_INCREMENT = 0x0010
        };
 };


#endif
