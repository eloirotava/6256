// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SSV6256 transmit path.
 *
 * Frames are queued per hardware queue and written to the chip by a
 * thread, because SDIO transfers sleep.  Each unicast frame asks the
 * chip for a report, which comes back through the receive path carrying
 * the descriptor with the result of every rate series it tried; that is
 * what mac80211 gets as the transmit status.
 */
#include <linux/kthread.h>
#include <linux/unaligned.h>

#include "ssv6256.h"

static const u8 ac_to_hwq[IEEE80211_NUM_ACS] = { 3, 2, 1, 0 };

static void ssv_send_one(struct ssv_dev *sd, struct sk_buff *skb);
static bool ssv_build_desc(struct ssv_dev *sd, struct sk_buff *skb,
			   struct ieee80211_sta *sta, int hwq);

int ssv_ac_to_hwq(u16 ac)
{
	return ac_to_hwq[ac & 3];
}

/* TID to access category, as in the WMM tables. */
int ssv_tid_to_hwq(u8 tid)
{
	static const u8 tid_to_ac[8] = {
		IEEE80211_AC_BE, IEEE80211_AC_BK, IEEE80211_AC_BK,
		IEEE80211_AC_BE, IEEE80211_AC_VI, IEEE80211_AC_VI,
		IEEE80211_AC_VO, IEEE80211_AC_VO,
	};

	return ac_to_hwq[tid_to_ac[tid & 7]];
}

void ssv_tx_kick(struct ssv_dev *sd)
{
	wake_up(&sd->tx_wait);
}

/* Airtime constants, microseconds */
#define CCK_SIFS		10
#define CCK_PREAMBLE_BITS	144
#define CCK_PLCP_BITS		48
#define OFDM_SIFS		16
#define OFDM_PREAMBLE		20
#define OFDM_PLCP_BITS		22
#define OFDM_SYMBOL		4
#define HT_SIFS			10
#define HT_PREAMBLE		(8 + 8 + 4 + 8 + 4 + 4 + 6)	/* L-STF..HT-LTF + ext */
#define ACK_LEN			14
#define RTS_LEN			20
#define CTS_LEN			14
#define FCS_LEN			4

static const u16 cck_kbps[4] = { 1000, 2000, 5500, 11000 };
static const u16 ofdm_kbps[8] = {
	6000, 9000, 12000, 18000, 24000, 36000, 48000, 54000,
};

static const u16 ht_bits_per_symbol[2][8] = {
	{ 26, 52, 78, 104, 156, 208, 234, 260 },	/* 20 MHz */
	{ 54, 108, 162, 216, 324, 432, 486, 540 },	/* 40 MHz */
};

static u32 ssv_rate_kbps(u8 code)
{
	u8 idx = FIELD_GET(RATE_INDEX, code);

	if (FIELD_GET(RATE_PHY_MODE, code) == RATE_PHY_CCK)
		return cck_kbps[idx & 3];
	return ofdm_kbps[idx];
}

static u32 ssv_legacy_airtime(u8 code, u32 len)
{
	u32 kbps = ssv_rate_kbps(code);
	u32 bits = len * 8;

	if (FIELD_GET(RATE_PHY_MODE, code) == RATE_PHY_CCK) {
		u32 pre = CCK_PREAMBLE_BITS + CCK_PLCP_BITS;

		if (code & RATE_SHORT)
			pre >>= 1;
		return CCK_SIFS + pre + bits * 1000 / kbps;
	}
	return OFDM_SIFS + OFDM_PREAMBLE +
	       DIV_ROUND_UP(OFDM_PLCP_BITS + bits, kbps * OFDM_SYMBOL / 1000) *
	       OFDM_SYMBOL;
}

static u32 ssv_ht_airtime(u8 code, u32 len)
{
	bool ht40 = code & RATE_HT40;
	u32 nsym = DIV_ROUND_UP(len * 8 + OFDM_PLCP_BITS,
				ht_bits_per_symbol[ht40][FIELD_GET(RATE_INDEX, code)]);
	u32 t = (code & RATE_SHORT) ? DIV_ROUND_UP((nsym * 18 + 4) / 5, 4) << 2 :
				      nsym << 2;

	return t + HT_PREAMBLE + HT_SIFS;
}

