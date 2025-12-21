#ifndef MAIN_H
#define MAIN_H

#include <stmcpp/scheduler.hpp>

void enc_inc(void);
void enc_dec(void);
void enc_psh(void);

extern struct Input encoder;

extern stmcpp::scheduler::Scheduler menuScroll;
extern stmcpp::scheduler::Scheduler menuRender;
extern stmcpp::scheduler::Scheduler ledProcess;
extern stmcpp::scheduler::Scheduler ioKeypress;
extern stmcpp::scheduler::Scheduler oledSleep;
extern stmcpp::scheduler::Scheduler commTimeout;

#endif
