// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SSV6256 A-MPDU transmit.
 *
 * The host builds the aggregate: each MPDU gets a delimiter, room for
 * the FCS the MAC fills in, and padding to a multiple of four.  The
 * whole thing goes to the chip as one frame with the aggregate mark
 * set, tagged with a run number.
 *
 * The peer's Block Ack comes back through the receive path carrying
 * that same run number, which is what ties it to the MPDUs it answers.
 * Those missing from its bitmap are sent again in a later aggregate,
 * up to AGG_MAX_TRIES; the rest are handed to mac80211 as acknowledged.
 */
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/unaligned.h>

#include "ssv6256.h"

#define AGG_MAX_TRIES		4
#define AGG_MAX_FRAMES		16
#define AGG_MAX_INFLIGHT	3
#define AGG_BA_TIMEOUT		msecs_to_jiffies(200)
#define AGG_DELIM_LEN		4
#define AGG_FCS_LEN		4
#define AGG_SIGNATURE		0x4e
#define AGG_MPDU_NAV		48
#define AGG_MAX_BYTES		8192
#define SEQ_MASK		0xfff

/* per-MPDU state while the driver owns it, in the tx_info status area */
struct ssv_agg_cb {
	u32 sent_at;
	u8 id;
} __packed;

struct ssv_ba_frame {
	__le16 frame_control;
	__le16 duration;
	u8 ra[ETH_ALEN];
	u8 ta[ETH_ALEN];
	__le16 control;		/* TID in bits 15..12 */
	__le16 ssc;		/* starting sequence << 4 */
	__le32 bitmap[2];
} __packed;

static u16 skb_seq(struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;

	return le16_to_cpu(hdr->seq_ctrl) >> 4;
}

static struct ssv_agg_cb *agg_cb(struct sk_buff *skb)
{
	BUILD_BUG_ON(sizeof(struct ssv_agg_cb) >
		     sizeof(IEEE80211_SKB_CB(skb)->status.status_driver_data));
	return (struct ssv_agg_cb *)IEEE80211_SKB_CB(skb)->status.status_driver_data;
}

/* true if sequence number @a comes before @b */
static bool seq_before(u16 a, u16 b)
{
	u16 d = (b - a) & SEQ_MASK;

	return d && d < 2048;
}

static u8 skb_tid(struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;

	return *ieee80211_get_qos_ctl(hdr) & IEEE80211_QOS_CTL_TID_MASK;
}

static u8 delim_half_crc(u8 v)
{
	u32 c = v, x = v;

	c ^= (x >> 1) | (x << 7);
	c ^= x >> 2;
	if (x & 2)
		c ^= 0xc0;
	c ^= (x << 4) & 0x30;
	return c;
}

static u8 delim_crc(const u8 *p)
{
	u8 crc = 0xcf;

	crc ^= delim_half_crc(p[0]);
	crc = delim_half_crc(crc) ^ delim_half_crc(p[1]);
	return ~crc;
}

static size_t agg_mpdu_size(struct sk_buff *skb)
{
	return round_up(AGG_DELIM_LEN + skb->len + AGG_FCS_LEN, 4);
}

void ssv_agg_init(struct ssv_sta *ss)
{
	int t;

	for (t = 0; t < SSV_AGG_TIDS; t++) {
		struct ssv_agg *a = &ss->agg[t];

		memset(a, 0, sizeof(*a));
		skb_queue_head_init(&a->q);
		skb_queue_head_init(&a->retry);
		skb_queue_head_init(&a->inflight);
	}
}

static void agg_done(struct ssv_dev *sd, struct sk_buff *skb, bool acked)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);

	ieee80211_tx_info_clear_status(info);
	if (acked)
		info->flags |= IEEE80211_TX_STAT_ACK;
	ieee80211_tx_status_ni(sd->hw, skb);
}

/* Queue an unacknowledged MPDU for resending; give up after AGG_MAX_TRIES. */
static bool agg_retry(struct ssv_agg *a, struct sk_buff *skb,
		      struct sk_buff_head *dropped)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	u16 seq = skb_seq(skb);
	u8 *tries = &a->tries[seq & (SSV_AGG_WINDOW - 1)];
	struct sk_buff *pos;

	if (++*tries >= AGG_MAX_TRIES) {
		*tries = 0;
		__skb_queue_tail(dropped, skb);
		return true;
	}
	hdr->frame_control |= cpu_to_le16(IEEE80211_FCTL_RETRY);
	skb_queue_walk(&a->retry, pos) {
		if (seq_before(seq, skb_seq(pos))) {
			__skb_queue_before(&a->retry, pos, skb);
			return false;
		}
	}
	__skb_queue_tail(&a->retry, skb);
	return false;
}

