/*
 * nvme_naive/u2_naive
 *
 *   even simpler NVMe access demonstration.
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

struct rte_mempool *request_mempool;

static struct spdk_nvme_ctrlr *u2_ctrlr;
static struct spdk_nvme_ns *u2_ns;

static uint32_t u2_ns_sector;
static uint64_t u2_ns_size;

static char *ealargs[] = { "nvme_simple", "-c 0x1", "-n 4", };

static int
u2_lat_bmk(int payload_size, int bmk_iter)
{
	uint32_t io_size, queue_depth;

	uint32_t io_size_blocks;
	uint64_t offset_in_ios, size_in_ios;
	void *wr_buf, *rd_buf;
	//void **wr_buf, **rd_buf;

	uint64_t tsc_rate, tsc_start;
	uint64_t wr_lat, rd_lat;

	int i, rc;

	io_size = payload_size;
	queue_depth = bmk_iter;

	io_size_blocks = io_size / u2_ns_sector;
	offset_in_ios = 0;
	size_in_ios = u2_ns_size / io_size;

	//wr_buf = malloc(queue_depth * sizeof(void *));
	//if (wr_buf == NULL) {
	//	fprintf(stderr, "failed to malloc write buffer array!\n");
	//	return 1;
	//}
	//rd_buf = malloc(queue_depth * sizeof(void *));
	//if (rd_buf == NULL) {
	//	fprintf(stderr, "failed to malloc read buffer array!\n");
	//	return 1;
	//}
	//for (i = 0; i < queue_depth; i++) {
	//	wr_buf[i] = rte_malloc(NULL, io_size, U2_BUFFER_ALIGN);
	//	if (wr_buf[i] == NULL) {
	//		fprintf(stderr, "failed to rte_malloc write buffer element!\n");
	//		return 1;
	//	}
	//	rd_buf[i] = rte_malloc(NULL, io_size, U2_BUFFER_ALIGN);
	//	if (rd_buf[i] == NULL) {
	//		fprintf(stderr, "failed to rte_malloc read buffer element!\n");
	//		return 1;
	//	}
	//}

	tsc_rate = rte_get_tsc_hz();
	wr_lat = 0;
	rd_lat = 0;

	if (spdk_nvme_register_io_thread()) {
		fprintf(stderr, "failed to register thread!\n");
		return 1;
	}

	for (i = 0; i < queue_depth; i++) {
		wr_buf = rte_malloc(NULL, io_size, U2_BUFFER_ALIGN);
		if (wr_buf == NULL) {
			fprintf(stderr, "failed to rte_malloc write buffer %d!\n", i);
			return 1;
		}
		tsc_start = rte_get_timer_cycles();
		if (rc = spdk_nvme_ns_cmd_write(u2_ns, wr_buf, offset_in_ios * io_size_blocks, io_size_blocks, NULL, NULL, 0)) {
			fprintf(stderr, "failed to submit write request %d, error %d!\n", i, rc);
			return 1;
		}
		while (spdk_nvme_ctrlr_process_io_completions(u2_ctrlr, 0) != 1);
		wr_lat += rte_get_timer_cycles() - tsc_start;
		rte_free(wr_buf);

		rd_buf = rte_malloc(NULL, io_size, U2_BUFFER_ALIGN);
		if (rd_buf == NULL) {
			fprintf(stderr, "failed to rte_malloc read buffer %d!\n", i);
			return 1;
		}
		tsc_start = rte_get_timer_cycles();
		if (rc = spdk_nvme_ns_cmd_read(u2_ns, rd_buf, offset_in_ios * io_size_blocks, io_size_blocks, NULL, NULL, 0)) {
			fprintf(stderr, "failed to submit read request %d, error %d!\n", i, rc);
			return 1;
		}
		while (spdk_nvme_ctrlr_process_io_completions(u2_ctrlr, 0) != 1);
		rd_lat += rte_get_timer_cycles() - tsc_start;

		rte_free(rd_buf);

		if (++offset_in_ios == size_in_ios) {
			offset_in_ios = 0;
		}
	}

	spdk_nvme_unregister_io_thread();

	//for (i = 0; i < queue_depth; i++) {
	//	rte_free(wr_buf[i]);
	//	rte_free(rd_buf[i]);
	//}
	//free(wr_buf);
	//free(rd_buf);

	printf("write latency: %5.1f us\n", (float) (wr_lat * 1000000) / (queue_depth * tsc_rate));
	printf("read latency:  %5.1f us\n", (float) (rd_lat * 1000000) / (queue_depth * tsc_rate));

	return 0;
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *dev)
{
	if (u2_ctrlr) {
		return false;
	}

	if (spdk_pci_device_has_non_uio_driver(dev)) {
		fprintf(stderr, "non-UIO/kernel driver detected!\n");
		return false;
	}

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *dev, struct spdk_nvme_ctrlr *ctrlr)
{
	u2_ctrlr = ctrlr;
	u2_ns = spdk_nvme_ctrlr_get_ns(u2_ctrlr, U2_NAMESPACE_ID);
	u2_ns_sector = spdk_nvme_ns_get_sector_size(u2_ns);
	u2_ns_size = spdk_nvme_ns_get_size(u2_ns);

	printf("attached to NVMe SSD!\n");
}

int main(int argc, char *argv[])
{
	int io_size_bytes, bmk_iter;
	int op;

	io_size_bytes = U2_SIZE_512B;
	bmk_iter = U2_QUEUE_DEPTH;

	while ((op = getopt(argc, argv, "q:")) != -1) {
		switch (op) {
		case 'q':
			bmk_iter = atoi(optarg);
			break;
		default:
			printf("%s -q [queue depth]\n", argv[0]);
			return 1;
		}
	}

	if (rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]), ealargs) < 0) {
		fprintf(stderr, "failed to initialize DPDK EAL!\n");
		return 1;
	}

	request_mempool = rte_mempool_create("nvme_request",
				U2_REQUEST_POOL_SIZE, spdk_nvme_request_size(),
				U2_REQUEST_CACHE_SIZE, U2_REQUEST_PRIVATE_SIZE,
				NULL, NULL, NULL, NULL,
				SOCKET_ID_ANY, 0);
	if (request_mempool == NULL) {
		fprintf(stderr, "failed to create request pool!\n");
		return 1;
	}

	if (spdk_nvme_probe(NULL, probe_cb, attach_cb)) {
		fprintf(stderr, "failed to probe and attach to NVMe device!\n");
		return 1;
	}

	while (1) {
		printf("latency benchmarking - IO size %d ...\n", io_size_bytes);

		if (u2_lat_bmk(io_size_bytes, bmk_iter)) {
			fprintf(stderr, "failed to benchmark latency - IO size %d!\n", io_size_bytes);
			return 1;
		}

		if ((io_size_bytes *= 2) > U2_SIZE_MAX) {
			break;
		}
	}

	spdk_nvme_detach(u2_ctrlr);

	return 0;
}

