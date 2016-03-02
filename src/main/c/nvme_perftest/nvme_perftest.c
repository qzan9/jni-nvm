/*
 * nvme_perftest
 *
 * do the same I/O perftest to all of the NVMe namespaces in the system.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <rte_lcore.h>

#include <spdk/file.h>
#include <spdk/nvme.h>
#include <spdk/pci.h>
#include <spdk/string.h>

struct ctrlr_entry {
	char name[256];
	struct spdk_nvme_ctrlr *ctrlr;

	struct ctrlr_entry *next;
};

struct ns_entry {
	char name[256];
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_ns    *ns;
	uint32_t io_size_blocks;    // payload size in blocks/LBs: g_io_size_bytes / sector_size
	uint64_t size_in_ios;       // number of containable payloads: sector_size * sector_num / g_io_size_bytes

	struct ns_entry *next;
};

struct ns_worker_ctx {
	struct ns_entry *ns_ent;
	uint64_t offset_in_ios;    // addressed by payload-size: sequential or random R/W?
	uint64_t current_queue_depth;
	uint64_t io_completed;
	bool is_draining;

	struct ns_worker_ctx *next;
};

struct worker_thread {
	unsigned lcore;    // iD of the logical core.
	struct ns_worker_ctx *ns_ctx;

	struct worker_thread *next;
};

struct perf_task {
	struct ns_worker_ctx *ns_ctx;
	void *buf;    // be allocated in hugepage memory, see task_ctor().
};

struct rte_mempool *request_mempool;    // pool for nvme requests.

static struct ctrlr_entry *g_controllers = NULL;

static struct ns_entry     *g_namespaces = NULL;
static int              g_num_namespaces = 0;

static struct worker_thread   *g_workers = NULL;
static int                 g_num_workers = 0;

static uint32_t g_io_size_bytes;    // payload size
static int      g_is_random;
static int      g_rw_percentage;

static int      g_queue_depth;
static uint32_t g_max_completions;    // 0 for unlimited.

static struct rte_mempool *task_pool;    // a pool of perf_task.

static const char *g_core_mask;
static int      g_time_in_sec;
static uint64_t    g_tsc_rate;

static __thread unsigned int seed = 0;

static char *ealargs[] = { "nvme_perftest", "-c 0x1", "-n 4", };    // DPDK initialization parameters.

/*
 * "thd" submits requests to and handles completions from "ns" through "ctx": thd -> ctx -> ns.
 *
 * - each thread associates with at least one context.
 *   one thread would associate with multiple contexts when there are more namespaces.
 *
 * - each context maps to just one namespace.
 *
 * - different contexts from different threads may be mapped to the same namespace, e.g. when there are more threads.
 *   different contexts from the same thread cannot be mapped to the same namespace.
 */

static int  register_ctrlr(struct spdk_nvme_ctrlr *);
static bool probe_cb(void *, struct spdk_pci_device *);
static void attach_cb(void *, struct spdk_pci_device *, struct spdk_nvme_ctrlr *);
static int  register_ctrlrs(void);
static void unregister_ctrlrs(void);

static int  register_ns(struct spdk_nvme_ctrlr *, struct spdk_nvme_ns *);
static void unregister_ns(void);

static int  register_workers(void);
static int  associate_workers_with_ns(void);
static void unregister_workers(void);

/*
 * each lcore submits I/O requests to each namespace according to user-specified queue depth.
 *
 * the io_complete callback would keep on submitting new I/O request if not "timeout"ed.
 *
 * when time expires, is_draining flag is set and io_complete stops submitting and turns to waiting for finishing.
 *
 * the number of completed I/O is recorded and then performance would be calculated.
 */

static int work_fn(void *);

static void io_complete(void *, const struct spdk_nvme_cpl *);
static void submit_single_io(struct ns_worker_ctx *);
static void submit_io(struct ns_worker_ctx *, int);

static void check_io(struct ns_worker_ctx *);
static void drain_io(struct ns_worker_ctx *);

