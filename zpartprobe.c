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

#include<errno.h>
#include<fcntl.h>
#include<unistd.h>
#include<sys/ioctl.h>
#include<linux/blkpg.h>
#include<linux/fs.h>

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
	uint8_t		reserved1[4];
	uint8_t		type;
	uint8_t		reserved2[3];
	uint32_t	startSector;
	uint32_t	sectorNum;
} MBRPartition;

typedef struct {
	uint8_t		sign[8];
	uint8_t 	rev[4];
	uint32_t	headerSize;
	uint32_t	headerCRC32;
	uint8_t		reserved[4];
	uint64_t	current;
	uint64_t	backup;
	uint64_t	spaceStart;
	uint64_t	spaceEnd;
	uint8_t		guid[16];
	uint64_t	parttableStart;
	uint32_t	partNum;
	uint32_t	partItemSize;
	uint32_t	tableCRC32;
} GPTHeader;

typedef struct {
	uint8_t		type[16];
	uint8_t		guid[16];
	uint64_t	start;
	uint64_t	last;
	uint64_t	flags;
	uint8_t		name[72];
} GPTPartition;

#pragma pack(pop)

#define perr(...) fprintf(stderr, __VA_ARGS__)

#define check(cond, ...) do {\
	if (!(cond)) {					\
		perr(__VA_ARGS__);		\
		return -1;				\
	}						\
} while (0)

#define alloc(name, size) do {\
	name = malloc(size);						\
	if (!(name)) {							\
		perr("Cannot allocate memory for %s\n", #name);		\
		exit(-1);						\
	}								\
} while (0)

static int
read_range(int fd, void *dst, off_t offset, size_t size)
{
	check(lseek(fd, offset, SEEK_SET) > 0, "Cannot seek on the file\n");
	check(read(fd, dst, size) > 0, "Cannot read from the disk\n");
	return 0;
}

static int
get_disk_type(const uint8_t *lba1)
{
	return memcmp(lba1, "EFI PART", strlen("EFI PART")) ?
		PARTTABLE_MBR : PARTTABLE_GPT;
}

static int
commit_clear_partitions(int disk)
{
	// There should be no more than 128 partitions, right?
	// ...right?
	struct blkpg_partition part = { .pno		= 1 };
	struct blkpg_ioctl_arg arg = {
					.op		= BLKPG_DEL_PARTITION,
					.datalen	= sizeof(part),
					.data		= &part
				     };
	for (int i = 1; i <= 128; i++) {
		part.pno = i;
		ioctl(disk, BLKPG, &arg);
	}

	return 0;
}

static int
commit_add_partition(int disk, int no, long long int start, long long int size)
{
	struct blkpg_partition part = {
					.pno	= no,
					.start	= start,
					.length	= size,
				      };
	struct blkpg_ioctl_arg arg = {
					.op		= BLKPG_ADD_PARTITION,
					.datalen	= sizeof(part),
					.data		= &part,
				     };
	return ioctl(disk, BLKPG, &arg);
}

static int
mbr_parse_one_table_and_commit(int disk, uint32_t sectorIndex, int partNo)
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

		int ret = commit_add_partition(disk, partNo + i,
				((long long int)table[i].startSector) *
					MBR_SECTOR_SIZE,
				((long long int)table[i].sectorNum) *
					MBR_SECTOR_SIZE);
		check(!ret,
		      "Cannot commit partition information to kernel: %s\n",
		      strerror(errno));

		if (table[i].type == MBR_PART_TYPE_EXTEND0 ||
		    table[i].type == MBR_PART_TYPE_EXTEND1) {
			lastPart =
				mbr_parse_one_table_and_commit
					(disk, table[i].startSector,
					 lastPart);
			if (lastPart < 0)
				return lastPart;
		}
	}
	return lastPart;
}

static int
parse_mbr_parttable_and_commit(int disk)
{
	return mbr_parse_one_table_and_commit(disk, 0, 1) < 0;
}

uint32_t
crc32_reflect(uint32_t word)
{
	uint32_t r = 0;
	for (int i = 0; i < 32; i++)
		r |= !!(word & (1 << (31 - i))) << i;
	return r;
}

uint32_t
crc32(const void *p, size_t size)
{
	const uint8_t *data = p;
	uint32_t r = 0xffffffff;
	for (size_t i = 0; i < size; i++) {
		r ^= crc32_reflect(data[i]);
		for (int j = 0; j < 8; j++)
			r = (r & (1 << 31)) ? (r << 1) ^ 0x04C11DB7 : (r << 1);
	}
	return crc32_reflect(r ^ 0xffffffff);
}

