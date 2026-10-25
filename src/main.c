#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);


// GPIO definition
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(status_led), gpios);

int gpio_init(void);

int main(void)
{
	gpio_init();
	
	while(1) {
		gpio_pin_toggle_dt(&led);
		k_sleep(K_MSEC(100));
	}

	return 0;
}

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


