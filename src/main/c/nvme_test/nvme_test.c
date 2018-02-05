/*
 * nvme_test: testing and verifying ...
 *
 * Author(s)
 *   azq    @qzan9    anzhongqi@ncic.ac.cn
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stddef.h>
#include <fcntl.h>

#include <unistd.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_cycles.h>

#include <spdk/nvme.h>

#define REQUEST_POOL_SIZE    (1024)
#define REQUEST_CACHE_SIZE   (0)
#define REQUEST_PRIVATE_SIZE (0)

#define NAMESPACE_ID         (1)

#define IO_SIZE              (4096)

#define BUFFER_ALIGN         (0x200)

static struct spdk_nvme_ctrlr *ctrlr;

static uint32_t ns_id;
static struct spdk_nvme_ns *ns;
static uint32_t ns_sector;
static uint64_t ns_size;

static struct spdk_nvme_qpair *qpair;

static uint32_t io_size;
static uint32_t io_depth;

struct rte_mempool *request_mempool;
static char *ealargs[] = { "nvme_test", "-c 0x100", "-n 1", };

static unsigned int seed = 0;

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *dev, struct spdk_nvme_ctrlr_opts *opts)
{
	if (ctrlr) {
		return false;
	}

	if (spdk_pci_device_has_non_uio_driver(dev)) {
		fprintf(stderr, "%04x:%02x:%02x.%02x: non-UIO/kernel driver detected!\n",
		                spdk_pci_device_get_domain(dev),
		                spdk_pci_device_get_bus(dev),
		                spdk_pci_device_get_dev(dev),
		                spdk_pci_device_get_func(dev));
		return false;
	}

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *dev, struct spdk_nvme_ctrlr *_ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	ctrlr = _ctrlr;
	ns = spdk_nvme_ctrlr_get_ns(ctrlr, ns_id);
	ns_sector = spdk_nvme_ns_get_sector_size(ns);
	ns_size = spdk_nvme_ns_get_size(ns);
	qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, 0);

	printf("attached to %04x:%02x:%02x.%02x!\n",
	       spdk_pci_device_get_domain(dev),
	       spdk_pci_device_get_bus(dev),
	       spdk_pci_device_get_dev(dev),
	       spdk_pci_device_get_func(dev));
}

static void
io_complete(void *cb_args, const struct spdk_nvme_cpl *completion)
{
	io_depth--;
}

int main(int argc, char *argv[])
{
	uint8_t *wbuf, *rbuf;

	uint64_t offset_in_blocks;
	uint32_t size_in_blocks;

	int rnd;

	// initialize EAL

	if (rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]), ealargs) < 0) {
		fprintf(stderr, "failed to initialize EAL!\n");
		goto FAIL;
	}

	printf("\n==================================\n");
	printf(  "  nvme_test - ict.ncic.syssw.ufo"    );
	printf("\n==================================\n");

	ctrlr = NULL;
	ns_id = NAMESPACE_ID;
	ns = NULL;

	io_size = IO_SIZE;
	io_depth = 0;

	request_mempool = rte_mempool_create("nvme_request",
	                                     REQUEST_POOL_SIZE, spdk_nvme_request_size(),
	                                     REQUEST_CACHE_SIZE, REQUEST_PRIVATE_SIZE,
	                                     NULL, NULL, NULL, NULL,
	                                     SOCKET_ID_ANY, 0);
	if (request_mempool == NULL) {
		fprintf(stderr, "failed to create request pool!\n");
		goto FAIL;
	}

	if (spdk_nvme_probe(NULL, probe_cb, attach_cb)) {
		fprintf(stderr, "failed to probe and attach to NVMe device!\n");
		goto FAIL;
	}

	if (!ctrlr) {
		fprintf(stderr, "failed to probe a suitable controller!\n");
		goto FAIL;
	}

	if (!spdk_nvme_ns_is_active(ns)) {
		fprintf(stderr, "namespace %d is in-active!\n", ns_id);
		goto FAIL;
	}

	if (ns_size < io_size) {
		fprintf(stderr, "invalid I/O size %"PRIu32"!\n", io_size);
		goto FAIL;
	}

	if (!qpair) {
		fprintf(stderr, "failed to allocate queue pair!\n");
		goto FAIL;
	}

	// prepare hugepage memory buffer

	wbuf = rte_malloc(NULL, io_size, BUFFER_ALIGN);
	if (wbuf == NULL) {
		fprintf(stderr, "failed to rte_malloc write buffer!\n");
		goto FAIL;
	}
	memset(wbuf, 0xff, io_size);    // set value to 11111111...

	rbuf = rte_malloc(NULL, io_size, BUFFER_ALIGN);
	if (rbuf == NULL) {
		fprintf(stderr, "failed to rte_malloc read buffer!\n");
		goto FAIL;
	}
	memset(rbuf, 0x00, io_size);    // set value to 00000000...

	// write down, then read back

	rnd = open("/dev/urandom", O_RDONLY);
	read(rnd, wbuf, io_size);    // randomly set the write buffer
	close(rnd);

	offset_in_blocks = io_size / ns_sector;    // any location could be OK ...
	size_in_blocks   = io_size / ns_sector;

	if (spdk_nvme_ns_cmd_write(ns, qpair, wbuf, offset_in_blocks, size_in_blocks, io_complete, NULL, 0)) {
		fprintf(stderr, "failed to submit write request!\n");
		goto FAIL;
	}
	io_depth++;
	while (io_depth > 0) {
		spdk_nvme_qpair_process_completions(qpair, 0);
	}

	if (spdk_nvme_ns_cmd_read(ns, qpair, rbuf, offset_in_blocks, size_in_blocks, io_complete, NULL, 0)) {
		fprintf(stderr, "failed to submit read request!\n");
		goto FAIL;
	}
	io_depth++;
	while (io_depth > 0) {
		spdk_nvme_qpair_process_completions(qpair, 0);
	}

	// compare two buffers

	if (memcmp((void *)wbuf, (void *)rbuf, io_size) == 0) {
		printf("YES!\n");
	}

	// release resources

	if (qpair) {
		spdk_nvme_ctrlr_free_io_qpair(qpair);
	}

	if (ctrlr) {
		spdk_nvme_detach(ctrlr);
	}

	return 0;

FAIL:
	if (qpair) {
		spdk_nvme_ctrlr_free_io_qpair(qpair);
	}

	if (ctrlr) {
		spdk_nvme_detach(ctrlr);
	}

	return 1;
}

