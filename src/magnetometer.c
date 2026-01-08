#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>

#include "magnetometer.h"
#include "scroll.h"

#define SENSOR_THREAD_PRIORITY 7
#define SENSOR_THREAD_STACKSIZE 1024

struct sensor_value rotation;

static const struct device *get_as5600_sensor(void)
 {
 	const struct device *const dev = DEVICE_DT_GET_ONE(ams_as5600);
 	//const struct device *const dev = DT_COMPAT_ams_as5600_BUS_i2c;
 
 	if (dev == NULL) {
 		/* No such node, or the node does not have status "okay". */
 		printk("\nError: no device found.\n");
 		return NULL;
 	}

 	if (!device_is_ready(dev)) {
 		printk("\nError: Device \"%s\" is not ready; "
 		       "check the driver initialization logs for errors.\n",
 		       dev->name);
 		return NULL;
 	}
 
 	printk("Found device \"%s\", getting sensor data\n", dev->name);
 	return dev;
 }

int sensor_data_collector(void)
{
	static int32_t prev_rotation_angle = 0;
	const struct device *sensor_dev = get_as5600_sensor();
	if (sensor_dev == NULL) {
		return -1;
	}

    while (1) {
		k_sleep(K_MSEC(20));

		
		int ret = sensor_sample_fetch(sensor_dev);
		if (ret != 0) {
			printk("sensor_sample_fetch failed: %d\n", ret);
			continue;	
		}
		ret = sensor_channel_get(sensor_dev, SENSOR_CHAN_ROTATION, &rotation);
		if (ret != 0) {
			printk("sensor_channel_get ROTATION failed: %d\n", ret);
			continue;
		}
		//printk("\rRotation: %d.%06d degrees", rotation.val1, rotation.val2);

		int32_t current_angle;
		int32_t angle_delta;
		int8_t scroll_delta;
		
		/* Convert sensor_value rotation (degrees) to integer angle */
		current_angle = rotation.val1;
		
		/* Calculate delta with wraparound handling (0-360 degrees) */
		angle_delta = current_angle - prev_rotation_angle;
		
		/* Handle wraparound at 0/360 degree boundary */
		if (angle_delta > 180) {
			angle_delta -= 360;
		} else if (angle_delta < -180) {
			angle_delta += 360;
		}
		
		/* Convert angle delta to scroll steps (e.g., ~30 degrees = 1 scroll step) */
		scroll_delta = (int8_t)(angle_delta / 30);
		
		/* Update previous angle for next calculation */
		if (scroll_delta != 0) {
			prev_rotation_angle = current_angle;

			printk("Scroll delta: %d\n", scroll_delta);
			//scroll_pos pos;
			//pos.scroll_val = scroll_delta;		
			// k_msgq_put(&hids_queue, &scroll_delta, K_NO_WAIT);

			// if (k_msgq_num_used_get(&hids_queue) == 1) {
			// 	k_work_submit(&hids_work);
			// }
		}
    }
}

K_THREAD_DEFINE(sensor_data_collector_id, SENSOR_THREAD_STACKSIZE, sensor_data_collector, NULL, NULL, NULL, SENSOR_THREAD_PRIORITY, 0, 1000);

