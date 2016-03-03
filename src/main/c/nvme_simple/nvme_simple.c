/*
 * nvme_simple/u2_simple
 *
 *   pick the first namespace of the first probed NVMe device and do some simple test.
 *
 *   limited to just ONE thread and ONE namespace.
 */

#include <stdio.h>
#include <stdlib.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_cycles.h>

#include <spdk/nvme.h>

#define U2_REQUEST_POOL_SIZE		(8192)
#define U2_REQUEST_CACHE_SIZE		(128)
#define U2_REQUEST_PRIVATE_SIZE		(0)

#define U2_TASK_POOL_SIZE		(8192)
#define U2_TASK_CACHE_SIZE		(128)
#define U2_TASK_PRIVATE_SIZE		(0)

#define U2_NAMESPACE_ID			(1)

#define U2_QUEUE_DEPTH			(1024)

#define U2_BUFFER_SIZE			(4096)
#define U2_BUFFER_ALIGN			(0x200)

#define U2_NVME_READ			(1)
#define U2_NVME_WRITE			(0)

struct u2_context {
//	char ctrlr_pci_id[13];
	struct spdk_nvme_ctrlr *ctrlr;
	char ctrlr_name[1024];
	uint32_t ns_num;

	uint32_t ns_id;
	struct spdk_nvme_ns *ns;
	uint32_t ns_sector_size;
	uint64_t ns_size;

	uint32_t io_size;
	uint32_t queue_depth;
	uint32_t io_completed;
	void **buf;

	uint64_t size_in_ios;
	uint64_t offset_in_ios;

	int is_random;
	int is_rw;

	uint64_t tsc_start;
	uint64_t tsc_end;
	uint64_t tsc_rate;
};

struct rte_mempool *request_mempool;

static struct u2_context *u2_ctx;

static __thread unsigned int seed = 0;

static char *ealargs[] = { "nvme_simple", "-c 0x1", "-n 4", };

static int
u2_init()
{
	int i, rc;

	/* initialize DPDK EAL. */
	rc = rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]),
				(char **)(void *)(uintptr_t)ealargs);
	if (rc < 0) {
		fprintf(stderr, "failed to initialize DPDK EAL!\n");
		return rc;
	}

	printf("\n=======================================\n");
	printf(  "  NVMe/U2 simple - ict.ncic.syssw.ufo"    );
	printf("\n=======================================\n");

	/* preliminarily initialize the "u2" context . */
	u2_ctx = malloc(sizeof(struct u2_context));
	if (u2_ctx == NULL) {
		fprintf(stderr, "failed to allocate u2_context!\n");
		return 1;
	}
	memset(u2_ctx, 0, sizeof(struct u2_context));

	u2_ctx->ns_id       = U2_NAMESPACE_ID;
	u2_ctx->io_size     = U2_BUFFER_SIZE;
	u2_ctx->queue_depth = U2_QUEUE_DEPTH;
	u2_ctx->tsc_rate    = rte_get_timer_hz();

	/* allocate memory pools for NVMe requests. */
	request_mempool = rte_mempool_create("nvme_request",
				U2_REQUEST_POOL_SIZE, spdk_nvme_request_size(),
				U2_REQUEST_CACHE_SIZE, U2_REQUEST_PRIVATE_SIZE,
				NULL, NULL, NULL, NULL,
				SOCKET_ID_ANY, 0);
	if (request_mempool == NULL) {
		fprintf(stderr, "failed to create request pool!\n");
		return 1;
	}

	/* allocate a group of buffers aligned to 512-byte. */
	u2_ctx->buf = malloc(u2_ctx->queue_depth * sizeof(void *));
	if (u2_ctx->buf == NULL) {
		fprintf(stderr, "failed to malloc buffer!\n");
		return 1;
	}
	for (i = 0; i < u2_ctx->queue_depth; i++) {
		u2_ctx->buf[i] = rte_malloc(NULL, u2_ctx->io_size, U2_BUFFER_ALIGN);
		if (u2_ctx->buf[i] == NULL) {
			fprintf(stderr, "failed to rte_malloc buffer!\n");
			return 1;
		}
	}

	return 0;
}

static void
u2_io_complete(void *cb_args, const struct spdk_nvme_cpl *completion)
{
	struct u2_context *u2_ctx = (struct u2_context *)cb_args;

	u2_ctx->io_completed++;

	if (u2_ctx->queue_depth == 0) {
		u2_ctx->tsc_end = rte_get_timer_cycles();
	}
}

