/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Register tables written verbatim at bring-up: MAC defaults, RF and
 * baseband settings for the Turismo C front end, and the MAC receive
 * decision table.
 *
 * Values from the vendor driver (ssv6006C_mac.c, turismoC_rf_reg.c,
 * turismoC_wifi_phy_reg.c, dev_tbl.h):
 * Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 * Copyright (c) 2015 iComm Corporation
 */
#ifndef SSV6256_TABLES_H
#define SSV6256_TABLES_H

#include <linux/bits.h>
#include <linux/types.h>

#include "reg.h"

struct ssv_reg {
	u32 addr;
	u32 data;
};

/* Reserved head room in front of every buffered frame, in 16-byte units. */
#define TX_PKT_RSVD_SETTING	3
/* Bytes of descriptor the hardware skips to reach the 802.11 header. */
#define PB_OFFSET_BYTES		80

static const struct ssv_reg mac_ini_table[] = {
	{ ADR_CONTROL,		0x12000006 },
	{ ADR_RX_TIME_STAMP_CFG, (28 << 8) | 0x01 },
	{ ADR_GLBLE_SET,	DUP_FLT |
				(TX_PKT_RSVD_SETTING << 18) |
				(PB_OFFSET_BYTES << 8) },
	{ ADR_TX_ETHER_TYPE_0,	0x00000000 },
	{ ADR_TX_ETHER_TYPE_1,	0x00000000 },
	{ ADR_RX_ETHER_TYPE_0,	0x00000000 },
	{ ADR_RX_ETHER_TYPE_1,	0x00000000 },
	{ ADR_REASON_TRAP0,	0x7fbc7f87 },
	{ ADR_REASON_TRAP1,	0x0000013f },
	{ ADR_TRAP_HW_ID,	M_ENG_CPU },
	{ ADR_WSID0,		0x00000000 },
	{ ADR_WSID1,		0x00000000 },
	{ ADR_WSID2,		0x00000000 },
	{ ADR_WSID3,		0x00000000 },
	{ ADR_WSID4,		0x00000000 },
	{ ADR_WSID5,		0x00000000 },
	{ ADR_WSID6,		0x00000000 },
	{ ADR_WSID7,		0x00000000 },
	{ ADR_MASK_TYPHOST_INT_MAP, 0xffff7fff },
	{ ADR_MASK_TYPHOST_INT_MAP_15, 0xff0fffff },
	{ ADR_MTX_RESPFRM_RATE_TABLE_01, 0x0000 },
	{ ADR_MTX_RESPFRM_RATE_TABLE_02, 0x0000 },
	{ ADR_MTX_RESPFRM_RATE_TABLE_03, 0x0002 },
	{ ADR_MTX_RESPFRM_RATE_TABLE_11, 0x0000 },
	{ ADR_MTX_RESPFRM_RATE_TABLE_12, 0x0000 },
	{ ADR_MTX_RESPFRM_RATE_TABLE_13, 0x0012 },
	{ ADR_MTX_RESPFRM_RATE_TABLE_92_B2, 0x9090 },
	{ ADR_MTX_RESPFRM_RATE_TABLE_94_B4, 0x9292 },
	{ ADR_MTX_RESPFRM_RATE_TABLE_C1_E1, 0x9090 },
	{ ADR_MTX_RESPFRM_RATE_TABLE_C3_E3, 0x9292 },
	{ ADR_MTX_RESPFRM_RATE_TABLE_D1_F1, 0x9090 },
	{ ADR_MTX_RESPFRM_RATE_TABLE_D3_F3, 0x9292 },
	{ ADR_BA_CTRL,		0x9 },
};

/* Turismo C RF front end, table version 20.00. */
static const struct ssv_reg rf_table[] = {
	{ 0xccb0a420, 0x0033e73f },
	{ 0xccb0a554, 0x03024444 },
	{ 0xccb0a594, 0x111e0950 },
	{ 0xccb0a598, 0x0f1e00ff },
	{ 0xccb0a530, 0x001f1f01 },
	{ 0xccb0a604, 0x001f1f01 },
	{ 0xccb0a62c, 0x9264924a },
	{ 0xccb0a630, 0x96dbb6cc },
	{ 0xccb0a634, 0x00000000 },
	{ 0xccb0a8cc, 0x141e157c },
	{ 0xccb0a8d0, 0x00001644 },
	{ 0xccb0a88c, 0x00000010 },
	{ 0xccb0a808, 0x88000000 },
	{ 0xccb0b000, 0x24844214 },
};

