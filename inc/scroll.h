#ifndef _SCROLL_H_
#define _SCROLL_H_

#include <stdint.h>
#include <zephyr/types.h>

/* Scroll resolution multiplier - standard is 120 units per notch */
#define SCROLL_RESOLUTION_MULTIPLIER 16
/* Degrees per notch - adjust for sensitivity (lower = more sensitive) */
#define SCROLL_DEGREES_PER_NOTCH 1.5f
/* Hysteresis threshold - minimum accumulated degrees before sending scroll event */
#define SCROLL_HYSTERESIS_THRESHOLD 3
/* Inverse scroll direction */
#define SCROLL_INVERSE 1

#define SCROLL_DEGREES_PER_TICK (SCROLL_DEGREES_PER_NOTCH / SCROLL_RESOLUTION_MULTIPLIER)

extern struct k_msgq scroll_queue;
extern struct k_work hids_work;

extern uint8_t scroll_resolution_multiplier;

#endif /* _SCROLL_H_ */