/*
 * iterate each namespace context of each thread, and calculate the performance.
 */

static void print_perf(void);

/*
 * the task_perf object initialization routine for task pool creation.
 */

static void task_ctor(struct rte_mempool *, void *, void *, unsigned);
static void task_free(void *, void *, void *, uint32_t);

/*
 * arguments display and analysis.
 */

static void usage(char *);
static int parse_args(int, char**);

/*
 * nvme_perftest ...
 */

int main(int argc, char **argv)
{
	struct worker_thread *worker;
	int rc;

	/* check the arguments. */
	rc = parse_args(argc, argv);
	if (rc != 0) {
		return rc;
	}

	/* initialize DPDK EAL. */
	ealargs[1] = spdk_sprintf_alloc("-c %s", g_core_mask ? g_core_mask : "0x1");
	if (NULL == ealargs[1]) {
		perror("ealargs spdk_sprintf_alloc");
		return 1;
	}
	rc = rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]), ealargs);
	if (rc < 0) {
		fprintf(stderr, "failed to initialize DPDK!\n");
		return 1;
	}
	free(ealargs[1]);

	printf("\n======================================\n");
	printf(  "  NVMe PerfTest - ict.ncic.syssw.ufo"    );
	printf("\n======================================\n");

	/* reserve a pool of nvme_requests. */
	request_mempool = rte_mempool_create("nvme_request",
	                                     1024, spdk_nvme_request_size(),
	                                     128, 0,
	                                     NULL, NULL, NULL, NULL,
	                                     SOCKET_ID_ANY, 0);
	if (NULL == request_mempool) {
		fprintf(stderr, "failed to initialize request mempool!\n");
		return 1;
	}

	/* task_ctor will initialize each perf_test object. */
	task_pool = rte_mempool_create("task_pool",
	                               4096, sizeof(struct perf_task),
	                               512, 0,
	                               NULL, NULL, task_ctor, NULL,
	                               SOCKET_ID_ANY, 0);
	if (NULL == task_pool) {
		fprintf(stderr, "failed to initialize request mempool!\n");
		return 1;
	}

	/* get the rate HZ of TSC. */
	g_tsc_rate = rte_get_timer_hz();

	/* probe NVMe devices and prepare lcore threads. */
	if (register_ctrlrs()) {
		return 1;
	}

	if (register_workers()) {
		return 1;
	}

	if (associate_workers_with_ns()) {
		return 1;
	}

	/* first launch all of the slave workers. */
	worker = g_workers->next;
	while (worker != NULL) {
		rte_eal_remote_launch(work_fn, worker, worker->lcore);
		worker = worker->next;
	}

	/* master lcore starts testing. */
	rc = work_fn(g_workers);

	/* wait for each lcore to finish its job. */
	worker = g_workers->next;
	while (worker) {
		if (rte_eal_wait_lcore(worker->lcore) < 0) {
			rc = -1;
		}
		worker = worker->next;
	}

	/* display the performance statistics. */
	print_perf();

	/* release resources. */
	//rte_mempool_obj_iter(...);
	unregister_workers();
	unregister_ns();
	unregister_ctrlrs();

	if (rc) {
		fprintf(stderr, "%s: errors occurred\n", argv[0]);
		return rc;
	}

	return rc;
}

