#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <math.h>
#include <errno.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

// Devices definition
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(status_led), gpios);
static const struct device *htu = DEVICE_DT_GET(DT_NODELABEL(htu21d));

struct htu21d_data {
		struct sensor_value temperature;
		struct sensor_value humidity;
	};

// Functions definitions
static int gpio_init(void);
static int htu21d_init(void);
static int htu21d_read(struct  htu21d_data *data);
static int dew_point_calculation(double t, double h, double *dew_point);
static int cloud_height_calculation(double t, double dew_point, double *cloud_height);


int main(void)
{
	struct htu21d_data data;
	double dew_point;
	double cloud_height;

	int ret = gpio_init();
	if (ret < 0) {
		return ret;
	}

	ret = htu21d_init();
	if (ret < 0) {
		return ret;
	}
	
	while(1) {
		gpio_pin_toggle_dt(&led);
		if (htu21d_read(&data) == 0){
			// Data conversion: val1 + val2 -> double temp/hum
			double temp = sensor_value_to_double(&data.temperature);
			double hum = sensor_value_to_double(&data.humidity);
		
			if (dew_point_calculation(temp, hum, &dew_point) == 0) {
			cloud_height_calculation(temp, dew_point, &cloud_height);
			LOG_INF("Temperature: %.2f, Humidity: %.2f, Dew point: %.2f, Cloud height: %.2f",
			temp, hum, dew_point, cloud_height);
		}
	}
	
		k_sleep(K_SECONDS(3));
	}


	return 0;
}

static int gpio_init(void) {
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

static int htu21d_init(void) {
if (!device_is_ready(htu)) {
	LOG_ERR("HTU21D device is not ready");
	return -ENODEV;
}
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

// Obliczanie punktu rosy
// Magnus-Tetens formula
static int dew_point_calculation(double t, double h, double  *dew_point) {
if (h > 100 || h <= 0) {
LOG_WRN("Data for the dew point calculation is not valid");
return -EINVAL;
}
else {
const double a = 17.625;
const double b = 243.04;

double alpha = ((a * t) / (b + t)) + log(h / 100);

*dew_point = ((b * alpha) / (a - alpha));
return 0;
}
}

static int cloud_height_calculation(double t, double dew_point, double *cloud_height) {
	if (dew_point > t) {
		*cloud_height = 0.0;
		return 0.0;
	}
	*cloud_height = (t - dew_point) * 125;
	return 0;
}

