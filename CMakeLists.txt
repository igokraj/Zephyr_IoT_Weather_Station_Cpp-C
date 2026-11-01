cmake_minimum_required(VERSION 3.28.0)

set(BOARD_FLASH_RUNNER openocd)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(IoT_WeatherStation)

target_include_directories(app PRIVATE include)

target_sources(app PRIVATE 
src/main.cpp 
src/formulas.c 
src/wifi.c
src/mqtt.c
src/init.c
src/watchdog.c
src/creds/ca.c
src/creds/cert.c
src/creds/key.c)

# Optional features:
target_compile_definitions(app PRIVATE
	# LED_DIODE               # status LED on during the active phase
	# WAKE_COUNT              # read the battery only every BATTERY_READ_EVERY-th wake-up
	# BATTERY_READ_EVERY=10   # needed together with WAKE_COUNT
)