/* Rotate Lenovo Yoga (2 Pro) display and touchscreen to match orientation of screen
 * Copyright (c) 2014 Peter F. Patel-Schneider
 *
 * Modified from industrialio buffer test code.
 * Copyright (c) 2008 Jonathan Cameron
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * Command line parameters
 * orientation -n <iio device name> -c <iterations (<0 is forever)> -d <debug level>
 * iio device name defaults to accel_3d
 * iterations defaults to -1
 * debug level defaults to 0 (useful range is -1 to 4)
 *
 * This program reads the Invensense accelerometer and uses gravity to
 * determine which edge of the Yoga screen is up.  If none of the edges is
 * up very much, this is treated as being flat.   When an edge is up twice
 * in a row, the orientation of the screen and touchscreen is adjusted
 * to match.
 *
 * WARNING:  This is not production quality code.
 *	     This code exec's xrandr and xinput as root.
 */

#define _GNU_SOURCE

#include "libs/common.h"

static char* touchScreenName = "";
static int debug_level = -1;
static bool orientation_lock = false;
static int screen_orientation = -1;
static int previous_orientation = -1;
static char *dev_dir_name;
static time_t last_sigusr_time = 0;


typedef enum {
	FLAT, TOP, RIGHT, BOTTOM, LEFT
} OrientationPositions; /* various orientations */

int rotate_left_orientation(OrientationPositions orientation) {
	return (LEFT == orientation) ? TOP : orientation + 1;
}

/**
 * process_scan_1() - get an integer value for a particular channel
 * @data:               pointer to the start of the scan
 * @channels:           information about the channels. Note
 *  size_from_channelarray must have been called first to fill the
 *  location offsets.
 * @num_channels:       number of channels
 * ch_name:		name of channel to get
 * ch_val:		value for the channel
 * ch_present:		whether the channel is present
 **/
void process_scan_1(char *data, struct iio_channel_info *channels, int num_channels,
		char *ch_name, int *ch_val, bool *ch_present) {
	int k;
	for (k = 0; k < num_channels; k++) {
		if (0 == strcmp(channels[k].name, ch_name)) {
			switch (channels[k].bytes) {
					/* only a few cases implemented so far */
				case 2:
					break;
				case 4:
					if (!channels[k].is_signed) {
						uint32_t val = *(uint32_t *) (data + channels[k].location);
						val = val >> channels[k].shift;
						if (channels[k].bits_used < 32) val &= ((uint32_t) 1 << channels[k].bits_used) - 1;
						*ch_val = (int) val;
						*ch_present = true;
					} else {
						int32_t val = *(int32_t *) (data + channels[k].location);
						val = val >> channels[k].shift;
						if (channels[k].bits_used < 32) val &= ((uint32_t) 1 << channels[k].bits_used) - 1;
						val = (int32_t) (val << (32 - channels[k].bits_used)) >> (32 - channels[k].bits_used);
						*ch_val = (int) val;
						*ch_present = true;
					}
					break;
				case 8:
					break;
			}
		}
	}
}

/**
 * process_scan_3() - get three integer values - see above
 **/
void process_scan_3(char *data, struct iio_channel_info *channels, int num_channels,
		char *ch_name_1, int *ch_val_1, bool *ch_present_1,
		char *ch_name_2, int *ch_val_2, bool *ch_present_2,
		char *ch_name_3, int *ch_val_3, bool *ch_present_3) {
	process_scan_1(data, channels, num_channels, ch_name_1, ch_val_1, ch_present_1);
	process_scan_1(data, channels, num_channels, ch_name_2, ch_val_2, ch_present_2);
	process_scan_1(data, channels, num_channels, ch_name_3, ch_val_3, ch_present_3);
}

