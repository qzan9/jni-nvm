/*
 * libjninvme: a naive user-level SSD API for Java.
 *
 * Author(s)
 *   azq    @qzan9    anzhongqi@ncic.ac.cn
 */

#ifdef __GNUC__
#	define _SVID_SOURCE
#endif /* __GNUC__ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stddef.h>

#include <unistd.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_mempool.h>

#include <spdk/nvme.h>

#include <jni.h>

#define U2_REQUEST_POOL_SIZE    (1024)
#define U2_REQUEST_CACHE_SIZE   (0)
#define U2_REQUEST_PRIVATE_SIZE (0)

#define U2_NAMESPACE_ID         (1)
#define U2_BUFFER_ALIGN         (0x200)

static struct spdk_nvme_ctrlr *u2_ctrlr;

static uint32_t u2_ns_id;
static struct spdk_nvme_ns *u2_ns;

static uint32_t u2_ns_sector;
static uint64_t u2_ns_size;

static struct spdk_nvme_qpair *u2_qpair;

static uint32_t io_depth;

struct rte_mempool *request_mempool;
static char *ealargs[] = { "libjninvme", "-c 0x100", "-n 1", };

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *, void *);
//JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *, void *);

JNIEXPORT void JNICALL nvmeInitialize(JNIEnv *, jobject);
JNIEXPORT void JNICALL nvmeFinalize  (JNIEnv *, jobject);

JNIEXPORT void JNICALL nvmeWrite(JNIEnv *, jobject, jobject, jlong, jlong);
JNIEXPORT void JNICALL nvmeRead (JNIEnv *, jobject, jobject, jlong, jlong);

JNIEXPORT jobject JNICALL allocateHugepageMemory(JNIEnv *, jobject, jlong);
JNIEXPORT void    JNICALL     freeHugepageMemory(JNIEnv *, jobject, jobject);

#ifdef __cplusplus
}
#endif

static const JNINativeMethod methods[] = {
	{ "nvmeInitialize",         "()V",                         (void *)nvmeInitialize         },
	{ "nvmeFinalize",           "()V",                         (void *)nvmeFinalize           },
	{ "nvmeWrite",              "(Ljava/nio/ByteBuffer;JJ)V",  (void *)nvmeWrite              },
	{ "nvmeRead",               "(Ljava/nio/ByteBuffer;JJ)V",  (void *)nvmeRead               },
	{ "allocateHugepageMemory", "(J)Ljava/nio/ByteBuffer;",    (void *)allocateHugepageMemory },
	{ "freeHugepageMemory",     "(Ljava/nio/ByteBuffer;)V",    (void *)freeHugepageMemory     },
};

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *jvm, void *reserved)
{
	JNIEnv *env = NULL;

//	if (jvm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK)
	if ((*jvm)->GetEnv(jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
		return JNI_ERR;
	}

//	if (env->RegisterNatives(env->FindClass("Lac/ncic/syssw/jni/JniNvme;"),
	if ((*env)->RegisterNatives(env,
	                            (*env)->FindClass(env, "Lac/ncic/syssw/jni/JniNvme;"),
	                            methods,
	                            sizeof(methods) / sizeof(methods[0])
	                           ) < -1) {
		return JNI_ERR;
	}

	return JNI_VERSION_1_6;
}

//JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *jvm, void *reserved)
//{
//}

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

JNIEXPORT void JNICALL nvmeInitialize(JNIEnv *env, jobject thisObj)
{
	if (rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]), ealargs) < 0) {
		fprintf(stderr, "failed to initialize EAL!\n");
		exit(1);
	}

	printf("\n========================================\n");
	printf(  "  jni_nvme/jni_u2 - ict.ncic.syssw.ufo"    );
	printf("\n========================================\n");

	u2_ctrlr = NULL;
	u2_ns_id = U2_NAMESPACE_ID;
	u2_ns = NULL;

	io_depth = 0;

	request_mempool = rte_mempool_create("nvme_request",
	                                     U2_REQUEST_POOL_SIZE, spdk_nvme_request_size(),
	                                     U2_REQUEST_CACHE_SIZE, U2_REQUEST_PRIVATE_SIZE,
	                                     NULL, NULL, NULL, NULL,
	                                     SOCKET_ID_ANY, 0);
	if (request_mempool == NULL) {
		fprintf(stderr, "failed to create request pool!\n");
		exit(1);
	}

	if (spdk_nvme_probe(NULL, probe_cb, attach_cb)) {
		fprintf(stderr, "failed to probe and attach to NVMe device!\n");
		exit(1);
	}

	if (!u2_ctrlr) {
		fprintf(stderr, "failed to probe a suitable controller!\n");
		exit(1);
	}

	if (!spdk_nvme_ns_is_active(u2_ns)) {
		fprintf(stderr, "namespace %d is in-active!\n", u2_ns_id);
		exit(1);
	}

	if (!u2_qpair) {
		fprintf(stderr, "failed to allocate queue pair!\n");
		exit(1);
	}
}

