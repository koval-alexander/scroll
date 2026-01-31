#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
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
#include <zephyr/logging/log.h>

#include "pairing.h"
#include "scroll.h"

LOG_MODULE_REGISTER(Scroll, LOG_LEVEL_DBG);

#define BASE_USB_HID_SPEC_VERSION   0x0101

#define INPUT_REP_WHEEL_BTN_LEN 4
#define INPUT_REP_WHEEL_BTN_ID  1
#define INPUT_REP_WHEEL_BTN_INDEX 0
#define WHEEL_BYTE_INDEX 3

#define FEATURE_REP_RES_LEN 1
#define FEATURE_REP_RES_ID 2
#define FEATURE_REP_RES_INDEX 0


/* HIDs queue size. */
#define HIDS_QUEUE_SIZE 50

/* HIDS instance. */
BT_HIDS_DEF(hids_obj,
	    INPUT_REP_WHEEL_BTN_LEN, FEATURE_REP_RES_LEN);

struct k_work hids_work;

K_MSGQ_DEFINE(scroll_queue,
	      sizeof(int8_t),
	      HIDS_QUEUE_SIZE,
	      4);

static const struct adc_dt_spec bat_adc_channel = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));
int16_t bat_adc_buf;
struct adc_sequence sequence = {
	.buffer = &bat_adc_buf,
	/* buffer size in bytes, not number of samples */
	.buffer_size = sizeof(bat_adc_buf),
	//Optional
	//.calibrate = true,
};

static const struct gpio_dt_spec red_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec green_led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec blue_led = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static const struct gpio_dt_spec bm_switch = GPIO_DT_SPEC_GET(DT_PATH(gpios, bm_switch), gpios);

bool bt_connected = false;

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
		/* Host is reading the feature report - send current multiplier */
		rep->data[0] = 1;//scroll_resolution_multiplier;
		printk("HID Feature Report read by %s, sending multiplier: 1\n", addr);
	} else {
		/* Host is writing the feature report - update multiplier */
		//scroll_resolution_multiplier = rep->data[0];
		printk("HID Feature Report written by %s, multiplier set to ON\n", addr);
		hirez_enabled = true;
	}
}

