/*
 *	zpartprobe
 *	/zpartprobe.c
 *	By Mozilla Public License Version 2.0
 *	Copyright (c) 2023 Yao Zi. All rights reserved.
 */

#include<stdio.h>
#include<stdlib.h>
#include<stdint.h>
#include<string.h>

#include<fcntl.h>
#include<unistd.h>

typedef enum {
	PARTTABLE_MBR,
	PARTTABLE_GPT,
	PARTTABLE_UNKNOWN
} PartTable_Type;

#define check(cond, fmt, ...) do {\
	if (!(cond)) {					\
		fprintf(stderr, fmt "\n", __VA_ARGS__);	\
		return -1;				\
	}						\
} while (0)

#define alloc(name, size) \
	check(name = malloc(size), "Cannot allocate memory for %s", #name)

static int
read_range(int fd, void *dst, off_t offset, size_t size)
{
	lseek(fd, SEEK_SET, offset);
	read(fd, dst, size);
	return read(fd, dst, size) > 0;
}

static int
get_disk_type(const uint8_t *lba1)
{
	return memcmp((const char *)lba1, "EFI PART", strlen("EFI PART")) ?
		PARTTABLE_MBR : PARTTABLE_GPT;
}

static int
probe_partition(const char *path)
{
	int disk = open(path, O_RDONLY);
	check(disk >= 0, "Cannot open disk %s", path);

	uint8_t *lba1;
	alloc(lba1, 512);

	read_range(disk, lba1, 512, 512);
	printf("disk type: %d\n", get_disk_type(lba1));

	free(lba1);
	check(!close(disk), "Cannot close disk %s", path);
	return 0;
}

int
main(int argc, const char *argv[])
{
	for (int i = 1; i < argc; i++) {
		if (probe_partition(argv[i]))
			return -1;
	}
	return 0;
}
