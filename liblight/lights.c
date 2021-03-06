/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "lights"
#include <cutils/log.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <hardware/lights.h>
#include <linux/leds-an30259a.h>

static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

char const *const LCD_FILE = "/sys/class/backlight/s6e8aa0/brightness";
char const *const LED_FILE = "/dev/an30259a_leds";

#define IMAX 0 // 12.75mA power consumption

// Slope values, based on total blink of 1000ms
#define SLOPE_UP_1	450
#define SLOPE_UP_2	(500-SLOPE_UP_1)
#define SLOPE_DOWN_1	SLOPE_UP_2
#define SLOPE_DOWN_2	SLOPE_UP_1
// brightness at mid-slope, on 0 - 127 scale
#define MID_BRIGHTNESS  31

enum LED_Type {
	LED_TYPE_ATTENTION = 0,
	LED_TYPE_NOTIFICATION = 1,
	LED_TYPE_CHARGING = 2,
	LED_TYPE_LAST = 3
};

// a "stack" of virtual LED states
static struct an30259a_pr_control g_led_states[LED_TYPE_LAST];

void init_g_lock(void)
{
	pthread_mutex_init(&g_lock, NULL);
	memset(g_led_states, 0, sizeof(g_led_states));
}

static int write_int(char const *path, int value)
{
	int fd;
	static int already_warned;

	already_warned = 0;

	ALOGV("write_int: path %s, value %d", path, value);
	fd = open(path, O_RDWR);

	if (fd >= 0) {
		char buffer[20];
		int bytes = sprintf(buffer, "%d\n", value);
		int amt = write(fd, buffer, bytes);
		close(fd);
		return amt == -1 ? -errno : 0;
	} else {
		if (already_warned == 0) {
			ALOGE("write_int failed to open %s\n", path);
			already_warned = 1;
		}
		return -errno;
	}
}

static int rgb_to_brightness(struct light_state_t const *state)
{
	int color = state->color & 0x00ffffff;

	return ((77*((color>>16) & 0x00ff))
		+ (150*((color>>8) & 0x00ff)) + (29*(color & 0x00ff))) >> 8;
}

static int set_light_backlight(struct light_device_t *dev __unused,
			struct light_state_t const *state)
{
	int err = 0;
	int brightness = rgb_to_brightness(state);

	pthread_mutex_lock(&g_lock);
	err = write_int(LCD_FILE, brightness);

	pthread_mutex_unlock(&g_lock);
	return err;
}

static int close_lights(struct light_device_t *dev)
{
	ALOGV("close_light is called");
	if (dev)
		free(dev);

	return 0;
}

/* LEDs */
static int write_leds(struct an30259a_pr_control *led)
{
	int err = 0;
	int imax = IMAX;
	int fd;

	pthread_mutex_lock(&g_lock);

	fd = open(LED_FILE, O_RDWR);
	if (fd >= 0) {
		err = ioctl(fd, AN30259A_PR_SET_IMAX, &imax);
		if (err)
			ALOGE("failed to set imax");

		err = ioctl(fd, AN30259A_PR_SET_LED, led);
		if (err < 0)
			ALOGE("failed to set leds!");

		close(fd);
	} else {
		ALOGE("failed to open %s!", LED_FILE);
		err =  -errno;
	}

	pthread_mutex_unlock(&g_lock);

	return err;
}

// similar to write_leds(), but deals with the priority of certain virtual LEDs over others
static int write_leds_priority()
{
	// find the highest priority virtual LED that should be illuminated and
	// call write_leds() with it
	int i;

	for (i = 0; i < LED_TYPE_LAST; i++) {
		// if the LED isn't off and isn't "black" then use it
		if (g_led_states[i].state != LED_LIGHT_OFF) {
			return write_leds(&g_led_states[i]);
		}
	}

	// nothing should be lit?  make sure to turn it off
	return write_leds(&g_led_states[LED_TYPE_LAST - 1]);
}

