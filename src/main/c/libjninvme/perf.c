#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <pciaccess.h>

#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <rte_lcore.h>

#include <spdk/file.h>
#include <spdk/nvme.h>
#include <spdk/pci.h>
#include <spdk/string.h>

#include "spdk_app.h"

struct ctrlr_entry {
	struct nvme_controller *ctrlr;
	char name[1024];

	struct ctrlr_entry *next;
};

struct ns_entry {
	struct nvme_controller *ctrlr;
	struct nvme_namespace  *ns;
	char     name[1024];
	uint32_t io_size_blocks;          // g_io_size_bytes / sector_size; SECTOR ... ???
	uint64_t size_in_ios;             // sector_size * sector_num / g_io_size_bytes

	struct ns_entry *next;
};

struct ns_worker_ctx {
	struct ns_entry *ns_ent;
	uint64_t offset_in_ios;           // ...
	uint64_t current_queue_depth;     // queue depth
	uint64_t io_completed;            // completed I/O commands
	bool     is_draining;             // ...

	struct ns_worker_ctx *next;
};

struct worker_thread {
	struct ns_worker_ctx *ns_ctx;     // worker thread namespace context
	unsigned lcore;                   // lcore iD.

	struct worker_thread *next;
};

struct perf_task {
	struct ns_worker_ctx *ns_ctx;
	void *buf;                        // buffer for test data.
};

static void register_ns(struct nvme_controller *, struct nvme_namespace *);
static void unregister_ns(void);
static void register_ctrlr(struct nvme_controller *);
static int  register_ctrlrs(void);
static void unregister_ctrlrs(void);
static void register_workers(void);
static void associate_workers_with_ns(void);
static void unregister_workers(void);

static void submit_single_io(struct ns_worker_ctx *);
static void submit_io(struct ns_worker_ctx *, int);
static void task_complete(struct perf_task *);
static void io_complete(void *, const struct nvme_completion *);
static void check_io(struct ns_worker_ctx *);
static void drain_io(struct ns_worker_ctx *);

static void task_ctor(struct rte_mempool *, void *, void *, unsigned);
static int work_fn(void *);

static int      g_io_size_bytes;
static int      g_rw_percentage;
static int      g_is_random;
static int      g_queue_depth;
static int      g_time_in_sec;
static uint32_t g_max_completions;

static uint64_t    g_tsc_rate;
static const char *g_core_mask;

static __thread unsigned int seed = 0;    // thread-local

static struct ctrlr_entry *g_controllers = NULL;
static struct ns_entry     *g_namespaces = NULL;
static int              g_num_namespaces = 0;
static struct worker_thread   *g_workers = NULL;
static int                 g_num_workers = 0;

struct rte_mempool  *request_mempool;
static struct rte_mempool *task_pool;

static void usage(char *program_name)
{
	printf("%s options", program_name);
	printf("\n");
	printf("\t[-q io depth]\n");
	printf("\t[-s io size in bytes]\n");
	printf("\t[-w io pattern type, must be one of\n");
	printf("\t\t(read, write, randread, randwrite, rw, randrw)]\n");
	printf("\t[-M rwmixread (100 for reads, 0 for writes)]\n");
	printf("\t[-t time in seconds]\n");
	printf("\t[-c core mask for I/O submission/completion.]\n");
	printf("\t\t(default: 1)]\n");
	printf("\t[-m max completions per poll]\n");
	printf("\t\t(default: 0 - unlimited)\n");
}

static void
print_stats(void)
{
	float io_per_second, mb_per_second;
	float total_io_per_second, total_mb_per_second;
	struct worker_thread	*worker;
	struct ns_worker_ctx	*ns_ctx;

	total_io_per_second = 0;
	total_mb_per_second = 0;

	worker = g_workers;
	while (worker) {
		ns_ctx = worker->ns_ctx;
		while (ns_ctx) {
			io_per_second = (float)ns_ctx->io_completed / g_time_in_sec;
			mb_per_second = io_per_second * g_io_size_bytes / (1024 * 1024);
			printf("%-43.43s from core %u: %10.2f IO/s %10.2f MB/s\n",
			       ns_ctx->ns_ent->name, worker->lcore,
			       io_per_second, mb_per_second);
			total_io_per_second += io_per_second;
			total_mb_per_second += mb_per_second;
			ns_ctx = ns_ctx->next;
		}
		worker = worker->next;
	}
	printf("========================================================\n");
	printf("%-55s: %10.2f IO/s %10.2f MB/s\n",
	       "total", total_io_per_second, total_mb_per_second);
}

