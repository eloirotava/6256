// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SSV6256 access point support.
 *
 * The MAC sends the beacon by itself out of a buffer in packet memory;
 * the driver rewrites it whenever mac80211 changes it or the TIM moves.
 * Group frames that have to wait for the DTIM beacon (because a station
 * is dozing) go to hardware queue 5, which the MAC holds back until the
 * DTIM beacon has gone out; while any are waiting the beacon announces
 * them through bit 0 of the TIM.
 */
#include <linux/etherdevice.h>

#include "ssv6256.h"

/* keep announcing buffered group frames this many DTIM periods */
#define DTIM_HOLD_PERIODS	2

bool ssv_is_ap(struct ssv_dev *sd)
{
	struct ieee80211_vif *vif = sd->vif;

	return vif && vif->type == NL80211_IFTYPE_AP;
}

/* Called with sd->mutex held. */
void ssv_ap_update_beacon(struct ssv_dev *sd)
{
	struct ieee80211_vif *vif = sd->vif;
	struct ieee80211_tx_info *info;
	struct ssv_tx_desc *d;
	struct sk_buff *skb;
	u16 tim_offset, tim_len;
	size_t len;
	u8 *buf;

	lockdep_assert_held(&sd->mutex);
	if (!ssv_is_ap(sd) || !sd->started || !vif->bss_conf.enable_beacon)
		return;

	skb = ieee80211_beacon_get_tim(sd->hw, vif, &tim_offset, &tim_len, 0);
	if (!skb)
		return;
	if (tim_offset && tim_len >= 6) {
		/* the MAC fills in the DTIM count */
		skb->data[tim_offset + 2] = 0;
		if (READ_ONCE(sd->dtim_bit))
			skb->data[tim_offset + 4] |= 1;
		else
			skb->data[tim_offset + 4] &= ~1;
	}

	len = SSV_TX_DESC_LEN + skb->len;
	buf = kzalloc(round_up(len, 4), GFP_KERNEL);
	if (!buf)
		goto out;

	info = IEEE80211_SKB_CB(skb);
	d = (struct ssv_tx_desc *)buf;
	d->w0 = cpu_to_le32(FIELD_PREP(TXD0_LEN, len) |
			    FIELD_PREP(TXD0_C_TYPE, SSV_CTYPE_TXREQ) |
			    TXD0_F80211);
	d->fcmd = cpu_to_le32(((SSV_HW_TXQ_DTIM + M_ENG_TX_EDCA0) << 4) |
			      M_ENG_HWHCI);
	d->w2 = cpu_to_le32(FIELD_PREP(TXD2_HDR_OFFSET, SSV_TX_DESC_LEN) |
			    FIELD_PREP(TXD2_HDR_LEN, 24));
	d->w3 = cpu_to_le32(FIELD_PREP(TXD3_WSID, 0xf) |
			    FIELD_PREP(TXD3_TXQ_IDX, SSV_HW_TXQ_DTIM));
	d->w5 = cpu_to_le32(FIELD_PREP(TXD5_RATE_RPT_MODE, RATE_RPT_OFF));
	/* beacons go out at the lowest basic rate, without an ack */
	ssv_fill_rate(&d->rate[0], ssv_rate_code(sd, &info->control.rates[0]),
		      1, skb->len + 4, false, false, true);
	memcpy(buf + SSV_TX_DESC_LEN, skb->data, skb->len);

	/* nothing changed and the chip already holds it: leave it alone */
	if (sd->bcn_last && sd->bcn_last_len == len &&
	    !memcmp(sd->bcn_last, buf, len) &&
	    (sd->bcn_buf[0] || sd->bcn_buf[1])) {
		kfree(buf);
		goto out;
	}
	if (ssv_beacon_set(sd, buf, len, SSV_TX_DESC_LEN + tim_offset + 2)) {
		dev_err(sd->dev, "cannot store the beacon\n");
		kfree(buf);
		goto out;
	}
	ssv_beacon_timing(sd, vif->bss_conf.beacon_int,
			  vif->bss_conf.dtim_period);
	kfree(sd->bcn_last);
	sd->bcn_last = buf;
	sd->bcn_last_len = len;
out:
	dev_kfree_skb(skb);
}

static void ssv_ap_beacon_work(struct work_struct *work)
{
	struct ssv_dev *sd = container_of(work, struct ssv_dev, beacon_work);

	mutex_lock(&sd->mutex);
	ssv_ap_update_beacon(sd);
	mutex_unlock(&sd->mutex);
}

static void ssv_ap_dtim_work(struct work_struct *work)
{
	struct ssv_dev *sd = container_of(to_delayed_work(work), struct ssv_dev,
					  dtim_work);

	WRITE_ONCE(sd->dtim_bit, false);
	ssv_ap_beacon_work(&sd->beacon_work);
}

/* A group frame went to the DTIM queue (TX path, atomic). */
void ssv_ap_group_queued(struct ssv_dev *sd)
{
	struct ieee80211_vif *vif = sd->vif;
	unsigned long hold;

	if (!vif)
		return;
	hold = msecs_to_jiffies(DTIM_HOLD_PERIODS *
				max_t(u8, vif->bss_conf.dtim_period, 1) *
				(vif->bss_conf.beacon_int ?: 100) * 1024 / 1000 +
				50);
	if (!READ_ONCE(sd->dtim_bit)) {
		WRITE_ONCE(sd->dtim_bit, true);
		schedule_work(&sd->beacon_work);
	}
	mod_delayed_work(system_wq, &sd->dtim_work, hold);
}

void ssv_ap_init(struct ssv_dev *sd)
{
	INIT_WORK(&sd->beacon_work, ssv_ap_beacon_work);
	INIT_DELAYED_WORK(&sd->dtim_work, ssv_ap_dtim_work);
}

/* The access point interface goes away (sd->mutex held). */
void ssv_ap_stop(struct ssv_dev *sd)
{
	lockdep_assert_held(&sd->mutex);
	WRITE_ONCE(sd->dtim_bit, false);
	if (sd->started) {
		ssv_beacon_release(sd);
		ssv_set_ap_mode(sd, false);
	}
	kfree(sd->bcn_last);
	sd->bcn_last = NULL;
	sd->bcn_last_len = 0;
}
