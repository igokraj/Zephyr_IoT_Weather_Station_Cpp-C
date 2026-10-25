cmake_minimum_required(VERSION 3.28.0)

set(BOARD_FLASH_RUNNER openocd)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(IoT_WeatherStation)

target_include_directories(app PRIVATE include)

target_sources(app PRIVATE 
src/main.c 
src/formulas.c 
src/wifi.c
src/mqtt.c
src/init.c
src/creds/ca.c
src/creds/cert.c
src/creds/key.c)