/****************************************************************************
 *
 *   Copyright (c) 2012, 2013 PX4 Development Team. All rights reserved.
 *   Author: @author Lorenz Meier <lm@inf.ethz.ch>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file reboot.c
 * Tool similar to UNIX reboot command
 */

#include <nuttx/config.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#include <systemlib/err.h>
#include <systemlib/param/param.h>

#include <drivers/drv_led.h>
#include <drivers/drv_hrt.h>
#include <drivers/drv_tone_alarm.h>
#include <drivers/drv_mag.h>
#include <drivers/drv_gyro.h>
#include <drivers/drv_accel.h>
#include <drivers/drv_baro.h>

#include <mavlink/mavlink_log.h>

__EXPORT int preflight_check_main(int argc, char *argv[]);
static int led_toggle(int leds, int led);
static int led_on(int leds, int led);
static int led_off(int leds, int led);

int preflight_check_main(int argc, char *argv[])
{
	bool fail_on_error = false;

	if (argc > 1 && !strcmp(argv[1], "--help")) {
		warnx("usage: preflight_check [--fail-on-error]\n\tif fail on error is enabled, will return 1 on error");
		exit(1);
	}

	if (argc > 1 && !strcmp(argv[1], "--fail-on-error")) {
		fail_on_error = true;
	}

	bool system_ok = true;

	int fd;
	/* open text message output path */
	int mavlink_fd = open(MAVLINK_LOG_DEVICE, 0);
	int ret;

	/* give the system some time to sample the sensors in the background */
	usleep(150000);


	/* ---- MAG ---- */
	close(fd);
	fd = open(MAG_DEVICE_PATH, 0);
	if (fd < 0) {
		warn("failed to open magnetometer - start with 'hmc5883 start' or 'lsm303d start'");
		mavlink_log_critical(mavlink_fd, "SENSOR FAIL: NO MAG");
		system_ok = false;
		goto system_eval;
	}
	ret = ioctl(fd, MAGIOCSELFTEST, 0);
	
	if (ret != OK) {
		warnx("magnetometer calibration missing or bad - calibrate magnetometer first");
		mavlink_log_critical(mavlink_fd, "SENSOR FAIL: MAG CHECK/CAL");
		system_ok = false;
		goto system_eval;
	}

	/* ---- ACCEL ---- */

	close(fd);
	fd = open(ACCEL_DEVICE_PATH, 0);
	ret = ioctl(fd, ACCELIOCSELFTEST, 0);
	
	if (ret != OK) {
		warnx("accel self test failed");
		mavlink_log_critical(mavlink_fd, "SENSOR FAIL: ACCEL CHECK/CAL");
		system_ok = false;
		goto system_eval;
	}

	/* ---- GYRO ---- */

	close(fd);
	fd = open(GYRO_DEVICE_PATH, 0);
	ret = ioctl(fd, GYROIOCSELFTEST, 0);
	
	if (ret != OK) {
		warnx("gyro self test failed");
		mavlink_log_critical(mavlink_fd, "SENSOR FAIL: GYRO CHECK/CAL");
		system_ok = false;
		goto system_eval;
	}

	/* ---- BARO ---- */

	close(fd);
	fd = open(BARO_DEVICE_PATH, 0);

	/* ---- RC CALIBRATION ---- */

	param_t _parameter_handles_min, _parameter_handles_trim, _parameter_handles_max,
	_parameter_handles_rev, _parameter_handles_dz;

	float param_min, param_max, param_trim, param_rev, param_dz;

	bool rc_ok = true;

	for (int i = 0; i < 12; i++) {
		/* should the channel be enabled? */
		uint8_t count = 0;

		/* min values */
		sprintf(nbuf, "RC%d_MIN", i + 1);
		_parameter_handles_min = param_find(nbuf);
		param_get(_parameter_handles_min, &param_min);

		/* trim values */
		sprintf(nbuf, "RC%d_TRIM", i + 1);
		_parameter_handles_trim = param_find(nbuf);
		param_get(_parameter_handles_trim, &param_trim);

		/* max values */
		sprintf(nbuf, "RC%d_MAX", i + 1);
		_parameter_handles_max = param_find(nbuf);
		param_get(_parameter_handles_max, &param_max);

		/* channel reverse */
		sprintf(nbuf, "RC%d_REV", i + 1);
		_parameter_handles_rev = param_find(nbuf);
		param_get(_parameter_handles_rev, &param_rev);

		/* channel deadzone */
		sprintf(nbuf, "RC%d_DZ", i + 1);
		_parameter_handles_dz = param_find(nbuf);
		param_get(_parameter_handles_dz, &param_dz);

		/* assert min..center..max ordering */
		if (param_min < 500) {
			count++;
			mavlink_log_critical(mavlink_fd, "ERR: RC_%d_MIN < 500", i+1);
			/* give system time to flush error message in case there are more */
			usleep(100000);
		}
		if (param_max > 2500) {
			count++;
			mavlink_log_critical(mavlink_fd, "ERR: RC_%d_MAX > 2500", i+1);
			/* give system time to flush error message in case there are more */
			usleep(100000);
		}
		if (param_trim < param_min) {
			count++;
			mavlink_log_critical(mavlink_fd, "ERR: RC_%d_TRIM < MIN", i+1);
			/* give system time to flush error message in case there are more */
			usleep(100000);
		}
		if (param_trim > param_max) {
			count++;
			mavlink_log_critical(mavlink_fd, "ERR: RC_%d_TRIM > MAX", i+1);
			/* give system time to flush error message in case there are more */
			usleep(100000);
		}

		/* assert deadzone is sane */
		if (param_dz > 500) {
			mavlink_log_critical(mavlink_fd, "ERR: RC_%d_DZ > 500", i+1);
			/* give system time to flush error message in case there are more */
			usleep(100000);
			count++;
		}

		/* XXX needs inspection of all the _MAP params */
		// if (conf[PX4IO_P_RC_CONFIG_ASSIGNMENT] >= MAX_CONTROL_CHANNELS) {
		// 	mavlink_log_critical(mavlink_fd, "ERR: RC_%d_MAP >= # CHANS", i+1);
		// 	/* give system time to flush error message in case there are more */
		// 	usleep(100000);
		// 	count++;
		// }

		/* sanity checks pass, enable channel */
		if (count) {
			mavlink_log_critical("ERROR: %d config error(s) for RC channel %d.", count, (channel + 1));
			usleep(100000);
			rc_ok = false;
		}
	}

	/* require RC ok to keep system_ok */
	system_ok &= rc_ok;


		

system_eval:
	
	if (system_ok) {
		/* all good, exit silently */
		exit(0);
	} else {
		fflush(stdout);

		int buzzer = open("/dev/tone_alarm", O_WRONLY);
		int leds = open(LED_DEVICE_PATH, 0);

		/* flip blue led into alternating amber */
		led_off(leds, LED_BLUE);
		led_off(leds, LED_AMBER);
		led_toggle(leds, LED_BLUE);

		/* display and sound error */
		for (int i = 0; i < 150; i++)
		{
			led_toggle(leds, LED_BLUE);
			led_toggle(leds, LED_AMBER);

			if (i % 10 == 0) {
				ioctl(buzzer, TONE_SET_ALARM, 4);
			} else if (i % 5 == 0) {
				ioctl(buzzer, TONE_SET_ALARM, 2);
			}
			usleep(100000);
		}

		/* stop alarm */
		ioctl(buzzer, TONE_SET_ALARM, 0);

		/* switch on leds */
		led_on(leds, LED_BLUE);
		led_on(leds, LED_AMBER);

		if (fail_on_error) {
			/* exit with error message */
			exit(1);
		} else {
			/* do not emit an error code to make sure system still boots */
			exit(0);
		}
	}
}

static int led_toggle(int leds, int led)
{
	static int last_blue = LED_ON;
	static int last_amber = LED_ON;

	if (led == LED_BLUE) last_blue = (last_blue == LED_ON) ? LED_OFF : LED_ON;

	if (led == LED_AMBER) last_amber = (last_amber == LED_ON) ? LED_OFF : LED_ON;

	return ioctl(leds, ((led == LED_BLUE) ? last_blue : last_amber), led);
}

static int led_off(int leds, int led)
{
	return ioctl(leds, LED_OFF, led);
}

static int led_on(int leds, int led)
{
	return ioctl(leds, LED_ON, led);
}