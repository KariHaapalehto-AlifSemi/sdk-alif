/* Copyright (C) 2024 Alif Semiconductor - All Rights Reserved.
 * Use, distribution and modification of this code is permitted under the
 * terms stated in the Alif Semiconductor Software License Agreement
 *
 * You should have received a copy of the Alif Semiconductor Software
 * License Agreement with this file. If not, please write to:
 * contact@alifsemi.com, or visit: https://alifsemi.com/license
 *
 * MHU Doorbell + Shared SRAM1 test: M55-HE <-> A32
 *
 * Protocol:
 *   - SRAM1 region at 0x027DD800 is used as shared memory.
 *   - MHU channel 0 is used as a doorbell (the 32-bit value sent via
 *     MHU carries the physical address of the data in shared SRAM1).
 *   - Both sides write their payload into shared SRAM, then ring the
 *     MHU doorbell with the physical address so the other side knows
 *     where to read.
 *
 * IMPORTANT: D-cache coherency
 *   A32 writes to SRAM via non-cacheable /dev/mem mapping, but M55-HE
 *   has D-cache enabled. Before reading shared SRAM written by A32,
 *   M55-HE must invalidate its D-cache for that region. After writing
 *   shared SRAM for A32, M55-HE must clean (flush) its D-cache.
 *
 * Shared memory layout (struct shared_msg at SHARED_MEM_BASE):
 *   [0x00] magic      - 0xA32F055E when written by A32, 0xF055EA32 by HE
 *   [0x04] msg_id     - incrementing message counter
 *   [0x08] data_len   - number of payload bytes (max 240)
 *   [0x0C] data[240]  - payload
 *   [0xFC] checksum   - sum of payload bytes
 *
 * Test flow (per iteration):
 *   1. HE waits for doorbell from A32 (MHU RX interrupt)
 *   2. HE invalidates D-cache, then reads shared SRAM
 *   3. HE writes a response into shared SRAM, then flushes D-cache
 *   4. HE rings doorbell to A32 (MHU TX) with the response address
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/ipm.h>
#include <string.h>

/* CMSIS D-cache maintenance functions (SCB_InvalidateDCache_by_Addr, etc.)
 * Available because __DCACHE_PRESENT=1 in soc.h for M55-HE */
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
#define CACHE_INVALIDATE(addr, size) SCB_InvalidateDCache_by_Addr((void *)(addr), (int32_t)(size))
#define CACHE_CLEAN(addr, size)      SCB_CleanDCache_by_Addr((void *)(addr), (int32_t)(size))
#else
#define CACHE_INVALIDATE(addr, size) do { __DSB(); } while (0)
#define CACHE_CLEAN(addr, size)      do { __DSB(); } while (0)
#endif

#define ITERATIONS       25
#define MAX_ERRORS       10
#define RX_TIMEOUT_MS    5000  /* Timeout waiting for A32 doorbell */
#define TX_TIMEOUT_MS    1000  /* Timeout waiting for TX completion */

/* Shared SRAM1 addresses — must match the Linux side
 * Region 0x027DD800–0x027DDFFF used for IPC shared memory
 * (immediately below arm-tf @ 0x027DE000; MHU services @ 0x027FE000) */
#define SHARED_MEM_BASE  0x027DDC00
#define A32_TO_HE_ADDR   (SHARED_MEM_BASE + 0x0000)  /* A32 writes here */
#define HE_TO_A32_ADDR   (SHARED_MEM_BASE + 0x0100)  /* HE writes here  */

#define MAGIC_A32        0xA32F055E   /* Written by A32 */
#define MAGIC_HE         0xF055EA32   /* Written by HE  */
#define MAX_PAYLOAD      240

/* Shared message structure — 256 bytes total */
struct shared_msg {
	uint32_t magic;
	uint32_t msg_id;
	uint32_t data_len;
	uint8_t  data[MAX_PAYLOAD];
	uint32_t checksum;
};

const struct device *mhu_rx_dev;  /* MHU receiver from A32 */
const struct device *mhu_tx_dev;  /* MHU sender to A32     */

static volatile bool doorbell_tx_done;
static volatile bool doorbell_rx_received;
static volatile uint32_t rx_doorbell_addr;

/* Callback: doorbell received from A32 */
static void mhu_rx_callback(const struct device *dev, void *user_data,
			    uint32_t id, volatile void *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	rx_doorbell_addr = *((uint32_t *)data);
	printk("M55-HE: Doorbell RX Ch%d, addr=0x%08x\n", id, rx_doorbell_addr);
	doorbell_rx_received = true;
}