/*
 *	If an error occured, returns -1. Otherwise the sector size.
 */
static int
get_logical_sector_size(int disk)
{
	static int warned = 0;
	int size = ioctl(disk, BLKSSZGET, &size);
	if (size < 0) {
		if (!warned) {
			perr("Cannot get logical sector size, "
			     "set to 512 by default\n");
			warned = 1;
		}
		size = 512;
	}
	return size;
}

static int
gpt_get_header(int disk, GPTHeader *header, size_t location)
{
	int secSize = get_logical_sector_size(disk);
	read_range(disk, header, secSize * location, sizeof(GPTHeader));
	uint32_t chksum = header->headerCRC32;
	printf("GPT Header checksum: 0x%08x\n", chksum);
	header->headerCRC32 = 0;
	uint32_t chksum2 = crc32(header, sizeof(GPTHeader));
	if (chksum2 != chksum) {
		perr("GPT Header checksum mismatch! "
		     "Calculated header checksum: 0x%08x\n", chksum2);
		return -1;
	} else {
		return 0;
	}
}

static GPTPartition *
gpt_get_parttable(int disk, GPTHeader *header)
{
	GPTPartition *parttable;
	alloc(parttable, sizeof(GPTPartition) * header->partNum);

	int secSize = get_logical_sector_size(disk);
	read_range(disk, parttable, header->parttableStart * secSize,
		   sizeof(GPTPartition) * header->partNum);

	printf("GPT Table checksum: 0x%08x\n", header->tableCRC32);
	uint32_t chksum = crc32(parttable,
				sizeof(GPTPartition) * header->partNum);
	if (chksum != header->tableCRC32) {
		perr("Table checksum mismatch! "
		     "Calculated checksum 0x%08x\n", chksum);
		free(parttable);
		return NULL;
	} else {
		return parttable;
	}
}

static int
gpt_get_header_and_table(int disk, GPTHeader *header, GPTPartition **parttable)
{
	/*	Try the main GPT header	*/
	if (gpt_get_header(disk, header, 1)) {
		perr("Try using backup\n");
		goto tryBackup;
	}

	*parttable = gpt_get_parttable(disk, header);
	if (!*parttable) {
		perr("Try using backup\n");
		goto tryBackup;
	}

	return 0;

tryBackup:

	if (gpt_get_header(disk, header, header->backup)) {
		perr("Backup header corrupted\n");
		goto err;
	}
	*parttable = gpt_get_parttable(disk, header);
	if (!*parttable) {
		perr("Backup parttable corrupted\n");
		goto err;
	}

	return 0;

err:
	perr("CORRUPTED GPT TABLE: GOOD LUCK\n");
	return -1;
}

static int
parse_gpt_parttable_and_commit(int disk)
{
	GPTHeader header;
	GPTPartition *parttable;
	int secSize = get_logical_sector_size(disk);

	if (gpt_get_header_and_table(disk, &header, &parttable))
		return -1;

	uint8_t zeros[16] = { 0 };

	for (int i = 0; i < (int32_t)header.partNum; i++) {
		if (!memcmp(parttable[i].type, zeros, sizeof(zeros)))
			continue;

		long long int start	= parttable[i].start;
		long long int size	= parttable[i].last - start;

		printf("part %d: start = %llu, end = %llu\n",
		       i + 1, start, size);

		int ret = commit_add_partition(disk, i + 1,
					       start * secSize,
					       size * secSize);
		check(!ret,
		      "Cannot commit partition information to kernel: %s\n",
		      strerror(errno));
	}

	free(parttable);
	return 0;
}

static int
parse_partition_table_and_commit(int disk, PartTable_Type type)
{
	check(!commit_clear_partitions(disk),
	      "Cannot delete existing partition information from kernel.\n");
	if (type == PARTTABLE_MBR) {
		return parse_mbr_parttable_and_commit(disk);
	} else if (type == PARTTABLE_GPT) {
		return parse_gpt_parttable_and_commit(disk);
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

	int sectorSize = get_logical_sector_size(disk);

	uint8_t *lba1;
	alloc(lba1, 512);

	read_range(disk, lba1, sectorSize * 1, 512);
	PartTable_Type type = get_disk_type(lba1);
	free(lba1);

	check(!parse_partition_table_and_commit(disk, type),
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
