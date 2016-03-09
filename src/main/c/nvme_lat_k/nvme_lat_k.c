/*
 * nvme_lat_k/u2_lat_k
 *
 *   simpler latency benchmark through /dev/nvme0n1.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <fcntl.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/fs.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_cycles.h>

#include <spdk/nvme.h>

#define U2_QUEUE_DEPTH			(1024)

#define U2_SIZE_512B			(512)
#define U2_SIZE_1KB			(1024)
#define U2_SIZE_4KB			(4096)
#define U2_SIZE_16KB			(16384)
#define U2_SIZE_64KB			(65536)
#define U2_SIZE_128KB			(131072)
#define U2_SIZE_256KB			(262144)
#define U2_SIZE_512KB			(524288)
#define U2_SIZE_1MB			(1048576)
#define U2_SIZE_2MB			(2097152)
#define U2_SIZE_4MB			(4194304)
#define U2_SIZE_MAX			(U2_SIZE_4MB)

#define U2_SLAB_SIZE			(U2_SIZE_1MB)

#define U2_RANDOM			(1)
#define U2_SEQUENTIAL			(0)

#define u2_mmap(_s)			_u2_mmap((size_t)(_s), __FILE__, __LINE__)
#define u2_munmap(_p, _s)		_u2_munmap(_p, (size_t)(_s), __FILE__, __LINE__)

static const char *nvme_device = "/dev/nvme0n1";
static int fd = -1;
static uint64_t ns_size = 800000000000;

static uint32_t io_size;
static uint32_t queue_depth;

static int is_random;
static char *core_mask;

static unsigned int seed = 0;

static char *ealargs[] = { "nvme_lat_k", "-c 0x1", "-n 4", };

static int
parse_args(int argc, char **argv)
{
	int op;
	char *mode;

	io_size = U2_SIZE_512B;

	while ((op = getopt(argc, argv, "q:m:c:")) != -1) {
		switch (op) {
		case 'q':
			queue_depth = atoi(optarg);
			break;
		case 'm':
			mode = optarg;
			break;
		case 'c':
			core_mask = optarg;
			break;
		default:
			printf("%s -q [queue depth] -m [R/W mode (rand, seq)] -c [core mask]\n", argv[0]);
			return 1;
		}
	}

	if (!queue_depth) {
		queue_depth = U2_QUEUE_DEPTH;
	}

	is_random = U2_RANDOM;
	if (mode) {
		if (!strcmp(mode, "seq")) {
			is_random = U2_SEQUENTIAL;
		}
	}

	if (core_mask) {
		ealargs[1] = malloc(sizeof("-c ") + strlen(core_mask));
		if (ealargs[1] == NULL) {
			fprintf(stderr, "failed to malloc ealargs[1]!\n");
			return 1;
		}
		sprintf(ealargs[1], "-c %s", core_mask);
	}

	return 0;
}

/*static int
u2_device_size(const char *path, size_t *size)
{
	int status;
	struct stat statinfo;
	int fd;

	status = stat(path, &statinfo);
	if (status < 0) {
		fprintf(stderr, "failed to stat '%s'!\n", path);
		return 1;
	}

	if (!S_ISREG(statinfo.st_mode) && !S_ISBLK(statinfo.st_mode)) {
		fprintf(stderr, "device size not supported on '%s'!\n", path);
		return 1;
	}

	if (S_ISREG(statinfo.st_mode)) {
		*size = (size_t)statinfo.st_size;
		return 0;
	}

	fd = open(path, O_RDONLY, 0644);
	if (fd < 0) {
		fprintf(stderr, "failed to open '%s'!\n", path);
		return 1;
	}

	status = ioctl(fd, BLKGETSIZE64, size);
	if (status < 0) {
		close(fd);
		fprintf(stderr, "failed to ioctl '%s'!\n", path);
		return 1;
	}

	close(fd);

	return 0;
}*/

static void *
_u2_mmap(size_t size, const char *name, int line)
{
	void *p;

	assert(size != 0);

	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == ((void *)-1)) {
		fprintf(stderr, "failed to mmap %zu bytes @ %s:%d!\n", size, name, line);
		return NULL;
	}

	return p;
}

static int
_u2_munmap(void *p, size_t size, const char *name, int line)
{
	int status;

	assert(p != NULL);
	assert(size != 0);

	status = munmap(p, size);
	if (status < 0) {
		fprintf(stderr, "failed to munmap %p @ %s:%d!\n", p, name, line);
	}

	return status;
}


