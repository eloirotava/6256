// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MAC setup: reset, timing, frame buffer accounting, receive filtering
 * and the per-interface registers.
 *
 * Sequence taken from the vendor driver (ssv6006C_mac.c):
 * Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 * Copyright (c) 2015 iComm Corporation
 */
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/jhash.h>
#include <linux/mmc/card.h>
#include <linux/of_net.h>
#include <linux/unaligned.h>

#include "ssv6256.h"
#include "tables.h"

/*
 * Frame buffer accounting.  The chip holds 128 packet ids and 256 pages
 * of 256 bytes; the rest of each pool is left for receive.
 */
#define ID_TX_THRESHOLD		62
#define ID_RX_THRESHOLD		64
#define PAGE_TX_THRESHOLD	192
#define PAGE_RX_THRESHOLD	64
#define TX_LOWTHRESHOLD_PAGE	96
#define TX_LOWTHRESHOLD_ID	61

/* Descriptor sizes the DMA engine has to skip over (see tx.c, rx.c). */
#define TX_DESC_SIZE		80
#define RX_DESC_SIZE		32
#define RX_PINFO_PAD		4

/* Largest aggregate the receive scoreboard can track. */
#define MAX_RX_AGGR_SIZE	64

/* e-fuse: a chip id register plus a bit-packed list of items */
#define EFUSE_ID_READ_SWITCH	0xc2000128
#define EFUSE_ID_RAW_DATA	0xc200014c
#define EFUSE_READ_SWITCH	0xc200012c
#define EFUSE_RAW_DATA		0xc2000150
#define EFUSE_SECTIONS		((256 - 32) >> 5)
#define EFUSE_ITEM_MAC		3
#define EFUSE_ITEM_MAC_NEW	11

/* Payload width of each e-fuse item, in bits, indexed by its id. */
static const u8 efuse_item_bits[] = {
	0, 8, 8, 48, 8, 8, 8, 4, 0, 16, 16, 48, 8, 8,
};