static int
register_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	int nsid, num_ns;
	struct ctrlr_entry *entry;
	const struct spdk_nvme_ctrlr_data *cdata;

	entry = malloc(sizeof(struct ctrlr_entry));
	if (NULL == entry) {
		perror("ctrlr_entry malloc");
		return 1;
	}
	memset(entry, 0, sizeof(struct ctrlr_entry));

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	snprintf(entry->name, sizeof(entry->name), "%s (%s)", cdata->mn, cdata->sn);
	entry->ctrlr  = ctrlr;
	entry->next   = g_controllers;    /* insert at the beginning. */
	g_controllers = entry;

	num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
	for (nsid = 1; nsid <= num_ns; nsid++) {
		if (register_ns(ctrlr, spdk_nvme_ctrlr_get_ns(ctrlr, nsid))) {
			return 1;
		}
	}

	return 0;
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *dev)
{
	if (spdk_pci_device_has_non_uio_driver(dev)) {
		fprintf(stderr, "non-uio kernel driver attached to NVMe!\n");
		fprintf(stderr, " controller at PCI address %04x:%02x:%02x.%02x\n",
		                spdk_pci_device_get_domain(dev),
		                spdk_pci_device_get_bus(dev),
		                spdk_pci_device_get_dev(dev),
		                spdk_pci_device_get_func(dev));
		fprintf(stderr, " skipping...\n");
		return false;
	}

	printf("attaching to %04x:%02x:%02x.%02x ... ",
	       spdk_pci_device_get_domain(dev),
	       spdk_pci_device_get_bus(dev),
	       spdk_pci_device_get_dev(dev),
	       spdk_pci_device_get_func(dev));

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *dev, struct spdk_nvme_ctrlr *ctrlr)
{
	printf("attached to %04x:%02x:%02x.%02x!\n",
	        spdk_pci_device_get_domain(dev),
	        spdk_pci_device_get_bus(dev),
	        spdk_pci_device_get_dev(dev),
	        spdk_pci_device_get_func(dev));

	if (register_ctrlr(ctrlr)) {
		exit(1);
	}
}

static int
register_ctrlrs(void)
{
	printf("initializing NVMe controllers ...\n");

	/*
	 * enumerate the NVMe devices attached to system and bind them the SPDK NVMe driver.
	 *
	 * - libpciaccess: while(pci_device_next(pci_dev_iter)) ...
	 *
	 * - rte_pci: rte_eal_pci_probe() ...
	 */
	if (spdk_nvme_probe(NULL, probe_cb, attach_cb) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed!\n");
		return 1;
	}

	return 0;
}

static void
unregister_ctrlrs(void)
{
	struct ctrlr_entry *entry, *next;

	entry = g_controllers;
	while (entry) {
		next = entry->next;
		spdk_nvme_detach(entry->ctrlr);
		free(entry);
		entry = next;
	}
}

static int
register_ns(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_ns *ns)
{
	struct ns_entry *entry;
	const struct spdk_nvme_ctrlr_data *cdata;

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	if (spdk_nvme_ns_get_size(ns) < g_io_size_bytes ||
	    spdk_nvme_ns_get_sector_size(ns) > g_io_size_bytes) {
		printf("WARNING: controller %s (%s) ns %u has invalid "
		       "ns size %" PRIu64 " / block size %u for I/O size %u\n",
		       cdata->mn, cdata->sn, spdk_nvme_ns_get_id(ns),
		       spdk_nvme_ns_get_size(ns), spdk_nvme_ns_get_sector_size(ns), g_io_size_bytes);
		return 1;
	}

	entry = malloc(sizeof(struct ns_entry));
	if (NULL == entry) {
		perror("ns_entry malloc");
		return 1;
	}
	memset(entry, 0, sizeof(struct ns_entry));

	snprintf(entry->name, sizeof(entry->name), "%s (%s) - %u", cdata->mn, cdata->sn, spdk_nvme_ns_get_id(ns));
	entry->ctrlr = ctrlr;
	entry->ns    = ns;
	entry->io_size_blocks = g_io_size_bytes / spdk_nvme_ns_get_sector_size(ns);
	entry->size_in_ios    = spdk_nvme_ns_get_size(ns) / g_io_size_bytes;
	entry->next = g_namespaces;    /* insert at the beginning. */
	g_namespaces = entry;
	g_num_namespaces++;

	return 0;
}

static void
unregister_ns(void)
{
	struct ns_entry *entry, *next;

	entry = g_namespaces;
	while (entry) {
		next = entry->next;
		free(entry);
		entry = next;
	}
}