int process_scan(SensorData data, Device_info info, Config config) {
	int orientation = FLAT, i;

	int accel_x, accel_y, accel_z;
	bool present_x, present_y, present_z;

	for (i = 0; i < data.read_size / data.scan_size; i++) {
		process_scan_3(data.data + data.scan_size*i, info.channels, info.channels_count,
				"in_accel_x", &accel_x, &present_x,
				"in_accel_y", &accel_y, &present_y,
				"in_accel_z", &accel_z, &present_z);
		/* Determine orientation */
		int accel_x_abs = abs(accel_x);
		int accel_y_abs = abs(accel_y);
		int accel_z_abs = abs(accel_z);
		/* printf("%u > %u && %u > %u\n", accel_z_abs, 4*accel_x_abs, accel_z_abs, 4*accel_y_abs); */
		if (accel_z_abs > 4 * accel_x_abs && accel_z_abs > 4 * accel_y_abs) {
		  /* printf("set FLAT\n"); */
			orientation = FLAT;
		} else if (3 * accel_y_abs > 2 * accel_x_abs) {
		  /* printf("set TOP/BOTTOM (%u, %u)\n", 3*accel_y_abs, 2*accel_x_abs); */
			orientation = accel_y > 0 ? BOTTOM : TOP;
		} else {
			orientation = accel_x > 0 ? LEFT : RIGHT;
			/* printf("set LEFT/RIGHT\n"); */
		}
		if (config.debug_level > 1) printf("Orientation %d, x:%5d, y:%5d, z:%5d\n",
				orientation, accel_x, accel_y, accel_z);
	}
	return orientation;
}

/* symbolic orientation as used in xrandr */
char * symbolic_orientation(OrientationPositions orientation) {
	char * orient;
	switch (orientation) {
		case FLAT: orient = "flat";
			break;
		case BOTTOM: orient = "inverted";
			break;
		case TOP: orient = "normal";
			break;
		case LEFT: orient = "left";
			break;
		case RIGHT: orient = "right";
			break;
	}
	return orient;
}

void rotate_to(OrientationPositions orient) {
	char * xrandr = "/usr/bin/xrandr";
	char * xinput = "/usr/bin/xinput";
	char * tsnormal[] = {xinput, "set-prop", touchScreenName, "Coordinate Transformation Matrix",
		"1", "0", "0", "0", "1", "0", "0", "0", "1", (char *) NULL};
	char * tsright[] = {xinput, "set-prop", touchScreenName, "Coordinate Transformation Matrix",
		"0", "1", "0", "-1", "0", "1", "0", "0", "1", (char *) NULL};
	char * tsleft[] = {xinput, "set-prop", touchScreenName, "Coordinate Transformation Matrix",
		"0", "-1", "1", "1", "0", "0", "0", "0", "1", (char *) NULL};
	char * tsinverted[] = {xinput, "set-prop", touchScreenName, "Coordinate Transformation Matrix",
		"-1", "0", "1", "0", "-1", "1", "0", "0", "1", (char *) NULL};
	int status, pid;
	char *orientation = symbolic_orientation(orient);

	printf("ROTATE to %s\n", orientation);
	screen_orientation = orient;
	if (0 == (pid = fork())) { /* rotate the screen */
		execl(xrandr, xrandr, "--orientation", orientation, (char *) NULL);
	} else {
		wait(&status);
		if (status) printf("First child (xrandr) returned %d\n", status);

		if (0 != strlen(touchScreenName) && 0 == (pid = fork())) { /* rotate the touchscreen */
			switch (orient) {
				case TOP: execv(xinput, tsnormal);
					break;
				case BOTTOM: execv(xinput, tsinverted);
					break;
				case LEFT: execv(xinput, tsleft);
					break;
				case RIGHT: execv(xinput, tsright);
					break;
			}
		} else {
			wait(&status);
			if (status) printf("Second child (xinput) returned %d\n", status);
		}
	}
}

