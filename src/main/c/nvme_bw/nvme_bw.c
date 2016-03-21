/*
 * nvme_bw/u2_bw
 *
 *   RESTRICTED to just ONE thread and ONE controller and ONE namespace.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stddef.h>

#include <unistd.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_cycles.h>

#include <spdk/nvme.h>

#define U2_REQUEST_POOL_SIZE		(8192)
#define U2_REQUEST_CACHE_SIZE		(0)
#define U2_REQUEST_PRIVATE_SIZE		(0)

#define U2_NAMESPACE_ID			(1)

#define U2_IO_DEPTH			(128)

#define U2_IO_SIZE_4KB			(4096)

#define U2_BUFFER_ALIGN			(0x200)

#define U2_RANDOM			(1)
#define U2_SEQUENTIAL			(0)

#define U2_READ				(1)
#define U2_WRITE			(0)

#define U2_TIME_SEC			(180)

//static struct spdk_pci_device *u2_dev;

static struct spdk_nvme_ctrlr *u2_ctrlr;
static char u2_ctrlr_name[1024];
static uint32_t u2_ctrlr_ns_num;

static uint32_t u2_ns_id;
static struct spdk_nvme_ns *u2_ns;
static uint32_t u2_ns_sector_size;
static uint64_t u2_ns_size;

static struct spdk_nvme_qpair *u2_qpair;

static uint32_t io_size_in_bytes;
static uint32_t io_depth;
static uint32_t io_completed;

static uint8_t is_random;
static uint8_t is_rw;

static char *core_mask;
static uint8_t mem_chn;
static uint32_t time_in_sec;

struct rte_mempool *request_mempool;

static unsigned int seed = 0;

static char *ealargs[] = { "nvme_bw", "-c 0x1", "-n 1", };

static int
parse_args(int argc, char **argv)
{
	int op;
	char *workload;

	u2_ctrlr = NULL;
	u2_ns_id = U2_NAMESPACE_ID;
	u2_ns = NULL;

	while ((op = getopt(argc, argv, "s:q:w:c:n:t:")) != -1) {
		switch (op) {
		case 's':
			io_size_in_bytes = atoi(optarg);
			break;
		case 'q':
			io_depth = atoi(optarg);
			break;
		case 'w':
			workload = optarg;
			break;
		case 'c':
			core_mask = optarg;
			break;
		case 'n':
			mem_chn = atoi(optarg);
			break;
		case 't':
			time_in_sec = atoi(optarg);
		default:
			return 1;
		}
	}

	if (!io_size_in_bytes) {
		io_size_in_bytes = U2_IO_SIZE_4KB;
	}

	if (!io_depth || io_depth >= U2_REQUEST_POOL_SIZE) {
		io_depth = U2_IO_DEPTH;
	}

	io_completed = 0;

	is_random = U2_RANDOM;
	is_rw = U2_READ;
	if (workload) {
		if (!strcmp(workload, "read")) {
			is_random = U2_SEQUENTIAL;
			is_rw = U2_READ;
		//} else if (!strcmp(workload, "randread")) {
		//	is_random = U2_RANDOM;
		//	is_rw = U2_READ;
		} else if (!strcmp(workload, "write")) {
			is_random = U2_SEQUENTIAL;
			is_rw = U2_WRITE;
		} else if (!strcmp(workload, "randwrite")) {
			is_random = U2_RANDOM;
			is_rw = U2_WRITE;
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

	if (mem_chn >= 2 && mem_chn <= 4) {
		ealargs[2] = malloc(sizeof("-n 1"));
		if (ealargs[2] == NULL) {
			fprintf(stderr, "failed to malloc ealargs[2]!\n");
			return 1;
		}
		sprintf(ealargs[2], "-n %d", mem_chn);
	} else {
		mem_chn = 1;
	}

	if (!time_in_sec) {
		time_in_sec = U2_TIME_SEC;
	}

	return 0;
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *dev, struct spdk_nvme_ctrlr_opts *opts)
{
	if (u2_ctrlr) {
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
attach_cb(void *cb_ctx, struct spdk_pci_device *dev, struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	const struct spdk_nvme_ctrlr_data *cdata;

	u2_ctrlr = ctrlr;

	cdata = spdk_nvme_ctrlr_get_data(u2_ctrlr);

	memset(u2_ctrlr_name, 0, sizeof(u2_ctrlr_name));
	snprintf(u2_ctrlr_name, sizeof(u2_ctrlr_name), "%s (%s)", cdata->mn, cdata->sn);
	u2_ctrlr_ns_num = spdk_nvme_ctrlr_get_num_ns(u2_ctrlr);

	u2_ns = spdk_nvme_ctrlr_get_ns(u2_ctrlr, u2_ns_id);
	u2_ns_sector_size = spdk_nvme_ns_get_sector_size(u2_ns);
	u2_ns_size = spdk_nvme_ns_get_size(u2_ns);

	u2_qpair = spdk_nvme_ctrlr_alloc_io_qpair(u2_ctrlr, 0);

	printf("attached to %04x:%02x:%02x.%02x!\n",
			spdk_pci_device_get_domain(dev),
			spdk_pci_device_get_bus(dev),
			spdk_pci_device_get_dev(dev),
			spdk_pci_device_get_func(dev));
}

static int
u2_init(void)
{
	if (rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]), ealargs) < 0) {
		fprintf(stderr, "failed to initialize DPDK EAL!\n");
		return 1;
	}

	printf("\n===================================\n");
	printf("  NVMe/U2 BW - ict.ncic.syssw.ufo"      );
	printf("\n===================================\n");

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

	if (!u2_ctrlr) {
		fprintf(stderr, "failed to probe a suitable controller!\n");
		return 1;
	}

	if (!spdk_nvme_ns_is_active(u2_ns)) {
		fprintf(stderr, "namespace %d of controller %s is IN-ACTIVE!\n", u2_ns_id, u2_ctrlr_name);
		return 1;
	}

	if (!u2_qpair) {
		fprintf(stderr, "failed to allocate queue pair!\n");
		return 1;
	}
	
	return 0;
}

static void
u2_io_complete(void *cb_args, const struct spdk_nvme_cpl *completion)
{
	io_completed++;
}

static int
u2_bench(void)
{
	int i, rc;

	//void *buf;
	void **buf;
	uint32_t io_size_in_blocks;
	uint64_t offset_in_ios;
	uint64_t size_in_ios;

	uint64_t tsc_elapsed, tsc_start;
	uint64_t tsc_rate;

	float io_per_sec, mb_per_sec;

	//buf = rte_malloc(NULL, io_size, U2_BUFFER_ALIGN);
	//if (buf == NULL) {
	//	fprintf(stderr, "failed to rte_malloc buffer!\n");
	//	return 1;
	//}
	//memset(buf, 0xff, io_size);

	buf = malloc(io_depth * sizeof(void *));
	if (buf == NULL) {
		fprintf(stderr, "failed to malloc buffer!\n");
		return 1;
	}
	for (i = 0; i < io_depth; i++) {
		buf[i] = rte_malloc(NULL, io_size_in_bytes, U2_BUFFER_ALIGN);
		if (buf[i] == NULL) {
			fprintf(stderr, "failed to rte_malloc buffer!\n");
			return 1;
		}
	}

	io_size_in_blocks = io_size_in_bytes / u2_ns_sector_size;
	offset_in_ios = -1;
	size_in_ios = u2_ns_size / io_size_in_bytes;

	tsc_elapsed = 0;
	tsc_rate = rte_get_timer_hz();

	tsc_start = rte_get_timer_cycles();
	for (i = 0; i < io_depth; i++) {
		if (is_random) {
			offset_in_ios = rand_r(&seed) % size_in_ios;
		} else {
			if (++offset_in_ios == size_in_ios) {
				offset_in_ios = 0;
			}
		}

		if (is_rw) {
			//rc = spdk_nvme_ns_cmd_read (u2_ns, buf, offset_in_ios * io_size_blocks, io_size_blocks, u2_io_complete, NULL, 0);
			rc = spdk_nvme_ns_cmd_read (u2_ns, u2_qpair, buf[i], offset_in_ios * io_size_in_blocks, io_size_in_blocks, u2_io_complete, NULL, 0);
		} else {
			//rc = spdk_nvme_ns_cmd_write(u2_ns, buf, offset_in_ios * io_size_blocks, io_size_blocks, u2_io_complete, NULL, 0);
			rc = spdk_nvme_ns_cmd_write(u2_ns, u2_qpair, buf[i], offset_in_ios * io_size_in_blocks, io_size_in_blocks, u2_io_complete, NULL, 0);
		}

		if (rc) {
			fprintf(stderr, "failed to submit I/O request %d!\n", i);
			return rc;
		}
	}
	/* polling/busy-waiting for the completions. */
	while (io_completed < io_depth) {
		spdk_nvme_qpair_process_completions(u2_qpair, 0);
	}
	tsc_elapsed = rte_get_timer_cycles() - tsc_start;

	//rte_free(buf);

	for (i = 0; i < io_depth; i++) {
		rte_free(buf[i]);
	}
	free(buf);

	io_per_sec = (float) io_completed / tsc_elapsed * tsc_rate;
	//mb_per_sec = (float) io_per_sec * io_size / (1024 * 1024);
	mb_per_sec = (float) io_per_sec * io_size_in_bytes / 1000000;

	printf("u2 benchmarking results:\n");
	printf("\telapsed time: %.2f us\n", (float) (tsc_elapsed * 1000000) / tsc_rate);
	printf("\tIO: %.2f IO/s, Bandwidth: %.2f MB/s\n", io_per_sec, mb_per_sec);

	return 0;
}

