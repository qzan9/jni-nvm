/*
 * nvme_lat_k/u2_lat_k
 *
 *   simpler latency benchmark through /dev/nvme0n1.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_cycles.h>

#include <spdk/nvme.h>

#define U2_REQUEST_POOL_SIZE		(65536)
#define U2_REQUEST_CACHE_SIZE		(0)
#define U2_REQUEST_PRIVATE_SIZE		(0)

#define U2_NAMESPACE_ID			(1)
#define U2_QUEUE_DEPTH			(512)

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

#define U2_BUFFER_ALIGN			(0x200)

#define u2_mmap(_s)			_u2_mmap((size_t)(_s), __FILE__, __LINE__)
#define u2_munmap(_p, _s)		_u2_munmap(_p, (size_t)(_s), __FILE__, __LINE__)


//struct rte_mempool *request_mempool;

//static struct spdk_nvme_ctrlr *u2_ctrlr;
//static struct spdk_nvme_ns *u2_ns;

static const char *nvme_device = "/dev/nvme0n1";
static int fd;

static uint64_t ns_size;

static uint8_t *rd_buf;
static uint8_t *wr_buf;

//static uint32_t u2_ns_sector;
//static uint64_t u2_ns_size;

static char *ealargs[] = { "nvme_lat_k", "-c 0x1", "-n 4", };

static int 
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
}

static void
_u2_mmap(size_t size, const char *name, int line)
{
	void *p;

	ASSERT(size != 0);

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

	ASSERT(p != NULL);
	ASSERT(size != 0);

	status = munmap(p, size);
	if (status < 0) {
		fprintf(stderr, "failed to munmap %p @ %s:%d!\n", p, name, line);
	}

	return status;
}


static int
u2_lat_bmk_k(int payload_size, int bmk_iter)
{
	uint32_t io_size, queue_depth;
	uint64_t offset;

	uint64_t tsc_rate, tsc_start;
	uint64_t wr_lat, rd_lat;

	int i, rc;

	io_size = payload_size;
	queue_depth = bmk_iter;

	offset = 0;
	ns_size = u2_device_size(nvme_device, &ns_size);

	tsc_rate = rte_get_tsc_hz();
	wr_lat = 0;
	rd_lat = 0;

	//if (spdk_nvme_register_io_thread()) {
	//	fprintf(stderr, "failed to register thread!\n");
	//	return 1;
	//}

	fd = open(nvme_device, O_RDWR | O_ODIRECT, 0644);
	if (fd < 0) {
		fprintf(stderr, "failed to open '%s'!\n", nvme_device);
		return 1;
	}

	for (i = 0; i < queue_depth; i++) {
		wr_buf = u2_mmap(io_size);
		if (wr_buf == NULL) {
			fprintf(stderr, "failed to mmap write buffer!\n");
			return 1;
		}

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

		tsc_start = rte_get_timer_cycles();
		if (pread(fd, rd_buf, io_size, offset) < io_size) {
			fprintf(stderr, "failed to pread: %d!\n", i);
			return 1;
		}
		rd_lat += rte_get_timer_cycles() - tsc_start;

		u2_munmap(rd_buf, io_size);

		if ((offset += io_size) == ns_size) {
			offset = 0;
		}
	}

	//spdk_nvme_unregister_io_thread();

	printf("write latency: %5.1f us\n", (float) (wr_lat * 1000000) / (queue_depth * tsc_rate));
	printf("read latency:  %5.1f us\n", (float) (rd_lat * 1000000) / (queue_depth * tsc_rate));

	return 0;
}

//static bool
//probe_cb(void *cb_ctx, struct spdk_pci_device *dev)
//{
//	if (u2_ctrlr) {
//		return false;
//	}
//
//	if (spdk_pci_device_has_non_uio_driver(dev)) {
//		fprintf(stderr, "non-UIO/kernel driver detected!\n");
//		return false;
//	}
//
//	return true;
//}

//static void
//attach_cb(void *cb_ctx, struct spdk_pci_device *dev, struct spdk_nvme_ctrlr *ctrlr)
//{
//	u2_ctrlr = ctrlr;
//	u2_ns = spdk_nvme_ctrlr_get_ns(u2_ctrlr, U2_NAMESPACE_ID);
//	u2_ns_sector = spdk_nvme_ns_get_sector_size(u2_ns);
//	u2_ns_size = spdk_nvme_ns_get_size(u2_ns);
//
//	printf("attached to NVMe SSD!\n");
//}

int main(int argc, char *argv[])
{
	int io_size_bytes, bmk_iter, max_io_size_bytes;
	int op;

	io_size_bytes = U2_SIZE_512B;
	bmk_iter = U2_QUEUE_DEPTH;
	max_io_size_bytes = U2_SIZE_MAX;

	while ((op = getopt(argc, argv, "q:m:")) != -1) {
		switch (op) {
		case 'q':
			bmk_iter = atoi(optarg);
			break;
		case 'm':
			max_io_size_bytes = atoi(optarg);
			break;
		default:
			printf("%s -q [queue depth] -m [max I/O size]\n", argv[0]);
			return 1;
		}
	}

	if (rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]), ealargs) < 0) {
		fprintf(stderr, "failed to initialize DPDK EAL!\n");
		return 1;
	}

	printf("\n===========================================\n");
	printf(  "  NVMe/U2 lat kernel - ict.ncic.syssw.ufo"    );
	printf("\n===========================================\n");

	//request_mempool = rte_mempool_create("nvme_request",
	//			U2_REQUEST_POOL_SIZE, spdk_nvme_request_size(),
	//			U2_REQUEST_CACHE_SIZE, U2_REQUEST_PRIVATE_SIZE,
	//			NULL, NULL, NULL, NULL,
	//			SOCKET_ID_ANY, 0);
	//if (request_mempool == NULL) {
	//	fprintf(stderr, "failed to create request pool!\n");
	//	return 1;
	//}

	//if (spdk_nvme_probe(NULL, probe_cb, attach_cb)) {
	//	fprintf(stderr, "failed to probe and attach to NVMe device!\n");
	//	return 1;
	//}

	while (1) {
		printf("u2 latency benchmarking - IO size %d ...\n", io_size_bytes);

		if (u2_lat_bmk_k(io_size_bytes, bmk_iter)) {
			fprintf(stderr, "failed to benchmark latency - IO size %d!\n", io_size_bytes);
			return 1;
		}

		if ((io_size_bytes *= 2) > max_io_size_bytes) {
			break;
		}
	}

	//spdk_nvme_detach(u2_ctrlr);

	return 0;
}

