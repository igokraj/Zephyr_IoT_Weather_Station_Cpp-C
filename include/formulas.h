#pragma once 

#ifdef __cplusplus
extern "C"
{
#endif

int cloud_height_calculation(double t, double dew_point, double *cloud_height);
int dew_point_calculation(double t, double h, double  *dew_point);

#ifdef __cplusplus
}
#endif