/* Callback: doorbell sent to A32 */
static void mhu_tx_callback(const struct device *dev, void *user_data,
			    uint32_t id, volatile void *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	ARG_UNUSED(data);

	printk("M55-HE: Doorbell TX Ch%d done\n", id);
	doorbell_tx_done = true;
}

/* Simple checksum over the payload */
static uint32_t calc_checksum(const uint8_t *buf, uint32_t len)
{
	uint32_t sum = 0;

	for (uint32_t i = 0; i < len; i++) {
		sum += buf[i];
	}
	return sum;
}

int main(void)
{
	int iter;
	volatile struct shared_msg *tx_msg =
			(volatile struct shared_msg *)HE_TO_A32_ADDR;

	printk("\n==========================================\n");
	printk("M55-HE <-> A32 : MHU Doorbell + Shared SRAM1\n");
	printk("  Shared RX (A32->HE): 0x%08x\n", A32_TO_HE_ADDR);
	printk("  Shared TX (HE->A32): 0x%08x\n", HE_TO_A32_ADDR);
	printk("==========================================\n");

	/* Get device handles from DTS aliases */
	mhu_rx_dev = DEVICE_DT_GET(DT_ALIAS(apsshemhu0r));
	mhu_tx_dev = DEVICE_DT_GET(DT_ALIAS(apsshemhu0s));

	if (!device_is_ready(mhu_rx_dev) || !device_is_ready(mhu_tx_dev)) {
		printk("ERROR: MHU devices not ready!\n");
		return -1;
	}
	printk("MHU devices ready\n");

	/* Register callbacks */
	ipm_register_callback(mhu_rx_dev, mhu_rx_callback, NULL);
	ipm_register_callback(mhu_tx_dev, mhu_tx_callback, NULL);

	/* Enable receiver interrupt */
	ipm_set_enabled(mhu_rx_dev, true);

	/*
	 * Only clear HE's own TX region.
	 * Do NOT clear A32_TO_HE region — A32 may have already written data
	 * there before HE boots.
	 */
	memset((void *)HE_TO_A32_ADDR, 0, sizeof(struct shared_msg));
	CACHE_CLEAN(HE_TO_A32_ADDR, sizeof(struct shared_msg));

	printk("Waiting for A32 doorbell...\n\n");

	int timeouts = 0;
	int completed = 0;
	int errors = 0;

	for (iter = 0; iter < ITERATIONS; iter++) {
		/* --- Wait for doorbell from A32 --- */
		doorbell_rx_received = false;

		if (iter == 0) {
			/* First iteration: wait indefinitely for Linux to boot */
			while (!doorbell_rx_received) {
				k_sleep(K_MSEC(1));
			}
		} else {
			/* Subsequent iterations: apply timeout */
			int rx_waited = 0;

			while (!doorbell_rx_received &&
			       rx_waited < RX_TIMEOUT_MS) {
				k_sleep(K_MSEC(1));
				rx_waited++;
			}
		}

		if (!doorbell_rx_received) {
			printk("WARN: Timeout waiting for A32 doorbell "
			       "(%d ms), retry %d\n",
			       RX_TIMEOUT_MS, timeouts + 1);
			if (++timeouts > 5) {
				printk("ERROR: Too many timeouts, aborting\n");
				break;
			}
			iter--; /* Don't consume this iteration */
			continue;
		}
		timeouts = 0;

		/* Validate doorbell RX address (exact match + range check) */
		if (rx_doorbell_addr != A32_TO_HE_ADDR) {
			printk("ERROR: Unexpected doorbell addr 0x%08x "
			       "(expected 0x%08x)\n",
			       rx_doorbell_addr, A32_TO_HE_ADDR);
			if (rx_doorbell_addr < SHARED_MEM_BASE ||
			    rx_doorbell_addr >= (SHARED_MEM_BASE + 0x0200)) {
				printk("ERROR: Doorbell addr outside shared "
				       "region [0x%08x-0x%08x)\n",
				       SHARED_MEM_BASE,
				       SHARED_MEM_BASE + 0x0200);
			}
			errors++;
			if (errors >= MAX_ERRORS) {
				printk("ERROR: Too many errors (%d), "
				       "aborting\n", errors);
				break;
			}
			continue;
		}

		/* --- Invalidate D-cache before reading A32's data --- */
		CACHE_INVALIDATE(rx_doorbell_addr, sizeof(struct shared_msg));
		__DSB();

		volatile struct shared_msg *incoming =
				(volatile struct shared_msg *)rx_doorbell_addr;

		/* Stale RX check: detect if A32 data hasn't been updated */
		if (incoming->msg_id != (uint32_t)iter) {
			printk("WARN: Possible stale RX (msg_id=%u, "
			       "expected %d)\n",
			       incoming->msg_id, iter);
		}

		if (incoming->magic != MAGIC_A32) {
			printk("ERROR: Bad magic 0x%08x (expected 0x%08x)\n",
			       incoming->magic, MAGIC_A32);
			errors++;
			continue;
		}

		if (incoming->msg_id != (uint32_t)iter) {
			printk("ERROR: msg_id mismatch: got %u, expected %d\n",
			       incoming->msg_id, iter);
			errors++;
			continue;
		}

		/* Bounds-check data_len to prevent out-of-bounds access */
		uint32_t rx_len = incoming->data_len;
		if (rx_len == 0) {
			printk("WARN: Zero-length payload from A32 "
			       "(iter=%d)\n", iter);
		}
		if (rx_len > MAX_PAYLOAD) {
			printk("ERROR: data_len %u exceeds MAX_PAYLOAD %u\n",
			       rx_len, MAX_PAYLOAD);
			errors++;
			continue;
		}

		uint32_t computed_cksum = calc_checksum((const uint8_t *)incoming->data,
							rx_len);
		printk("[%d] A32->HE: msg_id=%u len=%u cksum=0x%x/0x%x %s\n",
		       iter, incoming->msg_id, rx_len,
		       computed_cksum, incoming->checksum,
		       (computed_cksum == incoming->checksum) ? "PASS" : "FAIL");

		/* Print payload bytes (cap at 16 for readability) */
		printk("  data: ");
		uint32_t print_len = (rx_len > 16) ? 16 : rx_len;
		for (uint32_t j = 0; j < print_len; j++) {
			printk("0x%02x ", incoming->data[j]);
		}
		if (rx_len > 16)
			printk("... (%u more)", rx_len - 16);
		printk("\n");

		/* Small delay to let A32 prepare for RX */
		k_sleep(K_MSEC(10));

		/* --- Invalidate D-cache for TX region before writing --- */
		CACHE_INVALIDATE(HE_TO_A32_ADDR, sizeof(struct shared_msg));
		__DSB();

		/* --- Write response into shared SRAM --- */
		tx_msg->magic    = MAGIC_HE;
		tx_msg->msg_id   = iter;
		tx_msg->data_len = rx_len;

		/* Echo all incoming bytes XOR'd with per-iteration key */
		uint8_t xor_key = (uint8_t)(iter + 0x42);
		for (uint32_t j = 0; j < rx_len; j++) {
			tx_msg->data[j] = incoming->data[j] ^ xor_key;
		}
		/* Zero-pad unused data[] bytes to avoid stale residue */
		if (rx_len < MAX_PAYLOAD) {
			memset((void *)&tx_msg->data[rx_len], 0,
			       MAX_PAYLOAD - rx_len);
		}
		tx_msg->checksum = calc_checksum((const uint8_t *)tx_msg->data,
						 rx_len);

		/* --- Flush D-cache so A32 sees our writes in SRAM --- */
		CACHE_CLEAN(HE_TO_A32_ADDR, sizeof(struct shared_msg));
		__DSB();

		printk("[%d] HE->A32: Sending response at 0x%08x\n",
		       iter, HE_TO_A32_ADDR);

		/* --- Ring doorbell to A32 with address of response --- */
		doorbell_tx_done = false;
		uint32_t doorbell_val = HE_TO_A32_ADDR;

		int send_ret = ipm_send(mhu_tx_dev, 0, 0, &doorbell_val, 4);
		if (send_ret != 0) {
			printk("ERROR: ipm_send failed: %d\n", send_ret);
			errors++;
			continue;
		}

		int tx_waited = 0;

		while (!doorbell_tx_done && tx_waited < TX_TIMEOUT_MS) {
			k_sleep(K_MSEC(1));
			tx_waited++;
		}
		if (!doorbell_tx_done) {
			printk("ERROR: Timeout waiting for TX completion "
			       "(%d ms)\n", TX_TIMEOUT_MS);
			break;
		}

		if (computed_cksum == incoming->checksum) {
			printk("[%d] Complete\n\n", iter);
			completed++;
		} else {
			printk("[%d] CKSUM FAIL\n\n", iter);
			errors++;
		}

		if (errors >= MAX_ERRORS) {
			printk("ERROR: Too many errors (%d), aborting\n",
			       errors);
			break;
		}
	}

	ipm_set_enabled(mhu_rx_dev, false);
	printk("Done: %d/%d passed, %d failed\n",
	       completed, ITERATIONS, errors);
	return (completed + errors == ITERATIONS) ? 0 : -1;
}