static u32 ssv_airtime(u8 code, u32 len)
{
	if (FIELD_GET(RATE_PHY_MODE, code) == RATE_PHY_HT)
		return ssv_ht_airtime(code, len);
	return ssv_legacy_airtime(code, len);
}

/* Rate the peer answers on: the vendor's fixed mapping of data rates. */
static u8 ssv_ctrl_rate(u8 code)
{
	u8 idx = FIELD_GET(RATE_INDEX, code);
	u8 ofdm;

	switch (FIELD_GET(RATE_PHY_MODE, code)) {
	case RATE_PHY_CCK:
		return FIELD_PREP(RATE_PHY_MODE, RATE_PHY_CCK) |
		       (code & RATE_SHORT);
	case RATE_PHY_OFDM:
		ofdm = idx <= 2 ? 0 : (idx <= 4 ? 2 : 4);
		break;
	default:
		ofdm = idx <= 1 ? 0 : (idx <= 3 ? 2 : 4);
		break;
	}
	return FIELD_PREP(RATE_PHY_MODE, RATE_PHY_OFDM) |
	       FIELD_PREP(RATE_INDEX, ofdm);
}

/* Translate one mac80211 rate entry into the chip's rate byte. */
u8 ssv_rate_code(struct ssv_dev *sd, const struct ieee80211_tx_rate *r,
		 enum nl80211_band band)
{
	u8 code;

	if (r->flags & IEEE80211_TX_RC_MCS) {
		code = FIELD_PREP(RATE_PHY_MODE, RATE_PHY_HT) |
		       FIELD_PREP(RATE_INDEX, r->idx & 7);
		if (r->flags & IEEE80211_TX_RC_SHORT_GI)
			code |= RATE_SHORT;
		if (r->flags & IEEE80211_TX_RC_40_MHZ_WIDTH)
			code |= RATE_HT40;
		if (r->flags & IEEE80211_TX_RC_GREEN_FIELD)
			code |= RATE_GREENFIELD;
		return code;
	}

	/* only the 2.4 GHz band lists the four CCK rates first */
	if (band == NL80211_BAND_2GHZ) {
		if (r->idx < 4) {
			code = FIELD_PREP(RATE_PHY_MODE, RATE_PHY_CCK) |
			       FIELD_PREP(RATE_INDEX, r->idx);
			if (r->idx && sd->short_preamble)
				code |= RATE_SHORT;
			return code;
		}
		return FIELD_PREP(RATE_PHY_MODE, RATE_PHY_OFDM) |
		       FIELD_PREP(RATE_INDEX, r->idx - 4);
	}
	return FIELD_PREP(RATE_PHY_MODE, RATE_PHY_OFDM) |
	       FIELD_PREP(RATE_INDEX, r->idx);
}

/*
 * Fill one rate series.  Returns the ACK duration, which the first
 * series lends to the 802.11 Duration/ID field.
 */
u32 ssv_fill_rate(struct ssv_tx_rate *tr, u8 code, u8 tries, u32 len,
		  bool unicast, bool rts, bool last)
{
	u8 ctrl = ssv_ctrl_rate(code);
	u32 frame = ssv_airtime(code, len);
	u32 ack = 0, nav = 0, dl_length = 0;

	if (unicast)
		ack = ssv_legacy_airtime(ctrl, ACK_LEN);
	if (rts)
		nav = frame + ack + ssv_legacy_airtime(ctrl, CTS_LEN);
	if (FIELD_GET(RATE_PHY_MODE, code) == RATE_PHY_HT) {
		/* legacy L-SIG length spoofing the HT PPDU duration */
		u32 l = ((frame - HT_SIFS - (6 + 20)) + 3) >> 2;

		dl_length = l + (l << 1) - 3;
	}

	tr->w0 = cpu_to_le32(FIELD_PREP(TXR0_DRATE, code) |
			     FIELD_PREP(TXR0_CRATE, ctrl) |
			     FIELD_PREP(TXR0_RTS_CTS_NAV, nav));
	tr->w1 = cpu_to_le32(FIELD_PREP(TXR1_DL_LENGTH, dl_length) |
			     FIELD_PREP(TXR1_TRY_CNT, tries) |
			     FIELD_PREP(TXR1_ACK_POLICY, unicast ? 0 : 1) |
			     FIELD_PREP(TXR1_DO_RTS_CTS,
					rts ? IEEE80211_TX_RC_USE_RTS_CTS : 0) |
			     (last ? TXR1_IS_LAST_RATE : 0));
	return ack;
}

