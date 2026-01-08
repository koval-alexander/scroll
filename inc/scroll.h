#ifndef _SCROLL_H_
#define _SCROLL_H_

#include <stdint.h>
#include <zephyr/types.h>


extern struct k_msgq scroll_queue;
extern struct k_work hids_work;

#endif /* _SCROLL_H_ */