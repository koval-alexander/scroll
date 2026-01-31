#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/regulator.h>
#include <math.h>

#include "magnetometer.h"
#include "scroll.h"
#include "custom_as5600.h"

#define SENSOR_THREAD_PRIORITY 7
#define SENSOR_THREAD_STACKSIZE 1024

static const struct device *regulator_dev = DEVICE_DT_GET(DT_NODELABEL(mag_pwr));

/* Initialize scroll resolution multiplier with default value */
uint8_t scroll_resolution_multiplier = SCROLL_RESOLUTION_MULTIPLIER;
bool hirez_enabled = false;

static const struct device *get_as5600_sensor(void)
 {
 	const struct device *const dev = DEVICE_DT_GET_ONE(zephyr_custom_as5600);
 	//const struct device *const dev = DT_COMPAT_ams_as5600_BUS_i2c;
 
 	if (dev == NULL) {
 		/* No such node, or the node does not have status "okay". */
 		printk("\nError: no device found.\n");
 		return NULL;
 	}

	while (!device_is_ready(dev)) {
		k_sleep(K_MSEC(10));	
		printk("\nError: Device \"%s\" is not ready; "
 		       "check the driver initialization logs for errors.\n",
 		       dev->name);
	} 
 	printk("Found device \"%s\", getting sensor data\n", dev->name);
 	return dev;
 }

extern const struct init_entry __init_POST_KERNEL_start[];
extern const struct init_entry __init_APPLICATION_start[];

int device_user_init(const struct device* dev) {
    const struct init_entry* entry;
    int rc;

    rc = -1;
    for (entry = __init_POST_KERNEL_start; entry < __init_APPLICATION_start; entry++) {
        if (entry->dev == dev) {
            rc = entry->init_fn.dev(dev);

            /* Mark device initialized.  If initialization
             * failed, record the error condition.
             */
            if (rc != 0) {
                if (rc < 0) {
                    rc = -rc;
                }

                if (rc > UINT8_MAX) {
                    rc = UINT8_MAX;
                }
                dev->state->init_res = rc;
            }
            dev->state->initialized = true;
        }
    }

    return (rc);
}

static int64_t dt(int64_t time_start, int64_t time_end)
{
	return time_end - time_start;
}

enum power_mode {
	ACTIVE_MODE,
	LPM_MODE,
	DOZE_MODE
};

static void set_sensor_defaults(const struct device *sensor_dev)
{
	sensor_attr_set(sensor_dev, SENSOR_CHAN_ROTATION, AS5600_POWER_MODE, &(struct sensor_value){.val1 = AS5600_POWER_MODE_LPM1, .val2 = 0}); // Set initial power mode to LPM1
	sensor_attr_set(sensor_dev, SENSOR_CHAN_ROTATION, AS5600_HYSTERESIS, &(struct sensor_value){.val1 = AS5600_HYSTERESIS_2LSB, .val2 = 0}); // Set hysteresis to reduce jitter
}

