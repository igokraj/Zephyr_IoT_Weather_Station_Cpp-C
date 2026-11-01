#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

// Json Parsing
#include <zephyr/data/json.h>
#include <string.h>

// HTU21D BMP280 BH1750
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

#ifdef LED_DIODE
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(status_led), gpios);
#endif
static const struct gpio_dt_spec rain = GPIO_DT_SPEC_GET(DT_ALIAS(rain_status), gpios);
static const struct device *max = DEVICE_DT_GET(DT_NODELABEL(max17048));

/* Data of ADC io-channels specified in devicetree. */
static const struct adc_dt_spec rain_adc = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

// Functions definitions
static void Deep_sleep(void);
static int rain_adc_read(void);

#ifdef WAKE_COUNT
static RTC_DATA_ATTR uint32_t wake_count;
#endif
/*
 Keeping ptc in RTC memory means it survives deep sleep, so MQTT can send
 the last battery % reading on wake-ups where the battery is not read
 (only used when WAKE_COUNT is defined, otherwise it is read every wake-up)
*/
static RTC_DATA_ATTR int ptc = -1;

/* Wake up time kept in RTC memory, so a new interval from AWS survives deep sleep */
static RTC_DATA_ATTR uint32_t wakeup_time_sec = 60;

struct wake_time
{
	int32_t interval;
};

static const struct json_obj_descr command_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct wake_time, interval, JSON_TOK_NUMBER),
};

class Sensor
{

public:
	explicit Sensor(const struct device *dev) : dev_(dev) {}

	bool ready() const
	{
		return device_is_ready(dev_);
	}

	int fetch() const
	{
		return sensor_sample_fetch(dev_);
	}

	int get(enum sensor_channel chan, double &out) const
	{
		struct sensor_value val;

		int ret = sensor_channel_get(dev_, chan, &val);
		if (ret == 0)
		{
			out = sensor_value_to_double(&val);
		}
		return ret;
	}

private:
	const struct device *dev_;
};

int main(void)
{

	Sensor htu(DEVICE_DT_GET(DT_NODELABEL(htu21d)));
	Sensor bmp(DEVICE_DT_GET(DT_NODELABEL(bmp280)));
	Sensor bh(DEVICE_DT_GET(DT_NODELABEL(bh1750)));

	double temp, hum, press, lux;
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

	if (rain_init() != 0)
	{
		Deep_sleep();
	}

	if (adc_init() != 0)
	{
		Deep_sleep();
	}

	// Check the devices (htu, bmp, bh)
	if (!htu.ready() || !bmp.ready() || !bh.ready())
	{
		Deep_sleep();
	}

	// Get the measurements from 3 sensors (htu, bmp, bh)
	if (htu.fetch() != 0 ||
		htu.get(SENSOR_CHAN_AMBIENT_TEMP, temp) != 0 ||
		htu.get(SENSOR_CHAN_HUMIDITY, hum) != 0 ||
		bmp.fetch() != 0 ||
		bmp.get(SENSOR_CHAN_PRESS, press) != 0 ||
		bh.fetch() != 0 ||
		bh.get(SENSOR_CHAN_LIGHT, lux) != 0)
	{
		LOG_ERR("Sensor read failed -> skipping Wi-Fi and MQTT, going to sleep...");
		Deep_sleep();
	}
	press *= 10.0; // kPa -> hPa

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
	char payload[156];
	char payload2[64];

	bool rain_state = (gpio_pin_get_dt(&rain));
	int rain_mv = rain_adc_read();

	if (rain_mv < 0)
	{
		LOG_ERR("Rain ADC failed -> skipping Wi-Fi and MQTT, going to sleep...");
		Deep_sleep();
	}

	if (dew_point_calculation(temp, hum, &dew_point) == 0)
	{
		cloud_height_calculation(temp, dew_point, &cloud_height);
		LOG_INF("Lux: %.2f, Pressure: %.2f, Temperature: %.2f, Humidity: %.2f, Dew point: %.2f, Cloud height: %.2f, Rain: %d, Rain_ADC: %d",
				lux, press, temp, hum, dew_point, cloud_height, rain_state, rain_mv);

		snprintf(payload, sizeof(payload),
				 "{\"lux\":%.2f, \"pressure\":%.2f, \"temperature\":%.2f,\"humidity\":%.2f,"
				 "\"dew_point\":%.2f,\"cloud_height\":%.2f, \"Rain\":%d, \"Rain_ADC\":%d}",
				 lux, press, temp, hum, dew_point, cloud_height, rain_state, rain_mv);
	}
	else
	{
		LOG_ERR("Dew point calculation failed, sending other parameters...");

		snprintf(payload, sizeof(payload),
				 "{\"lux\":%.2f, \"pressure\":%.2f, \"temperature\":%.2f,\"humidity\":%.2f,\"Rain\":%d, \"Rain_ADC\":%d}",
				 lux, press, temp, hum, rain_state, rain_mv);
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

	// Receive a message from AWS
	char *message = aws_mqtt_get_message();

	// Accept the message only if these conditions below are true
	if (message != NULL)
	{
		struct wake_time time;
		int64_t ret = json_obj_parse(message, strlen(message),
									 command_descr, ARRAY_SIZE(command_descr), &time);

		// The possible duration of deep sleep ranges from min. 1 minute to max. 60 minutes
		if (ret > 0 && (ret & BIT(0)) && time.interval >= 60 && time.interval <= 3600)
		{
			wakeup_time_sec = time.interval;
			LOG_INF("New wake up time is %u [s]", wakeup_time_sec);
		}
		else
		{
			LOG_ERR("Invalid command from AWS...");
		}
	}

	Deep_sleep();
	return 0;
}

static void Deep_sleep(void)
{
	LOG_INF("Enabling timer wakeup, %us", wakeup_time_sec);
	esp_sleep_enable_timer_wakeup((uint64_t)wakeup_time_sec * 1000000);
	LOG_INF("Powering off");
	sys_poweroff();
}

static int rain_adc_read(void)
{

	uint32_t buf = 0;
	struct adc_sequence sequence = {
		.buffer = &buf,
		.buffer_size = sizeof(buf),
	};

	adc_sequence_init_dt(&rain_adc, &sequence);

	int ret = adc_read_dt(&rain_adc, &sequence);
	if (ret < 0)
	{
		LOG_ERR("Could not read rain ADC (%d)", ret);
		return ret;
	}

	int32_t val_mv = buf;

	ret = adc_raw_to_millivolts_dt(&rain_adc, &val_mv);
	if (ret < 0)
	{
		return ret;
	}
	return val_mv;
}
