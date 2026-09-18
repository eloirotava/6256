// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PLL, RF and baseband bring-up for the Turismo C front end.
 *
 * Unlike the SSV6051, whose firmware calibrates the radio, this chip
 * expects the host to run the calibrations: receive DC offset, receive
 * filter tuning for both bandwidths, transmit LO leakage and the TX/RX
 * IQ imbalance.  The sequences and magic values come from the vendor
 * driver (ssv6006_turismoC.c).
 *
 * Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 * Copyright (c) 2015 iComm Corporation
 */
#include <linux/delay.h>
#include <linux/ieee80211.h>

#include "ssv6256.h"
#include "tables.h"

/*
 * Charge pump current per crystal, indexed by the RG_*_XTAL_FREQ code.
 * Only used by the single band variant of the chip.
 */
static const u8 xtal_cp_isel[] = {
	0x8, 0x5, 0x5, 0x7, 0xb, 0x7, 0x5, 0x8,
};

/*
 * Every calibration reports completion through one bit of the same
 * status register.  They take well under a millisecond; the generous
 * bound only guards against a radio that never answers.
 */
static int ssv_cal_wait(struct ssv_dev *sd, u32 done, const char *what)
{
	u32 val;
	int i;

	for (i = 0; i < 1000; i++) {
		if (ssv_reg_read(sd, ADR_RF_D_CAL_TOP_1, &val))
			return -EIO;
		if (val & done)
			return 0;
		usleep_range(50, 100);
	}
	dev_err(sd->dev, "%s calibration did not finish\n", what);
	return -ETIMEDOUT;
}

/* Park the synthesiser on channel 6, where the calibrations are run. */
static void ssv_cal_channel(struct ssv_dev *sd)
{
	ssv_reg_set_bits(sd, ADR_SX_CH_TABLE,
			 (6 << __ffs(RG_SX_CHANNEL)) | RG_SX_RFCH_MAP_EN,
			 RG_SX_CHANNEL | RG_SX_RFCH_MAP_EN);
}

static void ssv_cal_start(struct ssv_dev *sd)
{
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_MODE, MODE_STANDBY);
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_CAL_INDEX, CAL_IDX_NONE);
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_MODE_MANUAL, 1);
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_MODE, MODE_CALIBRATION);
}

/* The radio has to return to standby between two calibrations. */
static void ssv_cal_next(struct ssv_dev *sd)
{
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_MODE, MODE_STANDBY);
	usleep_range(100, 200);
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_MODE, MODE_CALIBRATION);
}

static void ssv_cal_end(struct ssv_dev *sd)
{
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_MODE, MODE_STANDBY);
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_MODE_MANUAL, 0);
}

/* Receive DC offset: 21 IDAC registers the hardware fills in itself. */
static int ssv_cal_rxdc(struct ssv_dev *sd)
{
	int ret;

	ssv_cal_channel(sd);
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_CAL_INDEX,
			CAL_IDX_WIFI2P4G_RXDC);
	usleep_range(100, 200);

	ret = ssv_cal_wait(sd, RO_WF_DCCAL_DONE, "RX DC");
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_CAL_INDEX, CAL_IDX_NONE);
	return ret;
}

/* Receive filter (RC) tuning, once per bandwidth. */
static int ssv_cal_rxrc(struct ssv_dev *sd, bool ht40)
{
	int ret;

	if (ht40) {
		ssv_field_write(sd, ADR_CALIBRATION_TIMER_REGISTER,
				RG_RX_N_RCCAL_DELAY, 2);
		ssv_field_write(sd, ADR_RF_D_CAL_TOP_4, RG_PHASE_35M, 0x3fff);
		ssv_reg_set_bits(sd, ADR_RF_D_CAL_TOP_6,
				 0x213 << __ffs(RG_RX_RCCAL_40M_TARG),
				 RG_RX_RCCAL_40M_TARG | RG_RCCAL_POLAR_INV);
	} else {
		ssv_field_write(sd, ADR_CALIBRATION_TIMER_REGISTER,
				RG_RX_RCCAL_DELAY, 2);
		ssv_field_write(sd, ADR_RF_D_CAL_TOP_2, RG_PHASE_17P5M, 0x20d0);
		ssv_reg_set_bits(sd, ADR_RF_D_CAL_TOP_6,
				 0x22c << __ffs(RG_RX_RCCAL_TARG),
				 RG_RX_RCCAL_TARG | RG_RCCAL_POLAR_INV);
	}
	ssv_field_write(sd, ADR_RF_D_CAL_TOP_0, RG_ALPHA_SEL, 2);
	ssv_field_write(sd, ADR_CALIBRATION_GAIN_REGISTER0, RG_PGAG_RCCAL, 3);
	ssv_field_write(sd, ADR_DIGITAL_ADD_ON_4, RG_TONE_SCALE, 0x80);
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_CAL_INDEX,
			ht40 ? CAL_IDX_BW40_RXRC : CAL_IDX_BW20_RXRC);
	usleep_range(250, 500);

	ret = ssv_cal_wait(sd, RO_RCCAL_DONE, ht40 ? "HT40 RX RC" : "RX RC");
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_CAL_INDEX, CAL_IDX_NONE);
	return ret;
}

