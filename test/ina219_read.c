/* Simple userspace reader for the ina219_demo sysfs ABI. */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef EOVERFLOW
#define EOVERFLOW 75
#endif

static const char *attributes[] = {
	"voltage_mv", "shunt_voltage_uv", "current_ua", "power_uw"
};
static const char *units[] = { "mV", "uV", "uA", "uW" };

static int find_device(char *path, size_t size)
{
	DIR *dir = opendir("/sys/bus/i2c/devices");
	struct dirent *entry;
	char filename[PATH_MAX], name[128];
	FILE *file;

	if (!dir)
		return -1;
	while ((entry = readdir(dir))) {
		if (entry->d_name[0] == '.')
			continue;
		if (snprintf(filename, sizeof(filename),
			     "/sys/bus/i2c/devices/%s/name", entry->d_name) >=
		    (int)sizeof(filename))
			continue;
		file = fopen(filename, "r");
		if (!file)
			continue;
		if (fgets(name, sizeof(name), file) &&
		    strncmp(name, "ina219_demo", strlen("ina219_demo")) == 0) {
			fclose(file);
			if (snprintf(path, size, "/sys/bus/i2c/devices/%s",
				     entry->d_name) >= (int)size) {
				closedir(dir);
				return -1;
			}
			closedir(dir);
			return 0;
		}
		fclose(file);
	}
	closedir(dir);
	return -1;
}

static int read_value(const char *directory, const char *attribute,
		      long *value)
{
	char filename[PATH_MAX];
	char line[64], *end;
	FILE *file;

	if (snprintf(filename, sizeof(filename), "%s/%s", directory,
		     attribute) >= (int)sizeof(filename))
		return -1;
	file = fopen(filename, "r");
	if (!file)
		return -1;
	if (!fgets(line, sizeof(line), file)) {
		fclose(file);
		errno = EIO;
		return -1;
	}
	fclose(file);
	errno = 0;
	*value = strtol(line, &end, 10);
	if (errno || end == line) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static int write_shunt(const char *directory, unsigned long value)
{
	char filename[PATH_MAX];
	FILE *file;
	long actual;

	if (snprintf(filename, sizeof(filename), "%s/shunt_resistor_uohm",
		     directory) >= (int)sizeof(filename))
		return -1;
	file = fopen(filename, "w");
	if (!file)
		return -1;
	if (fprintf(file, "%lu\n", value) < 0) {
		fclose(file);
		return -1;
	}
	if (fclose(file) != 0)
		return -1;
	if (read_value(directory, "shunt_resistor_uohm", &actual) < 0)
		return -1;
	if (actual != (long long)value) {
		fprintf(stderr, "shunt write verification failed: read back %ld\n",
			actual);
		errno = EIO;
		return -1;
	}
	printf("shunt_resistor_uohm=%ld verified\n", actual);
	return 0;
}

static void usage(const char *program)
{
	fprintf(stderr, "Usage: %s [-d sysfs-directory] [-n count] "
		"[-i interval_ms] [-s shunt_uohm]\n", program);
}

int main(int argc, char **argv)
{
	char directory[PATH_MAX];
	unsigned long count = 1, interval_ms = 1000, shunt = 0;
	int explicit_directory = 0, set_shunt = 0, option, i;
	unsigned long sample;
	long values[4];

	while ((option = getopt(argc, argv, "d:n:i:s:")) != -1) {
		char *end;
		unsigned long parsed;

		switch (option) {
		case 'd':
			if (snprintf(directory, sizeof(directory), "%s", optarg) >=
			    (int)sizeof(directory)) {
				fprintf(stderr, "device path is too long\n");
				return 2;
			}
			explicit_directory = 1;
			break;
		case 'n':
		case 'i':
		case 's':
			errno = 0;
			parsed = strtoul(optarg, &end, 10);
			if (errno || !*optarg || *end) {
				usage(argv[0]);
				return 2;
			}
			if (option == 'n') count = parsed;
			if (option == 'i') interval_ms = parsed;
			if (option == 's') {
				shunt = parsed;
				set_shunt = 1;
			}
			break;
		default:
			usage(argv[0]);
			return 2;
		}
	}
	if (!count) {
		fprintf(stderr, "sample count must be positive\n");
		return 2;
	}
	if (!explicit_directory && find_device(directory, sizeof(directory)) < 0) {
		fprintf(stderr, "ina219_demo device not found in sysfs\n");
		return 1;
	}
	if (set_shunt && write_shunt(directory, shunt) < 0) {
		fprintf(stderr, "cannot set shunt resistor: %s\n", strerror(errno));
		return 1;
	}
	for (sample = 0; sample < count; sample++) {
		for (i = 0; i < 4; i++) {
			if (read_value(directory, attributes[i], &values[i]) < 0) {
				int err = errno;
				if (err == EOVERFLOW)
					fprintf(stderr, "%s: current/power overflow\n",
						attributes[i]);
				else
					fprintf(stderr, "read %s failed: %s\n",
						attributes[i], strerror(err));
				return 1;
			}
		}
		for (i = 0; i < 4; i++)
			printf("%s=%ld %s%s", attributes[i], values[i], units[i],
			       i == 3 ? "\n" : "  ");
		if (sample + 1 < count) {
			struct timespec delay = {
				.tv_sec = interval_ms / 1000,
				.tv_nsec = (interval_ms % 1000) * 1000000UL
			};
			nanosleep(&delay, NULL);
		}
	}
	return 0;
}
