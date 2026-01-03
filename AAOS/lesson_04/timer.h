#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

void timer_init(void);
void timer_handler(void);
void timer_bottom_half(void);

#endif
