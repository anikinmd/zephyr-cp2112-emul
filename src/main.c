/*
 * Copyright (c) 2026 Mikhail Anikin
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/usb/usbd.h>

#include <string.h>

LOG_MODULE_REGISTER(cp2112, LOG_LEVEL_INF);

#define CP2112_GPIO_CONFIG			0x02
#define CP2112_GPIO_GET				0x03
#define CP2112_GPIO_SET				0x04
#define CP2112_GET_VERSION_INFO			0x05
#define CP2112_SMBUS_CONFIG			0x06
#define CP2112_DATA_READ_REQUEST		0x10
#define CP2112_DATA_WRITE_READ_REQUEST		0x11
#define CP2112_DATA_READ_FORCE_SEND		0x12
#define CP2112_DATA_READ_RESPONSE		0x13
#define CP2112_DATA_WRITE_REQUEST		0x14
#define CP2112_TRANSFER_STATUS_REQUEST		0x15
#define CP2112_TRANSFER_STATUS_RESPONSE		0x16
#define CP2112_CANCEL_TRANSFER			0x17
#define CP2112_LOCK_BYTE			0x20
#define CP2112_USB_CONFIG			0x21
#define CP2112_MANUFACTURER_STRING		0x22
#define CP2112_PRODUCT_STRING			0x23
#define CP2112_SERIAL_STRING			0x24

#define STATUS0_IDLE				0x00
#define STATUS0_BUSY				0x01
#define STATUS0_COMPLETE			0x02
#define STATUS0_ERROR				0x03

#define STATUS1_TIMEOUT_NACK			0x00
#define STATUS1_TIMEOUT_BUS			0x01
#define STATUS1_ARBITRATION_LOST		0x02
#define STATUS1_READ_INCOMPLETE			0x03
#define STATUS1_WRITE_INCOMPLETE		0x04
#define STATUS1_SUCCESS				0x05

#define NUM_GPIOS				8
#define MAX_XFER_LEN				512
#define MAX_WR_BUF				64
#define REPORT_MAX				64

#define HID_REPORT_TYPE_INPUT			0x01
#define HID_REPORT_TYPE_OUTPUT			0x02
#define HID_REPORT_TYPE_FEATURE			0x03

#define CP2112_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_cp2112_emul)

#if !DT_NODE_EXISTS(CP2112_NODE)
#error "No enabled node with compatible \"zephyr,cp2112-emul\" found"
#endif

#if DT_PROP_LEN(CP2112_NODE, gpios) != NUM_GPIOS
#error "zephyr,cp2112-emul node must define exactly 8 gpios"
#endif

#define CP2112_I2C_BUS_NODE DT_PHANDLE(CP2112_NODE, i2c_bus)
#define CP2112_I2C_CLOCK_HZ \
	DT_PROP_OR(CP2112_I2C_BUS_NODE, clock_frequency, 100000)

#define HID_NODE DT_NODELABEL(cp2112_hid)
BUILD_ASSERT(DT_NODE_EXISTS(HID_NODE),
	     "Missing DT node 'cp2112_hid'");

#define UDC_NODE DT_NODELABEL(zephyr_udc0)

static const struct device *const i2c_dev =
	DEVICE_DT_GET(DT_PHANDLE(CP2112_NODE, i2c_bus));
static const struct device *const hid_dev = DEVICE_DT_GET(HID_NODE);

static const struct gpio_dt_spec cp2112_gpios[NUM_GPIOS] = {
	GPIO_DT_SPEC_GET_BY_IDX(CP2112_NODE, gpios, 0),
	GPIO_DT_SPEC_GET_BY_IDX(CP2112_NODE, gpios, 1),
	GPIO_DT_SPEC_GET_BY_IDX(CP2112_NODE, gpios, 2),
	GPIO_DT_SPEC_GET_BY_IDX(CP2112_NODE, gpios, 3),
	GPIO_DT_SPEC_GET_BY_IDX(CP2112_NODE, gpios, 4),
	GPIO_DT_SPEC_GET_BY_IDX(CP2112_NODE, gpios, 5),
	GPIO_DT_SPEC_GET_BY_IDX(CP2112_NODE, gpios, 6),
	GPIO_DT_SPEC_GET_BY_IDX(CP2112_NODE, gpios, 7),
};

USBD_DEVICE_DEFINE(usbd_ctx, DEVICE_DT_GET(UDC_NODE), 0x10C4, 0xEA90);

USBD_DESC_LANG_DEFINE(lang_desc);
USBD_DESC_MANUFACTURER_DEFINE(mfr_desc, "Silicon Labs");
USBD_DESC_PRODUCT_DEFINE(product_desc, "CP2112 HID USB-to-SMBus Bridge");
USBD_DESC_SERIAL_NUMBER_DEFINE(sn_desc);

USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(fs_config, USB_SCD_SELF_POWERED, 100, &fs_cfg_desc);

#define INPUT_REPORT(id, cnt) \
	0x85, (id), 0x95, (cnt), 0x09, 0x01, 0x81, 0x02
#define OUTPUT_REPORT(id, cnt) \
	0x85, (id), 0x95, (cnt), 0x09, 0x01, 0x91, 0x02
#define FEATURE_REPORT(id, cnt) \
	0x85, (id), 0x95, (cnt), 0x09, 0x01, 0xB1, 0x02

static const uint8_t cp2112_report_desc[] = {
	0x06, 0x00, 0xFF,
	0x09, 0x01,
	0xA1, 0x01,

	0x15, 0x00,
	0x26, 0xFF, 0x00,
	0x75, 0x08,

	INPUT_REPORT(CP2112_DATA_READ_RESPONSE, 63),
	INPUT_REPORT(CP2112_TRANSFER_STATUS_RESPONSE, 6),

	OUTPUT_REPORT(CP2112_DATA_READ_REQUEST, 3),
	OUTPUT_REPORT(CP2112_DATA_WRITE_READ_REQUEST, 21),
	OUTPUT_REPORT(CP2112_DATA_READ_FORCE_SEND, 2),
	OUTPUT_REPORT(CP2112_DATA_WRITE_REQUEST, 61),
	OUTPUT_REPORT(CP2112_TRANSFER_STATUS_REQUEST, 1),
	OUTPUT_REPORT(CP2112_CANCEL_TRANSFER, 1),

	FEATURE_REPORT(CP2112_GPIO_CONFIG, 4),
	FEATURE_REPORT(CP2112_GPIO_GET, 1),
	FEATURE_REPORT(CP2112_GPIO_SET, 2),
	FEATURE_REPORT(CP2112_GET_VERSION_INFO, 2),
	FEATURE_REPORT(CP2112_SMBUS_CONFIG, 13),
	FEATURE_REPORT(CP2112_LOCK_BYTE, 1),
	FEATURE_REPORT(CP2112_USB_CONFIG, 9),
	FEATURE_REPORT(CP2112_MANUFACTURER_STRING, 62),
	FEATURE_REPORT(CP2112_PRODUCT_STRING, 62),
	FEATURE_REPORT(CP2112_SERIAL_STRING, 62),

	0xC0,
};

struct cp2112_state {
	uint8_t gpio_dir;
	uint8_t gpio_pp;
	uint8_t gpio_out;

	uint8_t smbus_cfg[14];
	uint8_t usb_cfg[10];
	uint8_t manuf_str[63];
	uint8_t product_str[63];
	uint8_t serial_str[63];

	atomic_t status0;
	uint8_t status1;
	uint16_t length;
	uint16_t read_offset;
	uint8_t read_buf[MAX_XFER_LEN];

	uint8_t xfer_type;
	uint8_t slave;
	uint16_t read_len;
	uint16_t write_len;
	uint8_t write_buf[MAX_WR_BUF];
};

static struct cp2112_state state;

static K_SEM_DEFINE(in_ready_sem, 1, 1);
K_MSGQ_DEFINE(cmd_msgq, REPORT_MAX, 32, 4);

K_THREAD_STACK_DEFINE(i2c_wq_stack, 2048);
static struct k_work_q i2c_wq;
static struct k_work i2c_work;

static int send_input_report(const uint8_t *data, size_t len)
{
	int ret;

	if (k_sem_take(&in_ready_sem, K_MSEC(200)) != 0) {
		LOG_WRN("timeout waiting for input report completion");
	}

	ret = hid_device_submit_report(hid_dev, len, data);
	if (ret) {
		k_sem_give(&in_ready_sem);
	}

	return ret;
}

static uint8_t gpio_read_all(void)
{
	uint8_t val = 0U;

	for (int i = 0; i < NUM_GPIOS; i++) {
		int v;

		if (state.gpio_dir & BIT(i)) {
			v = (state.gpio_out >> i) & 0x1;
		} else {
			v = gpio_pin_get_dt(&cp2112_gpios[i]);
			if (v < 0) {
				v = 0;
			}
		}

		if (v) {
			val |= BIT(i);
		}
	}

	return val;
}

static void gpio_apply_config(void)
{
	for (int i = 0; i < NUM_GPIOS; i++) {
		gpio_flags_t flags;

		if (state.gpio_dir & BIT(i)) {
			flags = GPIO_OUTPUT;

			if (!(state.gpio_pp & BIT(i))) {
				flags |= GPIO_OPEN_DRAIN;
			}

			flags |= (state.gpio_out & BIT(i)) ?
				 GPIO_OUTPUT_INIT_HIGH :
				 GPIO_OUTPUT_INIT_LOW;
		} else {
			flags = GPIO_INPUT;
		}

		(void)gpio_pin_configure_dt(&cp2112_gpios[i], flags);
	}
}

static void gpio_apply_set(uint8_t values, uint8_t mask)
{
	for (int i = 0; i < NUM_GPIOS; i++) {
		int v;

		if (!(mask & BIT(i))) {
			continue;
		}

		if (!(state.gpio_dir & BIT(i))) {
			continue;
		}

		v = (values >> i) & 0x1;
		(void)gpio_pin_set_dt(&cp2112_gpios[i], v);

		if (v) {
			state.gpio_out |= BIT(i);
		} else {
			state.gpio_out &= ~BIT(i);
		}
	}
}

static uint8_t map_i2c_error(int err)
{
	switch (err) {
	case -ETIMEDOUT:
		return STATUS1_TIMEOUT_BUS;
	case -EBUSY:
	case -EAGAIN:
		return STATUS1_ARBITRATION_LOST;
	case -EIO:
	default:
		return STATUS1_TIMEOUT_NACK;
	}
}

static void i2c_work_handler(struct k_work *work)
{
	int ret = -EIO;

	ARG_UNUSED(work);

	switch (state.xfer_type) {
	case 0:
		ret = i2c_read(i2c_dev, state.read_buf, state.read_len,
			       state.slave);
		if (ret == 0) {
			state.length = state.read_len;
		}
		break;
	case 1:
		ret = i2c_write(i2c_dev, state.write_buf, state.write_len,
				state.slave);
		if (ret == 0) {
			state.length = state.write_len;
		}
		break;
	case 2:
		ret = i2c_write_read(i2c_dev, state.slave,
				     state.write_buf, state.write_len,
				     state.read_buf, state.read_len);
		if (ret == 0) {
			state.length = state.read_len;
		}
		break;
	default:
		break;
	}

	if (ret == 0) {
		state.status1 = STATUS1_SUCCESS;
		atomic_set(&state.status0, STATUS0_COMPLETE);
		return;
	}

	LOG_WRN("i2c xfer failed: type=%u addr=0x%02x err=%d",
		state.xfer_type, state.slave, ret);
	state.status1 = map_i2c_error(ret);
	atomic_set(&state.status0, STATUS0_ERROR);
}

static void send_status_response(void)
{
	uint8_t buf[7];

	buf[0] = CP2112_TRANSFER_STATUS_RESPONSE;
	buf[1] = (uint8_t)atomic_get(&state.status0);
	buf[2] = state.status1;
	sys_put_be16(0, &buf[3]);
	sys_put_be16(state.length, &buf[5]);

	(void)send_input_report(buf, sizeof(buf));
}

static void send_read_response(uint16_t requested)
{
	uint8_t buf[64];
	uint16_t remaining = 0U;
	uint16_t chunk;

	if (state.length > state.read_offset) {
		remaining = state.length - state.read_offset;
	}

	chunk = MIN(remaining, MIN(requested, (uint16_t)61));

	memset(buf, 0, sizeof(buf));
	buf[0] = CP2112_DATA_READ_RESPONSE;
	buf[1] = (chunk == 0U) ? STATUS0_IDLE : STATUS0_COMPLETE;
	buf[2] = (uint8_t)chunk;

	if (chunk != 0U) {
		memcpy(&buf[3], &state.read_buf[state.read_offset], chunk);
		state.read_offset += chunk;
	}

	(void)send_input_report(buf, sizeof(buf));
}

static void start_read(const uint8_t *data)
{
	struct k_work_sync sync;

	k_work_flush(&i2c_work, &sync);

	state.slave = data[1] >> 1;
	state.read_len = sys_get_be16(&data[2]);
	if (state.read_len > MAX_XFER_LEN) {
		state.read_len = MAX_XFER_LEN;
	}

	state.write_len = 0;
	state.read_offset = 0;
	state.length = 0;
	state.status1 = 0;
	state.xfer_type = 0;

	atomic_set(&state.status0, STATUS0_BUSY);
	(void)k_work_submit_to_queue(&i2c_wq, &i2c_work);
}

static void start_write_read(const uint8_t *data)
{
	struct k_work_sync sync;

	k_work_flush(&i2c_work, &sync);

	state.slave = data[1] >> 1;
	state.read_len = sys_get_be16(&data[2]);
	state.write_len = data[4];

	if (state.read_len > MAX_XFER_LEN) {
		state.read_len = MAX_XFER_LEN;
	}

	if (state.write_len > 16) {
		state.write_len = 16;
	}

	memcpy(state.write_buf, &data[5], state.write_len);

	state.read_offset = 0;
	state.length = 0;
	state.status1 = 0;
	state.xfer_type = 2;

	atomic_set(&state.status0, STATUS0_BUSY);
	(void)k_work_submit_to_queue(&i2c_wq, &i2c_work);
}

static void start_write(const uint8_t *data)
{
	struct k_work_sync sync;

	k_work_flush(&i2c_work, &sync);

	state.slave = data[1] >> 1;
	state.write_len = data[2];
	if (state.write_len > 61) {
		state.write_len = 61;
	}

	memcpy(state.write_buf, &data[3], state.write_len);

	state.read_len = 0;
	state.read_offset = 0;
	state.length = 0;
	state.status1 = 0;
	state.xfer_type = 1;

	atomic_set(&state.status0, STATUS0_BUSY);
	(void)k_work_submit_to_queue(&i2c_wq, &i2c_work);
}

static void handle_cancel(void)
{
	struct k_work_sync sync;

	k_work_flush(&i2c_work, &sync);

	atomic_set(&state.status0, STATUS0_IDLE);
	state.status1 = 0;
	state.length = 0;
	state.read_offset = 0;
}

static void handle_output_report(const uint8_t *data)
{
	switch (data[0]) {
	case CP2112_DATA_READ_REQUEST:
		start_read(data);
		break;
	case CP2112_DATA_WRITE_READ_REQUEST:
		start_write_read(data);
		break;
	case CP2112_DATA_WRITE_REQUEST:
		start_write(data);
		break;
	case CP2112_DATA_READ_FORCE_SEND:
		send_read_response(sys_get_be16(&data[1]));
		break;
	case CP2112_TRANSFER_STATUS_REQUEST:
		send_status_response();
		break;
	case CP2112_CANCEL_TRANSFER:
		handle_cancel();
		break;
	default:
		LOG_WRN("unknown output report 0x%02x", data[0]);
		break;
	}
}

static void on_iface_ready(const struct device *dev, bool ready)
{
	ARG_UNUSED(dev);

	LOG_INF("HID interface %s", ready ? "ready" : "down");
}

static int on_get_report(const struct device *dev, uint8_t type, uint8_t id,
			 uint16_t len, uint8_t *buf)
{
	ARG_UNUSED(dev);

	if (type != HID_REPORT_TYPE_FEATURE) {
		return -ENOTSUP;
	}

	switch (id) {
	case CP2112_GPIO_CONFIG:
		if (len < 5) {
			return -EINVAL;
		}

		buf[0] = CP2112_GPIO_CONFIG;
		buf[1] = state.gpio_dir;
		buf[2] = state.gpio_pp;
		buf[3] = 0;
		buf[4] = 0;
		return 5;

	case CP2112_GPIO_GET:
		if (len < 2) {
			return -EINVAL;
		}

		buf[0] = CP2112_GPIO_GET;
		buf[1] = gpio_read_all();
		return 2;

	case CP2112_GET_VERSION_INFO:
		if (len < 3) {
			return -EINVAL;
		}

		buf[0] = CP2112_GET_VERSION_INFO;
		buf[1] = 0x0C;
		buf[2] = 0x02;
		return 3;

	case CP2112_SMBUS_CONFIG:
		if (len < sizeof(state.smbus_cfg)) {
			return -EINVAL;
		}

		memcpy(buf, state.smbus_cfg, sizeof(state.smbus_cfg));
		return sizeof(state.smbus_cfg);

	case CP2112_LOCK_BYTE:
		if (len < 2) {
			return -EINVAL;
		}

		buf[0] = CP2112_LOCK_BYTE;
		buf[1] = 0xFF;
		return 2;

	case CP2112_USB_CONFIG:
		if (len < sizeof(state.usb_cfg)) {
			return -EINVAL;
		}

		memcpy(buf, state.usb_cfg, sizeof(state.usb_cfg));
		return sizeof(state.usb_cfg);

	case CP2112_MANUFACTURER_STRING: {
		size_t copy = MIN((size_t)len, sizeof(state.manuf_str));

		memcpy(buf, state.manuf_str, copy);
		return copy;
	}

	case CP2112_PRODUCT_STRING: {
		size_t copy = MIN((size_t)len, sizeof(state.product_str));

		memcpy(buf, state.product_str, copy);
		return copy;
	}

	case CP2112_SERIAL_STRING: {
		size_t copy = MIN((size_t)len, sizeof(state.serial_str));

		memcpy(buf, state.serial_str, copy);
		return copy;
	}

	default:
		LOG_WRN("unsupported get_report id 0x%02x", id);
		return -ENOTSUP;
	}
}

static const uint8_t *report_payload(const uint8_t *buf, uint16_t len,
				     uint8_t id, size_t expected)
{
	if (len == expected) {
		return buf;
	}

	if ((len == expected + 1U) && (buf[0] == id)) {
		return buf + 1;
	}

	return NULL;
}

static int on_set_report(const struct device *dev, uint8_t type, uint8_t id,
			 uint16_t len, const uint8_t *buf)
{
	const uint8_t *p;

	ARG_UNUSED(dev);

	if (type != HID_REPORT_TYPE_FEATURE) {
		return -ENOTSUP;
	}

	switch (id) {
	case CP2112_GPIO_CONFIG:
		p = report_payload(buf, len, id, 4);
		if (p == NULL) {
			return -EINVAL;
		}

		state.gpio_dir = p[0];
		state.gpio_pp = p[1];
		gpio_apply_config();
		return 0;

	case CP2112_GPIO_SET:
		p = report_payload(buf, len, id, 2);
		if (p == NULL) {
			return -EINVAL;
		}

		gpio_apply_set(p[0], p[1]);
		return 0;

	case CP2112_SMBUS_CONFIG:
		p = report_payload(buf, len, id, 13);
		if (p == NULL) {
			return -EINVAL;
		}

		state.smbus_cfg[0] = CP2112_SMBUS_CONFIG;
		memcpy(&state.smbus_cfg[1], p, 13);
		return 0;

	case CP2112_USB_CONFIG:
		p = report_payload(buf, len, id, 9);
		if (p == NULL) {
			return -EINVAL;
		}

		state.usb_cfg[0] = CP2112_USB_CONFIG;
		memcpy(&state.usb_cfg[1], p, 9);
		return 0;

	case CP2112_MANUFACTURER_STRING:
	case CP2112_PRODUCT_STRING:
	case CP2112_SERIAL_STRING: {
		uint8_t *dst =
			(id == CP2112_MANUFACTURER_STRING) ? state.manuf_str :
			(id == CP2112_PRODUCT_STRING) ? state.product_str :
							state.serial_str;
		size_t cap = sizeof(state.manuf_str);
		size_t copy = MIN((size_t)len, cap);

		memset(dst, 0, cap);

		if ((len > 0U) && (buf[0] == id)) {
			memcpy(dst, buf, copy);
		} else {
			dst[0] = id;
			memcpy(dst + 1, buf, MIN(copy, cap - 1));
		}

		return 0;
	}

	default:
		LOG_WRN("unsupported set_report id 0x%02x", id);
		return -ENOTSUP;
	}
}

static void on_input_report_done(const struct device *dev,
				 const uint8_t *report)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(report);

	k_sem_give(&in_ready_sem);
}

static void on_output_report(const struct device *dev, uint16_t len,
			     const uint8_t *buf)
{
	uint8_t cmd[REPORT_MAX] = { 0 };
	size_t n = MIN((size_t)len, sizeof(cmd));

	ARG_UNUSED(dev);

	if (n == 0U) {
		return;
	}

	memcpy(cmd, buf, n);

	if (k_msgq_put(&cmd_msgq, cmd, K_NO_WAIT) != 0) {
		LOG_WRN("cmd queue full, dropped report 0x%02x", cmd[0]);
	}
}

static const struct hid_device_ops hid_ops = {
	.iface_ready = on_iface_ready,
	.get_report = on_get_report,
	.set_report = on_set_report,
	.input_report_done = on_input_report_done,
	.output_report = on_output_report,
};

static void cmd_thread(void *a, void *b, void *c)
{
	uint8_t buf[REPORT_MAX];

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		if (k_msgq_get(&cmd_msgq, buf, K_FOREVER) == 0) {
			handle_output_report(buf);
		}
	}
}

K_THREAD_DEFINE(cmd_tid, 2048, cmd_thread, NULL, NULL, NULL, 6, 0, 0);

static void fill_string_report(uint8_t *dst, uint8_t id, const char *s)
{
	size_t i = 0U;

	memset(dst, 0, 63);
	dst[0] = id;
	dst[2] = 0x03;

	while (s[i] && i < 14) {
		dst[3 + i * 4 + 0] = (uint8_t)s[i];
		dst[3 + i * 4 + 1] = 0;
		dst[3 + i * 4 + 2] = 0;
		dst[3 + i * 4 + 3] = 0;
		i++;
	}

	dst[1] = (uint8_t)(i * 4 + 2);
}

static void init_default_configs(void)
{
	memset(state.smbus_cfg, 0, sizeof(state.smbus_cfg));
	state.smbus_cfg[0] = CP2112_SMBUS_CONFIG;
	sys_put_be32(CP2112_I2C_CLOCK_HZ, &state.smbus_cfg[1]);
	state.smbus_cfg[5] = 0x02;
	state.smbus_cfg[6] = 0x00;
	sys_put_be16(1000, &state.smbus_cfg[7]);
	sys_put_be16(1000, &state.smbus_cfg[9]);
	state.smbus_cfg[11] = 0x00;
	sys_put_be16(1, &state.smbus_cfg[12]);

	memset(state.usb_cfg, 0, sizeof(state.usb_cfg));
	state.usb_cfg[0] = CP2112_USB_CONFIG;
	sys_put_le16(0x10C4, &state.usb_cfg[1]);
	sys_put_le16(0xEA90, &state.usb_cfg[3]);
	state.usb_cfg[5] = 50;
	state.usb_cfg[6] = 0x00;
	state.usb_cfg[7] = 0x01;
	state.usb_cfg[8] = 0x00;
	state.usb_cfg[9] = 0x00;

	fill_string_report(state.manuf_str, CP2112_MANUFACTURER_STRING, "Zephyr");
	fill_string_report(state.product_str, CP2112_PRODUCT_STRING,
			   "CP2112 Emul");
	fill_string_report(state.serial_str, CP2112_SERIAL_STRING, "0001");

	state.gpio_dir = 0x00;
	state.gpio_pp = 0xFF;
	state.gpio_out = 0x00;
}

static int usbd_init_and_enable(void)
{
	int err;

	err = usbd_add_descriptor(&usbd_ctx, &lang_desc);
	if (err) {
		LOG_ERR("usbd_add_descriptor(lang): %d", err);
		return err;
	}

	err = usbd_add_descriptor(&usbd_ctx, &mfr_desc);
	if (err) {
		LOG_ERR("usbd_add_descriptor(mfr): %d", err);
		return err;
	}

	err = usbd_add_descriptor(&usbd_ctx, &product_desc);
	if (err) {
		LOG_ERR("usbd_add_descriptor(product): %d", err);
		return err;
	}

	err = usbd_add_descriptor(&usbd_ctx, &sn_desc);
	if (err) {
		LOG_ERR("usbd_add_descriptor(sn): %d", err);
		return err;
	}

	err = usbd_add_configuration(&usbd_ctx, USBD_SPEED_FS, &fs_config);
	if (err) {
		LOG_ERR("usbd_add_configuration: %d", err);
		return err;
	}

	err = usbd_register_all_classes(&usbd_ctx, USBD_SPEED_FS, 1, NULL);
	if (err) {
		LOG_ERR("usbd_register_all_classes: %d", err);
		return err;
	}

	err = usbd_init(&usbd_ctx);
	if (err) {
		LOG_ERR("usbd_init: %d", err);
		return err;
	}

	err = usbd_enable(&usbd_ctx);
	if (err) {
		LOG_ERR("usbd_enable: %d", err);
		return err;
	}

	return 0;
}

int main(void)
{
	int ret;

	LOG_INF("CP2112 emulator starting");

	if (!device_is_ready(i2c_dev)) {
		LOG_ERR("I2C bus %s not ready", i2c_dev->name);
		return -ENODEV;
	}

	for (int i = 0; i < NUM_GPIOS; i++) {
		if (!gpio_is_ready_dt(&cp2112_gpios[i])) {
			LOG_ERR("gpio[%d] not ready", i);
			return -ENODEV;
		}
	}

	if (!device_is_ready(hid_dev)) {
		LOG_ERR("HID device not ready");
		return -ENODEV;
	}

	init_default_configs();
	gpio_apply_config();

	k_work_queue_init(&i2c_wq);
	k_work_queue_start(&i2c_wq, i2c_wq_stack,
			   K_THREAD_STACK_SIZEOF(i2c_wq_stack),
			   4, NULL);
	k_thread_name_set(&i2c_wq.thread, "cp2112_i2c");
	k_work_init(&i2c_work, i2c_work_handler);

	ret = hid_device_register(hid_dev, cp2112_report_desc,
				  sizeof(cp2112_report_desc), &hid_ops);
	if (ret) {
		LOG_ERR("hid_device_register failed: %d", ret);
		return ret;
	}

	ret = usbd_init_and_enable();
	if (ret) {
		return ret;
	}

	LOG_INF("CP2112 emulator ready (i2c=%s, %d gpios)",
		i2c_dev->name, NUM_GPIOS);

	return 0;
}
