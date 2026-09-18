// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SSV6256 mac80211 glue: capabilities and callbacks.
 */
#include <linux/etherdevice.h>

#include "ssv6256.h"

#define CHAN(_ch, _freq) { .band = NL80211_BAND_2GHZ, .center_freq = (_freq), \
			   .hw_value = (_ch), .max_power = 20 }

static struct ieee80211_channel ssv_channels[] = {
	CHAN(1, 2412), CHAN(2, 2417), CHAN(3, 2422), CHAN(4, 2427),
	CHAN(5, 2432), CHAN(6, 2437), CHAN(7, 2442), CHAN(8, 2447),
	CHAN(9, 2452), CHAN(10, 2457), CHAN(11, 2462), CHAN(12, 2467),
	CHAN(13, 2472), CHAN(14, 2484),
};

/*
 * The chip's rate byte indexes CCK and OFDM rates separately, so the
 * hardware value is just the position in this table (see tx.c).
 */
static struct ieee80211_rate ssv_bitrates[] = {
	{ .bitrate = 10 },
	{ .bitrate = 20, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 55, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 110, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 60 },
	{ .bitrate = 90 },
	{ .bitrate = 120 },
	{ .bitrate = 180 },
	{ .bitrate = 240 },
	{ .bitrate = 360 },
	{ .bitrate = 480 },
	{ .bitrate = 540 },
};

static int ssv_start(struct ieee80211_hw *hw)
{
	struct ssv_dev *sd = hw->priv;
	int ret;

	mutex_lock(&sd->mutex);
	ret = ssv_hw_start(sd);
	if (!ret)
		ret = ssv_irq_enable(sd);
	if (!ret)
		sd->started = true;
	mutex_unlock(&sd->mutex);
	if (ret)
		dev_err(sd->dev, "start failed: %d\n", ret);
	return ret;
}

static void ssv_stop(struct ieee80211_hw *hw, bool suspend)
{
	struct ssv_dev *sd = hw->priv;

	mutex_lock(&sd->mutex);
	sd->started = false;
	ssv_irq_disable(sd);
	ssv_phy_enable(sd, false);
	ssv_tx_flush(sd);
	mutex_unlock(&sd->mutex);
}

static int ssv_add_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct ssv_dev *sd = hw->priv;

	if (vif->type != NL80211_IFTYPE_STATION)
		return -EOPNOTSUPP;
	if (sd->vif)
		return -EBUSY;

	mutex_lock(&sd->mutex);
	sd->vif = vif;
	mutex_unlock(&sd->mutex);
	return 0;
}

static void ssv_remove_interface(struct ieee80211_hw *hw,
				 struct ieee80211_vif *vif)
{
	struct ssv_dev *sd = hw->priv;

	mutex_lock(&sd->mutex);
	if (sd->vif == vif)
		sd->vif = NULL;
	mutex_unlock(&sd->mutex);
}

static int ssv_config(struct ieee80211_hw *hw, int radio_idx, u32 changed)
{
	struct ssv_dev *sd = hw->priv;
	struct ieee80211_channel *chan = hw->conf.chandef.chan;
	int ret = 0;

	if (!(changed & IEEE80211_CONF_CHANGE_CHANNEL) || !chan)
		return 0;

	mutex_lock(&sd->mutex);
	if (chan->hw_value != sd->channel) {
		ret = ssv_set_channel(sd, chan->hw_value);
		if (!ret)
			sd->channel = chan->hw_value;
	}
	mutex_unlock(&sd->mutex);
	return ret;
}

#define SSV_FILTERS (FIF_ALLMULTI | FIF_BCN_PRBRESP_PROMISC | FIF_PSPOLL)

static void ssv_configure_filter(struct ieee80211_hw *hw, unsigned int changed,
				 unsigned int *total, u64 multicast)
{
	*total &= SSV_FILTERS;
}

static void ssv_bss_info_changed(struct ieee80211_hw *hw,
				 struct ieee80211_vif *vif,
				 struct ieee80211_bss_conf *info, u64 changed)
{
	struct ssv_dev *sd = hw->priv;

	mutex_lock(&sd->mutex);
	if (changed & BSS_CHANGED_ERP_PREAMBLE)
		sd->short_preamble = info->use_short_preamble;
	if (changed & BSS_CHANGED_BSSID)
		ssv_set_bssid(sd, info->bssid);
	mutex_unlock(&sd->mutex);
}

static int ssv_sta_add(struct ssv_dev *sd, struct ieee80211_sta *sta)
{
	struct ssv_sta *ss = (struct ssv_sta *)sta->drv_priv;
	int wsid;

	for (wsid = 0; wsid < SSV_NUM_STA; wsid++)
		if (!rcu_access_pointer(sd->sta[wsid]))
			break;
	if (wsid == SSV_NUM_STA)
		return -ENOSPC;

	ss->wsid = wsid;
	rcu_assign_pointer(sd->sta[wsid], sta);
	return ssv_wsid_add(sd, wsid, sta->addr);
}

static void ssv_sta_del(struct ssv_dev *sd, struct ieee80211_sta *sta)
{
	struct ssv_sta *ss = (struct ssv_sta *)sta->drv_priv;

	if (ss->wsid < 0 || ss->wsid >= SSV_NUM_STA)
		return;
	ssv_wsid_del(sd, ss->wsid);
	RCU_INIT_POINTER(sd->sta[ss->wsid], NULL);
	ss->wsid = -1;
	synchronize_rcu();
}