static void
u2_cleanup(void)
{
	spdk_nvme_ctrlr_free_io_qpair(u2_qpair);

	spdk_nvme_detach(u2_ctrlr);

	if (core_mask) {
		free(ealargs[1]);
	}

	if (mem_chn >= 2 && mem_chn <= 4) {
		free(ealargs[2]);
	}
}

int main(int argc, char *argv[])
{
	if (parse_args(argc, argv)) {
		printf("usage: %s [OPTION]...\n", argv[0]);
		printf("\t-s [I/O size in bytes]\n");
		printf("\t-q [I/O queue depth]\n");
		printf("\t-w [I/O workload (read, randread, write, randwrite)]\n");
		printf("\t-n [memory channels]\n");
		printf("\t-c [core mask]\n");
		printf("\t-t [time in seconds]\n");
		return 1;
	}

	if (u2_init()) {
		fprintf(stderr, "failed to initialize u2 context!\n");
		return 1;
	}

	printf("u2 benchmarking started ...\n");
	printf("\tmemory channels: %d\n", mem_chn);
	printf("\tper I/O size: %d B, I/O depth: %d, RW type: %s %s, Time: %d\n",
		io_size_in_bytes, io_depth, is_random ? "random" : "sequential", is_rw ? "read" : "write", time_in_sec);
	if (u2_bench()) {
		fprintf(stderr, "failed to benchmark!\n");
		return 1;
	}

	u2_cleanup();

	return 0;
}

