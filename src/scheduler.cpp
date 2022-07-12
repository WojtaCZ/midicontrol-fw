#include "scheduler.hpp"
#include "base.hpp"


//Initialize keepalive scheduler
Scheduler keepaliveScheduler = Scheduler(1000, &Base::CurrentSource::toggle, Scheduler::PERIODICAL | Scheduler::ACTIVE | Scheduler::DISPATCH_ON_INCREMENT);

//Function to process and increment the scheduler counter
void Scheduler::increment(){
    if((this->counter >= this->interval) && !(this->flags & Scheduler::READY) && (this->flags & Scheduler::ACTIVE)){
        this->flags |= READY;
        if(this->flags & Scheduler::DISPATCH_ON_INCREMENT) this->dispatch();
    }else if(this->flags & Scheduler::ACTIVE){
        this->counter++;
    }
}

//Function to take care of callbacks after scheduler finshed
void Scheduler::dispatch(){
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

//Sets scheduler flags
uint16_t Scheduler::setFlags(uint16_t flags){
    this->flags = 0 | flags;
    return this->flags;
}

//Enables scheduler flags
uint16_t Scheduler::enableFlags(uint16_t flags){
    this->flags |= flags;
    return this->flags;
}

//Disables scheduler flags
uint16_t Scheduler::disableFlags(uint16_t flags){
    this->flags &= ~flags;
    return this->flags;
}

//Resets the scheduler timer and READY and COMPLETED flags
void Scheduler::reset(){
    this->disableFlags(Scheduler::READY | Scheduler::COMPLETED);
    this->counter = 0;
}

//Pauses the scheduler
void Scheduler::pause(){
    if(!this->isActive()) return;
    this->disableFlags(Scheduler::ACTIVE);
}

//Resumes the scheduler
void Scheduler::resume(){
    if(this->isActive()) return;
    this->enableFlags(Scheduler::ACTIVE);
}

//Indicates that the scheduler is ready
bool Scheduler::isReady(){
    if(this->flags & Scheduler::READY) return true;
    return false;
}

//Indicates that the scheduler is active
bool Scheduler::isActive(){
    if(this->flags & Scheduler::ACTIVE) return true;
    return false;
}

//Indicates that the scheduler is completed
bool Scheduler::isCompleted(){
    if(this->flags & Scheduler::COMPLETED) return true;
    return false;
}

//Sets the scheduler interval in ms
void Scheduler::setInterval(int interval){
    this->interval = interval;
}