/* Oldest MPDU not finished yet: the start of the window we may use. */
static bool agg_window_start(struct ssv_agg *a, u16 *start)
{
	struct sk_buff *skb;
	bool found = false;

	skb = skb_peek(&a->retry);
	if (skb) {
		*start = skb_seq(skb);
		found = true;
	}
	skb_queue_walk(&a->inflight, skb) {
		if (!found || seq_before(skb_seq(skb), *start)) {
			*start = skb_seq(skb);
			found = true;
		}
	}
	return found;
}

static int agg_inflight_count(struct ssv_agg *a)
{
	struct sk_buff *skb;
	int n = 0, last = -1;

	skb_queue_walk(&a->inflight, skb) {
		if (agg_cb(skb)->id != last) {
			last = agg_cb(skb)->id;
			n++;
		}
	}
	return n;
}

static void agg_complete(struct ssv_dev *sd, struct sk_buff_head *q, bool acked)
{
	struct sk_buff *skb;

	while ((skb = __skb_dequeue(q)))
		agg_done(sd, skb, acked);
}

/* Drop all frames of a TID (session torn down). */
void ssv_agg_flush(struct ssv_dev *sd, struct ssv_sta *ss, u8 tid)
{
	struct ssv_agg *a = &ss->agg[tid];
	struct sk_buff_head drop;

	__skb_queue_head_init(&drop);
	spin_lock_bh(&sd->sta_lock);
	a->state = SSV_AGG_OFF;
	skb_queue_splice_tail_init(&a->inflight, &drop);
	skb_queue_splice_tail_init(&a->retry, &drop);
	skb_queue_splice_tail_init(&a->q, &drop);
	spin_unlock_bh(&sd->sta_lock);
	agg_complete(sd, &drop, false);
}

/* Drop everything queued for aggregation, on every station. */
void ssv_agg_flush_all(struct ssv_dev *sd)
{
	int w, t;

	for (w = 0; w < SSV_NUM_STA; w++) {
		struct ieee80211_sta *sta;

		mutex_lock(&sd->agg_mutex);
		sta = rcu_dereference_protected(sd->sta[w],
						lockdep_is_held(&sd->agg_mutex));
		mutex_unlock(&sd->agg_mutex);
		if (!sta)
			continue;
		for (t = 0; t < SSV_AGG_TIDS; t++)
			ssv_agg_flush(sd, (struct ssv_sta *)sta->drv_priv, t);
	}
}

/* Called from ssv_tx(); returns true if the frame was taken. */
bool ssv_agg_tx(struct ssv_dev *sd, struct ieee80211_sta *sta,
		struct sk_buff *skb)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	struct ssv_sta *ss = (struct ssv_sta *)sta->drv_priv;
	struct ssv_agg *a;
	bool taken = false;
	u8 tid;

	if (!sta->deflink.ht_cap.ht_supported ||
	    !(info->control.rates[0].flags & IEEE80211_TX_RC_MCS) ||
	    !ieee80211_is_data_qos(hdr->frame_control) ||
	    skb->protocol == cpu_to_be16(ETH_P_PAE) ||
	    is_multicast_ether_addr(hdr->addr1))
		return false;

	tid = skb_tid(skb);
	a = &ss->agg[tid];

	spin_lock_bh(&sd->sta_lock);
	if (a->state == SSV_AGG_OPERATIONAL &&
	    (info->flags & IEEE80211_TX_CTL_AMPDU)) {
		__skb_queue_tail(&a->q, skb);
		taken = true;
	} else if (a->state == SSV_AGG_OFF &&
		   time_after(jiffies, a->retry_start)) {
		a->state = SSV_AGG_STARTING;
		a->retry_start = jiffies + 10 * HZ;
		spin_unlock_bh(&sd->sta_lock);
		if (ieee80211_start_tx_ba_session(sta, tid, 0)) {
			spin_lock_bh(&sd->sta_lock);
			a->state = SSV_AGG_OFF;
			spin_unlock_bh(&sd->sta_lock);
		}
		return false;
	}
	spin_unlock_bh(&sd->sta_lock);
	return taken;
}

/*
 * Build one aggregate for @a into sd->tx_buf.  Returns its length, or
 * zero when nothing is ready.
 */