static int
register_workers(void)
{
	struct worker_thread *worker, *prev_worker;
	unsigned lcore;

	/* set up the master worker thread. */
	worker = malloc(sizeof(struct worker_thread));
	if (NULL == worker) {
		perror("worker_thread malloc");
		return 1;
	}
	memset(worker, 0, sizeof(struct worker_thread));
	worker->lcore = rte_get_master_lcore();
	g_workers = worker;
	g_num_workers = 1;

	/*
	 * browse all running lcores except the MASTER lcore.
	 * lcore = rte_get_next_lcore();
	 */
	RTE_LCORE_FOREACH_SLAVE(lcore) {
		prev_worker = worker;

		worker = malloc(sizeof(struct worker_thread));
		if (worker == NULL) {
			perror("worker_thread malloc");
			return 1;
		}
		memset(worker, 0, sizeof(struct worker_thread));

		worker->lcore = lcore;
		prev_worker->next = worker;    // insert at the end.
		g_num_workers++;
	}

	return 0;
}

static int
associate_workers_with_ns(void)
{
	struct ns_entry      *ns_ent;
	struct worker_thread *worker;
	struct ns_worker_ctx *ns_ctx;
	int i, n;

	ns_ent = g_namespaces;
	worker = g_workers;
	n = g_num_namespaces > g_num_workers ? g_num_namespaces : g_num_workers;

	for (i = 0; i < n; i++) {
		if (NULL == ns_ent) {
			break;
		}

		ns_ctx = malloc(sizeof(struct ns_worker_ctx));
		if (NULL == ns_ctx) {
			perror("ns_worker_ctx malloc");
			return 1;
		}
		memset(ns_ctx, 0, sizeof(struct ns_worker_ctx));

		printf("associating [%s] with lcore %d ...\n", ns_ent->name, worker->lcore);
		ns_ctx->ns_ent = ns_ent;
		ns_ctx->next   = worker->ns_ctx;    // insert at the beginning.
		worker->ns_ctx = ns_ctx;

		worker = worker->next;
		if (NULL == worker) {
			worker = g_workers;
		}

		ns_ent = ns_ent->next;
		if (NULL == ns_ent) {
			ns_ent = g_namespaces;
		}
	}

	return 0;
}

static void
unregister_workers(void)
{
	struct worker_thread *worker, *worker_next;
	struct ns_worker_ctx *ns_ctx, *ns_ctx_next;

	worker = g_workers;
	while (worker) {
		worker_next = worker->next;

		ns_ctx = worker->ns_ctx;
		while (ns_ctx) {
			ns_ctx_next = ns_ctx->next;
			free(ns_ctx);
			ns_ctx = ns_ctx_next;
		}

		free(worker);
		worker = worker_next;
	}
}

static int
work_fn(void *arg)
{
	struct worker_thread *worker;
	struct ns_worker_ctx *ns_ctx;
	uint64_t tsc_end;

	worker = (struct worker_thread *)arg;

	printf("start testing on core %u ...\n", worker->lcore);

	tsc_end = rte_get_timer_cycles() + g_time_in_sec * g_tsc_rate;

	if (spdk_nvme_register_io_thread() != 0) {
		fprintf(stderr, "spdk_nvme_register_io_thread() failed on core %u!\n", worker->lcore);
		return 1;
	}

	/*
	 * submit I/O for each namespace according to user-specified queue depth.
	 *
	 * io_complete callback would keep on submitting new I/O requests if not "timeout"ed.
	 */
	ns_ctx = worker->ns_ctx;
	while (ns_ctx != NULL) {
		submit_io(ns_ctx, g_queue_depth);
		ns_ctx = ns_ctx->next;
	}

	/* check for completed I/O for each namespace; if time expires, break. */
	while (1) {
		ns_ctx = worker->ns_ctx;
		while (ns_ctx != NULL) {
			check_io(ns_ctx);
			ns_ctx = ns_ctx->next;
		}

		if (rte_get_timer_cycles() > tsc_end) {
			break;
		}
	}

	/* timeout, set the is_draining flag, and no more I/O would be submitted. */
	ns_ctx = worker->ns_ctx;
	while (ns_ctx != NULL) {
		drain_io(ns_ctx);
		ns_ctx = ns_ctx->next;
	}

	spdk_nvme_unregister_io_thread();

	return 0;
}

