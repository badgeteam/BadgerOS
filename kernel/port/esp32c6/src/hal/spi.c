
// SPDX-License-Identifier: MIT

#include "hal/spi.h"

#include <badge_strings.h>

#include "hal/gpio.h"
#include "port/clkconfig.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"
#include "soc/spi_struct.h"
#include "soc/pcr_struct.h"
#include "soc/clk_tree_defs.h"
#include "soc/io_mux_struct.h"
#include "soc/pmu_struct.h"

static void spi_clear_fifo(bool clear_rxfifo, bool clear_txfifo) {
    // Clear FIFOs.
    GPSPI2.dma_conf = (spi_dma_conf_reg_t){
        .buf_afifo_rst = clear_txfifo,
        .rx_afifo_rst = clear_rxfifo,
    };
    GPSPI2.dma_conf = (spi_dma_conf_reg_t){
        .buf_afifo_rst = false,
        .rx_afifo_rst = false,
    };
}

static void spi_config_apply(void) {
    // Apply the register configurations and wait until it's done.
    GPSPI2.cmd.update = 1;
    while(GPSPI2.cmd.update);
}

void spi_master_init(badge_err_t *ec, int spi_num, int sclk_pin, int mosi_pin, int miso_pin) {
    // Bounds check.
    if (spi_num != 0
    || sclk_pin < 0 || sclk_pin >= io_count()
    || mosi_pin < 0 || mosi_pin >= io_count()
    || miso_pin < 0 || miso_pin >= io_count()
    ) {
        badge_err_set(ec, ELOC_I2C, ECAUSE_RANGE);
        return;
    }

    // Pin availability check.
    if (io_is_peripheral(ec, sclk_pin)) {
        logkf(LOG_ERROR, "SCLK pin (%{d}) already in use", sclk_pin);
        return;
    } else if (io_is_peripheral(ec, mosi_pin)) {
        logkf(LOG_ERROR, "MOSI pin (%{d}) already in use", mosi_pin);
        return;
    } else if (io_is_peripheral(ec, miso_pin)) {
        logkf(LOG_ERROR, "MISO pin (%{d}) already in use", miso_pin);
        return;
    }

    // SPI master configuration.

    //Reset timing.
    GPSPI2.user1.cs_setup_time = 0;
    GPSPI2.user1.cs_hold_time = 0;

    //use all 64 bytes of the buffer
    GPSPI2.user.usr_miso_highpart = 0;
    GPSPI2.user.usr_mosi_highpart = 0;

    // Disable unneeded ints.
    GPSPI2.slave.val = 0;
    GPSPI2.user.val = 0;

    // Clock configuration.
    clkconfig_spi2(1000*1000, true, false);

    // TODO: determine function (copied from spi_ll.h)
    GPSPI2.dma_conf.val = 0;
    GPSPI2.dma_conf.slv_tx_seg_trans_clr_en = 1;
    GPSPI2.dma_conf.slv_rx_seg_trans_clr_en = 1;
    GPSPI2.dma_conf.dma_slv_seg_trans_en = 0;

    // Enable the trans_done interrupt.
    GPSPI2.dma_int_ena.trans_done = 1;
    // Set the trans_done interrupt.
    GPSPI2.dma_int_set.trans_done_int_set = 1;

    //Enable the write-data phase
    GPSPI2.user.usr_mosi = 1;
    //Enable the read-data phase
    GPSPI2.user.usr_miso = 1;

    spi_config_apply();

    // TODO: check values; copied from i2c
    IO_MUX.gpio[sclk_pin] = (io_mux_gpio_t){.mcu_sel = 1, .fun_ie = true, .mcu_ie = true};
    IO_MUX.gpio[miso_pin] = (io_mux_gpio_t){.mcu_sel = 1, .fun_ie = true, .mcu_ie = true};
    IO_MUX.gpio[mosi_pin] = (io_mux_gpio_t){.mcu_sel = 1, .fun_ie = true, .mcu_ie = true};

    // GPIO matrix configuration.
    GPIO.func_out_sel_cfg[sclk_pin] = (gpio_func_out_sel_cfg_reg_t){
        .oen_inv_sel = false,
        .oen_sel     = false,
        .out_inv_sel = false,
        .out_sel     = FSPICLK_OUT_IDX,
    };
    GPIO.func_out_sel_cfg[miso_pin] = (gpio_func_out_sel_cfg_reg_t){
        .oen_inv_sel = false,
        .oen_sel     = false,
        .out_inv_sel = false,
        .out_sel     = FSPIQ_OUT_IDX,
    };
    GPIO.func_out_sel_cfg[mosi_pin] = (gpio_func_out_sel_cfg_reg_t){
        .oen_inv_sel = false,
        .oen_sel     = false,
        .out_inv_sel = false,
        .out_sel     = FSPID_OUT_IDX,
    };

    GPIO.func_in_sel_cfg[FSPICLK_IN_IDX] = (gpio_func_in_sel_cfg_reg_t){
        .in_sel     = sclk_pin,
        .in_inv_sel = false,
        .sig_in_sel = true,
    };
    GPIO.func_in_sel_cfg[FSPIQ_IN_IDX] = (gpio_func_in_sel_cfg_reg_t){
        .in_sel     = miso_pin,
        .in_inv_sel = false,
        .sig_in_sel = true,
    };
    GPIO.func_in_sel_cfg[FSPIHD_IN_IDX] = (gpio_func_in_sel_cfg_reg_t){
        .in_sel     = mosi_pin,
        .in_inv_sel = false,
        .sig_in_sel = true,
    };
}

void spi_write_buffer(badge_err_t *ec, uint8_t *data, int len) {
    (void) ec; // TODO: proper checks and error handling

    size_t buf_idx = 0;

    spi_clear_fifo(true, true);

    while (len > 0) {
        int copy_len = sizeof(GPSPI2.data_buf);
        if (copy_len > len) {
            copy_len = len;
        }

        GPSPI2.ms_dlen.ms_data_bitlen = copy_len * 8-1;

        mem_copy((void *)GPSPI2.data_buf, data, copy_len);

        spi_config_apply();

        // Start transfer and wait for completion
        GPSPI2.cmd.usr = 1;
        while(GPSPI2.cmd.usr)
            logkf(LOG_DEBUG, "boop");

        // Copy chunk of received data back.
        mem_copy(data, (void *)GPSPI2.data_buf, copy_len);

        len -= copy_len;
        data += copy_len;
        buf_idx += 1;
    }
}
