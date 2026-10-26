#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2c.h>

// HTU21D
#include <zephyr/drivers/sensor.h>
// MAX17048
#include <zephyr/drivers/fuel_gauge.h>

#include <errno.h>
#include <zephyr/sys/poweroff.h>
#include <esp_sleep.h>
#include <stdio.h>

// RTC memory
#include <esp_attr.h>

// User .h files
#include "formulas.h"
#include "wifi.h"
#include "mqtt.h"
#include "init.h"
#include "watchdog.h"

// **** Additional options in the code ****

// 1) Status diode -> enable LED_DIODE in CMakeLists.txt
// 2) Read the battery only every BATTERY_READ_EVERY-th wake-up -> enable both
//    WAKE_COUNT and BATTERY_READ_EVERY in CMakeLists.txt

#if defined(WAKE_COUNT) && !defined(BATTERY_READ_EVERY)
#error "WAKE_COUNT needs BATTERY_READ_EVERY to be defined"
#endif

#define wakeup_time_sec 600

#ifdef LED_DIODE
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(status_led), gpios);
#endif
static const struct device *htu = DEVICE_DT_GET(DT_NODELABEL(htu21d));
static const struct device *max = DEVICE_DT_GET(DT_NODELABEL(max17048));

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

struct htu21d_data
{
	struct sensor_value temperature;
	struct sensor_value humidity;
};

// Functions definitions
static int htu21d_read(struct htu21d_data *data);
static void Deep_sleep(void);

#ifdef WAKE_COUNT
static RTC_DATA_ATTR uint32_t wake_count;
#endif
/*
 Keeping ptc in RTC memory means it survives deep sleep, so MQTT can send
 the last battery % reading on wake-ups where the battery is not read
 (only used when WAKE_COUNT is defined, otherwise it is read every wake-up)
*/
static RTC_DATA_ATTR int ptc = -1;

int main(void)
{
	struct htu21d_data data;
	double dew_point;
	double cloud_height;
	union fuel_gauge_prop_val val;

	if (watchdog_init() != 0)
	{
		Deep_sleep();
	}

#ifdef LED_DIODE
	if (gpio_init() == 0)
	{
		gpio_pin_set_dt(&led, 1);
	}
	else
	{
		Deep_sleep();
	}
#endif

	if (htu21d_init() != 0)
	{
		Deep_sleep();
	}

	// Do not disturb the whole process, just return an error
	if (max17048_init() != 0)
	{
		LOG_ERR("Battery level sensor failed...");
	}

#ifdef WAKE_COUNT
	wake_count++;
	// Read the % of the battery once every BATTERY_READ_EVERYth wake up times
	if ((wake_count - 1) % BATTERY_READ_EVERY == 0)
	{
#endif
		if (fuel_gauge_get_prop(max, FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE_PCT, &val) == 0)
		{
			ptc = val.relative_state_of_charge_pct;
			LOG_INF("Battery percent is: [%d%%]", ptc);
		}
		else
		{
			ptc = -1;
			LOG_ERR("Battery read failed...");
		}

#ifdef WAKE_COUNT
	}
#endif

	// Buffers for the MQTT messages
	char payload[128];
	char payload2[64];

	/* There is no point in connecting to Wi-Fi if the sensor is not working,
	   just go to sleep */
	if (htu21d_read(&data) != 0)
	{
		LOG_ERR("Sensor failed -> skipping Wi-Fi and MQTT, going to sleep...");
		Deep_sleep();
	}

	// Data conversion: val1 + val2 -> double temp/hum
	double temp = sensor_value_to_double(&data.temperature);
	double hum = sensor_value_to_double(&data.humidity);

	if (dew_point_calculation(temp, hum, &dew_point) == 0)
	{
		cloud_height_calculation(temp, dew_point, &cloud_height);
		LOG_INF("Temperature: %.2f, Humidity: %.2f, Dew point: %.2f, Cloud height: %.2f",
				temp, hum, dew_point, cloud_height);

		snprintf(payload, sizeof(payload),
				 "{\"temperature\":%.2f,\"humidity\":%.2f,"
				 "\"dew_point\":%.2f,\"cloud_height\":%.2f}",
				 temp, hum, dew_point, cloud_height);
	}
	else
	{
		LOG_ERR("Dew point calculation failed, sending temp and hum only...");

		snprintf(payload, sizeof(payload),
				 "{\"temperature\":%.2f,\"humidity\":%.2f}", temp, hum);
	}

	snprintf(payload2, sizeof(payload2),
			 "{\"battery percent\":%d}", ptc);

	if (wifi_connect() != 0)
	{
		Deep_sleep();
	}

	if (aws_mqtt_send(payload, payload2) != 0)
	{
		LOG_ERR("MQTT message send failed");
	}

	Deep_sleep();
	return 0;
}

static int htu21d_read(struct htu21d_data *data)
{
	int ret = sensor_sample_fetch(htu);
	if (ret < 0)
	{
		LOG_ERR("HTU21D fetch failed (err %d)", ret);
		return ret;
	}

	ret = sensor_channel_get(htu, SENSOR_CHAN_AMBIENT_TEMP, &data->temperature);
	if (ret < 0)
	{
		LOG_WRN("Temperature measurement failed...");
		return ret;
	}

	ret = sensor_channel_get(htu, SENSOR_CHAN_HUMIDITY, &data->humidity);
	if (ret < 0)
	{
		LOG_WRN("Humidity measurement failed...");
		return ret;
	}
	return 0;
}

static void Deep_sleep(void)
{
	LOG_INF("Enabling timer wakeup, %ds", wakeup_time_sec);
	esp_sleep_enable_timer_wakeup(wakeup_time_sec * 1000000);
	LOG_INF("Powering off");
	sys_poweroff();
}