static size_t agg_build(struct ssv_dev *sd, struct ssv_sta *ss,
			struct ssv_agg *a, int hwq)
{
	struct ssv_tx_desc *d = (struct ssv_tx_desc *)sd->tx_buf;
	struct ieee80211_tx_info *info;
	struct sk_buff_head *src;
	struct sk_buff *skb, *first = NULL;
	size_t len = SSV_TX_DESC_LEN, max_len, on_air;
	int n = 0, limit, hdrlen, i;
	u8 id, *p;
	u16 start;

	if (agg_inflight_count(a) >= AGG_MAX_INFLIGHT)
		return 0;
	max_len = min_t(size_t, AGG_MAX_BYTES, SSV_TX_BUF_SIZE);
	limit = min_t(int, a->buf_size, AGG_MAX_FRAMES);
	/* the run number is device wide: a report or Block Ack carries it
	 * back and has to name exactly one aggregate
	 */
	id = sd->agg_next_id++ & (SSV_AGG_IDS - 1);

	/* everything sent must stay inside the peer's window */
	if (!agg_window_start(a, &start)) {
		skb = skb_peek(&a->q);
		if (!skb)
			return 0;
		start = skb_seq(skb);
	}

	/* retries first (they are older), then new frames */
	p = sd->tx_buf + SSV_TX_DESC_LEN;
	for (src = &a->retry; ; src = &a->q) {
		while (n < limit && (skb = skb_peek(src))) {
			struct ieee80211_hdr *hdr;
			size_t sz = agg_mpdu_size(skb);
			u16 dl;

			if (((skb_seq(skb) - start) & SEQ_MASK) >= a->buf_size ||
			    len + sz > max_len)
				break;
			__skb_unlink(skb, src);

			hdr = (struct ieee80211_hdr *)skb->data;
			hdr->duration_id = cpu_to_le16(AGG_MPDU_NAV);
			dl = skb->len + AGG_FCS_LEN;
			put_unaligned_le16(dl << 4, p);
			p[2] = delim_crc(p);
			p[3] = AGG_SIGNATURE;
			memcpy(p + AGG_DELIM_LEN, skb->data, skb->len);
			memset(p + AGG_DELIM_LEN + skb->len, 0,
			       sz - AGG_DELIM_LEN - skb->len);
			p += sz;
			len += sz;
			n++;

			agg_cb(skb)->sent_at = jiffies;
			agg_cb(skb)->id = id;
			__skb_queue_tail(&a->inflight, skb);
			if (!first)
				first = skb;
		}
		if (src == &a->q)
			break;
	}
	if (!n)
		return 0;

	info = IEEE80211_SKB_CB(first);
	hdrlen = ieee80211_hdrlen(((struct ieee80211_hdr *)first->data)->frame_control) +
		 AGG_DELIM_LEN;
	on_air = len - SSV_TX_DESC_LEN + AGG_FCS_LEN;

	memset(d, 0, SSV_TX_DESC_LEN);
	d->w0 = cpu_to_le32(FIELD_PREP(TXD0_LEN, len) |
			    FIELD_PREP(TXD0_C_TYPE, SSV_CTYPE_TXREQ) |
			    TXD0_F80211 | TXD0_QOS | TXD0_HT);
	d->fcmd = cpu_to_le32(((hwq + M_ENG_TX_EDCA0) << 4) | M_ENG_HWHCI);
	d->w2 = cpu_to_le32(FIELD_PREP(TXD2_HDR_OFFSET, SSV_TX_DESC_LEN) |
			    TXD2_UNICAST |
			    FIELD_PREP(TXD2_HDR_LEN, hdrlen) |
			    FIELD_PREP(TXD2_AGGR, 2));
	d->w3 = cpu_to_le32(FIELD_PREP(TXD3_PKT_RUN_NO, SSV_AGG_RUN_NO(id)) |
			    FIELD_PREP(TXD3_WSID, ss->wsid) |
			    FIELD_PREP(TXD3_TXQ_IDX, hwq));
	d->w5 = cpu_to_le32(FIELD_PREP(TXD5_RATE_RPT_MODE, RATE_RPT_ON));

