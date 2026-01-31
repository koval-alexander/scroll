#ifndef _SCROLL_H_
#define _SCROLL_H_

#include <stdint.h>
#include <zephyr/types.h>

/* Scroll resolution multiplier - standard is 120 units per notch */
#define SCROLL_RESOLUTION_MULTIPLIER 16
/* Degrees per notch - adjust for sensitivity (lower = more sensitive) */
#define SCROLL_DEGREES_PER_NOTCH 2.f
/* Hysteresis threshold - minimum accumulated notches before sending scroll event */
#define SCROLL_HYSTERESIS_THRESHOLD 3
/* Inverse scroll direction */
#define SCROLL_INVERSE 1
/* Degrees per notch in normal mode (without multiplier) */
#define SCROLL_DEGREES_PER_TICK_NORMAL 10.0f
/* Degrees per tick adjusted by resolution multiplier */
#define SCROLL_DEGREES_PER_TICK (SCROLL_DEGREES_PER_NOTCH / SCROLL_RESOLUTION_MULTIPLIER)
/* Low power mode timeouts in milliseconds */
#define LPM_TIMEOUT_MS 3000
#define DOZE_TIMEOUT_MS 10000

#define ACTIVE_MODE_PERIOD_MS 15
#define LPM_MODE_PERIOD_MS 50
#define DOZE_MODE_PERIOD_MS 5000


extern struct k_msgq scroll_queue;
extern struct k_work hids_work;

extern bool hirez_enabled;
extern bool bt_connected;

#endif /* _SCROLL_H_ */