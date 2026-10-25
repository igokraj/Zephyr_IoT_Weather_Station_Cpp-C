#include <zephyr/logging/log.h>
#include <math.h>
#include <errno.h>
#include <formulas.h>

LOG_MODULE_REGISTER(formulas, LOG_LEVEL_DBG);

// Obliczanie punktu rosy
// Magnus-Tetens formula
int dew_point_calculation(double t, double h, double  *dew_point) {
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

// Wzor na wysokosc podstawy chmur
int cloud_height_calculation(double t, double dew_point, double *cloud_height) {
	if (dew_point > t) {
		*cloud_height = 0.0;
		return 0;
	}
	*cloud_height = (t - dew_point) * 125;
	return 0;
}