/* Shared setup of the transmit calibrations: gain, tone and timing. */
static void ssv_cal_tx_setup(struct ssv_dev *sd)
{
	ssv_cal_channel(sd);
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_TXGAIN_PHYCTRL, 1);
	ssv_reg_set_bits(sd, ADR_CALIBRATION_GAIN_REGISTER0,
			 (6 << __ffs(RG_TX_GAIN_TXCAL)) |
			 (3 << __ffs(RG_PGAG_TXCAL)),
			 RG_TX_GAIN_TXCAL | RG_PGAG_TXCAL);
	ssv_field_write(sd, ADR_DIGITAL_ADD_ON_4, RG_TONE_SCALE, 0x80);
	ssv_field_write(sd, ADR_RF_D_CAL_TOP_9, RG_PRE_DC_AUTO, 1);
	ssv_field_write(sd, ADR_DIGITAL_ADD_ON_3, RG_TX_IQCAL_TIME, 1);
	ssv_reg_set_bits(sd, ADR_RF_D_CAL_TOP_3,
			 (0xccc << __ffs(RG_PHASE_1M)) |
			 (0xccc << __ffs(RG_PHASE_RXIQ_1M)),
			 RG_PHASE_1M | RG_PHASE_RXIQ_1M);
	ssv_field_write(sd, ADR_RF_D_CAL_TOP_0, RG_ALPHA_SEL, 2);
}

/* Transmit LO leakage, corrected through the DAC offsets. */
static int ssv_cal_txdc(struct ssv_dev *sd)
{
	int ret;

	ssv_cal_tx_setup(sd);
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_CAL_INDEX,
			CAL_IDX_WIFI2P4G_TXLO);
	usleep_range(250, 500);

	ret = ssv_cal_wait(sd, RO_TXDC_DONE, "TX DC");
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_CAL_INDEX, CAL_IDX_NONE);
	return ret;
}

/* Transmit IQ imbalance (alpha and theta of the compensation matrix). */
static int ssv_cal_txiq(struct ssv_dev *sd)
{
	int ret;

	ssv_cal_tx_setup(sd);
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_CAL_INDEX,
			CAL_IDX_WIFI2P4G_TXIQ);
	usleep_range(250, 500);

	ret = ssv_cal_wait(sd, RO_TXIQ_DONE, "TX IQ");
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_CAL_INDEX, CAL_IDX_NONE);
	return ret;
}

/* Receive IQ imbalance; the receive chain gets its own gain settings. */
static int ssv_cal_rxiq(struct ssv_dev *sd)
{
	int ret;

	ssv_cal_channel(sd);
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_TXGAIN_PHYCTRL, 1);
	ssv_reg_set_bits(sd, ADR_CALIBRATION_GAIN_REGISTER0,
			 (3 << __ffs(RG_PGAG_RXIQCAL)) |
			 (6 << __ffs(RG_TX_GAIN_RXIQCAL)),
			 RG_RFG_RXIQCAL | RG_PGAG_RXIQCAL | RG_TX_GAIN_RXIQCAL);
	ssv_field_write(sd, ADR_DIGITAL_ADD_ON_4, RG_TONE_SCALE, 0x80);
	ssv_field_write(sd, ADR_RF_D_CAL_TOP_9, RG_PRE_DC_AUTO, 1);
	ssv_field_write(sd, ADR_DIGITAL_ADD_ON_3, RG_TX_IQCAL_TIME, 1);
	ssv_reg_set_bits(sd, ADR_RF_D_CAL_TOP_3,
			 (0xccc << __ffs(RG_PHASE_1M)) |
			 (0xccc << __ffs(RG_PHASE_RXIQ_1M)),
			 RG_PHASE_1M | RG_PHASE_RXIQ_1M);
	ssv_field_write(sd, ADR_RF_D_CAL_TOP_0, RG_ALPHA_SEL, 2);
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_CAL_INDEX,
			CAL_IDX_WIFI2P4G_RXIQ);
	usleep_range(250, 500);

	ret = ssv_cal_wait(sd, RO_RXIQ_DONE, "RX IQ");
	ssv_field_write(sd, ADR_RF_D_CAL_TOP_0, RG_PHASE_STEP_VALUE, 0xccc);
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_CAL_INDEX, CAL_IDX_NONE);
	return ret;
}