	/* aggregates always go with RTS/CTS, and ask for a Block Ack */
	for (i = 0; i < SSV_TX_MAX_RATES; i++) {
		const struct ieee80211_tx_rate *r = &info->control.rates[i];
		bool last = i == SSV_TX_MAX_RATES - 1 || r->idx < 0 ||
			    info->control.rates[i + 1].idx < 0;

		if (r->idx < 0) {
			/* no rate control yet: the slowest HT rate */
			if (i == 0)
				ssv_fill_rate(&d->rate[0],
					      FIELD_PREP(RATE_PHY_MODE,
							 RATE_PHY_HT),
					      2, on_air, true, true, true);
			break;
		}
		ssv_fill_rate(&d->rate[i], ssv_rate_code(sd, r),
			      max_t(u8, r->count, 2), on_air, true, true, last);
		if (last)
			break;
	}

	dev_dbg(sd->dev, "agg: q%d id %u send %d mpdu seq %u len %zu\n",
		hwq, id, n, skb_seq(first), len);
	return len;
}

/* Tell the peer to move its window past MPDUs we gave up on. */
static void agg_send_bar(struct ssv_dev *sd, struct ieee80211_sta *sta,
			 struct ssv_agg *a, u8 tid)
{
	struct sk_buff *skb;
	u16 start;

	if (!sd->vif)
		return;
	if (!agg_window_start(a, &start)) {
		skb = skb_peek(&a->q);
		if (!skb)
			return;
		start = skb_seq(skb);
	}
	ieee80211_send_bar(sd->vif, sta->addr, tid, start);
}

/* TX thread: send what is pending.  Returns true if something went out. */
bool ssv_agg_pump(struct ssv_dev *sd)
{
	struct sk_buff_head drop;
	bool sent = false;
	int w, t;

	if (!sd->started)
		return false;

	__skb_queue_head_init(&drop);
	for (w = 0; w < SSV_NUM_STA; w++) {
		struct ieee80211_sta *sta;
		struct ssv_sta *ss;

		/* aggregates are written to the bus, which sleeps: no RCU */
		mutex_lock(&sd->agg_mutex);
		sta = rcu_dereference_protected(sd->sta[w],
						lockdep_is_held(&sd->agg_mutex));
		if (!sta) {
			mutex_unlock(&sd->agg_mutex);
			continue;
		}
		ss = (struct ssv_sta *)sta->drv_priv;
		for (t = 0; t < SSV_AGG_TIDS; t++) {
			struct ssv_agg *a = &ss->agg[t];
			struct sk_buff *skb, *next;
			bool gave_up = false;
			size_t len;

			if (a->state != SSV_AGG_OPERATIONAL)
				continue;

			/* aggregates whose Block Ack never came */
			spin_lock_bh(&sd->sta_lock);
			skb_queue_walk_safe(&a->inflight, skb, next) {
				if ((u32)jiffies - agg_cb(skb)->sent_at <
				    AGG_BA_TIMEOUT)
					continue;
				__skb_unlink(skb, &a->inflight);
				gave_up |= agg_retry(a, skb, &drop);
			}
			spin_unlock_bh(&sd->sta_lock);
			if (gave_up)
				agg_send_bar(sd, sta, a, t);

			while (!skb_queue_empty(&a->retry) ||
			       !skb_queue_empty(&a->q)) {
				spin_lock_bh(&sd->sta_lock);
				len = agg_build(sd, ss, a, ssv_tid_to_hwq(t));
				spin_unlock_bh(&sd->sta_lock);
				if (!len)
					break;
				if (ssv_write_data(sd, sd->tx_buf, len))
					break;	/* the timeout takes care of them */
				sent = true;
			}
		}
		mutex_unlock(&sd->agg_mutex);
	}
	agg_complete(sd, &drop, false);
	return sent;
}

/*
 * Settle the MPDUs of aggregate @id: acknowledged when @bitmap (which
 * starts at @ssn) has their bit, resent otherwise.  A NULL bitmap means
 * the peer answered nothing.
 */
static void agg_settle(struct ssv_dev *sd, struct ieee80211_sta *sta,
		       struct ssv_agg *a, u8 tid, u8 id, u16 ssn,
		       const __le32 *bitmap)
{
	struct sk_buff_head done, drop;
	struct sk_buff *skb, *next;
	int frames = 0, acked = 0;

	__skb_queue_head_init(&done);
	__skb_queue_head_init(&drop);

	spin_lock_bh(&sd->sta_lock);
	skb_queue_walk_safe(&a->inflight, skb, next) {
		u16 off;

		if (agg_cb(skb)->id != id)
			continue;
		__skb_unlink(skb, &a->inflight);
		frames++;
		off = (skb_seq(skb) - ssn) & SEQ_MASK;
		if (bitmap && off < 64 &&
		    (le32_to_cpu(bitmap[off / 32]) & BIT(off % 32))) {
			a->tries[skb_seq(skb) & (SSV_AGG_WINDOW - 1)] = 0;
			__skb_queue_tail(&done, skb);
			acked++;
		} else {
			agg_retry(a, skb, &drop);
		}
	}
	spin_unlock_bh(&sd->sta_lock);
	if (!frames)
		return;

	dev_dbg(sd->dev, "agg: %s tid %u id %u ssn %u acked %d/%d\n",
		bitmap ? "BA" : "no BA", tid, id, ssn, acked, frames);
	if (!skb_queue_empty(&drop))
		agg_send_bar(sd, sta, a, tid);
	ssv_tx_kick(sd);
	agg_complete(sd, &done, true);
	agg_complete(sd, &drop, false);
}

