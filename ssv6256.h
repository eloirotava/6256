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
#include <linux/if_ether.h>
#include <linux/types.h>
#include <linux/mmc/sdio_func.h>

#include "reg.h"

#define SSV_FIRMWARE		"ssv/ssv6x5x-sw.bin"

/* Crystal fitted on the module, as a RG_*_XTAL_FREQ code. */
#define SSV_XTAL		XTAL24M

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

/* Packet engines, as used by the receive flow and trap registers. */
#define M_ENG_CPU		0x00
#define M_ENG_HWHCI		0x01
#define M_ENG_MACRX		0x04
#define M_ENG_ENCRYPT_SEC	0x0b
#define M_ENG_MIC_SEC		0x0c
#define M_ENG_TRASH_CAN		0x0f

struct ssv_reg;

struct ssv_dev {
	struct sdio_func *func;
	struct device *dev;

	/* SDIO */
	u32 bus_clock;		/* negotiated by the MMC core */
	u32 data_port;
	u32 reg_port;
	u8 *io_buf;		/* DMA-safe scratch, used under the SDIO host lock */

	char chip_id[20];
	u8 mac[ETH_ALEN];
};

/* sdio.c */
int ssv_reg_read(struct ssv_dev *sd, u32 addr, u32 *val);
int ssv_reg_write(struct ssv_dev *sd, u32 addr, u32 val);
int ssv_reg_set_bits(struct ssv_dev *sd, u32 addr, u32 set, u32 mask);
int ssv_write_data(struct ssv_dev *sd, const u8 *buf, size_t len);
int ssv_load_firmware(struct ssv_dev *sd);

/* hw.c */
int ssv_write_table(struct ssv_dev *sd, const struct ssv_reg *t, size_t n);
int ssv_hw_start(struct ssv_dev *sd);
void ssv_set_bssid(struct ssv_dev *sd, const u8 *bssid);

/* phy.c */
int ssv_phy_init(struct ssv_dev *sd);
int ssv_phy_enable(struct ssv_dev *sd, bool enable);
int ssv_set_channel(struct ssv_dev *sd, int channel);
int ssv_set_bandwidth(struct ssv_dev *sd, bool ht40, bool sec_above);

/* Read-modify-write of one register field, given its mask. */
static inline int ssv_field_write(struct ssv_dev *sd, u32 addr, u32 mask,
				  u32 val)
{
	return ssv_reg_set_bits(sd, addr, val << __ffs(mask), mask);
}

static inline int ssv_field_read(struct ssv_dev *sd, u32 addr, u32 mask,
				 u32 *val)
{
	u32 regval;
	int ret;

	ret = ssv_reg_read(sd, addr, &regval);
	if (ret)
		return ret;
	*val = (regval & mask) >> __ffs(mask);
	return 0;
}

#endif
