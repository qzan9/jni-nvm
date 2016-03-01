/*
 * nvme_devinfo
 *
 * display basic information of all the NVMe devices currently attached.
 *
 * for SPDK, this demonstrates the usage of:
 *
 * - spdk_nvme_probe(): enumerate the NVMe devices attached to the system and attach the userspace NVMe driver to them if desired.
 *
 * - spdk_nvme_detach(): detaches the specified device.
 *
 * - spdk_nvme_ctrlr_get_data(): get the identity controller data as defined by the NVMe specification.
 *
 * - spdk_nvme_ctrlr_cmd_get_log_page(): get a specific log page from the NVMe controller.
 *
 * - spdk_nvme_ctrlr_cmd_admin_raw(): a low-level interface to send the given admin command to the NVMe controller.
 *
 * - spdk_nvme_ctrlr_process_admin_completions(): process any outstanding completions for administration commands.
 */

#include <stdbool.h>
#include <unistd.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_lcore.h>

#include <spdk/nvme.h>
#include <spdk/pci.h>

struct feature {
	uint32_t result;
	bool valid;
};

struct rte_mempool *request_mempool;

static struct feature features[256];
static struct spdk_nvme_health_information_page *health_page;

static int outstanding_commands;

static void
print_uint128_hex(uint64_t *v)
{
	unsigned long long lo = v[0], hi = v[1];
	if (hi) {
		printf("0x%llX%016llX", hi, lo);
	} else {
		printf("0x%llX", lo);
	}
}

static void
print_uint128_dec(uint64_t *v)
{
	unsigned long long lo = v[0], hi = v[1];
	if (hi) {
		/* can't handle large (>64-bit) decimal values for now, so fall back to hex */
		print_uint128_hex(v);
	} else {
		printf("%llu", (unsigned long long)lo);
	}
}

static void
get_feature_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	struct feature *feature = cb_arg;
	int fid = feature - features;

	if (spdk_nvme_cpl_is_error(cpl)) {
		fprintf(stderr, "get_feature(0x%02X) failed!\n", fid);
	} else {
		/* record command dword 0. */
		feature->result = cpl->cdw0;
		feature->valid = true;
	}
	outstanding_commands--;
}

static int
get_feature(struct spdk_nvme_ctrlr *ctrlr, uint8_t fid)
{
	struct spdk_nvme_cmd cmd = {};    // 16-dword/64-bytes

	cmd.opc = SPDK_NVME_OPC_GET_FEATURES;
	cmd.cdw10 = fid;

	/* invoke the low-level interface for directly submitting admin commands to query features. */
	return spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, NULL, 0, get_feature_completion, &features[fid]);
}

static void
get_features(struct spdk_nvme_ctrlr *ctrlr)
{
	size_t i;

	/* NVMe features are defined in nvme_spec.h. */
	uint8_t features_to_get[] = {
	//	SPDK_NVME_FEAT_ARBITRATION,
	//	SPDK_NVME_FEAT_POWER_MANAGEMENT,
		SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD,
	//	SPDK_NVME_FEAT_ERROR_RECOVERY,
	};

	outstanding_commands = 0;
	for (i = 0; i < sizeof(features_to_get) / sizeof(*features_to_get); i++) {
		if (get_feature(ctrlr, features_to_get[i]) == 0) {
			outstanding_commands++;
		} else {
			fprintf(stderr, "get_feature(0x%02X) failed to submit command!\n", features_to_get[i]);
		}
	}

	while (outstanding_commands) {
		/* process any unsettled completions for admin commands. */
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}
}

static void
get_log_page_completion(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		fprintf(stderr, "get log page failed!\n");
	}
	outstanding_commands--;
}

