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
#include <net/mac80211.h>

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
#define SSV_BUS_CLOCK_MAX	50000000U
#define SDIO_TX_ALLOC_SHIFT	0x07
#define SDIO_TX_ALLOC_ENABLE	0x10

/* Interrupt status bit set when a buffer is waiting to be read. */
#define SSV_INT_RX		BIT(0)

#define IO_BUF_SIZE		16

/* Bounce buffer for one aggregate plus its descriptor. */
#define SSV_TX_BUF_SIZE		16384
/* Largest frame the chip hands back, including descriptor and padding. */
#define SSV_RX_BUF_SIZE		4096

/* Channel width, and which side the secondary channel is on. */
enum ssv_bandwidth {
	SSV_BW_20,
	SSV_BW_40_ABOVE,
	SSV_BW_40_BELOW,
};

/*
 * Hardware transmit queues: one per access category plus one for
 * management frames, which is also where group frames waiting for the
 * DTIM beacon go.
 */
#define SSV_HW_TXQ_NUM		5
#define SSV_HW_TXQ_MGMT		4

/* Packet engines, as used by the receive flow and trap registers. */
#define M_ENG_CPU		0x00
#define M_ENG_HWHCI		0x01
#define M_ENG_MACRX		0x04
#define M_ENG_TX_EDCA0		0x06
#define M_ENG_ENCRYPT_SEC	0x0b
#define M_ENG_MIC_SEC		0x0c
#define M_ENG_TRASH_CAN		0x0f

/* One entry of a register table written verbatim at bring-up. */
struct ssv_reg {
	u32 addr;
	u32 data;
};

/* Frame types in the c_type field of every descriptor word 0 */
#define SSV_CTYPE_TXREQ		2
#define SSV_CTYPE_HOST_CMD	4
#define SSV_CTYPE_HOST_EVENT	6
#define SSV_CTYPE_RATE_RPT	7

/* Host commands understood by the firmware */
enum ssv_host_cmd {
	SSV_CMD_LOG = 1,
	SSV_CMD_PS = 2,
	SSV_CMD_WSID_OP = 7,
};

/*
 * Rate code, one byte, used both in the TX descriptor and in the PHY
 * information of a received frame.
 */
#define RATE_INDEX		GENMASK(2, 0)
#define RATE_GREENFIELD		BIT(3)
#define RATE_SHORT		BIT(4)	/* short preamble, or short GI in HT */
#define RATE_HT40		BIT(5)
#define RATE_PHY_MODE		GENMASK(7, 6)
#define  RATE_PHY_CCK		0
#define  RATE_PHY_OFDM		2
#define  RATE_PHY_HT		3

/*
 * Wire formats: little-endian 32-bit words.  Field masks are named
 * <struct><word>_<field>.
 */
/* TX descriptor, TX_DESC_SIZE bytes in front of every frame */
#define TXD0_LEN		GENMASK(15, 0)
#define TXD0_C_TYPE		GENMASK(18, 16)
#define TXD0_F80211		BIT(19)
#define TXD0_QOS		BIT(20)
#define TXD0_HT			BIT(21)
#define TXD0_USE_4ADDR		BIT(22)
#define TXD0_SECURITY		BIT(27)
#define TXD0_MORE_DATA		BIT(28)
#define TXD0_STYPE_B5B4		GENMASK(30, 29)
#define TXD2_HDR_OFFSET		GENMASK(7, 0)
#define TXD2_FRAG		BIT(8)
#define TXD2_UNICAST		BIT(9)
#define TXD2_HDR_LEN		GENMASK(15, 10)
#define TXD2_AGGR		GENMASK(21, 20)
#define TXD2_BSSIDX		GENMASK(25, 24)
#define TXD3_PKT_RUN_NO		GENMASK(15, 8)
#define TXD3_WSID		GENMASK(22, 19)
#define TXD3_TXQ_IDX		GENMASK(25, 23)
#define TXD5_RATE_RPT_MODE	GENMASK(19, 18)
#define  RATE_RPT_ON		1
#define  RATE_RPT_OFF		2

/* One of the four rate series, two words each */
#define TXR0_DRATE		GENMASK(7, 0)
#define TXR0_CRATE		GENMASK(15, 8)
#define TXR0_RTS_CTS_NAV	GENMASK(31, 16)
#define TXR1_DL_LENGTH		GENMASK(11, 0)
#define TXR1_TRY_CNT		GENMASK(15, 12)
#define TXR1_ACK_POLICY		GENMASK(17, 16)
#define TXR1_DO_RTS_CTS		GENMASK(19, 18)
#define TXR1_IS_LAST_RATE	BIT(20)
#define TXR1_RPT_RESULT		GENMASK(23, 22)
#define TXR1_RPT_TRYCNT		GENMASK(27, 24)

#define SSV_TX_MAX_RATES	4

struct ssv_tx_rate {
	__le32 w0;
	__le32 w1;
};