/* Park a frame until its report comes back; -1 when no slot is free. */
static int ssv_status_put(struct ssv_dev *sd, struct sk_buff *skb)
{
	unsigned long flags;
	int i, slot = -1;

	spin_lock_irqsave(&sd->status_lock, flags);
	for (i = 0; i < SSV_STATUS_SLOTS; i++) {
		u8 n = (sd->status_next + i) % SSV_STATUS_SLOTS;

		if (!sd->status[n]) {
			sd->status[n] = skb;
			sd->status_at[n] = jiffies;
			sd->status_next = (n + 1) % SSV_STATUS_SLOTS;
			slot = n;
			break;
		}
	}
	spin_unlock_irqrestore(&sd->status_lock, flags);
	return slot;
}

static struct sk_buff *ssv_status_take(struct ssv_dev *sd, u8 slot)
{
	struct sk_buff *skb = NULL;
	unsigned long flags;

	if (slot >= SSV_STATUS_SLOTS)
		return NULL;
	spin_lock_irqsave(&sd->status_lock, flags);
	skb = sd->status[slot];
	sd->status[slot] = NULL;
	spin_unlock_irqrestore(&sd->status_lock, flags);
	return skb;
}

static void ssv_tx_done(struct ssv_dev *sd, struct sk_buff *skb, bool acked,
			int tries)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_tx_rate rate = info->control.rates[0];

	skb_pull(skb, SSV_TX_DESC_LEN);
	ieee80211_tx_info_clear_status(info);
	/* rate control needs to know which rate was used, and how often */
	info->status.rates[0] = rate;
	info->status.rates[0].count = max(tries, 1);
	if (acked && !(info->flags & IEEE80211_TX_CTL_NO_ACK))
		info->flags |= IEEE80211_TX_STAT_ACK;
	ieee80211_tx_status_ni(sd->hw, skb);
}

/*
 * The chip returns the descriptor it was given, with the result of each
 * rate series filled in: result 0 means the series failed.
 */
void ssv_tx_status(struct ssv_dev *sd, struct sk_buff *rpt)
{
	struct ssv_tx_desc *d = (struct ssv_tx_desc *)rpt->data;
	struct sk_buff *skb;
	bool acked = false;
	int tries = 0, i;
	u8 slot;

	if (rpt->len < SSV_TX_DESC_LEN)
		return;
	slot = le32_get_bits(d->w3, TXD3_PKT_RUN_NO);

	for (i = 0; i < SSV_TX_MAX_RATES; i++) {
		u32 w1 = le32_to_cpu(d->rate[i].w1);

		tries += FIELD_GET(TXR1_RPT_TRYCNT, w1);
		if (FIELD_GET(TXR1_RPT_RESULT, w1))
			acked = true;
		if (w1 & TXR1_IS_LAST_RATE)
			break;
	}

	/*
	 * An aggregate is settled by its Block Ack: the report the chip
	 * returns for one says nothing useful about whether the peer got
	 * the frames.
	 */
	if (ssv_is_agg_run_no(slot))
		return;

	skb = ssv_status_take(sd, slot);
	if (!skb)
		return;
	ssv_tx_done(sd, skb, acked, tries);
}

