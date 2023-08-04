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

#define MBR_SECTOR_SIZE 512
#define MBR_PART_TYPE_UNUSED	0x00
#define MBR_PART_TYPE_EXTEND0	0x05
#define MBR_PART_TYPE_EXTEND1	0x0f

typedef enum {
	PARTTABLE_MBR,
	PARTTABLE_GPT,
	PARTTABLE_UNKNOWN
} PartTable_Type;

#pragma pack(push, 1)

typedef struct {
	uint8_t reserved1[4];
	uint8_t type;
	uint8_t reserved2[3];
	uint32_t startSector;
	uint32_t sectorNum;
} MBRPartition;

#pragma pack(pop)

#define perr(...) fprintf(stderr, __VA_ARGS__)

#define check(cond, ...) do {\
	if (!(cond)) {					\
		perr(__VA_ARGS__);		\
		return -1;				\
	}						\
} while (0)

#define alloc(name, size) \
	check(name = malloc(size), "Cannot allocate memory for %s\n", #name)

static int
read_range(int fd, void *dst, off_t offset, size_t size)
{
	check(lseek(fd, offset, SEEK_SET) > 0, "Cannot seek on the file\n");
	return read(fd, dst, size) > 0;
}

static int
get_disk_type(const uint8_t *lba1)
{
	return memcmp(lba1, "EFI PART", strlen("EFI PART")) ?
		PARTTABLE_MBR : PARTTABLE_GPT;
}

static int
mbr_parse_one_table(int disk, uint32_t sectorIndex, int partNo)
{
	MBRPartition table[4];
	read_range(disk, table, sectorIndex * MBR_SECTOR_SIZE + 446, 64);
	int lastPart = partNo + 4;

	for (int i = 0; i < 4; i++) {
		if (table[i].type == MBR_PART_TYPE_UNUSED)
			continue;

		printf("part %d: start = %u, num = %u\n",
		       i + partNo,
		       table[i].startSector + sectorIndex, table[i].sectorNum);

		if (table[i].type == MBR_PART_TYPE_EXTEND0 ||
		    table[i].type == MBR_PART_TYPE_EXTEND1) {
			lastPart = mbr_parse_one_table(disk, table[i].startSector,
						       lastPart);
			if (lastPart < 0)
				return lastPart;
		}
	}
	return lastPart;
}

static int
parse_mbr_parttable(int disk)
{
	return mbr_parse_one_table(disk, 0, 1) < 0;
}

static int
parse_partition_table(int disk, PartTable_Type type)
{
	if (type == PARTTABLE_MBR) {
		return parse_mbr_parttable(disk);
	} else {
		perr("Unsupportd partition table type %d\n", type);
		return -1;
	}
}

static int
probe_partition(const char *path)
{
	int disk = open(path, O_RDONLY);
	check(disk >= 0, "Cannot open disk %s\n", path);

	uint8_t *lba1;
	alloc(lba1, 512);

	read_range(disk, lba1, 512, 512);
	PartTable_Type type = get_disk_type(lba1);
	free(lba1);

	check(!parse_partition_table(disk, type),
	      "Failed to parse the partition table\n");

	check(!close(disk), "Cannot close disk %s\n", path);
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