static int
u2_perf_test()
{
	struct u2_task *task;
	uint64_t offset_in_ios;
	uint32_t io_size_blocks;

	int elapsed_time;
	float io_per_sec, mb_per_sec;

	int rc;

	printf("u2 benchmarking started ... ");

	io_size_blocks = u2_ctx->io_size / u2_ctx->ns_sector_size;
	u2_ctx->tsc_start = rte_get_timer_cycles();

	if (spdk_nvme_register_io_thread()) {
		fprintf(stderr, "failed to register thread!\n");
		return 1;
	}

	while (u2_ctx->queue_depth-- > 0) {
		if (u2_ctx->is_random) {
			offset_in_ios = rand_r(&seed) % u2_ctx->size_in_ios;
		}
		else {
			offset_in_ios = u2_ctx->offset_in_ios++;
			if (u2_ctx->offset_in_ios == u2_ctx->size_in_ios) {
				u2_ctx->offset_in_ios = 0;
			}
		}

		if (u2_ctx->is_rw) {
			rc = spdk_nvme_ns_cmd_read (u2_ctx->ns, task->buf[u2_ctx->io_completed], offset_in_ios * io_size_blocks,
					io_size_blocks, u2_io_complete, u2_ctx, 0);
		}
		else {
			rc = spdk_nvme_ns_cmd_write(u2_ctx->ns, task->buf[u2_ctx->io_completed], offset_in_ios * io_size_blocks,
					io_size_blocks, u2_io_complete, u2_ctx, 0);
		}

		if (rc) {
			fprintf(stderr, "failed to submit I/O request!\n");
			return rc;
		}
	}

	while (!u2_ctx->tsc_end) {
		spdk_nvme_ctrlr_process_io_completions(u2_ctx->ctrlr, 0);
	}

	spdk_nvme_unregister_io_thread();

	printf("done!\n");

	elapsed_time = (u2_ctx->tsc_end - u2_ctx->tsc_start) * 1000 * 1000 / u2_ctx->tsc_rate;
	printf("elapsed time: %d us\n", elapsed_time);
	io_per_sec = (float)u2_ctx->io_completed * 1000 * 1000 / elapsed_time;
	mb_per_sec = io_per_sec * u2_ctx->io_size / (1024 * 1024);
	printf("IO: %.2f IO/s, Bandwidth: %.2f MB/s\n", io_per_sec, mb_per_sec);

	return 0;
}

static void
u2_cleanup(void)
{
	int i;

	for (i = 0; i < u2_ctx->io_completed; i++) {
		rte_free(u2_ctx->buf[i]);
	}
	free(u2_ctx->buf);

	free(u2_ctx);
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *dev)
{
	if (u2_ctx->ctrlr) {
		return false;
	} else {
		if (spdk_pci_device_has_non_uio_driver(dev)) {
			fprintf(stderr, "non-uio/kernel driver attached to NVMe!\n");
			fprintf(stderr, "  controller at PCI address %04x:%02x:%02x.%02x\n",
					spdk_pci_device_get_domain(dev),
					spdk_pci_device_get_bus(dev),
					spdk_pci_device_get_dev(dev),
					spdk_pci_device_get_func(dev));
			fprintf(stderr, "  skipping...\n");
			return false;
		}
		else {
			printf("attaching to %04x:%02x:%02x.%02x ... ",
				spdk_pci_device_get_domain(dev),
				spdk_pci_device_get_bus(dev),
				spdk_pci_device_get_dev(dev),
				spdk_pci_device_get_func(dev));
			return true;
		}
	}
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *dev, struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_ctrlr_data *cdata;

	printf("attached to %04x:%02x:%02x.%02x!\n",
		spdk_pci_device_get_domain(dev),
		spdk_pci_device_get_bus(dev),
		spdk_pci_device_get_dev(dev),
		spdk_pci_device_get_func(dev));

	u2_ctx = (struct u2_context *)cb_ctx;
	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	/* finish initializing the whole "u2" test context. */
	u2_ctx->ctrlr = ctrlr;
	snprintf(u2_ctx->ctrlr_name, sizeof(u2_ctx->ctrlr_name), "%s (%s)", cdata->mn, cdata->sn);
	u2_ctx->ns_num = spdk_nvme_ctrlr_get_num_ns(u2_ctx->ctrlr);

	u2_ctx->ns = spdk_nvme_ctrlr_get_ns(u2_ctx->ctrlr, u2_ctx->ns_id);
	u2_ctx->ns_sector_size = spdk_nvme_ns_get_sector_size(u2_ctx->ns);
	u2_ctx->ns_size = spdk_nvme_ns_get_size(u2_ctx->ns);

	u2_ctx->size_in_ios = u2_ctx->ns_size / u2_ctx->io_size;
}

int main(int argc, char *argv[])
{
	/* initialize and prepare for the benchmark. */
	if (u2_init()) {
		fprintf(stderr, "failed to initialize u2 context!\n");
		return 1;
	}

	/* execute I/O performance benchmark. */
	if (u2_perf_test()) {
		fprintf(stderr, "failed to benchmark I/O!\n");
		return 1;
	}

	u2_cleanup();

	return 0;
}