static int
parse_args(int argc, char **argv)
{
	const char *workload_type;
	int op;
	bool mix_specified = false;

	/* default value*/
	g_queue_depth = 0;
	g_io_size_bytes = 0;
	workload_type = NULL;
	g_time_in_sec = 0;
	g_rw_percentage = -1;
	g_core_mask = NULL;
	g_max_completions = 0;

	while ((op = getopt(argc, argv, "c:m:q:s:t:w:M:")) != -1) {
		switch (op) {
		case 'c':
			g_core_mask = optarg;
			break;
		case 'm':
			g_max_completions = atoi(optarg);
			break;
		case 'q':
			g_queue_depth = atoi(optarg);
			break;
		case 's':
			g_io_size_bytes = atoi(optarg);
			break;
		case 't':
			g_time_in_sec = atoi(optarg);
			break;
		case 'w':
			workload_type = optarg;
			break;
		case 'M':
			g_rw_percentage = atoi(optarg);
			mix_specified = true;
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
	if (!g_time_in_sec) {
		usage(argv[0]);
		return 1;
	}

	if (strcmp(workload_type, "read") &&
	    strcmp(workload_type, "write") &&
	    strcmp(workload_type, "randread") &&
	    strcmp(workload_type, "randwrite") &&
	    strcmp(workload_type, "rw") &&
	    strcmp(workload_type, "randrw")) {
		fprintf(stderr,
			"io pattern type must be one of\n"
			"(read, write, randread, randwrite, rw, randrw)\n");
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

	if (!strcmp(workload_type, "read") ||
	    !strcmp(workload_type, "randread") ||
	    !strcmp(workload_type, "write") ||
	    !strcmp(workload_type, "randwrite")) {
		if (mix_specified) {
			fprintf(stderr, "ignoring -M option... please use -M option"
				" only when using rw or randrw.\n");
		}
	}

	if (!strcmp(workload_type, "rw") ||
	    !strcmp(workload_type, "randrw")) {
		if (g_rw_percentage < 0 || g_rw_percentage > 100) {
			fprintf(stderr,
				"-M must be specified to value from 0 to 100 "
				"for rw or randrw.\n");
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

	optind = 1;
	return 0;
}

int perf(int argc, char **argv)
{
	struct worker_thread *worker;
	static char *ealargs[] = {
		"perf",
		"-c 0x1",    /* this must be the second parameter. it is overwritten by index. */
		"-n 4",
	};
	int rc;

	ealargs[1] = sprintf_alloc("-c %s", g_core_mask ? g_core_mask : "0x1");
	if (NULL == ealargs[1]) {
		perror("ealargs sprintf_alloc failed!");
		return 1;
	}

	/*
	 * initialize EAL by MASTER lcore; SLAVE lcores are put in the WAIT state.
	 */
	rc = rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]), ealargs);

	free(ealargs[1]);

	if (rc < 0) {
		fprintf(stderr, "could not initialize DPDK!\n");
		return 1;
	}


	/*
	 * struct rte_mempool* rte_mempool_create(const char *name,
         *                                        unsigned n,
         *                                        unsigned elt_size,
         *                                        unsigned cache_size,
         *                                        unsigned private_data_size,
         *                                        rte_mempool_ctor_t *mp_init,
         *                                        void *mp_init_arg,
         *                                        rte_mempool_obj_ctor_t *obj_init,
         *                                        void *obj_init_arg,
         *                                        int socket_id,
         *                                        unsigned flags
         *                                       )
	 */
	request_mempool = rte_mempool_create("nvme_request", 8192,
					     nvme_request_size(), 128, 0,
					     NULL, NULL, NULL, NULL,
					     SOCKET_ID_ANY, 0);
	if (NULL == request_mempool) {
		fprintf(stderr, "could not initialize request mempool!\n");
		return 1;
	}

	task_pool = rte_mempool_create("task_pool", 8192,
	                               sizeof(struct perf_task), 64, 0,
	                               NULL, NULL, task_ctor, NULL,
	                               SOCKET_ID_ANY, 0);
	if (NULL == task_pool) {
		fprintf(stderr, "could not initialize task mempool!\n");
		return 1;
	}

	g_tsc_rate = rte_get_timer_hz();

	register_ctrlrs();
	register_workers();
	associate_workers_with_ns();

	printf("initialization complete, launching workers ...\n");

	/*
	 * int rte_eal_remote_launch(lcore_function *f, void *arg, unsigned slave_id)
	 *
	 * launch a function on another SLAVE lcore (by MASTER lcore).
	 *
	 * sends a message to a SLAVE lcore (identified by the slave_id) that
	 * is in the WAIT state. when the remote lcore receives the message,
	 * it switches to the RUNNING state, then calls the function f with
	 * argument arg.
	 */
	worker = g_workers->next;
	while (worker != NULL) {
		rte_eal_remote_launch(work_fn, worker, worker->lcore);
		worker = worker->next;
	}

	/* call work_fn on MASTER lcore too. */
	rc = work_fn(g_workers);

	worker = g_workers->next;
	while (worker != NULL) {
		/* wait untile an lcore finishes its job; executed on the MASTER lcore. */
		if (rte_eal_wait_lcore(worker->lcore) < 0) {
			rc = -1;
		}
		worker = worker->next;
	}

	print_stats();

	unregister_workers();
	unregister_ctrlrs();

	return rc;
}

static void
register_ns(struct nvme_controller *ctrlr, struct nvme_namespace *ns)
{
	struct ns_entry *entry;
	const struct nvme_controller_data *cdata;

	entry = malloc(sizeof(struct ns_entry));
	if (NULL == entry) {
		perror("ns_entry malloc failed!");
		exit(1);
	}
	memset(entry, 0, sizeof(struct ns_entry));

	cdata = nvme_ctrlr_get_data(ctrlr);

	entry->ctrlr = ctrlr;
	entry->ns    = ns;
	entry->io_size_blocks = g_io_size_bytes / nvme_ns_get_sector_size(ns);
	entry->size_in_ios    = nvme_ns_get_size(ns) / g_io_size_bytes;
	snprintf(entry->name, 44, "%-20.20s (%-20.20s)", cdata->mn, cdata->sn);

	g_num_namespaces++;
	entry->next = g_namespaces;
	g_namespaces = entry;
}

static void
unregister_ns(void)
{
	struct ns_entry *entry, *next;

	entry = g_namespaces;
	while(entry) {
		next = entry->next;
		free(entry);
		entry = next;
	}
}

static void
register_ctrlr(struct nvme_controller *ctrlr)
{
	int nsid, num_ns;
	struct ctrlr_entry *entry;

	entry = malloc(sizeof(struct ctrlr_entry));
	if (NULL == entry) {
		perror("ctrlr_entry malloc failed!");
		exit(1);
	}
	memset(entry, 0, sizeof(struct ctrlr_entry));

	entry->ctrlr  = ctrlr;
	entry->next   = g_controllers;
	g_controllers = entry;

	num_ns = nvme_ctrlr_get_num_ns(ctrlr);
	for (nsid = 1; nsid <= num_ns; nsid++) {
		register_ns(ctrlr, nvme_ctrlr_get_ns(ctrlr, nsid));
	}
}

static int
register_ctrlrs(void)
{
	struct pci_device_iterator *pci_dev_iter;
	struct pci_device          *pci_dev;
	struct pci_id_match         match;
	struct nvme_controller     *ctrlr;
	int                         rc;

	pci_system_init();

	match.vendor_id    = PCI_MATCH_ANY;
	match.subvendor_id = PCI_MATCH_ANY;
	match.subdevice_id = PCI_MATCH_ANY;
	match.device_id    = PCI_MATCH_ANY;
	match.device_class = NVME_CLASS_CODE;
	match.device_class_mask = 0xFFFFFF;

	pci_dev_iter = pci_id_match_iterator_create(&match);

	/* scan and register all NVMe devices. */
	rc = 0;
	while ((pci_dev = pci_device_next(pci_dev_iter))) {
		if (pci_device_has_non_null_driver(pci_dev)) {
			fprintf(stderr, "non-null kernel driver attached to nvme.\n");
			fprintf(stderr, "controller at pci BDF %d:%d:%d\n",
				pci_dev->bus, pci_dev->dev, pci_dev->func);
			fprintf(stderr, "skipping...\n");
			continue;
		}

		pci_device_probe(pci_dev);

		ctrlr = nvme_attach(pci_dev);
		if (NULL == ctrlr) {
			fprintf(stderr, "nvme_attach failed for controller at pci BDF %d:%d:%d\n",
				pci_dev->bus, pci_dev->dev, pci_dev->func);
			rc = 1;
			continue;
		}

		register_ctrlr(ctrlr);
	}

	pci_iterator_destroy(pci_dev_iter);

	return rc;
}

static void
unregister_ctrlrs(void)
{
	struct ctrlr_entry *entry, *next;

	entry = g_controllers;
	while(entry) {
		next = entry->next;
		nvme_detach(entry->ctrlr);
		free(entry);
		entry = next;
	}
}

static void
register_workers(void)
{
	struct worker_thread *worker,
	                     *prev_worker;
	unsigned lcore;

	worker = malloc(sizeof(struct worker_thread));
	if (NULL == worker) {
		perror("worker_thread malloc failed!");
		exit(1);
	}
	memset(worker, 0, sizeof(struct worker_thread));

	/* get the iD of the master lcore. */
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
		if (NULL == worker) {
			perror("worker_thread malloc failed!");
			exit(1);
		}
		memset(worker, 0, sizeof(struct worker_thread));

		worker->lcore = lcore;
		prev_worker->next = worker;
		g_num_workers++;
	}
}

static void
associate_workers_with_ns(void)
{
	struct ns_entry      *ns_ent = g_namespaces;
	struct worker_thread *worker = g_workers;
	struct ns_worker_ctx *ns_ctx;
	int i, n;

	n = g_num_namespaces > g_num_workers ? g_num_namespaces : g_num_workers;

	for (i = 0; i < n; i++) {
		ns_ctx = malloc(sizeof(struct ns_worker_ctx));
		if (NULL == ns_ctx) {
			perror("ns_worker_ctx malloc failed!");
			exit(1);
		}
		memset(ns_ctx, 0, sizeof(struct ns_worker_ctx));

		/* namespaces are divided among worker threads. */
		ns_ctx->ns_ent = ns_ent;
		ns_ctx->next   = worker->ns_ctx;
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
}

static void
unregister_workers(void)
{
	struct worker_thread *worker, *worker_next;
	struct ns_worker_ctx *ns_ctx, *ns_ctx_next;

	worker = g_workers;
	while(worker) {
		worker_next = worker->next;

		ns_ctx = worker->ns_ctx;
		while(ns_ctx) {
			ns_ctx_next = ns_ctx->next;
			free(ns_ctx);
			ns_ctx = ns_ctx_next;
		}

		free(worker);
		worker = worker_next;
	}
}

static void
submit_single_io(struct ns_worker_ctx *ns_ctx)
{
	struct perf_task *task = NULL;
	struct ns_entry  *ns_ent = ns_ctx->ns_ent;
	uint64_t          offset_in_ios;
	int               rc;

	/* get task object for task mempool. */
	if (rte_mempool_get(task_pool, (void **)&task) != 0) {
		fprintf(stderr, "task_pool rte_mempool_get failed!\n");
		exit(1);
	}

	task->ns_ctx = ns_ctx;

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
		rc = nvme_ns_cmd_read (ns_ent->ns, task->buf, offset_in_ios * ns_ent->io_size_blocks,
		                       ns_ent->io_size_blocks, io_complete, task);
	} else {
		rc = nvme_ns_cmd_write(ns_ent->ns, task->buf, offset_in_ios * ns_ent->io_size_blocks,
		                       ns_ent->io_size_blocks, io_complete, task);
	}

	if (rc) {
		fprintf(stderr, "starting I/O failed!\n");
	}

	ns_ctx->current_queue_depth++;
}

static void
submit_io(struct ns_worker_ctx *ns_ctx, int queue_depth)
{
	while (queue_depth-- > 0) {
		submit_single_io(ns_ctx);
	}
}

static void
task_complete(struct perf_task *task)
{
	struct ns_worker_ctx *ns_ctx;

	ns_ctx = task->ns_ctx;
	ns_ctx->current_queue_depth--;
	ns_ctx->io_completed++;

	/* put the task object back to task mempool. */
	rte_mempool_put(task_pool, task);

	/*
	 * is_draining indicates when time has expired for the test run and
	 * we are just waiting for the previously submitted I/O to complete.
	 * in this case, do not submit a new I/O to replace the one just
	 * completed.
	 */
	if (!ns_ctx->is_draining) {
		submit_single_io(ns_ctx);
	}
}

static void
io_complete(void *ctx, const struct nvme_completion *completion)
{
	task_complete((struct perf_task *)ctx);
}

static void
check_io(struct ns_worker_ctx *ns_ctx)
{
	nvme_ctrlr_process_io_completions(ns_ctx->ns_ent->ctrlr, g_max_completions);
}

static void
drain_io(struct ns_worker_ctx *ns_ctx)
{
	ns_ctx->is_draining = true;
	while (ns_ctx->current_queue_depth > 0) {
		check_io(ns_ctx);
	}
}

static void
task_ctor(struct rte_mempool *mp, void *arg, void *__task, unsigned id)
{
	struct perf_task *task = __task;

	/*
	 * allocate buffer from the huge-page area of memory for test data.
	 * the memory is not cleared.
	 *
	 * in NUMA systems, the memory allocated resides on the same NUMA
	 * socket as the core that calls this function.
	 */
	task->buf = rte_malloc(NULL, g_io_size_bytes, 0x200);
	if (NULL == task->buf) {
		fprintf(stderr, "task->buf rte_malloc failed!\n");
		exit(1);
	}
}

static int
work_fn(void *arg)
{
	uint64_t tsc_end = rte_get_timer_cycles() + g_time_in_sec * g_tsc_rate;
	struct worker_thread *worker = (struct worker_thread *)arg;
	struct ns_worker_ctx *ns_ctx = NULL;

	printf("starting thread on core %u...\n", worker->lcore);

	if (nvme_register_io_thread() != 0) {
		fprintf(stderr, "nvme_register_io_thread() failed on core %u\n", worker->lcore);
		return -1;
	}

	/* submit initial I/O for each namespace. */
	ns_ctx = worker->ns_ctx;
	while (ns_ctx != NULL) {
		submit_io(ns_ctx, g_queue_depth);
		ns_ctx = ns_ctx->next;
	}

	while (1) {
		/*
		 * check for completed I/O for each controller. a new I/O will
		 * be submitted in the io_complete callback to replace each I/O
		 * that is completed.
		 */
		ns_ctx = worker->ns_ctx;
		while (ns_ctx != NULL) {
			check_io(ns_ctx);
			ns_ctx = ns_ctx->next;
		}

		if (rte_get_timer_cycles() > tsc_end) {
			break;
		}
	}

	ns_ctx = worker->ns_ctx;
	while (ns_ctx != NULL) {
		drain_io(ns_ctx);
		ns_ctx = ns_ctx->next;
	}

	nvme_unregister_io_thread();

	return 0;
}