static void hid_init(void)
{
	int err;
	struct bt_hids_init_param hids_init_param = { 0 };
	struct bt_hids_inp_rep *hids_inp_rep;
	
	static const uint8_t report_map[] = {
		0x05, 0x01,        // Usage Page (Generic Desktop Ctrls)
		0x09, 0x02,        // Usage (Mouse)
		0xA1, 0x01,        // Collection (Application)
		0x85, 0x01,        //   Report ID (1)
		0x09, 0x01,        //   Usage (Pointer)
		0xA1, 0x00,        //   Collection (Physical)
		// Buttons
		0x05, 0x09,        //     Usage Page (Button)
		0x19, 0x01,        //     Usage Minimum (0x01)
		0x29, 0x03,        //     Usage Maximum (0x03)
		0x95, 0x03,        //     Report Count (3)
		0x75, 0x01,        //     Report Size (1)
		0x15, 0x00,        //     Logical Minimum (0)
		0x25, 0x01,        //     Logical Maximum (1)
		0x81, 0x02,        //     Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
		// Padding
		0x75, 0x05,        //     Report Size (5)
		0x95, 0x01,        //     Report Count (1)
		0x81, 0x01,        //     Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
		// X and Y axis
		0x05, 0x01,        //     Usage Page (Generic Desktop Ctrls)
		0x09, 0x30,        //     Usage (X)
		0x09, 0x31,        //     Usage (Y)
		0x95, 0x02,        //     Report Count (2)
		0x75, 0x08,        //     Report Size (8)
		0x15, 0x81,        //     Logical Minimum (-127)
		0x25, 0x7F,        //     Logical Maximum (127)
		0x81, 0x06,        //     Input (Data,Var,Rel,No Wrap,Linear,Preferred State,No Null Position)
		// Wheel
		0xA1, 0x02,        //     Collection (Logical)
		// Resolution Multiplier Feature Report
		0x85, 0x02,        //       Report ID (2)
		0x09, 0x48,        //       Usage (0x48)
		0x95, 0x01,        //       Report Count (1)
		0x75, 0x08,        //       Report Size (8)
		0x15, 0x00,        //       Logical Minimum (0)
		0x25, 0x01,        //       Logical Maximum (1)
		0x35, 0x01,        //       Physical Minimum (1)
		0x45, 0x10,        //       Physical Maximum (16) <-------
		0xB1, 0x02,        //       Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position,Non-volatile)
		// Wheel
		0x85, 0x01,        //       Report ID (1)
		0x09, 0x38,        //       Usage (Wheel)
		0x35, 0x00,        //       Physical Minimum (0)
		0x45, 0x00,        //       Physical Maximum (0)
		0x15, 0x81,        //       Logical Minimum (-127)
		0x25, 0x7F,        //       Logical Maximum (127)
		0x75, 0x08,        //       Report Size (8)
		0x81, 0x06,        //       Input (Data,Var,Rel,No Wrap,Linear,Preferred State,No Null Position)
		0xC0,              //     End Collection
		0xC0,              //   End Collection
		0xC0,              // End Collection
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

/* Setup Feature Report for Resolution Multiplier */
	struct bt_hids_outp_feat_rep *hids_feat_rep;
	hids_feat_rep = &hids_init_param.feat_rep_group_init.reports[0];
	hids_feat_rep->size = FEATURE_REP_RES_LEN;
	hids_feat_rep->id = FEATURE_REP_RES_ID;
	hids_feat_rep->handler = hid_feature_report_handler;
	hids_init_param.feat_rep_group_init.cnt++;

	hids_init_param.is_mouse = true;
	hids_init_param.pm_evt_handler = hids_pm_evt_handler;

	err = bt_hids_init(&hids_obj, &hids_init_param);
	__ASSERT(err == 0, "HIDS initialization failed\n");
}

static void mouse_scroll_send(int8_t scroll_delta)
{
	//printk("Sending scroll delta: %d\n", scroll_delta);
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

	bt_connected = true;

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

	hirez_enabled = false;
	bt_connected = false;

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



/* Литиевый аккумулятор: таблица зависимости процента заряда от напряжения */
static const struct {
	uint16_t voltage_mv;
	uint8_t percentage;
} battery_voltage_table[] = {
	{4200, 100},
	{4100, 90},
	{4000, 80},
	{3900, 70},
	{3800, 60},
	{3700, 50},
	{3600, 40},
	{3500, 30},
	{3400, 20},
	{3300, 10},
	{3000, 0}
};

/* Преобразование напряжения в милливольтах в процент заряда 
 * с линейной интерполяцией между табличными значениями */
static uint8_t voltage_to_battery_percentage(int32_t voltage_mv)
{
	const uint8_t table_size = ARRAY_SIZE(battery_voltage_table);
	
	/* Если напряжение выше максимального - 100% */
	if (voltage_mv >= battery_voltage_table[0].voltage_mv) {
		return battery_voltage_table[0].percentage;
	}
	
	/* Если напряжение ниже минимального - 0% */
	if (voltage_mv <= battery_voltage_table[table_size - 1].voltage_mv) {
		return battery_voltage_table[table_size - 1].percentage;
	}
	
	/* Линейная интерполяция между табличными значениями */
	for (uint8_t i = 0; i < table_size - 1; i++) {
		if (voltage_mv <= battery_voltage_table[i].voltage_mv && 
		    voltage_mv > battery_voltage_table[i + 1].voltage_mv) {
			
			int32_t v_high = battery_voltage_table[i].voltage_mv;
			int32_t v_low = battery_voltage_table[i + 1].voltage_mv;
			int32_t p_high = battery_voltage_table[i].percentage;
			int32_t p_low = battery_voltage_table[i + 1].percentage;
			
			/* Линейная интерполяция: p = p_low + (v - v_low) * (p_high - p_low) / (v_high - v_low) */
			int32_t percentage = p_low + ((voltage_mv - v_low) * (p_high - p_low)) / (v_high - v_low);
			
			return (uint8_t)percentage;
		}
	}
	
	return 0; /* Не должно произойти */
}

static void bas_notify(void)
{
	static uint8_t battery_level = 100;
	int	err;
	int32_t val_mv = 0;

	// If battery level is below 10%, blink red LED
	if (battery_level < 10) {
		gpio_pin_set_dt(&red_led, 1);
	}
	gpio_pin_set_dt(&bm_switch, 1);
	
	err = adc_read(bat_adc_channel.dev, &sequence);
	if (err < 0) {
		LOG_ERR("Could not read (%d)", err);
		return;
	}

	//LOG_DBG("ADC raw value: %X", bat_adc_buf);

	val_mv = bat_adc_buf;
	err = adc_raw_to_millivolts_dt(&bat_adc_channel, &val_mv);
	val_mv *= 151; // Voltage divider correction factor
	val_mv /= 51;
	/* conversion to mV may not be supported, skip if not */
	if (err < 0) {
	    LOG_ERR(" (value in mV not available)\n");
	} else {
		//LOG_DBG("Battery voltage = %d mV\n", val_mv);
	}

	gpio_pin_set_dt(&red_led, 0);
	gpio_pin_set_dt(&bm_switch, 0);

	battery_level = voltage_to_battery_percentage(val_mv);
	//LOG_INF("Battery level: %d%%\n", battery_level);
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

void adc_init(void)
{
	int ret;

	if (!adc_is_ready_dt(&bat_adc_channel)) {
		printk("ADC device not ready\n");
		return;
	}

	ret = adc_channel_setup_dt(&bat_adc_channel);
	if (ret < 0) {
		printk("Failed to setup ADC channel (err %d)\n", ret);
		return;
	}

	int	err = adc_sequence_init_dt(&bat_adc_channel, &sequence);
	if (err < 0) {
		printk("Could not initalize sequnce\n");
		return;
	}
}

void gpio_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&red_led)) {
		printk("Red LED device not ready\n");
		return;
	}
	ret = gpio_pin_configure_dt(&red_led, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		printk("Failed to configure red LED pin\n");
		return;
	}
	if (!gpio_is_ready_dt(&green_led)) {
		printk("Green LED device not ready\n");
		return;
	}
	ret = gpio_pin_configure_dt(&green_led, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		printk("Failed to configure green LED pin\n");
		return;
	}
	if (!gpio_is_ready_dt(&blue_led)) {
		printk("Blue LED device not ready\n");
		return;
	}
	ret = gpio_pin_configure_dt(&blue_led, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		printk("Failed to configure blue LED pin\n");
		return;
	}
	if (!gpio_is_ready_dt(&bm_switch)) {
		printk("BM Switch device not ready\n");
		return;
	}
	ret = gpio_pin_configure_dt(&bm_switch, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		printk("Failed to configure BM Switch pin\n");
		return;
	}
}

//void (*func)(const struct bt_bond_info *info, void *user_data)
void count_handler(const struct bt_bond_info *info, void *user_data)
{
	int *count = (int *)user_data;
	(*count)++;
}

int bonds_count(void)
{
	int count = 0;
	bt_foreach_bond(BT_ID_DEFAULT, count_handler, &count);
	return count;
}

int main(void)
{
	int err;

	write_word_to_uicr(&NRF_UICR->PSELRESET[0], 0);
	write_word_to_uicr(&NRF_UICR->PSELRESET[1], 0);

	gpio_init();

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

	adc_init();

	advertising_start();

	configure_buttons();

	while (1) {
		k_sleep(K_SECONDS(1));

		if (bt_connected) {
			bas_notify();
		}

		if (is_adv_running && bonds_count() == 0) {
			// Flash blue LED to indicate pairing mode
			gpio_pin_set_dt(&blue_led, 1);
			k_sleep(K_MSEC(100));
			gpio_pin_set_dt(&blue_led, 0);
		}
	}
}