int sensor_data_collector(void)
{
	struct sensor_value rotation;
	static float prev_rotation_angle = 500;
	static float scroll_accumulator = 0; /* Accumulator for fractional scroll units */
	static bool prev_neg = false;
	static int64_t last_time = 0;
	static enum power_mode current_power_mode = ACTIVE_MODE;
	const struct device *sensor_dev = get_as5600_sensor();
	k_timeout_t sleep_timeout = K_MSEC(ACTIVE_MODE_PERIOD_MS);

	if (sensor_dev == NULL) {
		return -1;
	}

	set_sensor_defaults(sensor_dev);

    while (1) {
		k_sleep(sleep_timeout); // 15ms delay ~66Hz sampling

		if (bt_connected) {
			if (regulator_is_enabled(regulator_dev) == false) {
				regulator_enable(regulator_dev);
				printk("Magnetometer power enabled\n");
				k_sleep(K_MSEC(15)); // Wait for sensor to power up
				set_sensor_defaults(sensor_dev); // Re-initialize sensor after power-up
				sensor_sample_fetch(sensor_dev); // Discard first sample after power-up
			}
		} else {
			if (regulator_is_enabled(regulator_dev) == true) {
				regulator_disable(regulator_dev);
				printk("Magnetometer power disabled\n");
			}
			k_sleep(K_MSEC(300));
			continue;
		}
		
		
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
		
		printk("\rRotation: %d.%06d degrees", rotation.val1, rotation.val2);

		float current_angle;
		float angle_delta;
		int8_t scroll_delta;
		
		/* Convert sensor_value rotation (degrees) to integer angle */
		current_angle = rotation.val1 + (rotation.val2 / 1000000.0f);
		//if (current_angle > 360.f) continue;
		/* Calculate delta with wraparound handling (0-360 degrees) */
		if (prev_rotation_angle > 360) prev_rotation_angle = current_angle;
		angle_delta = current_angle - prev_rotation_angle;
		
		/* Handle wraparound at 0/360 degree boundary */
		if (angle_delta > 180.f) {
			angle_delta -= 360.f;
		} else if (angle_delta < -180.f) {
			angle_delta += 360.f;
		}
		// if (current_power_mode == DOZE_MODE) {
		// 	printf("Current angle: %f, Previous angle: %f, Angle delta: %f\n", current_angle, prev_rotation_angle, angle_delta);
		// }
		/* Update previous angle */
		prev_rotation_angle = current_angle;
		
		scroll_accumulator += angle_delta;
		
		/* Convert accumulated units to integer scroll steps */
		if (hirez_enabled) {
			scroll_delta = (int8_t)(scroll_accumulator / SCROLL_DEGREES_PER_TICK);
		} else {
			scroll_delta = (int8_t)(scroll_accumulator / SCROLL_DEGREES_PER_TICK_NORMAL);
		}
		/* Apply hysteresis to avoid small jittery scrolls */		
		if (scroll_delta > 0 && prev_neg && scroll_delta < SCROLL_HYSTERESIS_THRESHOLD) continue;
		if (scroll_delta < 0 && !prev_neg && scroll_delta > -SCROLL_HYSTERESIS_THRESHOLD) continue;
		prev_neg = (scroll_delta < 0);

		/* Send scroll events if we have full steps */
		if (scroll_delta != 0) {
			/* Subtract sent units from accumulator, keeping remainder */
			scroll_accumulator -= (scroll_delta * SCROLL_DEGREES_PER_TICK);

			//printk("Scroll delta: %d\n", scroll_delta);
			
			#if SCROLL_INVERSE
			scroll_delta = -scroll_delta;
			#endif
			k_msgq_put(&scroll_queue, &scroll_delta, K_NO_WAIT);

			if (k_msgq_num_used_get(&scroll_queue) == 1) {
				k_work_submit(&hids_work);
			}
			last_time = k_uptime_get();
		}

		/* Power mode management based on inactivity time */
		int64_t current_time = k_uptime_get();
		int64_t inactive_time = dt(last_time, current_time);
		if (inactive_time >= DOZE_TIMEOUT_MS && current_power_mode != DOZE_MODE) {
			current_power_mode = DOZE_MODE;
			printk("Switching to DOZE mode\n");
			regulator_disable(regulator_dev); // Power down sensor in DOZE mode, will be re-enabled on next loop
			sleep_timeout = K_MSEC(DOZE_MODE_PERIOD_MS); // Reduce sampling rate in DOZE mode
		} else if (inactive_time >= LPM_TIMEOUT_MS && current_power_mode != LPM_MODE) {
			current_power_mode = LPM_MODE;
			printk("Switching to LPM mode\n");
			sensor_attr_set(sensor_dev, SENSOR_CHAN_ROTATION, AS5600_POWER_MODE, &(struct sensor_value){.val1 = AS5600_POWER_MODE_LPM2, .val2 = 0}); // Set low power mode
			sleep_timeout = K_MSEC(LPM_MODE_PERIOD_MS); // Reduce sampling rate in LPM mode
		} else if (inactive_time < LPM_TIMEOUT_MS && current_power_mode != ACTIVE_MODE) {
			current_power_mode = ACTIVE_MODE;
			printk("Switching to ACTIVE mode\n");
			sensor_attr_set(sensor_dev, SENSOR_CHAN_ROTATION, AS5600_POWER_MODE, &(struct sensor_value){.val1 = AS5600_POWER_MODE_LPM1, .val2 = 0}); // Set active mode. LPM1 is default (sufficiently fast)
			sleep_timeout = K_MSEC(ACTIVE_MODE_PERIOD_MS); // Restore normal sampling rate
		}
    }
}

K_THREAD_DEFINE(sensor_data_collector_id, SENSOR_THREAD_STACKSIZE, sensor_data_collector, NULL, NULL, NULL, SENSOR_THREAD_PRIORITY, 0, 1000);

