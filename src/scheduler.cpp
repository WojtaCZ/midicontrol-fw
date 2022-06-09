#include "scheduler.hpp"
#include "oled.hpp"
#include "menu.hpp"
#include "base.hpp"


#include <libopencm3/stm32/usart.h>

extern void menu_scroll_callback(void);
Scheduler menuScroll(500, &menu_scroll_callback, Scheduler::PERIODICAL);

extern void render(void);
Scheduler menuRender(30, &render, Scheduler::PERIODICAL | Scheduler::ACTIVE);
/*
extern void led_dev_process_pending_status(void);
Scheduler ledProcess(50, &led_dev_process_pending_status, Scheduler::PERIODICAL | Scheduler::ACTIVE);



Scheduler ioKeypress(10, &led_dev_process_pending_status, Scheduler::PERIODICAL | Scheduler::ACTIVE);
*/
Scheduler oledSleep(OLED_SLEEP_INTERVAL, &oled_sleep_callback, Scheduler::ACTIVE);
/*
extern void comm_decode_callback(void);
Scheduler sched_comm_decode = {0, 0, &comm_decode_callback, 0};
*/
/*
extern void comm_timeout_callback(void);
Scheduler sched_comm_timeout(3000, &comm_timeout_callback);
*/
Scheduler::Scheduler(int interval, void (*callback)(void), uint16_t flags, int counter){
    this->interval = interval;
    this->callback = callback;
    this->flags = flags;
    this->counter = counter;
}

void Scheduler::check(){
    if((this->counter >= this->interval) && !(this->flags & Scheduler::READY) && (this->flags & Scheduler::ACTIVE)){
        this->flags |= READY;
    }else if(this->flags & Scheduler::ACTIVE){
        this->counter++;
    }
}
void Scheduler::process(){
    if(this->flags & Scheduler::READY){
        (*this->callback)();

        this->counter = 0;
        
        if(!(this->flags & Scheduler::PERIODICAL)){
            this->flags &= ~Scheduler::READY;
            this->flags |= Scheduler::COMPLETED;
        }

        this->flags &= ~Scheduler::READY;
    }
}

//Set scheduler flags
//Returns: Scheduler flags
uint16_t Scheduler::setFlags(uint16_t flags){
    this->flags = 0 | flags;
    return this->flags;
}

uint16_t Scheduler::enableFlags(uint16_t flags){
    this->flags |= flags;
    return this->flags;
}

uint16_t Scheduler::disableFlags(uint16_t flags){
    this->flags &= ~flags;
    return this->flags;
}

//Resets the scheduler and starts it immediately after reset
void Scheduler::reset(){
    this->disableFlags(Scheduler::READY | Scheduler::COMPLETED);
    this->counter = 0;
}

void Scheduler::pause(){
    if(!this->isActive()) return;
    this->disableFlags(Scheduler::ACTIVE);
}

void Scheduler::resume(){
    if(this->isActive()) return;
    this->enableFlags(Scheduler::ACTIVE);
}

bool Scheduler::isReady(){
    if(this->flags & Scheduler::READY) return true;
    return false;
}

bool Scheduler::isActive(){
    if(this->flags & Scheduler::ACTIVE) return true;
    return false;
}

bool Scheduler::isCompleted(){
    if(this->flags & Scheduler::COMPLETED) return true;
    return false;
}

