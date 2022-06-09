#ifndef MAIN_H
#define MAIN_H

#include "scheduler.hpp"

void enc_inc(void);
void enc_dec(void);
void enc_psh(void);

extern struct Input encoder;

extern Scheduler menuScroll;
extern Scheduler menuRender;
extern Scheduler ledProcess;
extern Scheduler ioKeypress;
extern Scheduler oledSleep;
extern Scheduler commTimeout;

#endif