struct ssv_tx_desc {
	__le32 w0;
	__le32 fcmd;		/* packet engine route */
	__le32 w2;
	__le32 w3;
	__le32 nav12;		/* NAV of rate series 1 and 2 */
	__le32 w5;		/* NAV of series 3, report mode, AMPDU SSN */
	struct ssv_tx_rate rate[SSV_TX_MAX_RATES];
	__le32 ampdu[3];
	__le32 dummy[3];
};

/* RX descriptor and PHY information in front of every received frame */
#define RXD0_LEN		GENMASK(15, 0)
#define RXD0_C_TYPE		GENMASK(18, 16)
#define RXD2_RX_RESULT		GENMASK(23, 16)
#define RXD3_PKT_RUN_NO		GENMASK(15, 8)
#define RXD3_WSID		GENMASK(22, 19)

struct ssv_rx_desc {
	__le32 w0;
	__le32 w1;
	__le32 w2;
	__le32 w3;
};

#define RXPHY0_RATE		GENMASK(23, 16)
#define RXPHY0_AGGREGATE	BIT(26)
#define RXPHY1_RSSI		GENMASK(23, 16)
#define RXPHY1_SNR		GENMASK(31, 24)

struct ssv_rxphy_info {
	__le32 w0;
	__le32 w1;
	__le32 w2;
	__le32 timestamp;
};

static_assert(sizeof(struct ssv_tx_desc) == 80);
static_assert(sizeof(struct ssv_rx_desc) + sizeof(struct ssv_rxphy_info) == 32);

#define SSV_TX_DESC_LEN		sizeof(struct ssv_tx_desc)
#define SSV_RX_DESC_LEN		(sizeof(struct ssv_rx_desc) + \
				 sizeof(struct ssv_rxphy_info))
/* The PHY appends four more bytes after the frame. */
#define SSV_RX_PINFO_PAD	4

/* Host command header, and the payload of a WSID operation */
#define HDR0_LEN		GENMASK(15, 0)
#define HDR0_C_TYPE		GENMASK(18, 16)
#define HDR0_CMD		GENMASK(31, 24)

struct ssv_host_hdr {
	__le32 w0;
	__le32 seq;
	u8 data[];
};

/* Hardware station table: one entry per peer. */
#define SSV_NUM_STA		8
/* Frames that may be waiting for the chip's transmit report. */
#define SSV_STATUS_SLOTS	32
/* How long to wait for a report before giving up on a frame. */
#define SSV_STATUS_TIMEOUT	(HZ / 2)

/* Aggregation state, one per traffic identifier of a station. */
#define SSV_AGG_TIDS		8
#define SSV_AGG_WINDOW		64
/* Run numbers: 0..31 identify single frames, 64..127 whole aggregates. */
#define SSV_AGG_IDS		64
#define SSV_AGG_RUN_NO(id)	((id) | SSV_AGG_IDS)
#define SSV_AGG_ID(run)		((run) & (SSV_AGG_IDS - 1))
static inline bool ssv_is_agg_run_no(u8 run)
{
	return run >= SSV_AGG_IDS && run < 2 * SSV_AGG_IDS;
}

enum ssv_agg_state {
	SSV_AGG_OFF,
	SSV_AGG_STARTING,
	SSV_AGG_OPERATIONAL,
};

struct ssv_agg {
	struct sk_buff_head q;		/* waiting to be aggregated */
	struct sk_buff_head retry;	/* unacknowledged, to send again */
	struct sk_buff_head inflight;	/* handed to the chip */
	unsigned long retry_start;	/* when to ask for a session again */
	u16 buf_size;			/* window the peer granted */
	u8 tries[SSV_AGG_WINDOW];
	u8 state;
};

struct ssv_sta {
	int wsid;
	struct ssv_agg agg[SSV_AGG_TIDS];
};

struct ssv_dev {
	struct sdio_func *func;
	struct device *dev;
	struct ieee80211_hw *hw;
	struct ieee80211_vif *vif;
	struct ieee80211_supported_band band;
	struct ieee80211_supported_band band5;

	struct mutex mutex;	/* serialises chip access outside the RX path */
	bool started;

	/* SDIO */
	u32 bus_clock;		/* negotiated by the MMC core */
	u32 data_port;
	u32 reg_port;
	u8 *io_buf;		/* DMA-safe scratch, used under the SDIO host lock */

	char chip_id[20];
	bool dual_band;		/* the part also covers 5 GHz */
	u8 mac[ETH_ALEN];
	int channel;
	enum ssv_bandwidth bw;
	bool short_preamble;

	struct ieee80211_sta __rcu *sta[SSV_NUM_STA];
	spinlock_t sta_lock;	/* protects the aggregation queues */
	u8 agg_next_id;		/* run number of the next aggregate */
	struct mutex agg_mutex;	/* serialises aggregate building and sending */

	/* receive: how many interrupts arrived with nothing behind them */
	struct delayed_work rx_unmask_work;
	unsigned int rx_empty;

	/* transmit: one queue per hardware queue, drained by a thread */
	struct sk_buff_head txq[SSV_HW_TXQ_NUM];
	wait_queue_head_t tx_wait;
	struct task_struct *tx_thread;
	u8 *tx_buf;		/* DMA-safe, used only by the TX thread */

