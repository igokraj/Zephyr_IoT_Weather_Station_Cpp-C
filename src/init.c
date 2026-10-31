#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <errno.h>
#include <stdio.h>

// User .h files
#include "init.h"


LOG_MODULE_REGISTER(init, LOG_LEVEL_DBG);

// Devices definition
#ifdef LED_DIODE
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(status_led), gpios);
#endif
static const struct gpio_dt_spec rain = GPIO_DT_SPEC_GET(DT_ALIAS(rain_status), gpios);
static const struct device *htu = DEVICE_DT_GET(DT_NODELABEL(htu21d));
static const struct device *max = DEVICE_DT_GET(DT_NODELABEL(max17048));
static const struct device *bmp = DEVICE_DT_GET(DT_NODELABEL(bmp280));
static const struct device *bh = DEVICE_DT_GET(DT_NODELABEL(bh1750));

#ifdef LED_DIODE
int gpio_init(void) {
if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("Status LED device is not ready...");
		return -ENODEV;
	}
	int ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Status LED config failed (err %d)", ret);
		return ret;
	}
	return 0;
}
#endif

int rain_init(void) {
if (!gpio_is_ready_dt(&rain)) {
		LOG_ERR("Rain device is not ready...");
		return -ENODEV;
	}
	int ret = gpio_pin_configure_dt(&rain, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Rain device config failed (err %d)", ret);
		return ret;
	}
	return 0;
}

int htu21d_init(void) {
if (!device_is_ready(htu)) {
	LOG_ERR("HTU21D device is not ready");
	return -ENODEV;
}
return 0;
}

int bmp280_init(void) {
if (!device_is_ready(bmp)) {
	LOG_ERR("BMP280 device is not ready");
	return -ENODEV;
}
return 0;
}

int max17048_init(void) {
if (!device_is_ready(max)) {
	LOG_ERR("MAX17048 device is not ready");
	return -ENODEV;
}
return 0;
}

int bh1750_init(void) {
if (!device_is_ready(bh)) {
	LOG_ERR("BH1750 device is not ready");
	return -ENODEV;
}
return 0;
}