static bool ssv_build_desc(struct ssv_dev *sd, struct sk_buff *skb,
			   struct ieee80211_sta *sta, int hwq)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	struct ssv_sta *ss = NULL;
	struct ssv_tx_desc *d;
	bool unicast, qos, ht = false, rts;
	u32 len, ack = 0, tmp;
	int hdrlen, i, slot = -1;
	u8 wsid = 0xf;

	if (skb_headroom(skb) < SSV_TX_DESC_LEN)
		return false;

	if (sta) {
		ss = (struct ssv_sta *)sta->drv_priv;
		if (ss->wsid >= 0)
			wsid = ss->wsid;
	}
	hdrlen = ieee80211_hdrlen(hdr->frame_control);
	unicast = !is_multicast_ether_addr(hdr->addr1);
	qos = ieee80211_is_data_qos(hdr->frame_control);
	rts = info->control.rates[0].flags & IEEE80211_TX_RC_USE_RTS_CTS;
	len = skb->len + SSV_TX_DESC_LEN;

	d = skb_push(skb, SSV_TX_DESC_LEN);
	memset(d, 0, SSV_TX_DESC_LEN);

	for (i = 0; i < SSV_TX_MAX_RATES; i++) {
		const struct ieee80211_tx_rate *r = &info->control.rates[i];
		bool last = i == SSV_TX_MAX_RATES - 1 || r->count == 0 ||
			    info->control.rates[i + 1].idx < 0;
		u8 code;

		if (r->idx < 0) {
			if (i == 0) {
				/* no rate control yet: slowest rate of the band */
				ack = ssv_fill_rate(&d->rate[0],
						    info->band == NL80211_BAND_2GHZ ?
						    0 : FIELD_PREP(RATE_PHY_MODE,
								   RATE_PHY_OFDM),
						    15,
						    skb->len - SSV_TX_DESC_LEN +
						    FCS_LEN, unicast, false,
						    true);
			}
			break;
		}
		code = ssv_rate_code(sd, r, info->band);
		if (i == 0 && FIELD_GET(RATE_PHY_MODE, code) == RATE_PHY_HT)
			ht = true;
		tmp = ssv_fill_rate(&d->rate[i], code, max_t(u8, r->count, 1),
				    skb->len - SSV_TX_DESC_LEN + FCS_LEN,
				    unicast, rts, last);
		if (i == 0)
			ack = tmp;
		if (last)
			break;
	}

	if (unicast)
		slot = ssv_status_put(sd, skb);

	d->w0 = cpu_to_le32(FIELD_PREP(TXD0_LEN, len) |
			    FIELD_PREP(TXD0_C_TYPE, SSV_CTYPE_TXREQ) |
			    TXD0_F80211 |
			    (qos ? TXD0_QOS : 0) |
			    (ht ? TXD0_HT : 0) |
			    (ieee80211_has_a4(hdr->frame_control) ?
			     TXD0_USE_4ADDR : 0) |
			    (ieee80211_has_morefrags(hdr->frame_control) ?
			     TXD0_MORE_DATA : 0) |
			    FIELD_PREP(TXD0_STYPE_B5B4,
				       (le16_to_cpu(hdr->frame_control) >> 4) & 3));
	/* the frame leaves through one EDCA queue and comes back to the host */
	d->fcmd = cpu_to_le32(((hwq + M_ENG_TX_EDCA0) << 4) | M_ENG_HWHCI);
	d->w2 = cpu_to_le32(FIELD_PREP(TXD2_HDR_OFFSET, SSV_TX_DESC_LEN) |
			    (ieee80211_has_morefrags(hdr->frame_control) ||
			     (hdr->seq_ctrl & cpu_to_le16(IEEE80211_SCTL_FRAG)) ?
			     TXD2_FRAG : 0) |
			    (unicast ? TXD2_UNICAST : 0) |
			    FIELD_PREP(TXD2_HDR_LEN, hdrlen));
	d->w3 = cpu_to_le32(FIELD_PREP(TXD3_PKT_RUN_NO, slot < 0 ? 0xff : slot) |
			    FIELD_PREP(TXD3_WSID, wsid) |
			    FIELD_PREP(TXD3_TXQ_IDX, hwq));
	d->w5 = cpu_to_le32(FIELD_PREP(TXD5_RATE_RPT_MODE,
				       slot < 0 ? RATE_RPT_OFF : RATE_RPT_ON));

	if (unicast && !ieee80211_is_ctl(hdr->frame_control))
		hdr->duration_id = cpu_to_le16(ack);
	return true;
}