/* Find the station and TID holding MPDUs of aggregate @id. */
static struct ssv_agg *agg_lookup(struct ssv_dev *sd, u8 id, u8 tid,
				  struct ieee80211_sta **stap)
{
	int w;

	if (tid >= SSV_AGG_TIDS)
		return NULL;
	for (w = 0; w < SSV_NUM_STA; w++) {
		struct ieee80211_sta *sta = rcu_dereference(sd->sta[w]);
		struct ssv_agg *a;

		if (!sta)
			continue;
		a = &((struct ssv_sta *)sta->drv_priv)->agg[tid];
		if (a->state != SSV_AGG_OPERATIONAL)
			continue;
		*stap = sta;
		return a;
	}
	return NULL;
}

/* A Block Ack answering one of our aggregates (RX path). */
void ssv_agg_ba(struct ssv_dev *sd, struct sk_buff *skb, u8 run_no)
{
	const struct ssv_ba_frame *ba = (const struct ssv_ba_frame *)skb->data;
	struct ieee80211_sta *sta = NULL;
	struct ssv_agg *a;
	u8 tid;

	if (skb->len < sizeof(*ba) || !SSV_IS_AGG_RUN_NO(run_no))
		return;
	tid = le16_to_cpu(ba->control) >> 12;

	rcu_read_lock();
	a = agg_lookup(sd, SSV_AGG_ID(run_no), tid, &sta);
	if (a)
		agg_settle(sd, sta, a, tid, SSV_AGG_ID(run_no),
			   le16_to_cpu(ba->ssc) >> 4, ba->bitmap);
	rcu_read_unlock();
}

/* The chip gave up on an aggregate: no Block Ack came back. */
void ssv_agg_failed(struct ssv_dev *sd, u8 run_no)
{
	struct ieee80211_sta *sta = NULL;
	struct ssv_agg *a;
	int t;

	if (!SSV_IS_AGG_RUN_NO(run_no))
		return;
	rcu_read_lock();
	for (t = 0; t < SSV_AGG_TIDS; t++) {
		a = agg_lookup(sd, SSV_AGG_ID(run_no), t, &sta);
		if (a)
			agg_settle(sd, sta, a, t, SSV_AGG_ID(run_no), 0, NULL);
	}
	rcu_read_unlock();
}

int ssv_agg_action(struct ssv_dev *sd, struct ieee80211_vif *vif,
		   struct ieee80211_ampdu_params *params)
{
	struct ssv_sta *ss = (struct ssv_sta *)params->sta->drv_priv;
	u8 tid = params->tid;
	struct ssv_agg *a;

	if (tid >= SSV_AGG_TIDS)
		return -EINVAL;
	a = &ss->agg[tid];

	switch (params->action) {
	case IEEE80211_AMPDU_TX_START:
		spin_lock_bh(&sd->sta_lock);
		a->state = SSV_AGG_STARTING;
		spin_unlock_bh(&sd->sta_lock);
		return IEEE80211_AMPDU_TX_START_IMMEDIATE;
	case IEEE80211_AMPDU_TX_OPERATIONAL:
		spin_lock_bh(&sd->sta_lock);
		a->buf_size = clamp_t(u16, params->buf_size, 1, SSV_AGG_WINDOW);
		memset(a->tries, 0, sizeof(a->tries));
		a->state = SSV_AGG_OPERATIONAL;
		spin_unlock_bh(&sd->sta_lock);
		return 0;
	case IEEE80211_AMPDU_TX_STOP_CONT:
		ssv_agg_flush(sd, ss, tid);
		ieee80211_stop_tx_ba_cb_irqsafe(vif, params->sta->addr, tid);
		return 0;
	case IEEE80211_AMPDU_TX_STOP_FLUSH:
	case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
		ssv_agg_flush(sd, ss, tid);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}