static void
io_complete(void *ctx, const struct spdk_nvme_cpl *completion)
{
	struct ns_worker_ctx *ns_ctx;
	struct perf_task *task = (struct perf_task *)ctx;

	ns_ctx = task->ns_ctx;

	ns_ctx->current_queue_depth--;
	ns_ctx->io_completed++;
	rte_mempool_put(task_pool, task);    // put back the perf_task object.

	/* when it's not "is_draining" (0), keep on submitting new I/O requests. */
	if (!ns_ctx->is_draining) {
		submit_single_io(ns_ctx);
	}
}

static void
submit_single_io(struct ns_worker_ctx *ns_ctx)
{
	struct ns_entry  *ns_ent;
	struct perf_task *task;
	uint64_t offset_in_ios;
	int rc;

	ns_ent = ns_ctx->ns_ent;

	if (rte_mempool_get(task_pool, (void **)&task)) {    // get a pre-allocated perf_task object.
		fprintf(stderr, "task_pool rte_mempool_get failed!\n");
		exit(1);
	}
	task->ns_ctx = ns_ctx;    // *task would be passed to io_complete.

	if (g_is_random) {
		offset_in_ios = rand_r(&seed) % ns_ent->size_in_ios;
	} else {
		offset_in_ios = ns_ctx->offset_in_ios++;
		if (ns_ctx->offset_in_ios == ns_ent->size_in_ios) {
			ns_ctx->offset_in_ios = 0;
		}
	}

	/*
	 * _nvme_ns_cmd_rw(struct nvme_namespace *ns, void *payload, uint64_t lba,
	 *                 uint32_t lba_count, nvme_cb_fn_t cb_fn, void *cb_arg,
	 *                 uint32_t opc)
	 * - ns: namespace to submit I/O request,
	 * - payload: virtual address pointer to the data payload,
	 * - lba: starting LBA to R/W,
	 * - lba_count: length (in sectors),
	 * - cb_fn: callback function to invoke when I/O is completed,
	 * - cb_arg: argument to pass to the callback function.
	 */
	if ((g_rw_percentage == 100) ||
	    (g_rw_percentage != 0 && ((rand_r(&seed) % 100) < g_rw_percentage))) {
		rc = spdk_nvme_ns_cmd_read (ns_ent->ns, task->buf, offset_in_ios * ns_ent->io_size_blocks,
		                            ns_ent->io_size_blocks, io_complete, task, 0);
	} else {
		rc = spdk_nvme_ns_cmd_write(ns_ent->ns, task->buf, offset_in_ios * ns_ent->io_size_blocks,
		                            ns_ent->io_size_blocks, io_complete, task, 0);
	}

	if (rc) {
		fprintf(stderr, "starting I/O failed!\n");
	}

	ns_ctx->current_queue_depth++;
}

static void
submit_io(struct ns_worker_ctx *ns_ctx, int queue_depth)
{
	while (queue_depth-- > 0) {    // submit I/O requests according to user-specified queue depth.
		submit_single_io(ns_ctx);
	}
}

static void
check_io(struct ns_worker_ctx *ns_ctx)
{
	/* process any outstanding completions. */
	spdk_nvme_ctrlr_process_io_completions(ns_ctx->ns_ent->ctrlr, g_max_completions);
}

static void
drain_io(struct ns_worker_ctx *ns_ctx)
{
	ns_ctx->is_draining = true;    // set the is_draining flag so io_complete would stop submitting new requests.
	while (ns_ctx->current_queue_depth > 0) {
		check_io(ns_ctx);
	}
}