static int set_light_leds(struct light_state_t const *state, int type)
{
	if (type < 0 || type >= LED_TYPE_LAST) {
		return -EINVAL;
	}

	struct an30259a_pr_control *led_state = &g_led_states[type];

	// set the LED information to the proper element of the array without actually
	// changing the physical LED yet
	memset(led_state, 0, sizeof(*led_state));

	// if the color is 0, turn off the LED
	if (state->color & 0xffffff) {
		led_state->color = state->color & 0x00ffffff;
		// tweak to eliminate purplish tint from white color
		if (led_state->color == 0x00ffffff) {
			led_state->color = 0x80ff80;
		}

		switch (state->flashMode) {
		case LIGHT_FLASH_NONE:
			led_state->state = LED_LIGHT_ON;
			break;
		case LIGHT_FLASH_TIMED:
		case LIGHT_FLASH_HARDWARE:
			led_state->state = LED_LIGHT_SLOPE;
			// scale slope times based on flashOnMS
			led_state->time_slope_up_1 = (SLOPE_UP_1 * state->flashOnMS) / 1000;
			led_state->time_slope_up_2 = (SLOPE_UP_2 * state->flashOnMS) / 1000;
			led_state->time_slope_down_1 = (SLOPE_DOWN_1 * state->flashOnMS) / 1000;
			led_state->time_slope_down_2 = (SLOPE_DOWN_2 * state->flashOnMS) / 1000;
			led_state->mid_brightness = MID_BRIGHTNESS;
			led_state->time_off = state->flashOffMS;
			break;
		default:
			return -EINVAL;
		}
	} else {
		led_state->state = LED_LIGHT_OFF;
	}

	// allow write_leds_priority determine if the physical LED should be changed
	return write_leds_priority();
}

static int set_light_leds_notifications(struct light_device_t *dev __unused,
			struct light_state_t const *state)
{
	return set_light_leds(state, LED_TYPE_NOTIFICATION);
}

static int set_light_leds_attention(struct light_device_t *dev __unused,
			struct light_state_t const *state)
{
	struct light_state_t attention_state = *state;
	if (attention_state.flashMode == LIGHT_FLASH_NONE) {
	    // that's actually NotificationManager's way of turning it off
	    attention_state.color = 0;
	}
	return set_light_leds(&attention_state, LED_TYPE_ATTENTION);
}

static int set_light_leds_battery(struct light_device_t *dev __unused,
			struct light_state_t const *state)
{
	return set_light_leds(state, LED_TYPE_CHARGING);
}

static int open_lights(const struct hw_module_t *module, char const *name,
						struct hw_device_t **device)
{
	int (*set_light)(struct light_device_t *dev,
		struct light_state_t const *state);

	if (0 == strcmp(LIGHT_ID_BACKLIGHT, name))
		set_light = set_light_backlight;
	else if (0 == strcmp(LIGHT_ID_NOTIFICATIONS, name))
		set_light = set_light_leds_notifications;
	else if (0 == strcmp(LIGHT_ID_ATTENTION, name))
		set_light = set_light_leds_attention;
	else if (0 == strcmp(LIGHT_ID_BATTERY, name))
		set_light = set_light_leds_battery;
	else
		return -EINVAL;

	pthread_once(&g_init, init_g_lock);

	struct light_device_t *dev = malloc(sizeof(struct light_device_t));
	memset(dev, 0, sizeof(*dev));

	dev->common.tag = HARDWARE_DEVICE_TAG;
	dev->common.version = 0;
	dev->common.module = (struct hw_module_t *)module;
	dev->common.close = (int (*)(struct hw_device_t *))close_lights;
	dev->set_light = set_light;

	*device = (struct hw_device_t *)dev;

	return 0;
}

static struct hw_module_methods_t lights_module_methods = {
	.open = open_lights,
};

struct hw_module_t HAL_MODULE_INFO_SYM = {
	.tag = HARDWARE_MODULE_TAG,
	.module_api_version = 1,
	.hal_api_version = HARDWARE_HAL_API_VERSION,
	.id = LIGHTS_HARDWARE_MODULE_ID,
	.name = "lights Module",
	.author = "Google, Inc.",
	.methods = &lights_module_methods,
};
