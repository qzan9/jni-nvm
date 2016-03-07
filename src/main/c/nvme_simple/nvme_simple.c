/*
 * nvme_simple/u2_simple
 *
 *   pick the first namespace of the first probed NVMe device and do some simple test.
 *
 *   RESTRICTED to just ONE thread and ONE namespace.
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

#define U2_IO_SIZE			(4096)
#define U2_BUFFER_ALIGN			(0x200)

#define U2_RANDOM			(1)
#define U2_SEQUENTIAL			(0)

#define U2_READ				(1)
#define U2_WRITE			(0)

struct u2_user {
//	char ctrlr_pci_id[16];
	uint32_t ns_id;
	uint32_t io_size;
	uint32_t queue_depth;
	int is_random;
	int is_rw;
};

struct u2_context {
//	char ctrlr_pci_id[16];
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

	int is_random;
	int is_rw;

	uint64_t tsc_start;
	uint64_t tsc_end;
	uint64_t tsc_rate;
};

struct rte_mempool *request_mempool;

static struct u2_user *u2_cfg;
static struct u2_context *u2_ctx;

static __thread unsigned int seed = 0;

static char *ealargs[] = { "nvme_simple", "-c 0x1", "-n 4", };

static int
parse_args(int argc, char **argv)
{
	int op;
	const char *workload;

	u2_cfg = malloc(sizeof(struct u2_user));
	if (u2_cfg == NULL) {
		fprintf(stderr, "failed to malloc u2_cfg!\n");
		return 1;
	}
	memset(u2_cfg, 0, sizeof(struct u2_user));

	u2_cfg->ns_id = U2_NAMESPACE_ID;

	/* -s io_size -q queue_depth -w workload */
	while ((op = getopt(argc, argv, "s:q:w:")) != -1) {
		switch (op) {
		case 's':
			u2_cfg->io_size = atoi(optarg);
			break;
		case 'q':
			u2_cfg->queue_depth = atoi(optarg);
			break;
		case 'w':
			workload = optarg;
			break;
		default:
			return 1;
		}
	}

	if (!u2_cfg->io_size) {
		u2_cfg->io_size = U2_IO_SIZE;
	}

	if (!u2_cfg->queue_depth) {
		u2_cfg->queue_depth = U2_QUEUE_DEPTH;
	}

	if (!strcmp(workload, "read")) {
		u2_cfg->is_random = U2_SEQUENTIAL;
		u2_cfg->is_rw     = U2_READ;
	} else if (!strcmp(workload, "write")) {
		u2_cfg->is_random = U2_SEQUENTIAL;
		u2_cfg->is_rw     = U2_WRITE;
	} else if (!strcmp(workload, "randread")) {
		u2_cfg->is_random = U2_RANDOM;
		u2_cfg->is_rw     = U2_READ;
	} else if (!strcmp(workload, "randwrite")) {
		u2_cfg->is_random = U2_RANDOM;
		u2_cfg->is_rw     = U2_WRITE;
	} else {
		u2_cfg->is_random = U2_RANDOM;
		u2_cfg->is_rw     = U2_READ;
	}

	return 0;
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *dev)
{
	/* just attach to the firstly probed NVMe device. */
	if (u2_ctx->ctrlr) {
		return false;
	}

	if (spdk_pci_device_has_non_uio_driver(dev)) {
		fprintf(stderr, "non-uio/kernel driver attached to NVMe!\n");
		fprintf(stderr, "  controller at PCI address %04x:%02x:%02x.%02x\n",
				spdk_pci_device_get_domain(dev),
				spdk_pci_device_get_bus(dev),
				spdk_pci_device_get_dev(dev),
				spdk_pci_device_get_func(dev));
		fprintf(stderr, "  skipping...\n");
		return false;
	} else {
		printf("attaching to %04x:%02x:%02x.%02x ... ",
			spdk_pci_device_get_domain(dev),
			spdk_pci_device_get_bus(dev),
			spdk_pci_device_get_dev(dev),
			spdk_pci_device_get_func(dev));
		return true;
	}
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *dev, struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_ctrlr_data *cdata;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	/* finish "u2" context setup. */
	u2_ctx->ctrlr = ctrlr;
	snprintf(u2_ctx->ctrlr_name, sizeof(u2_ctx->ctrlr_name), "%s (%s)", cdata->mn, cdata->sn);
	u2_ctx->ns_num = spdk_nvme_ctrlr_get_num_ns(u2_ctx->ctrlr);

	u2_ctx->ns = spdk_nvme_ctrlr_get_ns(u2_ctx->ctrlr, u2_ctx->ns_id);
	u2_ctx->ns_sector_size = spdk_nvme_ns_get_sector_size(u2_ctx->ns);
	u2_ctx->ns_size = spdk_nvme_ns_get_size(u2_ctx->ns);

	printf("attached to [%s - %d]!\n", u2_ctx->ctrlr_name, u2_ctx->ns_id);
}

