#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

void timer_init(void);
void timer_handler(void);
void timer_bottom_half(void);
uint64_t get_timer_freq(void);

#endif