/*
 * Send one frame the aggregation path decided not to aggregate: a lone
 * MPDU inside an A-MPDU would be answered with a plain acknowledgement
 * rather than a Block Ack, which nothing here would be waiting for.
 */
void ssv_tx_single(struct ssv_dev *sd, struct sk_buff *skb,
		   struct ieee80211_sta *sta, int hwq)
{
	if (!ssv_build_desc(sd, skb, sta, hwq)) {
		ieee80211_free_txskb(sd->hw, skb);
		return;
	}
	ssv_send_one(sd, skb);
}

static void ssv_send_one(struct ssv_dev *sd, struct sk_buff *skb)
{
	struct ssv_tx_desc *d = (struct ssv_tx_desc *)skb->data;
	u8 slot = le32_get_bits(d->w3, TXD3_PKT_RUN_NO);
	size_t aligned = sdio_align_size(sd->func, skb->len);
	int ret;

	if (aligned > SSV_TX_BUF_SIZE) {
		ret = -EMSGSIZE;
	} else {
		/* bounce: DMA-safe, zero-padded, and the frame may be paged */
		skb_copy_bits(skb, 0, sd->tx_buf, skb->len);
		memset(sd->tx_buf + skb->len, 0, aligned - skb->len);
		ret = ssv_write_data(sd, sd->tx_buf, skb->len);
	}

	if (!ret && slot < SSV_STATUS_SLOTS)
		return;		/* the report will complete it */

	/* no report is coming: tell mac80211 now */
	skb = ssv_status_take(sd, slot) ?: skb;
	ssv_tx_done(sd, skb, !ret, 1);
}

/* Management first, then the access categories in priority order. */
static struct sk_buff *ssv_tx_next(struct ssv_dev *sd)
{
	struct sk_buff *skb;
	int q;

	skb = skb_dequeue(&sd->txq[SSV_HW_TXQ_MGMT]);
	for (q = 0; !skb && q < SSV_HW_TXQ_MGMT; q++)
		skb = skb_dequeue(&sd->txq[q]);
	return skb;
}

/* Frames the driver still holds, in either path. */
bool ssv_tx_queued(struct ssv_dev *sd)
{
	int q, w, t;

	for (q = 0; q < SSV_HW_TXQ_NUM; q++)
		if (!skb_queue_empty(&sd->txq[q]))
			return true;
	for (w = 0; w < SSV_NUM_STA; w++) {
		struct ieee80211_sta *sta;
		struct ssv_sta *ss;

		rcu_read_lock();
		sta = rcu_dereference(sd->sta[w]);
		ss = sta ? (struct ssv_sta *)sta->drv_priv : NULL;
		for (t = 0; ss && t < SSV_AGG_TIDS; t++) {
			if (!skb_queue_empty(&ss->agg[t].q) ||
			    !skb_queue_empty(&ss->agg[t].retry)) {
				rcu_read_unlock();
				return true;
			}
		}
		rcu_read_unlock();
	}
	return false;
}

static bool ssv_tx_pending(struct ssv_dev *sd)
{
	int q;

	for (q = 0; q < SSV_HW_TXQ_NUM; q++)
		if (!skb_queue_empty(&sd->txq[q]))
			return true;
	return false;
}

/*
 * A report can go missing, for instance when the chip gives up on a
 * frame it never managed to send.  Hand those frames back to mac80211
 * rather than sit on them: otherwise the slots run out and, worse, the
 * buffers are never freed.
 */
