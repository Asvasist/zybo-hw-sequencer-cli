/*
 * sleep.h - host mock of the BSP sleep functions.
 *
 * usleep() winds the modelled clock forward instead of waiting, so a test
 * runs through simulated days in microseconds.
 */
#ifndef SLEEP_H
#define SLEEP_H

#include "xil_types.h"

int usleep(unsigned long useconds);
int sleep(unsigned int seconds);

#endif /* SLEEP_H */