static int
get_health_log_page(struct spdk_nvme_ctrlr *ctrlr)
{
	if (NULL == health_page) {
		/* allocate zero'ed memory from the RTE heap. */
		health_page = rte_zmalloc("nvme health", sizeof(*health_page), 4096);
		if (NULL == health_page) {
			fprintf(stderr, "failed to allocate health page!\n");
			return 1;
		}
		/*
		 * using malloc would cause error "could not find 2MB vfn 0xf in DPDK mem config".
		 */
		/*health_page = malloc(sizeof(*health_page));
		if (NULL == health_page) {
			perror("health_page malloc");
			return 1;
		}*/
	}

	/* get health log page from the NVMe controller. */
	if (spdk_nvme_ctrlr_cmd_get_log_page(ctrlr, SPDK_NVME_LOG_HEALTH_INFORMATION, SPDK_NVME_GLOBAL_NS_TAG,
	                                     health_page, sizeof(*health_page), get_log_page_completion, NULL)) {
		fprintf(stderr, "spdk_nvme_ctrlr_cmd_get_log_page() failed!\n");
		return 1;
	}

	return 0;
}

static void
get_log_pages(struct spdk_nvme_ctrlr *ctrlr)
{
	const struct spdk_nvme_ctrlr_data *ctrlr_data;
	outstanding_commands = 0;

	if (get_health_log_page(ctrlr) == 0) {
		outstanding_commands++;
	} else {
		fprintf(stderr, "failed to get log page (SMART/health)!\n");
	}

	while (outstanding_commands) {
		spdk_nvme_ctrlr_process_admin_completions(ctrlr);
	}
}

static void
print_namespace(struct spdk_nvme_ns *ns)
{
	const struct spdk_nvme_ns_data *nsdata;
	uint32_t i;

	nsdata = spdk_nvme_ns_get_data(ns);

	printf("\t  Namespace ID: %d\n", spdk_nvme_ns_get_id(ns));
	printf("\t    Size (in LBAs):         %lld (%lldM)\n", (long long)nsdata->nsze, (long long)nsdata->nsze/1024/1024);
	printf("\t    Capacity (in LBAs):     %lld (%lldM)\n", (long long)nsdata->ncap, (long long)nsdata->ncap/1024/1024);
	printf("\t    Number of LBA Formats:  %d\n", nsdata->nlbaf + 1);
	for (i = 0; i <= nsdata->nlbaf; i++) {
	printf("\t      LBA Format #%02d: Data Size - %4d, Metadata Size - %2d\n", i, 1 << nsdata->lbaf[i].lbads, nsdata->lbaf[i].ms);
	}
	printf("\t    Current LBA Format:     #%02d\n", nsdata->flbas.format);
}

static void
print_controller(struct spdk_nvme_ctrlr *ctrlr, struct spdk_pci_device *pci_dev)
{
	const struct spdk_nvme_ctrlr_data *cdata;
	uint8_t  str[128];
	uint32_t i;

	get_features(ctrlr);
	get_log_pages(ctrlr);

	cdata = spdk_nvme_ctrlr_get_data(ctrlr);

	printf("NVMe controller found at PCI bus %d, device %d, function %d\n",
	       spdk_pci_device_get_bus(pci_dev), spdk_pci_device_get_dev(pci_dev), spdk_pci_device_get_func(pci_dev));
	printf("\tVendor ID:                  %04x\n", cdata->vid);
	printf("\tSubsystem Vendor ID:        %04x\n", cdata->ssvid);
	snprintf(str, sizeof(cdata->sn) + 1, "%s", cdata->sn);
	printf("\tSerial Number:              %s\n", str);
	snprintf(str, sizeof(cdata->mn) + 1, "%s", cdata->mn);
	printf("\tModel Number:               %s\n", str);
	snprintf(str, sizeof(cdata->fr) + 1, "%s", cdata->fr);
	printf("\tFirmware Version:           %s\n", str);
	printf("\tSubmission Queue Entry Size\n");
	printf("\t  Max:                      %d\n", 1 << cdata->sqes.max);
	printf("\t  Min:                      %d\n", 1 << cdata->sqes.min);
	printf("\tCompletion Queue Entry Size\n");
	printf("\t  Max:                      %d\n", 1 << cdata->cqes.max);
	printf("\t  Min:                      %d\n", 1 << cdata->cqes.min);
	printf("\tNumber of Namespaces:       %d\n", cdata->nn);
	for (i = 1; i <= spdk_nvme_ctrlr_get_num_ns(ctrlr); i++) {
	print_namespace(spdk_nvme_ctrlr_get_ns(ctrlr, i));
	}
	if (features[SPDK_NVME_FEAT_TEMPERATURE_THRESHOLD].valid && health_page) {
	printf("\tHealth Information\n");
	printf("\t  Current Temperature:      %u Kelvin (%u Celsius)\n", health_page->temperature, health_page->temperature - 273);
	printf("\t  Available Spare:          %u%%\n", health_page->available_spare);
	printf("\t  Life Percentage Used:     %u%%\n", health_page->percentage_used);
	printf("\t  Data Units Read:          "); print_uint128_dec(health_page->data_units_read);     printf("\n");
	printf("\t  Data Units Written:       "); print_uint128_dec(health_page->data_units_written);  printf("\n");
	printf("\t  Host Read Commands:       "); print_uint128_dec(health_page->host_read_commands);  printf("\n");
	printf("\t  Host Write Commands:      "); print_uint128_dec(health_page->host_write_commands); printf("\n");
	printf("\t  Power Cycles:             "); print_uint128_dec(health_page->power_cycles);        printf("\n");
	printf("\t  Power On Hours:           "); print_uint128_dec(health_page->power_on_hours);      printf(" hours\n");
	printf("\t  Unsafe Shutdowns:         "); print_uint128_dec(health_page->unsafe_shutdowns);    printf("\n");
	printf("\t  Media Errors:             "); print_uint128_dec(health_page->media_errors);        printf("\n");
	}
}

