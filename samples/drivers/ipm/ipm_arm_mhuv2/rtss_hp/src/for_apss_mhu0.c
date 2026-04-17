/* Copyright (C) 2024 Alif Semiconductor */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/ipm.h>

#define ITERATIONS 10

/*
 * M55 HP <-> A32 Communication Test (MHU0)
 *
 * Aliases must be defined in DTS:
 * apssmhu0r -> MHU0 Receiver (from A32)
 * apssmhu0s -> MHU0 Sender (to A32)
 */

const struct device *mhu0_r;  // Receiver from A32
const struct device *mhu0_s;  // Sender to A32

static volatile bool msg_sent;
static volatile bool msg_received;
static uint32_t tx_msg;

/* Callback when data received from A32 */
static void recv_cb(const struct device *dev, void *user_data,
                    uint32_t id, volatile void *data)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);

    /* Print received data */
    printk("M55-HP: RX MHU0 Ch%d = 0x%x\n", id, *((uint32_t *)data));
    msg_received = true;
}

/* Callback when data sent to A32 */
static void send_cb(const struct device *dev, void *user_data,
                    uint32_t id, volatile void *data)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);
    ARG_UNUSED(data);

    printk("M55-HP: TX MHU0 Ch%d Done\n", id);
    msg_sent = true;
}

/* Send reply to A32 */
static void send_reply(void)
{
    /*
     * Send replies on MHU0 Channel 0.
     * A32 Linux should listen on "rxdb1" (M55 HP MHU0 RX endpoint).
     */

    /* Send 1st Message */
    msg_sent = false;
    tx_msg = 0xff23fe32;  // Different data pattern for HP MHU0
    printk("M55-HP: Sending 0x%x on MHU0 Ch0...\n", tx_msg);
    ipm_send(mhu0_s, 0, 0, &tx_msg, 4);
    while (!msg_sent);

    /* Send 2nd Message */
    msg_sent = false;
    tx_msg = 0xff23fe33;  // Different data pattern for HP MHU0
    printk("M55-HP: Sending 0x%x on MHU0 Ch0...\n", tx_msg);
    ipm_send(mhu0_s, 0, 0, &tx_msg, 4);
    while (!msg_sent);
}

int main(void)
{
    uint32_t recv_data;
    int i = 0;

    printk("\n========================================\n");
    printk("M55-HP <-> A32 (APSS) MHU0 Test\n");
    printk("========================================\n");

    /* Get device handles from DTS aliases */
    mhu0_r = DEVICE_DT_GET(DT_ALIAS(apssmhu0r));
    mhu0_s = DEVICE_DT_GET(DT_ALIAS(apssmhu0s));

    if (!device_is_ready(mhu0_r) || !device_is_ready(mhu0_s)) {
        printk("ERROR: MHU0 devices not ready!\n");
        return -1;
    }

    printk("MHU0 devices ready\n");

    /* Register callbacks */
    ipm_register_callback(mhu0_r, recv_cb, &recv_data);
    ipm_register_callback(mhu0_s, send_cb, NULL);

    /* Enable receiver interrupt */
    ipm_set_enabled(mhu0_r, true);

    printk("Waiting for A32 messages on MHU0...\n\n");

    /* Main loop: wait for A32, then reply */
    while (i < ITERATIONS) {
        msg_received = false;

        /* Wait for A32 to send data */
        while (!msg_received);

        /* Give a small delay to ensure A32 is ready to receive */
        k_sleep(K_MSEC(10));

        /* Send reply */
        send_reply();

        printk("Iteration %d complete\n\n", i + 1);
        ++i;
    }

    /* Disable receiver */
    ipm_set_enabled(mhu0_r, false);

    printk("MHU0 Test completed successfully!\n");
    return 0;
}

