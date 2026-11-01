#pragma once 

#ifdef __cplusplus
extern "C"
{
#endif

int gpio_init(void);
int max17048_init(void);
int rain_init(void);
int adc_init(void);

#ifdef __cplusplus
}
#endif