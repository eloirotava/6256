/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mac80211 driver for the iComm SSV6256 SDIO 802.11a/b/g/n chip
 * ("Turismo" family: 2.4 and 5 GHz, HT20/40, one spatial stream).
 *
 * Hardware interface derived from the iComm vendor driver:
 * Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 * Copyright (c) 2015 iComm Corporation
 */
#ifndef SSV6256_H
#define SSV6256_H

#include <linux/bitfield.h>
#include <linux/types.h>
#include <linux/mmc/sdio_func.h>

#include "reg.h"

#define SSV_FIRMWARE		"ssv/ssv6x5x-sw.bin"

/* SDIO function 1 registers (CMD52) */
#define SDIO_REG_DATA_PORT0	0x00
#define SDIO_REG_INT_MASK	0x04
#define SDIO_REG_INT_STATUS	0x08
#define SDIO_REG_FN1_STATUS	0x0c
#define SDIO_REG_RX_LEN0	0x10
#define SDIO_REG_RX_LEN1	0x11
#define SDIO_REG_OUTPUT_TIMING	0x55
#define SDIO_REG_PMU_WAKEUP	0x67
#define SDIO_REG_REG_PORT0	0x70
#define SDIO_REG_TX_ALLOC	0x99

#define SDIO_BLOCK_SIZE		128
#define SDIO_OUTPUT_TIMING	0
#define SDIO_CLOCK_INIT		25000000U
#define SDIO_TX_ALLOC_SHIFT	0x07
#define SDIO_TX_ALLOC_ENABLE	0x10

#define IO_BUF_SIZE		16

struct ssv_dev {
	struct sdio_func *func;
	struct device *dev;

	/* SDIO */
	u32 bus_clock;		/* negotiated by the MMC core */
	u32 data_port;
	u32 reg_port;
	u8 *io_buf;		/* DMA-safe scratch, used under the SDIO host lock */

	char chip_id[20];
};

/* sdio.c */
int ssv_reg_read(struct ssv_dev *sd, u32 addr, u32 *val);
int ssv_reg_write(struct ssv_dev *sd, u32 addr, u32 val);
int ssv_reg_set_bits(struct ssv_dev *sd, u32 addr, u32 set, u32 mask);
int ssv_write_data(struct ssv_dev *sd, const u8 *buf, size_t len);
int ssv_load_firmware(struct ssv_dev *sd);

#endif