static void
print_perf(void)
{
	struct worker_thread *worker;
	struct ns_worker_ctx *ns_ctx;

	float io_per_second, mb_per_second;

	worker = g_workers;
	while (worker) {
		ns_ctx = worker->ns_ctx;
		while (ns_ctx) {
			io_per_second = (float)ns_ctx->io_completed / g_time_in_sec;
			mb_per_second = io_per_second * g_io_size_bytes / (1024 * 1024);
			printf("[%s] on core %u: %.2f IO/s, %.2f MB/s\n",
			       ns_ctx->ns_ent->name, worker->lcore,
			       io_per_second, mb_per_second);

			ns_ctx = ns_ctx->next;
		}
		worker = worker->next;
	}
}

static void
task_ctor(struct rte_mempool *mp, void *arg, void *__task, unsigned id)
{
	struct perf_task *task = __task;
	/* allocate memory from the huge-page area. */
	task->buf = rte_malloc(NULL, g_io_size_bytes, 0x200);
	if (NULL == task->buf) {
		fprintf(stderr, "task->buf rte_malloc failed!\n");
		exit(1);
	}
}

//static void
//task_free(void *, void *, void *, uint32_t)
//{
//}

/*
 * arguments display and analysis.
 */

static void
usage(char *program_name)
{
	printf("usage: %s [OPTION]...\n", program_name);
	printf("\t-q IO queue depth\n");
	printf("\t-s IO size in bytes\n");
	printf("\t-w IO pattern, must be one of\n");
	printf("\t\t(read, write, randread, randwrite, rw, randrw)\n");
	printf("\t-t test time in seconds\n");
	printf("\t-M rwmixread (100 for reads, 0 for writes)\n");
	printf("\t-m max completions per poll (default: 0 - unlimited)\n");
	printf("\t-c core mask (default: 1)\n");
}

static int
parse_args(int argc, char **argv)
{
	const char *workload_type;
	int op;
	bool mix_specified = false;

	g_queue_depth     = 0;
	g_io_size_bytes   = 0;
	workload_type     = NULL;
	g_time_in_sec     = 0;
	g_rw_percentage   = -1;
	g_max_completions = 0;
	g_core_mask       = NULL;

	while ((op = getopt(argc, argv, "q:s:w:t:M:m:c")) != -1) {
		switch (op) {
		case 'q':
			g_queue_depth = atoi(optarg);
			break;
		case 's':
			g_io_size_bytes = atoi(optarg);
			break;
		case 'w':
			workload_type = optarg;
			break;
		case 't':
			g_time_in_sec = atoi(optarg);
			break;
		case 'M':
			g_rw_percentage = atoi(optarg);
			mix_specified = true;
			break;
		case 'm':
			g_max_completions = atoi(optarg);
			break;
		case 'c':
			g_core_mask = optarg;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!g_queue_depth) {
		usage(argv[0]);
		return 1;
	}

	if (!g_io_size_bytes) {
		usage(argv[0]);
		return 1;
	}

	if (!workload_type) {
		usage(argv[0]);
		return 1;
	}

	if (strcmp(workload_type, "read")      &&
	    strcmp(workload_type, "write")     &&
	    strcmp(workload_type, "randread")  &&
	    strcmp(workload_type, "randwrite") &&
	    strcmp(workload_type, "rw")        &&
	    strcmp(workload_type, "randrw")) {
		fprintf(stderr, "invalid workload type!\n");
		usage(argv[0]);
		return 1;
	}

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "randread")) {
		g_rw_percentage = 100;
	}

	if (!strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "randwrite")) {
		g_rw_percentage = 0;
	}

	if (!strcmp(workload_type, "rw") ||
	    !strcmp(workload_type, "randrw")) {
		if (g_rw_percentage < 0 || g_rw_percentage > 100) {
			fprintf(stderr, "-M must be specified!");
			usage(argv[0]);
			return 1;
		}
	}

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "rw")) {
		g_is_random = 0;
	} else {
		g_is_random = 1;
	}

	if (!g_time_in_sec) {
		usage(argv[0]);
		return 1;
	}

	return 0;
}
