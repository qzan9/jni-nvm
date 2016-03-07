/*
 * dpdk_memtest: naive DPDK hugepage memory test.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rte_config.h>
#include <rte_malloc.h>

#define U2_SIZE			(32)

#define U2_BUFFER_ALIGN		(0x200)

static void *dpdk_buf_w, *dpdk_buf_r;
static void *buf_w, *buf_r;

static int seed;

static char *ealargs[] = { "dpdk_memtest", "-c 0x1", "-n 4", };

int main(int argc, char *argv[])
{
	int i;

	if (rte_eal_init(sizeof(ealargs) / sizeof(ealargs[0]), ealargs) < 0) {
		fprintf(stderr, "failed to initialize DPDK EAL!\n");
		return 1;
	}

	dpdk_buf_w = rte_malloc(NULL, U2_SIZE, U2_BUFFER_ALIGN);
	if (dpdk_buf_w == NULL) {
		fprintf(stderr, "failed to rte_malloc buffer!\n");
		return 1;
	}
	memset(dpdk_buf_w, rand_r(&seed), U2_SIZE);

	buf_w = malloc(U2_SIZE);
	if (buf_w == NULL) {
		fprintf(stderr, "failed to malloc buffer!\n");
		return 1;
	}
	memcpy(buf_w, dpdk_buf_w, U2_SIZE);

	dpdk_buf_r = rte_malloc(NULL, U2_SIZE, U2_BUFFER_ALIGN);
	if (dpdk_buf_r == NULL) {
		fprintf(stderr, "failed to rte_malloc buffer!\n");
		return 1;
	}
	//memset(dpdk_buf_r, rand_r(&seed), U2_SIZE);
	memcpy(dpdk_buf_r, dpdk_buf_w, U2_SIZE);

	//buf_r = malloc(U2_SIZE_32B);
	//if (buf_r == NULL) {
	//	fprintf(stderr, "failed to malloc buffer!\n");
	//	return 1;
	//}
	//memcpy(buf_r, dpdk_buf_r, U2_SIZE_32B);

	rte_free(dpdk_buf_w);

	if (!memcmp(buf_w, dpdk_buf_r, U2_SIZE)) {
		printf("same!\n");
	}

	rte_free(dpdk_buf_r);

	free(buf_w);
	//free(buf_r);

	return 0;
}