static bool
probe_cb(void *cb_ctx, struct spdk_pci_device *dev)
{
	if (spdk_pci_device_has_non_uio_driver(dev)) {
		fprintf(stderr, "non-uio kernel driver attached to the NVMe deivce!\n");
		fprintf(stderr, "  controller at PCI address %04x:%02x:%02x.%02x\n",
			spdk_pci_device_get_domain(dev),
			spdk_pci_device_get_bus(dev),
			spdk_pci_device_get_dev(dev),
			spdk_pci_device_get_func(dev));
		fprintf(stderr, "  skipping...\n");
		return false;
	}

	return true;
}

static void
attach_cb(void *cb_ctx, struct spdk_pci_device *pci_dev, struct spdk_nvme_ctrlr *ctrlr)
{
	print_controller(ctrlr, pci_dev);
	spdk_nvme_detach(ctrlr);
}

int main(int argc, char **argv)
{
	int rc;
	const char *ealargs[] = { "nvme_devinfo", "-c 0x1", "-n 4", };    // be careful with the number of memory channels.

	/*
	 * initialize DPDK EAL: set up hugepage memory and PCI bus access, and create a thread for each core.
	 *
	 * arguments are defined in lib/librte_eal/common/eal_common_options.c.
	 *
	 * here c: coremask and n: memory channels.
	 */
	rc = rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]),
			  (char **)(void *)(uintptr_t)ealargs);
	if (rc < 0) {
		fprintf(stderr, "failed to initialize DPDK EAL!\n");
		return rc;
	}

	/*
	 * allocate a physically continuous chunk of memory for NVMe requests.
	 *
	 * request_mempool is declared in lib/nvme/nvme_impl.h as
	 *
	 *     extern struct rte_mempool *request_mempool;
	 *
	 * NVMe requests are stored in this mempool and allocated for each I/O.
	 */
	request_mempool = rte_mempool_create("nvme_request", 1024,
					     spdk_nvme_request_size(), 128, 0,
					     NULL, NULL, NULL, NULL,
					     SOCKET_ID_ANY, 0);
	if (request_mempool == NULL) {
		fprintf(stderr, "failed to initialize request mempool!\n");
		return 1;
	}

	printf("\n==================================\n");
	printf(  "  NVMe Info - ict.ncic.syssw.ufo"    );
	printf("\n==================================\n");

	/*
	 * enumerate NVMe devices and attach them to userspace.
	 *
	 * probe_cb verifies whether non-uio driver exists.
	 *
	 * attach_cb prints controller and namespace information and then detaches the controller.
	 */
	rc = 0;
	if (spdk_nvme_probe(NULL, probe_cb, attach_cb) != 0) {
		fprintf(stderr, "spdk_nvme_probe() failed!\n");
		rc = 1;
	}

	/*
	 * free the RTE-allocated resources.
	 */
	if (health_page) {
		rte_free(health_page);
		health_page = NULL;
	}

	return rc;
}