static void ssv_tx_expire(struct ssv_dev *sd)
{
	struct sk_buff *old[SSV_STATUS_SLOTS];
	unsigned long flags;
	int i, n = 0;

	if (time_before(jiffies, sd->status_sweep))
		return;
	sd->status_sweep = jiffies + SSV_STATUS_TIMEOUT;

	spin_lock_irqsave(&sd->status_lock, flags);
	for (i = 0; i < SSV_STATUS_SLOTS; i++) {
		if (!sd->status[i])
			continue;
		if (time_before(sd->status_at[i], jiffies - SSV_STATUS_TIMEOUT)) {
			old[n++] = sd->status[i];
			sd->status[i] = NULL;
		}
	}
	spin_unlock_irqrestore(&sd->status_lock, flags);

	for (i = 0; i < n; i++)
		ssv_tx_done(sd, old[i], false, 1);
}

static int ssv_tx_thread(void *data)
{
	struct ssv_dev *sd = data;

	while (!kthread_should_stop()) {
		struct sk_buff *skb = ssv_tx_next(sd);
		bool sent;

		ssv_tx_expire(sd);
		sent = ssv_agg_pump(sd);
		if (!skb && sent)
			continue;
		if (!skb) {
			/* ssv_tx_queued() also sees the aggregation queues */
			wait_event_interruptible_timeout(sd->tx_wait,
							 ssv_tx_queued(sd) ||
							 kthread_should_stop(),
							 SSV_STATUS_TIMEOUT);
			continue;
		}
		ssv_send_one(sd, skb);
	}
	return 0;
}

void ssv_tx(struct ieee80211_hw *hw, struct ieee80211_tx_control *control,
	    struct sk_buff *skb)
{
	struct ssv_dev *sd = hw->priv;
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	struct ieee80211_sta *sta = control ? control->sta : NULL;
	int hwq;

	/* group frames for dozing stations are announced in the beacon */
	if (info->flags & IEEE80211_TX_CTL_SEND_AFTER_DTIM) {
		hwq = SSV_HW_TXQ_MGMT;
		ssv_ap_group_queued(sd);
	} else if (ieee80211_is_mgmt(hdr->frame_control) ||
		   ieee80211_is_nullfunc(hdr->frame_control)) {
		hwq = SSV_HW_TXQ_MGMT;
	} else {
		hwq = ac_to_hwq[skb_get_queue_mapping(skb) & 3];
	}

	if (sd->started && sta && ssv_agg_tx(sd, sta, skb)) {
		wake_up(&sd->tx_wait);
		return;
	}
	if (!sd->started || !ssv_build_desc(sd, skb, sta, hwq)) {
		ieee80211_free_txskb(hw, skb);
		return;
	}
	skb_queue_tail(&sd->txq[hwq], skb);
	wake_up(&sd->tx_wait);
}

void ssv_tx_flush(struct ssv_dev *sd)
{
	struct sk_buff *skb;
	int i;

	ssv_agg_flush_all(sd);

	for (i = 0; i < SSV_HW_TXQ_NUM; i++)
		while ((skb = skb_dequeue(&sd->txq[i])))
			ieee80211_free_txskb(sd->hw, skb);
	for (i = 0; i < SSV_STATUS_SLOTS; i++) {
		skb = ssv_status_take(sd, i);
		if (skb)
			ssv_tx_done(sd, skb, false, 1);
	}
}

int ssv_tx_init(struct ssv_dev *sd)
{
	int i;

	for (i = 0; i < SSV_HW_TXQ_NUM; i++)
		skb_queue_head_init(&sd->txq[i]);
	init_waitqueue_head(&sd->tx_wait);
	spin_lock_init(&sd->status_lock);
	sd->tx_buf = devm_kzalloc(sd->dev, SSV_TX_BUF_SIZE, GFP_KERNEL);
	if (!sd->tx_buf)
		return -ENOMEM;
	sd->tx_thread = kthread_run(ssv_tx_thread, sd, "ssv6256-tx");
	if (IS_ERR(sd->tx_thread))
		return PTR_ERR(sd->tx_thread);
	return 0;
}

void ssv_tx_deinit(struct ssv_dev *sd)
{
	if (sd->tx_thread)
		kthread_stop(sd->tx_thread);
	sd->tx_thread = NULL;
	ssv_tx_flush(sd);
}
