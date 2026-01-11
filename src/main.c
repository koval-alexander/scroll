#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <soc.h>
#include <assert.h>

#include <zephyr/settings/settings.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include <zephyr/bluetooth/services/bas.h>
#include <bluetooth/services/hids.h>
#include <zephyr/bluetooth/services/dis.h>
#include <dk_buttons_and_leds.h>

#include "pairing.h"
#include "scroll.h"


#define BASE_USB_HID_SPEC_VERSION   0x0101

#define INPUT_REP_WHEEL_BTN_LEN 2
#define INPUT_REP_WHEEL_BTN_ID  1
#define INPUT_REP_WHEEL_BTN_INDEX 0
#define WHEEL_BYTE_INDEX 1

#define FEATURE_REP_RES_LEN 1
#define FEATURE_REP_RES_ID 2
#define FEATURE_REP_RES_INDEX 0

/* HIDs queue size. */
#define HIDS_QUEUE_SIZE 10

/* HIDS instance. */
BT_HIDS_DEF(hids_obj,
	    INPUT_REP_WHEEL_BTN_LEN);

struct k_work hids_work;

K_MSGQ_DEFINE(scroll_queue,
	      sizeof(int8_t),
	      HIDS_QUEUE_SIZE,
	      4);

static void hids_pm_evt_handler(enum bt_hids_pm_evt evt, struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];
	size_t i;

	for (i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (conn_mode[i].conn == conn) {
			break;
		}
	}

	if (i >= CONFIG_BT_HIDS_MAX_CLIENT_COUNT) {
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	switch (evt) {
	case BT_HIDS_PM_EVT_BOOT_MODE_ENTERED:
		printk("Boot mode entered %s\n", addr);
		conn_mode[i].in_boot_mode = true;
		break;

	case BT_HIDS_PM_EVT_REPORT_MODE_ENTERED:
		printk("Report mode entered %s\n", addr);
		conn_mode[i].in_boot_mode = false;
		break;

	default:
		break;
	}
}

static void hid_feature_report_handler(struct bt_hids_rep *rep, struct bt_conn *conn, bool write)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (write) {
		printk("HID Feature Report written by %s, id: %d, size: %d\n",
		       addr, rep->data[0], rep->size);
		rep->data[0] = scroll_resolution_multiplier;
	} else {
		printk("HID Feature Report read by %s, id: %d, size: %d\n",
		       addr, rep->data[0], rep->size);
		scroll_resolution_multiplier = rep->data[0];
		printk("Scroll resolution multiplier set to: %d\n", scroll_resolution_multiplier);
	}
}

static void hid_init(void)
{
	int err;
	struct bt_hids_init_param hids_init_param = { 0 };
	struct bt_hids_inp_rep *hids_inp_rep;
	
	static const uint8_t report_map[] = {
		0x05, 0x01,     /* Usage Page (Generic Desktop) */
		0x09, 0x02,     /* Usage (Mouse) */

		0xA1, 0x01,     /* Collection (Application) */

		/* Report ID 1: Mouse buttons + scroll/pan */
		0x85, 0x01,       /* Report Id 1 */
		0x09, 0x01,       /* Usage (Pointer) */
		0xA1, 0x00,       /* Collection (Physical) */
		0x95, 0x05,       /* Report Count (3) */
		0x75, 0x01,       /* Report Size (1) */
		0x05, 0x09,       /* Usage Page (Buttons) */
		0x19, 0x01,       /* Usage Minimum (01) */
		0x29, 0x05,       /* Usage Maximum (05) */
		0x15, 0x00,       /* Logical Minimum (0) */
		0x25, 0x01,       /* Logical Maximum (1) */
		0x81, 0x02,       /* Input (Data, Variable, Absolute) */
		0x95, 0x01,       /* Report Count (1) */
		0x75, 0x03,       /* Report Size (3) */
		0x81, 0x01,       /* Input (Constant) for padding */
		0x75, 0x08,       /* Report Size (8) */
		0x95, 0x01,       /* Report Count (1) */
		0x05, 0x01,       /* Usage Page (Generic Desktop) */
		0x09, 0x38,       /* Usage (Wheel) */
		0x15, 0x81,       /* Logical Minimum (-127) */
		0x25, 0x7F,       /* Logical Maximum (127) */
		0x81, 0x06,       /* Input (Data, Variable, Relative) */
		0xC0,             /* End Collection (Physical) */

		/* Report ID 2: Mouse motion */
		0x85, 0x02,       /* Report Id 2 */
		0x09, 0x01,       /* Usage (Pointer) */
		0xA1, 0x00,       /* Collection (Physical) */
		0x75, 0x0C,       /* Report Size (12) */
		0x95, 0x02,       /* Report Count (2) */
		0x05, 0x01,       /* Usage Page (Generic Desktop) */
		0x09, 0x30,       /* Usage (X) */
		0x09, 0x31,       /* Usage (Y) */
		0x16, 0x01, 0xF8, /* Logical maximum (2047) */
		0x26, 0xFF, 0x07, /* Logical minimum (-2047) */
		0x81, 0x06,       /* Input (Data, Variable, Relative) */
		0xC0,             /* End Collection (Physical) */
		
		0xC0,             /* End Collection (Application) */
	};

	hids_init_param.rep_map.data = report_map;
	hids_init_param.rep_map.size = sizeof(report_map);

	hids_init_param.info.bcd_hid = BASE_USB_HID_SPEC_VERSION;
	hids_init_param.info.b_country_code = 0x00;
	hids_init_param.info.flags = (BT_HIDS_REMOTE_WAKE | BT_HIDS_NORMALLY_CONNECTABLE);

	hids_inp_rep = &hids_init_param.inp_rep_group_init.reports[0];
	hids_inp_rep->size = INPUT_REP_WHEEL_BTN_LEN;
	hids_inp_rep->id = INPUT_REP_WHEEL_BTN_ID;
	hids_init_param.inp_rep_group_init.cnt++;

	hids_init_param.is_mouse = true;
	hids_init_param.pm_evt_handler = hids_pm_evt_handler;

	err = bt_hids_init(&hids_obj, &hids_init_param);
	__ASSERT(err == 0, "HIDS initialization failed\n");
}