int ssv_write_table(struct ssv_dev *sd, const struct ssv_reg *t, size_t n)
{
	size_t i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = ssv_reg_write(sd, t[i].addr, t[i].data);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * The e-fuse holds a bit-packed list of items, each a 4-bit id followed
 * by its payload; only the MAC address is used here.  A newer address
 * may be programmed on top of the original one, and wins.
 */
static void ssv_read_efuse(struct ssv_dev *sd)
{
	u8 map[EFUSE_SECTIONS * 4] = {};
	u32 val, pos = 0;
	int i;

	ssv_reg_write(sd, EFUSE_ID_READ_SWITCH, 1);
	if (!ssv_reg_read(sd, EFUSE_ID_RAW_DATA, &val))
		dev_dbg(sd->dev, "e-fuse chip identity 0x%08x\n", val);

	for (i = 0; i < EFUSE_SECTIONS; i++) {
		ssv_reg_write(sd, EFUSE_READ_SWITCH + i * 4, 1);
		if (ssv_reg_read(sd, EFUSE_RAW_DATA + i * 4, &val))
			return;
		put_unaligned_le32(val, map + i * 4);
	}

	eth_zero_addr(sd->mac);
	while (map[0] && pos + 4 + 48 <= sizeof(map) * 8) {
		u8 id = (get_unaligned_le16(map + pos / 8) >> (pos % 8)) & 0xf;
		int j;

		if (id == 0 || id >= ARRAY_SIZE(efuse_item_bits))
			break;
		pos += 4;
		if (id == EFUSE_ITEM_MAC || id == EFUSE_ITEM_MAC_NEW) {
			for (j = 0; j < ETH_ALEN; j++, pos += 8)
				sd->mac[j] = get_unaligned_le16(map + pos / 8) >>
					     (pos % 8);
		} else {
			pos += efuse_item_bits[id];
		}
	}

	/*
	 * Modules with a blank e-fuse depend on the board: the device
	 * tree can carry the address the vendor printed on the label.
	 */
	if (!is_valid_ether_addr(sd->mac))
		of_get_mac_address(sd->dev->of_node, sd->mac);

	/*
	 * Still nothing: derive a stable locally administered address from
	 * the card identification, so that it survives a reload.
	 */
	if (!is_valid_ether_addr(sd->mac)) {
		u32 h = jhash_2words(sd->func->card->cid.serial,
				     sd->func->card->cid.manfid, 0);

		sd->mac[0] = 0x02;
		sd->mac[1] = sd->func->card->cid.oemid;
		put_unaligned_le32(h, sd->mac + 2);
		eth_addr_inc(sd->mac);	/* keeps it valid if the hash is zero */
		dev_warn(sd->dev, "no MAC address in e-fuse or device tree, using %pM\n",
			 sd->mac);
	}
}

/* The reset bit clears itself once the MAC has come back. */
static int ssv_mac_reset(struct ssv_dev *sd)
{
	u32 val = MAC_SW_RST;
	int i;

	ssv_reg_write(sd, ADR_BRG_SW_RST, MAC_SW_RST);
	for (i = 0; i < 1000; i++) {
		if (ssv_reg_read(sd, ADR_BRG_SW_RST, &val))
			return -EIO;
		if (!val)
			return 0;
		usleep_range(50, 100);
	}
	dev_err(sd->dev, "MAC reset did not complete\n");
	return -ETIMEDOUT;
}

/*
 * MAC timing follows whatever digital clock the PLL settled on, so this
 * has to run after the radio is up.
 */
static int ssv_mac_clock(struct ssv_dev *sd)
{
	u32 clk;
	int ret;

	ret = ssv_field_read(sd, ADR_CLOCK_SELECTION, CLK_DIGI_SEL, &clk);
	if (ret)
		return ret;

	switch (clk) {
	case CLK_DIGI_80M:
		ssv_reg_set_bits(sd, ADR_MTX_TIME_FINETUNE,
				 MAC_CLK_80M | (26 << __ffs(PHYTXSTART_NCYCLE)),
				 MAC_CLK_80M | PHYTXSTART_NCYCLE);
		return ssv_field_write(sd, ADR_PRESCALER_USTIMER,
				       PRESCALER_US, 80);
	case CLK_DIGI_40M:
		ssv_reg_set_bits(sd, ADR_MTX_TIME_FINETUNE,
				 13 << __ffs(PHYTXSTART_NCYCLE),
				 MAC_CLK_80M | PHYTXSTART_NCYCLE);
		return ssv_field_write(sd, ADR_PRESCALER_USTIMER,
				       PRESCALER_US, 40);
	}

	dev_err(sd->dev, "invalid digital clock selection %u\n", clk);
	return -EINVAL;
}

static void ssv_set_macaddr(struct ssv_dev *sd, const u8 *addr)
{
	ssv_reg_write(sd, ADR_STA_MAC_0, get_unaligned_le32(addr));
	ssv_reg_write(sd, ADR_STA_MAC_1, get_unaligned_le16(addr + 4));
}

void ssv_set_bssid(struct ssv_dev *sd, const u8 *bssid)
{
	ssv_reg_write(sd, ADR_BSSID_0, get_unaligned_le32(bssid));
	ssv_reg_write(sd, ADR_BSSID_1, get_unaligned_le16(bssid + 4));
}

static int ssv_mac_init(struct ssv_dev *sd)
{
	static const u8 zero_bssid[ETH_ALEN] = {};
	u32 val;
	int ret, i;

	ret = ssv_mac_reset(sd);
	if (ret)
		return ret;
	ret = ssv_mac_clock(sd);
	if (ret)
		return ret;
	ret = ssv_write_table(sd, mac_ini_table, ARRAY_SIZE(mac_ini_table));
	if (ret)
		return ret;

	ssv_field_write(sd, ADR_MTX_BCN_EN_MISC, MTX_TSF_TIMER_EN, 1);

	/* where the 802.11 header starts, and how long each descriptor is */
	ssv_reg_write(sd, ADR_HCI_TX_RX_INFO_SIZE,
		      (PB_OFFSET_BYTES << __ffs(TX_PBOFFSET)) |
		      (TX_DESC_SIZE << __ffs(TX_INFO_SIZE)) |
		      (RX_DESC_SIZE << __ffs(RX_INFO_SIZE)) |
		      (RX_PINFO_PAD << __ffs(RX_LAST_PHY_SIZE)));

	/* the receive watchdog drops frames the host is slow to read */
	if (!ssv_reg_read(sd, ADR_MRX_WATCH_DOG, &val))
		ssv_reg_write(sd, ADR_MRX_WATCH_DOG, val & ~0xfU);

	ssv_reg_set_bits(sd, ADR_TRX_ID_THRESHOLD,
			 (ID_TX_THRESHOLD << __ffs(TX_ID_THOLD)) |
			 (ID_RX_THRESHOLD << __ffs(RX_ID_THOLD)),
			 TX_ID_THOLD | RX_ID_THOLD);
	ssv_reg_set_bits(sd, ADR_ID_LEN_THREADSHOLD1,
			 (PAGE_TX_THRESHOLD << __ffs(ID_TX_LEN_THOLD)) |
			 (PAGE_RX_THRESHOLD << __ffs(ID_RX_LEN_THOLD)),
			 ID_TX_LEN_THOLD | ID_RX_LEN_THOLD);
	ssv_reg_write(sd, ADR_TX_LIMIT_INTR, TX_LIMIT_INT_EN |
		      (TX_LOWTHRESHOLD_ID << __ffs(TX_COUNT_LIMIT)) |
		      (TX_LOWTHRESHOLD_PAGE << __ffs(TX_PAGE_LIMIT)));

	/* halt the firmware mailbox on error rather than let it spin */
	ssv_field_write(sd, ADR_MBOX_HALT_CFG, MB_ERR_AUTO_HALT_EN, 1);
	ssv_field_write(sd, ADR_MB_DBG_CFG1, MB_DBG_EN, 1);

	ssv_set_macaddr(sd, sd->mac);
	ssv_set_bssid(sd, zero_bssid);

	/* everything goes straight to the host: crypto is done in software */
	ssv_reg_write(sd, ADR_RX_FLOW_DATA, M_ENG_MACRX | (M_ENG_HWHCI << 4));
	ssv_reg_write(sd, ADR_RX_FLOW_MNG, M_ENG_MACRX | (M_ENG_HWHCI << 4));
	ssv_reg_write(sd, ADR_RX_FLOW_CTRL, M_ENG_MACRX | (M_ENG_HWHCI << 4));

	for (i = 0; i < DECI_TBL1_SIZE; i++)
		ssv_reg_write(sd, ADR_MRX_FLT_TB0 + i * 4, deci_tbl[i]);
	for (i = 0; i < DECI_TBL2_SIZE; i++)
		ssv_reg_write(sd, ADR_MRX_FLT_EN0 + i * 4,
			      deci_tbl[DECI_TBL1_SIZE + i]);

	ssv_reg_set_bits(sd, ADR_GLBLE_SET,
			 (OPMODE_STA << __ffs(OP_MODE)) | CCMP_H_SEL |
			 SEC_LUT_SEL, OP_MODE | CCMP_H_SEL | SEC_LUT_SEL);
	ssv_field_write(sd, ADR_MTX_RATERPT, MTX_RATERPT_HWID, M_ENG_HWHCI);
	ssv_reg_write(sd, ADR_AMPDU_SCOREBOAD_SIZE, MAX_RX_AGGR_SIZE);
	return 0;
}

/*
 * Full bring-up.  The radio comes first: the MAC takes its timing from
 * whatever clock the PLL settles on.  The firmware is loaded last, and
 * only then is the baseband allowed to receive.
 */
int ssv_hw_start(struct ssv_dev *sd)
{
	int ret;

	ssv_phy_enable(sd, false);
	ret = ssv_phy_init(sd);
	if (ret)
		return ret;
	ret = ssv_mac_init(sd);
	if (ret)
		return ret;
	ret = ssv_load_firmware(sd);
	if (ret)
		return ret;
	dev_info(sd->dev, "firmware running\n");

	ssv_phy_enable(sd, true);
	ssv_set_bandwidth(sd, false, false);
	return ssv_set_channel(sd, sd->channel);
}

/* Read what the driver needs before registering with mac80211. */
void ssv_hw_probe(struct ssv_dev *sd)
{
	ssv_read_efuse(sd);
	dev_info(sd->dev, "chip %s, MAC %pM\n", sd->chip_id, sd->mac);
}

int ssv_wsid_add(struct ssv_dev *sd, int wsid, const u8 *addr)
{
	int ret;

	ret = ssv_reg_write(sd, wsid_reg[wsid] + WSID_PEER_MAC0,
			    get_unaligned_le32(addr));
	ret = ret ?: ssv_reg_write(sd, wsid_reg[wsid] + WSID_PEER_MAC1,
				   get_unaligned_le16(addr + 4));
	return ret ?: ssv_reg_write(sd, wsid_reg[wsid], 1);
}

void ssv_wsid_del(struct ssv_dev *sd, int wsid)
{
	ssv_reg_write(sd, wsid_reg[wsid], 0);
}