static int ssv_calibrate(struct ssv_dev *sd)
{
	u32 alpha, theta;
	int ret;

	ssv_field_write(sd, ADR_WIFI_PADPD_2G_CONTROL_REG, RG_DPD_AM_EN, 0);
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_TXGAIN_PHYCTRL, 1);

	ssv_cal_start(sd);
	ret = ssv_cal_rxdc(sd);
	ssv_cal_next(sd);
	ret = ret ?: ssv_cal_rxrc(sd, false);
	ssv_cal_next(sd);
	ret = ret ?: ssv_cal_rxrc(sd, true);
	ssv_cal_next(sd);
	ret = ret ?: ssv_cal_txdc(sd);
	ssv_cal_next(sd);
	ret = ret ?: ssv_cal_txiq(sd);
	ssv_cal_next(sd);
	ret = ret ?: ssv_cal_rxiq(sd);
	ssv_cal_end(sd);
	if (ret)
		return ret;

	ssv_field_read(sd, ADR_TRX_IQ_COMP_2G, RG_TX_IQ_2500_ALPHA, &alpha);
	ssv_field_read(sd, ADR_TRX_IQ_COMP_2G, RG_TX_IQ_2500_THETA, &theta);
	dev_dbg(sd->dev, "calibrated, TX IQ alpha %u theta %u\n", alpha, theta);
	return 0;
}

/*
 * The PLL locks once the RF table has been handed over; the PMU then
 * reports its ready state and the digital clock can be switched over.
 */
static int ssv_init_pll(struct ssv_dev *sd)
{
	u32 val = 0;
	int i;

	ssv_field_write(sd, ADR_PMU_REG_2, RG_LOAD_RFTABLE_RDY, 1);
	for (i = 0; i < 100; i++) {
		usleep_range(1000, 2000);
		if (ssv_reg_read(sd, ADR_PMU_STATE_REG, &val))
			return -EIO;
		if (val == PMU_STATE_READY)
			break;
	}
	if (val != PMU_STATE_READY) {
		dev_err(sd->dev, "PLL did not lock (PMU state 0x%x)\n", val);
		return -ETIMEDOUT;
	}

	usleep_range(1000, 2000);
	ssv_reg_write(sd, ADR_WIFI_PHY_COMMON_SYS_REG, 0x80010000);
	ssv_reg_write(sd, ADR_CLOCK_SELECTION, CLK_DIGI_80M);
	usleep_range(1000, 2000);
	return 0;
}

/*
 * The single band parts share the die with the dual band ones but need
 * a different synthesiser setup to keep spurs away from the band.
 */
static void ssv_single_band_patch(struct ssv_dev *sd)
{
	u32 id;

	if (ssv_reg_read(sd, ADR_CHIP_ID_2, &id) || id == DUAL_BAND_ID)
		return;

	ssv_field_write(sd, ADR_SX_2_4GB_LPF, RG_SX_LPF_C2_WF, 0xe);
	ssv_field_write(sd, ADR_PMU_REG_1, RG_XO_LDO_LEVEL, 0x6);
	ssv_field_write(sd, ADR_2_4G_LDO_REGISTER, RG_SX_LDO_LO_LEVEL, 0x3);
	ssv_field_write(sd, ADR_SX_2_4GB_VCOBF, RG_SX_VCO_RXOB_AW, 1);
	ssv_field_write(sd, ADR_SX_2_4GB_VCOBF, RG_SX_VCO_TXOB_AW, 1);
	ssv_field_write(sd, ADR_SX_2_4GB_PFD_CHP, RG_SX_CP_ISEL_WF,
			xtal_cp_isel[SSV_XTAL]);
}

/*
 * Turn the baseband blocks on: receive and transmit chains, their FIFOs
 * and the 11b and 11g/n demodulators.  The master enable is separate.
 */