static void mouse_scroll_send(int8_t scroll_delta)
{
	printk("Sending scroll delta: %d\n", scroll_delta);
	for (size_t i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (!conn_mode[i].conn) {
			continue;
		}

		if (!conn_mode[i].in_boot_mode) {
			uint8_t buffer[INPUT_REP_WHEEL_BTN_LEN] = {0};
			buffer[WHEEL_BYTE_INDEX] = scroll_delta;

			bt_hids_inp_rep_send(&hids_obj, conn_mode[i].conn,
						  INPUT_REP_WHEEL_BTN_INDEX,
						  buffer, sizeof(buffer), NULL);
		}
	}
}

static void mouse_handler(struct k_work *work)
{
	uint8_t scroll_delta;
	while (!k_msgq_get(&scroll_queue, &scroll_delta, K_NO_WAIT)) {
		mouse_scroll_send(scroll_delta);
	}
}

void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	is_adv_running = false;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err) {
		if (err == BT_HCI_ERR_ADV_TIMEOUT) {
			printk("Direct advertising to %s timed out\n", addr);
			k_work_submit(&adv_work);
		} else {
			printk("Failed to connect to %s 0x%02x %s\n", addr, err,
			       bt_hci_err_to_str(err));
		}
		return;
	}

	printk("Connected %s\n", addr);

	err = bt_hids_connected(&hids_obj, conn);

	if (err) {
		printk("Failed to notify HID service about connection\n");
		return;
	}

	insert_conn_object(conn);

	if (is_conn_slot_free()) {
		advertising_start();
	}
}


void disconnected(struct bt_conn *conn, uint8_t reason)
{
	int err;
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Disconnected from %s, reason 0x%02x %s\n", addr, reason, bt_hci_err_to_str(reason));

	err = bt_hids_disconnected(&hids_obj, conn);

	if (err) {
		printk("Failed to notify HID service about disconnection\n");
	}

	for (size_t i = 0; i < CONFIG_BT_HIDS_MAX_CLIENT_COUNT; i++) {
		if (conn_mode[i].conn == conn) {
			conn_mode[i].conn = NULL;
			break;
		}
	}

	advertising_start();
}

void button_changed(uint32_t button_state, uint32_t has_changed)
{
	uint32_t buttons = button_state & has_changed;
	printk("Button state changed: 0x%08X, changed: 0x%08X\n", button_state, has_changed);

	if (buttons) {
		bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
		printk("Cleared all connections\n");
	}
}

void configure_buttons(void)
{
	int err;

	err = dk_buttons_init(button_changed);
	if (err) {
		printk("Cannot init buttons (err: %d)\n", err);
	}
}



static void bas_notify(void)
{
	uint8_t battery_level = bt_bas_get_battery_level();

	battery_level--;

	if (!battery_level) {
		battery_level = 100U;
	}

	bt_bas_set_battery_level(battery_level);
}

static bool write_word_to_uicr(volatile uint32_t * addr, uint32_t word)
{
    if (*addr == word)
    {
        // Allready set. Nothing more to do.
        return true;
    }
    else
    {
        // Register has default value. Ready to write...
        NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen;
        while (NRF_NVMC->READY == NVMC_READY_READY_Busy){}

        *addr = word;

        NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
        while (NRF_NVMC->READY == NVMC_READY_READY_Busy){}

        NVIC_SystemReset();
    }
}



int main(void)
{
	int err;

	write_word_to_uicr(&NRF_UICR->PSELRESET[0], 0);
	write_word_to_uicr(&NRF_UICR->PSELRESET[1], 0);

	printk("Starting Bluetooth Peripheral HIDS mouse example\n");

	if (IS_ENABLED(CONFIG_BT_HIDS_SECURITY_ENABLED)) {
		register_auth_callbacks();
	}

	/* DIS initialized at system boot with SYS_INIT macro. */
	hid_init();

	err = bt_enable(NULL);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return 0;
	}

	printk("Bluetooth initialized\n");

	k_work_init(&hids_work, mouse_handler);
	register_pairing_work();

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	advertising_start();

	configure_buttons();

	while (1) {
		k_sleep(K_SECONDS(1));
		/* Battery level simulation */
		bas_notify();
	}
}


