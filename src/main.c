#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <math.h>
#include <errno.h>
#include <zephyr/sys/poweroff.h>
#include <esp_sleep.h>
#include <stdio.h>

// User .h files
#include "formulas.h"
#include "wifi.h"
#include "mqtt.h"
#include "init.h"

#define wakeup_time_sec 20

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(status_led), gpios);
static const struct device *htu = DEVICE_DT_GET(DT_NODELABEL(htu21d));
static const struct device *max = DEVICE_DT_GET(DT_NODELABEL(htu21d));

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

struct htu21d_data {
		struct sensor_value temperature;
		struct sensor_value humidity;
	};

// Functions definitions
static int htu21d_read(struct  htu21d_data *data);
static void Deep_sleep(void);

int main(void)
{
	struct htu21d_data data;
	double dew_point;
	double cloud_height;

	if (gpio_init() == 0) {
	gpio_pin_set_dt(&led, 1);
	}
	else {
	Deep_sleep();
	}

	if (htu21d_init() != 0) {
	Deep_sleep();
	}

	// Do not disturb the whole process, just return an error
	if (max17048_init() != 0) {
	LOG_ERR("Battery level sensor failed...");
	}

	if (wifi_connect() != 0) {
	Deep_sleep();
	}
	
	if (htu21d_read(&data) == 0){
			// Data conversion: val1 + val2 -> double temp/hum
			double temp = sensor_value_to_double(&data.temperature);
			double hum = sensor_value_to_double(&data.humidity);
		
			if (dew_point_calculation(temp, hum, &dew_point) == 0) {
			cloud_height_calculation(temp, dew_point, &cloud_height);
			LOG_INF("Temperature: %.2f, Humidity: %.2f, Dew point: %.2f, Cloud height: %.2f",
			temp, hum, dew_point, cloud_height);

			char payload[128];
			snprintf(payload, sizeof(payload),
			"{\"temperature\":%.2f,\"humidity\":%.2f,"
				"\"dew_point\":%.2f,\"cloud_height\":%.2f}",
				temp, hum, dew_point, cloud_height);

			if (aws_mqtt_send(payload) != 0) {
				LOG_ERR("MQTT message send failed");
			}
		}
	}
	else {
	LOG_ERR("Sensor failed...");
	}
	Deep_sleep();
	return 0;
}



static int htu21d_read(struct  htu21d_data *data)
{
	int ret = sensor_sample_fetch(htu);
	if (ret < 0) {
		LOG_ERR("HTU21D fetch failed (err %d)", ret);
		return ret;
	}

	ret = sensor_channel_get(htu, SENSOR_CHAN_AMBIENT_TEMP, &data->temperature);
	if (ret < 0) {
		LOG_WRN("Temperature measurement failed...");
		return ret;
	}

	ret = sensor_channel_get(htu, SENSOR_CHAN_HUMIDITY, &data->humidity);
	if (ret < 0) {
		LOG_WRN("Humidity measurement failed...");
		return ret;
	}
	return 0;
}

static void Deep_sleep(void) {
	LOG_INF("Enabling timer wakeup, %ds", wakeup_time_sec);
	esp_sleep_enable_timer_wakeup(wakeup_time_sec * 1000000);
	LOG_INF("Powering off");
	sys_poweroff();
}