static int
u2_lat_bmk_k(void)
{
	uint64_t offset, size_in_ios;
	uint8_t *wr_buf, *rd_buf;

	uint64_t tsc_rate, tsc_start;
	uint64_t wr_lat, rd_lat;

	int i;

	offset = 0 - io_size;
	size_in_ios = ns_size / io_size;

	tsc_rate = rte_get_tsc_hz();
	wr_lat = 0;
	rd_lat = 0;

	fd = open(nvme_device, O_RDWR | O_DIRECT, 0644);
	if (fd < 0) {
		fprintf(stderr, "failed to open '%s'!\n", nvme_device);
		return 1;
	}

	/*for (i = 0; i < queue_depth; i++) {
		if (is_random) {
			offset = io_size * (rand_r(&seed) % size_in_ios);
		} else {
			if ((offset += io_size) > ns_size - io_size) {
				offset = 0;
			}
		}

		wr_buf = u2_mmap(io_size);
		if (wr_buf == NULL) {
			fprintf(stderr, "failed to mmap write buffer!\n");
			return 1;
		}
		memset(wr_buf, 0xff, io_size);

		tsc_start = rte_get_timer_cycles();
		if (pwrite(fd, wr_buf, io_size, offset) < io_size) {
			fprintf(stderr, "failed to pwrite: %d!\n", i);
			return 1;
		}
		wr_lat += rte_get_timer_cycles() - tsc_start;

		u2_munmap(wr_buf, io_size);

		rd_buf = u2_mmap(io_size);
		if (rd_buf == NULL) {
			fprintf(stderr, "failed to mmap read buffer!\n");
			return 1;
		}
		memset(rd_buf, 0xff, io_size);

		tsc_start = rte_get_timer_cycles();
		if (pread(fd, rd_buf, io_size, offset) < io_size) {
			fprintf(stderr, "failed to pread: %d!\n", i);
			return 1;
		}
		rd_lat += rte_get_timer_cycles() - tsc_start;

		u2_munmap(rd_buf, io_size);
	}*/

	for (i = 0; i < queue_depth; i++) {
		if (is_random) {
			offset = io_size * (rand_r(&seed) % size_in_ios);
		} else {
			if ((offset += io_size) > ns_size - io_size) {
				offset = 0;
			}
		}

		wr_buf = u2_mmap(io_size);
		if (wr_buf == NULL) {
			fprintf(stderr, "failed to mmap write buffer!\n");
			return 1;
		}
		memset(wr_buf, 0xff, io_size);

		tsc_start = rte_get_timer_cycles();
		if (pwrite(fd, wr_buf, io_size, offset) < io_size) {
			fprintf(stderr, "failed to pwrite: %d!\n", i);
			return 1;
		}
		wr_lat += rte_get_timer_cycles() - tsc_start;

		u2_munmap(wr_buf, io_size);
	}

	printf("\t\t%9.1f us", (float) (wr_lat * 1000000) / (queue_depth * tsc_rate));

	for (i = 0; i < queue_depth; i++) {
		if (is_random) {
			offset = io_size * (rand_r(&seed) % size_in_ios);
		} else {
			if ((offset += io_size) > ns_size - io_size) {
				offset = 0;
			}
		}

		rd_buf = u2_mmap(io_size);
		if (rd_buf == NULL) {
			fprintf(stderr, "failed to mmap read buffer!\n");
			return 1;
		}
		memset(rd_buf, 0xff, io_size);

		tsc_start = rte_get_timer_cycles();
		if (pread(fd, rd_buf, io_size, offset) < io_size) {
			fprintf(stderr, "failed to pread: %d!\n", i);
			return 1;
		}
		rd_lat += rte_get_timer_cycles() - tsc_start;

		u2_munmap(rd_buf, io_size);
	}

	printf("\t\t%9.1f us", (float)(rd_lat * 1000000) / (queue_depth * tsc_rate));

	printf("\n");

	return 0;
}

int main(int argc, char *argv[])
{
	if (parse_args(argc, argv)) {
		fprintf(stderr, "failed to parse arguments!\n");
		return 1;
	}

	if (rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]), ealargs) < 0) {
		fprintf(stderr, "failed to initialize DPDK EAL!\n");
		return 1;
	}

	printf("\n===========================================\n");
	printf(  "  NVMe/U2 LAT kernel - ict.ncic.syssw.ufo"    );
	printf("\n===========================================\n");

	printf("u2 latency benchmarking ... queue depth: %d, mode: %s\n", queue_depth, is_random ? "random" : "sequential");
	printf("\t%8s\t\t%12s\t\t%12s\n", "I/O size", "write lat", "read lat");
	while (1) {
		printf("\t%8d", io_size);

		if (u2_lat_bmk_k()) {
			fprintf(stderr, "failed to benchmark latency - IO size %d!\n", io_size);
			return 1;
		}

		if ((io_size *= 2) > U2_SIZE_MAX) {
			break;
		}
	}

	if (core_mask) {
		free(ealargs[1]);
	}

	return 0;
}