static int
u2_init(void)
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

	/* preliminarily initialize the "u2" context . */
	u2_ctx = malloc(sizeof(struct u2_context));
	if (u2_ctx == NULL) {
		fprintf(stderr, "failed to allocate u2_context!\n");
		return 1;
	}
	memset(u2_ctx, 0, sizeof(struct u2_context));

	u2_ctx->ns_id       = u2_cfg->ns_id;

	u2_ctx->io_size     = u2_cfg->io_size;
	u2_ctx->queue_depth = (u2_cfg->queue_depth < U2_REQUEST_POOL_SIZE) ? u2_cfg->queue_depth : U2_QUEUE_DEPTH;

	u2_ctx->is_random   = u2_cfg->is_random;
	u2_ctx->is_rw       = u2_cfg->is_random;

	u2_ctx->tsc_rate    = rte_get_timer_hz();

	/* probe and attach to the namespace. */
	if (spdk_nvme_probe(NULL, probe_cb, attach_cb)) {
		fprintf(stderr, "failed to probe and attach to NVMe device!\n");
		return 1;
	}

	/* allocate a group of buffers aligned to 512-byte from hugepages. */
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

	/* count the I/O completions. */
	u2_ctx->io_completed++;
}

static int
u2_perf_test()
{
	int i, rc;

	uint32_t io_size_blocks;

	uint64_t offset_in_ios;
	uint64_t size_in_ios;

	int elapsed_time;
	float io_per_sec, mb_per_sec;

	printf("u2 benchmarking started ...\n");
	printf("\tper I/O size: %d B, queue depth: %d\n", u2_ctx->io_size, u2_ctx->queue_depth);
	printf("\ttotal I/O size: %d MB\n", u2_ctx->io_size * u2_ctx->queue_depth / 1024 / 1024);
	printf("\tRW mode: %s %s\n", u2_cfg->is_random ? "random" : "sequential", u2_cfg->is_rw ? "read" : "write");

	io_size_blocks = u2_ctx->io_size / u2_ctx->ns_sector_size;
	offset_in_ios  = -1;
	size_in_ios    = u2_ctx->ns_size / u2_ctx->io_size;

	if (spdk_nvme_register_io_thread()) {
		fprintf(stderr, "failed to register thread!\n");
		return 1;
	}

	u2_ctx->tsc_start = rte_get_timer_cycles();

	/* submit I/O requests of number of queue_depth. */
	for (i = 0; i < u2_ctx->queue_depth; i++) {
		if (u2_ctx->is_random) {
			offset_in_ios = rand_r(&seed) % size_in_ios;
		} else {
			offset_in_ios++;
			if (offset_in_ios == size_in_ios) {
				offset_in_ios = 0;
			}
		}

		if (u2_ctx->is_rw) {
			rc = spdk_nvme_ns_cmd_read (u2_ctx->ns, u2_ctx->buf[i], offset_in_ios * io_size_blocks,
					io_size_blocks, u2_io_complete, u2_ctx, 0);
		} else {
			rc = spdk_nvme_ns_cmd_write(u2_ctx->ns, u2_ctx->buf[i], offset_in_ios * io_size_blocks,
					io_size_blocks, u2_io_complete, u2_ctx, 0);
		}

		if (rc) {
			fprintf(stderr, "failed to submit request %d!\n", i);
			return rc;
		}
	}

	/* polling/busy-waiting for the completions. */
	while (u2_ctx->io_completed != u2_ctx->queue_depth - 1) {
		spdk_nvme_ctrlr_process_io_completions(u2_ctx->ctrlr, 0);
	}

	u2_ctx->tsc_end = rte_get_timer_cycles();

	spdk_nvme_unregister_io_thread();

	/* calculate and display the performance statistics. */
	elapsed_time = (u2_ctx->tsc_end - u2_ctx->tsc_start) * 1000 * 1000 / u2_ctx->tsc_rate;
	io_per_sec = (float)u2_ctx->io_completed * 1000 * 1000 / elapsed_time;
	mb_per_sec = io_per_sec * u2_ctx->io_size / (1024 * 1024);

	printf("u2 benchmarking results:\n");
	printf("\telapsed time: %d us\n", elapsed_time);
	printf("\tIO: %.2f IO/s, Bandwidth: %.2f MB/s\n", io_per_sec, mb_per_sec);

	return 0;
}

static void
u2_cleanup(void)
{
	int i;

	spdk_nvme_detach(u2_ctx->ctrlr);

	for (i = 0; i < u2_ctx->io_completed; i++) {
		rte_free(u2_ctx->buf[i]);
	}
	free(u2_ctx->buf);

	free(u2_ctx);
	free(u2_cfg);
}

int main(int argc, char *argv[])
{
	/* parse the arguments. */
	if (parse_args(argc, argv)) {
		printf("usage: %s [OPTION]...\n", argv[0]);
		printf("\t-s IO size in bytes\n");
		printf("\t-q IO queue depth\n");
		printf("\t-w IO pattern, must be one of (read, write, randread, randwrite)\n");
		return 1;
	}

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

	/* release the resources. */
	u2_cleanup();

	return 0;
}