static int ssv_sta_state(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			 struct ieee80211_sta *sta, enum ieee80211_sta_state old,
			 enum ieee80211_sta_state new)
{
	struct ssv_dev *sd = hw->priv;
	int ret = 0;

	mutex_lock(&sd->mutex);
	if (old == IEEE80211_STA_NOTEXIST && new == IEEE80211_STA_NONE)
		ret = ssv_sta_add(sd, sta);
	else if (old == IEEE80211_STA_NONE && new == IEEE80211_STA_NOTEXIST)
		ssv_sta_del(sd, sta);
	mutex_unlock(&sd->mutex);
	return ret;
}

/*
 * Receiving aggregates needs nothing from the driver: the MAC answers
 * the Block Ack requests and hands the subframes over one by one, and
 * mac80211 puts them back in order.  Sending them is not implemented.
 */
static int ssv_ampdu_action(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			    struct ieee80211_ampdu_params *params)
{
	switch (params->action) {
	case IEEE80211_AMPDU_RX_START:
	case IEEE80211_AMPDU_RX_STOP:
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct ieee80211_ops ssv_ops = {
	.add_chanctx = ieee80211_emulate_add_chanctx,
	.remove_chanctx = ieee80211_emulate_remove_chanctx,
	.change_chanctx = ieee80211_emulate_change_chanctx,
	.switch_vif_chanctx = ieee80211_emulate_switch_vif_chanctx,
	.wake_tx_queue = ieee80211_handle_wake_tx_queue,
	.tx = ssv_tx,
	.start = ssv_start,
	.stop = ssv_stop,
	.add_interface = ssv_add_interface,
	.remove_interface = ssv_remove_interface,
	.config = ssv_config,
	.configure_filter = ssv_configure_filter,
	.bss_info_changed = ssv_bss_info_changed,
	.sta_state = ssv_sta_state,
	.ampdu_action = ssv_ampdu_action,
};

struct ssv_dev *ssv_mac_alloc(struct device *dev)
{
	struct ieee80211_hw *hw;
	struct ssv_dev *sd;

	hw = ieee80211_alloc_hw(sizeof(*sd), &ssv_ops);
	if (!hw)
		return NULL;
	sd = hw->priv;
	sd->hw = hw;
	sd->dev = dev;
	sd->channel = 1;
	mutex_init(&sd->mutex);
	SET_IEEE80211_DEV(hw, dev);
	return sd;
}

void ssv_mac_free(struct ssv_dev *sd)
{
	ieee80211_free_hw(sd->hw);
}

int ssv_mac_register(struct ssv_dev *sd)
{
	struct ieee80211_hw *hw = sd->hw;
	struct ieee80211_sta_ht_cap *ht = &sd->band.ht_cap;
	int ret;

	ieee80211_hw_set(hw, SIGNAL_DBM);
	ieee80211_hw_set(hw, MFP_CAPABLE);
	ieee80211_hw_set(hw, REPORTS_TX_ACK_STATUS);
	ieee80211_hw_set(hw, AMPDU_AGGREGATION);
	hw->max_rx_aggregation_subframes = 32;
	hw->queues = IEEE80211_NUM_ACS;
	hw->extra_tx_headroom = SSV_TX_DESC_LEN;
	hw->max_rates = SSV_TX_MAX_RATES;
	hw->max_rate_tries = 15;
	hw->sta_data_size = sizeof(struct ssv_sta);
	hw->vif_data_size = 0;
	hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	hw->wiphy->flags &= ~WIPHY_FLAG_PS_ON_BY_DEFAULT;

	sd->band.band = NL80211_BAND_2GHZ;
	sd->band.channels = ssv_channels;
	sd->band.n_channels = ARRAY_SIZE(ssv_channels);
	sd->band.bitrates = ssv_bitrates;
	sd->band.n_bitrates = ARRAY_SIZE(ssv_bitrates);

	/* one spatial stream; 40 MHz and aggregation come later */
	ht->ht_supported = true;
	ht->cap = IEEE80211_HT_CAP_SGI_20 | IEEE80211_HT_CAP_SM_PS;
	ht->ampdu_factor = IEEE80211_HT_MAX_AMPDU_32K;
	ht->ampdu_density = IEEE80211_HT_MPDU_DENSITY_8;
	ht->mcs.rx_mask[0] = 0xff;
	ht->mcs.rx_highest = cpu_to_le16(72);
	ht->mcs.tx_params = IEEE80211_HT_MCS_TX_DEFINED;
	hw->wiphy->bands[NL80211_BAND_2GHZ] = &sd->band;

	SET_IEEE80211_PERM_ADDR(hw, sd->mac);

	ret = ssv_tx_init(sd);
	if (ret)
		return ret;
	ret = ieee80211_register_hw(hw);
	if (ret) {
		ssv_tx_deinit(sd);
		return ret;
	}
	wiphy_info(hw->wiphy, "SSV6256 ready\n");
	return 0;
}

void ssv_mac_unregister(struct ssv_dev *sd)
{
	ieee80211_unregister_hw(sd->hw);
	ssv_tx_deinit(sd);
}