static int ssv_phy_mode(struct ssv_dev *sd, bool enable)
{
	u32 val = RG_PHYRX_MD_EN | RG_PHYTX_MD_EN | RG_PHY11GN_MD_EN |
		  RG_PHY11B_MD_EN | RG_PHYRXFIFO_MD_EN | RG_PHYTXFIFO_MD_EN |
		  RG_PHY11BGN_MD_EN;

	return ssv_reg_write(sd, ADR_WIFI_PHY_COMMON_ENABLE_REG,
			     enable ? val : 0);
}

int ssv_phy_enable(struct ssv_dev *sd, bool enable)
{
	return ssv_field_write(sd, ADR_WIFI_PHY_COMMON_ENABLE_REG,
			       RG_PHY_MD_EN, enable);
}

/* Retune the synthesiser; the receivers are reset afterwards. */
int ssv_set_channel(struct ssv_dev *sd, int channel)
{
	u32 cur;

	ssv_field_write(sd, ADR_WIFI_PHY_COMMON_SYS_REG, RG_RF_5G_BAND, 0);
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_MODE_MANUAL, 1);
	ssv_field_write(sd, ADR_SX_CH_TABLE, RG_SX_RFCH_MAP_EN, 1);

	/* a write that does not change the channel does not retune */
	if (!ssv_field_read(sd, ADR_SX_CH_TABLE, RG_SX_CHANNEL, &cur) &&
	    cur == channel)
		ssv_field_write(sd, ADR_SX_CH_TABLE, RG_SX_CHANNEL,
				channel != 1 ? 1 : 11);
	usleep_range(100, 200);
	ssv_field_write(sd, ADR_SX_CH_TABLE, RG_SX_CHANNEL, channel);

	ssv_field_write(sd, ADR_MODE_REGISTER, RG_MODE, MODE_STANDBY);
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_MODE, MODE_WIFI2P4G_RX);
	ssv_field_write(sd, ADR_MODE_REGISTER, RG_MODE_MANUAL, 0);
	ssv_field_write(sd, ADR_WIFI_11GN_RX_REG_255, RG_SOFT_RST_N_11GN_RX, 1);
	return ssv_field_write(sd, ADR_WIFI_11B_RX_REG_255,
			       RG_SOFT_RST_N_11B_RX, 1);
}

/*
 * @sec_above says where the secondary channel sits for HT40; it is
 * ignored for HT20.
 */
int ssv_set_bandwidth(struct ssv_dev *sd, bool ht40, bool sec_above)
{
	u32 sys = 0, add_on = 0;

	if (ht40) {
		sys = RG_SYSTEM_BW | (sec_above ? 0 : RG_PRIMARY_CH_SIDE);
		add_on = RG_40M_MODE | (sec_above ? RG_LO_UP_CH : 0);
	}
	ssv_field_write(sd, ADR_MTX_MISC_EN,
			MTX_BLOCKTX_IGNORE_CCA_ED_SECONDARY, !ht40);
	ssv_reg_set_bits(sd, ADR_WIFI_PHY_COMMON_SYS_REG, sys,
			 RG_SYSTEM_BW | RG_PRIMARY_CH_SIDE);
	return ssv_reg_set_bits(sd, ADR_DIGITAL_ADD_ON_0, add_on,
				RG_40M_MODE | RG_LO_UP_CH);
}

/*
 * Bring the radio up: RF table, PLL, baseband table and the
 * calibrations.  Runs before the MAC is initialised, because the MAC
 * picks its clock from what the PLL ends up providing.
 */
int ssv_phy_init(struct ssv_dev *sd)
{
	int ret;

	ret = ssv_write_table(sd, rf_table, ARRAY_SIZE(rf_table));
	if (ret)
		return ret;

	ssv_field_write(sd, ADR_PMU_DPLL_REG_0, RG_DP_XTAL_FREQ, SSV_XTAL);
	ssv_field_write(sd, ADR_SX_CH_TABLE, RG_SX_XTAL_FREQ, SSV_XTAL);
	ssv_field_write(sd, ADR_PMU_DPLL_REG_0, RG_EN_IOTADC_160M, 0);

	ret = ssv_init_pll(sd);
	if (ret)
		return ret;

	ssv_reg_write(sd, ADR_WIFI_PHY_COMMON_ENABLE_REG, 0);
	ret = ssv_write_table(sd, phy_table, ARRAY_SIZE(phy_table));
	if (ret)
		return ret;

	ssv_single_band_patch(sd);
	ssv_field_write(sd, ADR_CLOCK_SELECTION, CLK_DIGI_SEL, CLK_DIGI_80M);
	udelay(1);

	ret = ssv_calibrate(sd);
	return ret ?: ssv_phy_mode(sd, true);
}
