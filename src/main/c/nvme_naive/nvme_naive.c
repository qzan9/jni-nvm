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

#define U2_REQUEST_POOL_SIZE		(8192)
#define U2_REQUEST_CACHE_SIZE		(128)
#define U2_REQUEST_PRIVATE_SIZE		(0)

#define U2_NAMESPACE_ID			(1)
#define U2_QUEUE_DEPTH			(7168)

#define U2_SIZE_512B			(512)
#define U2_SIZE_4KB			(4096)
#define U2_SIZE_128KB			(131072)
#define U2_SIZE_1MB			(1048576)

#define U2_SLAB_SIZE			(1048576)

#define U2_BUFFER_ALIGN			(0x200)

struct rte_mempool *request_mempool;

static struct spdk_nvme_ctrlr *u2_ctrlr;
static struct spdk_nvme_ns *u2_ns;

static uint32_t u2_ns_sector;
static uint64_t u2_ns_size;

static char *ealargs[] = { "nvme_simple", "-c 0x1", "-n 4", };

static int
lat_bmk(uint32_t payload_size)
{
	uint32_t io_size, io_size_blocks;
	uint64_t offset_in_ios, size_in_ios;
	void *rd_buf, *wr_buf;

	uint64_t tsc_rate, tsc_start;
	int rd_lat, wr_lat;

	int i;

	io_size = payload_size;
	io_size_blocks = io_size / u2_ns_sector;
	offset_in_ios = 0;
	size_in_ios = u2_ns_size / io_size;

	rd_lat = 0;
	wr_lat = 0;

	tsc_rate = rte_get_tsc_hz();

	for (i = 0; i < U2_QUEUE_DEPTH; i++) {
		rd_buf = rte_malloc(NULL, io_size, U2_BUFFER_ALIGN);
		wr_buf = rte_malloc(NULL, io_size, U2_BUFFER_ALIGN);

		if (offset_in_ios == size_in_ios) {
			offset_in_ios = 0;
		}

		tsc_start = rte_get_timer_cycles();
		if (spdk_nvme_ns_cmd_write(u2_ns, wr_buf, i * io_size_blocks, io_size_blocks, NULL, NULL, 0)) {
			fprintf(stderr, "failed to submit write request %d!\n", i);
			return 1;
		}
		while (spdk_nvme_ctrlr_process_io_completions(u2_ctrlr, 0) != 1);
		wr_lat += rte_get_timer_cycles() - tsc_start;

		tsc_start = rte_get_timer_cycles();
		if (spdk_nvme_ns_cmd_read(u2_ns, rd_buf, i * io_size_blocks, io_size_blocks, NULL, NULL, 0)) {
			fprintf(stderr, "failed to submit read request %d!\n", i);
			return 1;
		}
		while (spdk_nvme_ctrlr_process_io_completions(u2_ctrlr, 0) != 1);
		rd_lat += rte_get_timer_cycles() - tsc_start;

		if (!memcmp(rd_buf, wr_buf, io_size)) {
			fprintf(stderr, "oops, error R/W SSD ...\n");
			return 1;
		}

		rte_free(rd_buf);
		rte_free(wr_buf);
	}

	printf("read latency: %d us\n", rd_lat * 1000 * 1000 / U2_QUEUE_DEPTH / tsc_rate);
	printf("write latency: %d us\n", wr_lat * 1000 * 1000 / U2_QUEUE_DEPTH / tsc_rate);

	return 0;
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *dev)
{
	if (u2_ctrlr) {
		return false;
	}

	if (spdk_pci_device_has_non_uio_driver(dev)) {
		fprintf(stderr, "non-uio/kernel driver detected!\n");
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
	int i;

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

	if (lat_bmk(U2_SIZE_512B)) {
		fprintf(stderr, "failed to benchmark 512B I/O latency!\n");
		return 1;
	}

	if (lat_bmk(U2_SIZE_4KB)) {
		fprintf(stderr, "failed to benchmark 4KB I/O latency!\n");
		return 1;
	}

	if (lat_bmk(U2_SIZE_128KB)) {
		fprintf(stderr, "failed to benchmark 128KB I/O latency!\n");
		return 1;
	}

	if (lat_bmk(U2_SIZE_1MB)) {
		fprintf(stderr, "failed to benchmark 1MB I/O latency!\n");
		return 1;
	}

	spdk_nvme_detach(u2_ctrlr);

	return 0;
}