void sigint_callback_handler(int signum) {
	if (dev_dir_name) {
		/* Disconnect the trigger - just write a dummy name. */
		write_sysfs_string("trigger/current_trigger", dev_dir_name, "NULL");
		/* Stop the buffer */
		write_sysfs_int("buffer/enable", dev_dir_name, 0);
	}
	exit(signum);
}

void sigusr_callback_handler(int signum) {
	int now = time(NULL), pid, status, bufflen;
	char * buffer;
	char * notify = "/usr/bin/notify-send"; // -i /home/buri/.utils/yoga/rotateon.png "Rotace aktivována"";
	char * tslockon[] = {notify, "-i", "/home/buri/.utils/yoga/rotateon.png", "Autorotate enabled", (char*) NULL};
	char * tslockoff[] = {notify, "-i", "/home/buri/.utils/yoga/rotateoff.png", "Autorotate disabled", (char*) NULL};

	previous_orientation = screen_orientation;
	if (now <= last_sigusr_time + 1) {
		orientation_lock = true;
		if (debug_level > 0) {
			printf("Quick second signal rotates and then suspends rotation at %ld diff %ld\n",
					(long) now, (long) (now - last_sigusr_time));
		} else if (debug_level > -1) {
			printf("Quick second signal rotates and then suspends rotation\n");
		}
		last_sigusr_time = 0;
		rotate_to(rotate_left_orientation(screen_orientation));
	} else {
		orientation_lock = !orientation_lock;
		if (debug_level > 0) {
			printf("Signal %s rotation at %ld diff %ld\n",
					orientation_lock ? "suspends" : "resumes", (long) now, (long) (now - last_sigusr_time));
		} else if (debug_level > -1) {
			printf("Signal %s rotation\n", orientation_lock ? "suspends" : "resumes");
		}
		last_sigusr_time = now;
		if (0 == (pid = fork())) {
			if (orientation_lock) {
				system("/usr/bin/notify-send -i /home/buri/.utils/yoga/rotateoff.png \"Autorotate disabled\"");
			} else {
				system("/usr/bin/notify-send -i /home/buri/.utils/yoga/rotateon.png \"Autorotate enabled\"");
			}
		} else {
			wait(&status);
		}
	}
}