/* Turismo C baseband, table version 20.00. */
static const struct ssv_reg phy_table[] = {
	{ 0xccb0e010, 0x00000fff },
	{ 0xccb0e014, 0x00807f03 },
	{ 0xccb0e018, 0x0055003c },
	{ 0xccb0e01c, 0x00000064 },
	{ 0xccb0e020, 0x00000000 },
	{ 0xccb0e02c, 0x7004606c },
	{ 0xccb0e030, 0x7004606c },
	{ 0xccb0e034, 0x1a040400 },
	{ 0xccb0e038, 0x630f36d0 },
	{ 0xccb0e03c, 0x100c0003 },
	{ 0xccb0e040, 0x11600800 },
	{ 0xccb0e044, 0x00080868 },
	{ 0xccb0e048, 0xff001160 },
	{ 0xccb0e04c, 0x00100040 },
	{ 0xccb0e060, 0x11501150 },
	{ 0xccb0e12c, 0x00001160 },
	{ 0xccb0e130, 0x00100040 },
	{ 0xccb0e134, 0x00080010 },
	{ 0xccb0e180, 0x00010060 },
	{ 0xccb0e184, 0xb5a19080 },
	{ 0xccb0e188, 0xb5a19080 },
	{ 0xccb0e18c, 0xb5a19080 },
	{ 0xccb0e190, 0x00010006 },
	{ 0xccb0e194, 0x06060606 },
	{ 0xccb0e198, 0x06060606 },
	{ 0xccb0e19c, 0x06060606 },
	{ 0xccb0e080, 0x0110000f },
	{ 0xccb0e098, 0x00102000 },
	{ 0xccb0e09c, 0x00100018 },
	{ 0xccb0e4b4, 0x00002001 },
	{ 0xccb0eca4, 0x00009001 },
	{ 0xccb0ecb8, 0x000c50cc },
	{ 0xccb0fc44, 0x00028080 },
	{ 0xccb0f008, 0x00004775 },
	{ 0xccb0f00c, 0x10000075 },
	{ 0xccb0f010, 0x3f304905 },
	{ 0xccb0f014, 0x40182000 },
	{ 0xccb0f018, 0x20600000 },
	{ 0xccb0f01c, 0x0c010080 },
	{ 0xccb0f03c, 0x0000005a },
	{ 0xccb0f020, 0x20202020 },
	{ 0xccb0f024, 0x20000000 },
	{ 0xccb0f028, 0x50505050 },
	{ 0xccb0f02c, 0x20202020 },
	{ 0xccb0f030, 0x20000000 },
	{ 0xccb0f034, 0x00002424 },
	{ 0xccb0f09c, 0x000030a0 },
	{ 0xccb0f0c0, 0x0f0003c0 },
	{ 0xccb0f0c4, 0x30023003 },
	{ 0xccb0f0cc, 0x00000120 },
	{ 0xccb0f0d0, 0x00000020 },
	{ 0xccb0f130, 0x40000000 },
	{ 0xccb0f164, 0x000e0090 },
	{ 0xccb0f188, 0x82000000 },
	{ 0xccb0f190, 0x00000020 },
	{ 0xccb0f194, 0x09360001 },
	{ 0xccb0f3f8, 0x00100001 },
	{ 0xccb0f3fc, 0x00010425 },
	{ 0xccb0e804, 0x00020000 },
	{ 0xccb0e808, 0x20280060 },
	{ 0xccb0e80c, 0x00003467 },
	{ 0xccb0e810, 0x00430000 },
	{ 0xccb0e814, 0x30000015 },
	{ 0xccb0e818, 0x00390005 },
	{ 0xccb0e81c, 0x05050005 },
	{ 0xccb0e820, 0x00570057 },
	{ 0xccb0e824, 0x00570057 },
	{ 0xccb0e828, 0x00236700 },
	{ 0xccb0e82c, 0x000d1746 },
	{ 0xccb0e830, 0x05051787 },
	{ 0xccb0e834, 0x07800000 },
	{ 0xccb0e89c, 0x009000b0 },
	{ 0xccb0e8a0, 0x00000000 },
	{ 0xccb0ebf8, 0x00100000 },
	{ 0xccb0ebfc, 0x00000001 },
};

#define DECI(_type, _mask, _action, _drop) \
	((_type) << 9 | (_mask) << 3 | (_action) << 1 | (_drop))
#define DECI_NOP	0
#define DECI_NAV_UPD	1
#define DECI_NAV_RST	2
#define DECI_ACK	3

/* MAC RX filter: 16 decision entries followed by 9 enable masks. */
static const u16 deci_tbl[] = {
	DECI(0x1e, 0x3e, DECI_NAV_RST, 1),
	DECI(0x18, 0x3e, DECI_ACK, 0),
	DECI(0x1a, 0x3f, DECI_ACK, 1),
	DECI(0x10, 0x38, DECI_NOP, 1),
	DECI(0x25, 0x3f, DECI_NOP, 1),
	DECI(0x26, 0x36, DECI_NOP, 1),
	DECI(0x08, 0x3f, DECI_NOP, 0),
	DECI(0x05, 0x3f, DECI_ACK, 0),
	DECI(0x0b, 0x3f, DECI_ACK, 0),
	DECI(0x01, 0x3d, DECI_ACK, 0),
	DECI(0x20, 0x30, DECI_ACK, 0),
	DECI(0x00, 0x00, DECI_ACK, 0),
	DECI(0x00, 0x00, DECI_NOP, 1),
	DECI(0x00, 0x00, DECI_NAV_UPD, 1),
	DECI(0x00, 0x00, DECI_NAV_RST, 1),
	DECI(0x00, 0x00, DECI_ACK, 1),
	0x2008, 0x1001, 0x0808, 0x1040, 0x2008, 0x800e, 0x0bb8, 0x2b88, 0x0800,
};

#define DECI_TBL1_SIZE	16
#define DECI_TBL2_SIZE	9

#endif
