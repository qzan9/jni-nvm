/*
 * nvme_lat/u2_lat: simple latency benchmarking.
 *
 * NOTE: RESTRICTED to just ONE thread and ONE controller and ONE namespace.
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

#define U2_REQUEST_POOL_SIZE    (1024)
#define U2_REQUEST_CACHE_SIZE   (0)
#define U2_REQUEST_PRIVATE_SIZE (0)

#define U2_NAMESPACE_ID         (1)

#define U2_SIZE_512B            (512)
#define U2_SIZE_1KB             (1024)
#define U2_SIZE_4KB             (4096)
#define U2_SIZE_8KB             (8192)
#define U2_SIZE_16KB            (16384)
#define U2_SIZE_64KB            (65536)
#define U2_SIZE_128KB           (131072)
#define U2_SIZE_256KB           (262144)
#define U2_SIZE_512KB           (524288)
#define U2_SIZE_1MB             (1048576)
#define U2_SIZE_2MB             (2097152)
#define U2_SIZE_4MB             (4194304)

#define U2_IO_SIZE_MIN          (U2_SIZE_512B)
#define U2_IO_SIZE_MAX          (U2_SIZE_4MB)
#define U2_BUFFER_ALIGN         (0x200)

#define U2_IO_NUM               (8192)

#define U2_RANDOM               (1)
#define U2_SEQUENTIAL           (0)
#define U2_READ                 (1)
#define U2_WRITE                (0)

//#define U2_TIME_SEC             (4)

static struct spdk_nvme_ctrlr *u2_ctrlr;

static uint32_t u2_ns_id;
static struct spdk_nvme_ns *u2_ns;
static uint32_t u2_ns_sector;
static uint64_t u2_ns_size;

static struct spdk_nvme_qpair *u2_qpair;

static uint32_t io_size;
static uint64_t io_num;
static uint32_t io_depth;

static uint8_t is_random;
static uint8_t is_rw;

static char *core_mask;
static uint8_t mem_chn;
//static uint32_t time_in_sec;

struct rte_mempool *request_mempool;
static char *ealargs[] = { "nvme_lat", "-c 0x1", "-n 1", };

static unsigned int seed = 0;

static int
parse_args(int argc, char **argv)
{
	int op;
	char *workload;

	u2_ctrlr = NULL;
	u2_ns_id = U2_NAMESPACE_ID;
	u2_ns = NULL;

	io_size = U2_IO_SIZE_MIN;

	while ((op = getopt(argc, argv, "q:w:c:n:")) != -1) {
	//while ((op = getopt(argc, argv, "w:c:n:t:")) != -1) {
		switch (op) {
		case 'q':
			io_num = atoi(optarg);
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
		//case 't':
		//	time_in_sec = atoi(optarg);
		//	break;
		default:
			return 1;
		}
	}

	if (!io_num) {
		io_num = U2_IO_NUM;
	}

	io_depth = 0;

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

	//if (!time_in_sec) {
	//	time_in_sec = U2_TIME_SEC;
	//}

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
	u2_ctrlr = ctrlr;
	u2_ns = spdk_nvme_ctrlr_get_ns(u2_ctrlr, u2_ns_id);
	u2_ns_sector = spdk_nvme_ns_get_sector_size(u2_ns);
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
	if (rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]),ealargs) < 0) {
		fprintf(stderr, "failed to initialize DPDK EAL!\n");
		return 1;
	}

	printf("\n========================================\n");
	printf(  "  nvme_lat/u2_lat - ict.ncic.syssw.ufo"    );
	printf("\n========================================\n");

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
		fprintf(stderr, "namespace %d is IN-ACTIVE!\n", u2_ns_id);
		return 1;
	}

	if (u2_ns_size < io_size) {
		fprintf(stderr, "invalid I/O size %"PRIu32"!\n", io_size);
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
	//io_num++;
	io_depth--;
}

static int
u2_lat_bench(void)
{
	int i, rc;
	//int rc;

	void *buf;

	uint32_t io_size_blocks;
	uint64_t offset_in_ios, size_in_ios;

	uint64_t tsc_rate;
	uint64_t tsc_start, tsc_elapsed;
	//uint64_t tsc_end;

	buf = rte_malloc(NULL, io_size, U2_BUFFER_ALIGN);
	if (buf == NULL) {
		fprintf(stderr, "failed to rte_malloc buffer!\n");
		return 1;
	}
	memset(buf, 0xff, io_size);

	//io_num = 0;
	//io_depth = 0;

	io_size_blocks = io_size / u2_ns_sector;
	offset_in_ios = -1;
	size_in_ios = u2_ns_size / io_size;

	tsc_rate = rte_get_tsc_hz();
	tsc_elapsed = 0;
	tsc_start = rte_get_timer_cycles();
	//tsc_end = rte_get_timer_cycles() + time_in_sec * tsc_rate;
	for (i = 0; i < io_num; i++) {
	//while (1) {
		if (is_random) {
			offset_in_ios = rand_r(&seed) % size_in_ios;
		} else {
			if (++offset_in_ios >= size_in_ios) {
				offset_in_ios = 0;
			}
		}

		if (is_rw) {
			rc = spdk_nvme_ns_cmd_read (u2_ns, u2_qpair, buf, offset_in_ios * io_size_blocks, io_size_blocks, u2_io_complete, NULL, 0);
		} else {
			rc = spdk_nvme_ns_cmd_write(u2_ns, u2_qpair, buf, offset_in_ios * io_size_blocks, io_size_blocks, u2_io_complete, NULL, 0);
		}
		if (rc) {
			fprintf(stderr, "failed to submit request %d!\n", i);
			//fprintf(stderr, "failed to submit request %d!\n", io_num);
			return rc;
		}
		io_depth++;    // for latency benchmarking, queue depth stays at 1.

		while (io_depth > 0) {
			spdk_nvme_qpair_process_completions(u2_qpair, 0);
		}

		//if (rte_get_timer_cycles() > tsc_end) {
		//	break;
		//}
	}
	tsc_elapsed = rte_get_timer_cycles() - tsc_start;

	printf("\t\t%9.1f us", (float) (tsc_elapsed * 1000000) / (io_num * tsc_rate));
	printf("\t\t%10.1f s", (float) tsc_elapsed / tsc_rate);
	printf("\n");

	//printf("\t\t%9.1f us", (float) (time_in_sec * 1000000) / io_num);
	//printf("\t\t%12"PRIu64"\n", io_num);

	rte_free(buf);

	return 0;
}

static void
u2_cleanup(void)
{
	if (u2_qpair) {
		spdk_nvme_ctrlr_free_io_qpair(u2_qpair);
	}

	if (u2_ctrlr) {
		spdk_nvme_detach(u2_ctrlr);
	}

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
		printf("\t-q [IO number]\n");
		printf("\t-w [workload type (read, randread, write, randwrite)]\n");
		printf("\t-c [core mask]\n");
		printf("\t-n [memory channels]\n");
		//printf("\t-t [time in seconds]\n");
		goto FAIL;
	}

	if (u2_init()) {
		fprintf(stderr, "failed to initialize u2 benchmarking context!\n");
		goto FAIL;
	}

	printf("u2 latency benchmarking ... RW type: %s %s, IOs: %"PRIu64"\n",
	       is_random ? "random" : "sequential", is_rw ? "read" : "write", io_num);
	printf("\t%8s\t\t%12s\t\t%12s\n", "I/O size", "latency", "elapsed time");
	//printf("u2 latency benchmarking ... RW type: %s %s, Time (s): %"PRIu32"\n",
	//       is_random ? "random" : "sequential", is_rw ? "read" : "write", time_in_sec);
	//printf("\t%8s\t\t%12s\t\t%12s\n", "I/O size", "latency", "I/O count");
	while (1) {
		printf("\t%8d", io_size);

		if (u2_lat_bench()) {
			fprintf(stderr, "failed to benchmark latency - IO size %d!\n", io_size);
			goto FAIL;
		}

		if ((io_size *= 2) > U2_IO_SIZE_MAX) {
			break;
		}
	}

	u2_cleanup();
	return 0;

FAIL:
	u2_cleanup();
	return 1;
}