JNIEXPORT void JNICALL nvmeFinalize(JNIEnv *env, jobject thisObj)
{
	if (u2_qpair) {
		spdk_nvme_ctrlr_free_io_qpair(u2_qpair);
	}

	if (u2_ctrlr) {
		spdk_nvme_detach(u2_ctrlr);
	}
}

static void
u2_io_complete(void *cb_args, const struct spdk_nvme_cpl *completion)
{
	io_depth--;
}

JNIEXPORT void JNICALL nvmeWrite(JNIEnv *env, jobject thisObj, jobject buffer, jlong offset, jlong size)
{
	uint8_t *buf;
	uint64_t offset_in_blocks;
	uint32_t size_in_blocks;

	if (u2_ns_size < size) {
		fprintf(stderr, "invalid I/O size %"PRId64"!\n", (int64_t)size);
		exit(1);
	}

	buf = (uint8_t *)(*env)->GetDirectBufferAddress(env, buffer);
	offset_in_blocks = offset / u2_ns_sector;    // byte-address -> block-address: here is naive stupid wrong!!!
	size_in_blocks = size / u2_ns_sector;

	if (spdk_nvme_ns_cmd_write(u2_ns, u2_qpair, buf, offset_in_blocks, size_in_blocks, u2_io_complete, NULL, 0)) {
		fprintf(stderr, "failed to submit request!\n");
		exit(1);
	}
	io_depth++;

	while (io_depth > 0) {
		spdk_nvme_qpair_process_completions(u2_qpair, 0);
	}
}

JNIEXPORT void JNICALL nvmeRead(JNIEnv *env, jobject thisObj, jobject buffer, jlong offset, jlong size)
{
	uint8_t *buf;
	uint64_t offset_in_blocks;
	uint32_t size_in_blocks;

	if (u2_ns_size < size) {
		fprintf(stderr, "invalid I/O size %"PRId64"!\n", (int64_t)size);
		exit(1);
	}

	buf = (uint8_t *)(*env)->GetDirectBufferAddress(env, buffer);
	offset_in_blocks = offset / u2_ns_sector;    // byte-address -> block-address: here is naive stupid wrong!!!
	size_in_blocks = size / u2_ns_sector;

	if (spdk_nvme_ns_cmd_read(u2_ns, u2_qpair, buf, offset_in_blocks, size_in_blocks, u2_io_complete, NULL, 0)) {
		fprintf(stderr, "failed to submit request!\n");
		exit(1);
	}
	io_depth++;

	while (io_depth > 0) {
		spdk_nvme_qpair_process_completions(u2_qpair, 0);
	}
}

JNIEXPORT jobject JNICALL allocateHugepageMemory(JNIEnv *env, jobject thisObj, jlong size)
{
	uint8_t *buf;

	buf = rte_malloc(NULL, size, U2_BUFFER_ALIGN);
	if (buf == NULL) {
		fprintf(stderr, "failed to allocate hugepage memory!\n");
		exit(1);
	}
	memset(buf, 0x00, size);

	return (*env)->NewDirectByteBuffer(env, buf, (jlong)size);
}

JNIEXPORT void JNICALL freeHugepageMemory(JNIEnv *env, jobject thisObj, jobject buffer)
{
	uint8_t *buf = (uint8_t *)(*env)->GetDirectBufferAddress(env, buffer);

	rte_free(buf);
}

