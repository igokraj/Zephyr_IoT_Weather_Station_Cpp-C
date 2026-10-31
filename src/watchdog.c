#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>

#include "watchdog.h"

static const struct device *wdt = DEVICE_DT_GET(DT_NODELABEL(wdt0));

static int wdt_channel;

static struct wdt_timeout_cfg wdt_cfg = {
    .window.min = 0,
    .window.max = 30000, // 30000 -> 30 s 
    .callback = NULL,
    .flags = WDT_FLAG_RESET_SOC
};


int watchdog_init(void) {
    if (!device_is_ready(wdt)) {
        return -1;
    }

    wdt_channel = wdt_install_timeout(wdt, &wdt_cfg);
    if (wdt_channel < 0 || wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG) < 0) {
        return -1;
    }

    return 0;
}