	/* frames handed to the chip, waiting for their transmit report */
	spinlock_t status_lock;	/* protects status[] and status_next */
	struct sk_buff *status[SSV_STATUS_SLOTS];
	u8 status_next;
	unsigned long status_at[SSV_STATUS_SLOTS];
	unsigned long status_sweep;

	/* access point mode */
	struct work_struct beacon_work;
	struct delayed_work dtim_work;
	u32 bcn_buf[2];
	size_t bcn_len[2];
	u8 *bcn_last;
	size_t bcn_last_len;
	bool dtim_bit;
};

/* sdio.c */
int ssv_reg_read(struct ssv_dev *sd, u32 addr, u32 *val);
int ssv_reg_write(struct ssv_dev *sd, u32 addr, u32 val);
int ssv_reg_set_bits(struct ssv_dev *sd, u32 addr, u32 set, u32 mask);
int ssv_write_data(struct ssv_dev *sd, const u8 *buf, size_t len);
int ssv_load_firmware(struct ssv_dev *sd);
int ssv_irq_mask(struct ssv_dev *sd, u8 mask);
void ssv_set_bus_clock(struct ssv_dev *sd, u32 hz);
int ssv_irq_enable(struct ssv_dev *sd);
void ssv_irq_disable(struct ssv_dev *sd);

/* hw.c */
int ssv_wsid_add(struct ssv_dev *sd, int wsid, const u8 *addr);
void ssv_wsid_del(struct ssv_dev *sd, int wsid);
int ssv_write_table(struct ssv_dev *sd, const struct ssv_reg *t, size_t n);
int ssv_hw_start(struct ssv_dev *sd);
void ssv_hw_probe(struct ssv_dev *sd);
void ssv_set_bssid(struct ssv_dev *sd, const u8 *bssid);
void ssv_set_ap_mode(struct ssv_dev *sd, bool ap);
u32 ssv_pbuf_alloc(struct ssv_dev *sd, size_t size, u32 type);
void ssv_pbuf_free(struct ssv_dev *sd, u32 addr);
void ssv_beacon_timing(struct ssv_dev *sd, u16 interval, u8 dtim_period);
int ssv_beacon_enable(struct ssv_dev *sd, bool enable);
int ssv_beacon_set(struct ssv_dev *sd, const u8 *buf, size_t len,
		   u16 dtim_offset);
void ssv_beacon_release(struct ssv_dev *sd);

/* mac.c */
struct ssv_dev *ssv_mac_alloc(struct device *dev);
void ssv_mac_free(struct ssv_dev *sd);
int ssv_mac_register(struct ssv_dev *sd);
void ssv_mac_unregister(struct ssv_dev *sd);

/* tx.c */
void ssv_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
	    struct sk_buff *skb);
void ssv_tx_status(struct ssv_dev *sd, struct sk_buff *skb);
void ssv_tx_kick(struct ssv_dev *sd);
bool ssv_tx_queued(struct ssv_dev *sd);
int ssv_tid_to_hwq(u8 tid);
int ssv_ac_to_hwq(u16 ac);
u8 ssv_rate_code(struct ssv_dev *sd, const struct ieee80211_tx_rate *r,
		 enum nl80211_band band);
u32 ssv_fill_rate(struct ssv_tx_rate *tr, u8 code, u8 tries, u32 len,
		  bool unicast, bool rts, bool last);
void ssv_tx_flush(struct ssv_dev *sd);
int ssv_tx_init(struct ssv_dev *sd);
void ssv_tx_deinit(struct ssv_dev *sd);

/* rx.c */
void ssv_rx_init(struct ssv_dev *sd);
void ssv_rx_irq(struct ssv_dev *sd);

/* ap.c */
bool ssv_is_ap(struct ssv_dev *sd);
void ssv_ap_init(struct ssv_dev *sd);
void ssv_ap_update_beacon(struct ssv_dev *sd);
void ssv_ap_group_queued(struct ssv_dev *sd);
void ssv_ap_stop(struct ssv_dev *sd);

/* ampdu.c */
void ssv_agg_init(struct ssv_sta *ss);
void ssv_agg_flush(struct ssv_dev *sd, struct ssv_sta *ss, u8 tid);
void ssv_agg_flush_all(struct ssv_dev *sd);
bool ssv_agg_tx(struct ssv_dev *sd, struct ieee80211_sta *sta,
		struct sk_buff *skb);
bool ssv_agg_pump(struct ssv_dev *sd);
void ssv_agg_ba(struct ssv_dev *sd, struct sk_buff *skb, u8 run_no);
void ssv_agg_failed(struct ssv_dev *sd, u8 run_no);
int ssv_agg_action(struct ssv_dev *sd, struct ieee80211_vif *vif,
		   struct ieee80211_ampdu_params *params);

/* phy.c */
int ssv_phy_init(struct ssv_dev *sd);
int ssv_phy_enable(struct ssv_dev *sd, bool enable);
int ssv_set_channel(struct ssv_dev *sd, int channel, enum ssv_bandwidth bw);
int ssv_set_bandwidth(struct ssv_dev *sd, enum ssv_bandwidth bw);

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