int main(int argc, char **argv) {
	/* Configuration variables */
	Config config = Config_default;
	char *trigger_name = NULL, *device_name = "accel_3d";


	// Update default settings
	config.device_name = "accel_3d";
	touchScreenName = config.or_touchScreenName;
	debug_level = config.debug_level;

	/* Arguments definition */
	static int version_flag = 0, help_flag = 0;
	static char* version = "orientation version 0.3\n";
	static char* help;
	asprintf(&help, "orientation monitors the Yoga accelerometer and\n\
rotates the screen and touchscreen to match\n\
\n\
Options:\n\
  --help		Print this help message and exit\n\
  --version		Print version information and exit\n\
  --count=iterations	If >0 run for only this number of iterations [%d]\n\
  --name=accel_name	Industrial IO accelerometer device name [%s]\n\
  --usleep=time		Polling sleep time in microseconds [%u]\n\
  --debug=level		Print out debugging information (-1 through 4) [%d]\n\
  --touchscreen=ts_name	TouchScreen name [%s]\n\
\n\
orientation responds to single SIGUSR1 interrupts by toggling whether it\n\
rotates the screen and two SIGUSR1 interrupts within a second or two by \n\
rotating the screen clockwise and suspending rotations.\n\
Use via something like\n\
    pkill --signal SIGUSR1 --exact orientation\n",
			config.iterations, config.device_name, config.poll_timeout, config.debug_level,
			config.or_touchScreenName);

	/* Device info */
	Device_info info;

	/* Other variables */
	int ret, c, i;
	char * dummy;

	unsigned int sleeping = 1000000;
	int orientation = 0;

	static struct option long_options[] = {
		{"version", no_argument, &version_flag, 1},
		{"help", no_argument, &help_flag, 1},
		{"count", required_argument, 0, 'c'},
		{"name", required_argument, 0, 'n'},
		{"touchscreen", required_argument, 0, 't'},
		{"usleep", required_argument, 0, 'u'},
		{"debug", required_argument, 0, 'd'},
		{0, 0, 0, 0}
	};
	int option_index = 0;

	while ((c = getopt_long(argc, argv, "c:n:d:t:u:", long_options, &option_index))
			!= -1) {
		switch (c) {
			case 0:
				break;
			case 'c':
				config.iterations = strtol(optarg, &dummy, 10);
				break;
			case 'n':
				config.device_name = optarg;
				break;
			case 't':
				config.or_touchScreenName = optarg;
				touchScreenName = optarg;
				break;
			case 'd':
				config.debug_level = strtol(optarg, &dummy, 10);
				debug_level = config.debug_level;
				break;
			case 'u':
				config.poll_timeout = strtol(optarg, &dummy, 10);
				break;
			case '?':
				printf("Invalid flag\n");
				return -1;
		}
	}

	if (version_flag) {
		printf("%s", version);
		exit(EXIT_SUCCESS);
	}
	if (help_flag) {
		printf("%s", help);
		exit(EXIT_SUCCESS);
	}

	signal(SIGINT, sigint_callback_handler);
	signal(SIGHUP, sigint_callback_handler);
	signal(SIGUSR1, sigusr_callback_handler);

	/* Find the device requested */
	info.device_id = find_type_by_name(device_name, "iio:device");
	if (info.device_id < 0) {
		printf("Failed to find the %s sensor\n", device_name);
		ret = -ENODEV;
		goto error_ret;
	}
	if (debug_level > -1) printf("iio device number being used is %d\n", info.device_id);

	/* enable the sensors in the device */
	asprintf(&dev_dir_name, "%siio:device%d", iio_dir, info.device_id);
	if (ret < 0) {
		ret = -ENOMEM;
		goto error_ret;
	}

	enable_sensors(dev_dir_name);

	if (trigger_name == NULL) {
		/* Build the trigger name. */
		ret = asprintf(&trigger_name,
				"%s-dev%d", config.device_name, info.device_id);
		if (ret < 0) {
			ret = -ENOMEM;
			goto error_ret;
		}
	}
	/* Verify the trigger exists */
	ret = find_type_by_name(trigger_name, "trigger");
	if (ret < 0) {
		printf("Failed to find the trigger %s\n", trigger_name);
		ret = -ENODEV;
		goto error_free_triggername;
	}
	if (debug_level > -1) printf("iio trigger number being used is %d\n", ret);

	/* Parse the files in scan_elements to identify what channels are present */
	ret = build_channel_array(dev_dir_name, &(info.channels), &(info.channels_count));
	if (ret) {
		printf("Problem reading scan element information\n");
		printf("diag %s\n", dev_dir_name);
		goto error_free_triggername;
	}

	for (i = 0; i != config.iterations; i++) {
		if (config.debug_level > 2) printf("Finding orientation %d\n", orientation);
		if ((orientation = prepare_output(&info, dev_dir_name, trigger_name, &process_scan, config)) < 0) break;
		if (config.debug_level > 2) printf("Found orientation: or:%d, prev:%d, scr:%d, %d\n", orientation, previous_orientation, screen_orientation, FLAT);
		if (config.debug_level > 0) printf("Orientation at %3.1f is %s\n", ((double) config.poll_timeout / 1000000.0) * i, symbolic_orientation(orientation));
		if (previous_orientation == orientation /* only rotate when stable */ &&
				orientation != screen_orientation && orientation != FLAT && !orientation_lock) {
			rotate_to(orientation);
		}
		previous_orientation = orientation;
		usleep(config.poll_timeout);
	}

	return 0;

error_free_triggername:
	free(trigger_name);
error_free_dev_dir_name:
	free(dev_dir_name);
error_ret:
	return ret;
}
