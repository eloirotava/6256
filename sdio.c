// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SSV6256 SDIO transport: register and data ports, firmware upload and
 * probe.  The bus side is the same as on the older SSV6051; what differs
 * is the way the MCU is held in reset and started.
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "ssv6256.h"

#ifndef SDIO_VENDOR_ID_SSV
#define SDIO_VENDOR_ID_SSV		0x3030
#define SDIO_DEVICE_ID_SSV_6256		0x3030
#endif

#define FW_BLOCK_SIZE		0x8000
#define FW_CHECKSUM_BLOCK	1024
#define FW_CHECKSUM_INIT	0x12345678
#define FW_STATUS_MASK		0x00ff0000

/*
 * Register access goes through the "register port": write the address
 * (and value) with CMD53, read the value back with CMD53.  The MMC core
 * needs DMA-safe buffers, hence the per-device scratch buffer; every
 * user holds the SDIO host.
 */
int ssv_reg_read(struct ssv_dev *sd, u32 addr, u32 *val)
{
	struct sdio_func *func = sd->func;
	int ret;

	sdio_claim_host(func);
	put_unaligned_le32(addr, sd->io_buf);
	ret = sdio_memcpy_toio(func, sd->reg_port, sd->io_buf, 4);
	if (!ret)
		ret = sdio_memcpy_fromio(func, sd->io_buf, sd->reg_port, 4);
	sdio_release_host(func);
	if (ret) {
		dev_err_ratelimited(sd->dev, "read 0x%08x failed: %d\n", addr, ret);
		*val = 0xffffffff;
		return ret;
	}
	*val = get_unaligned_le32(sd->io_buf);
	return 0;
}

int ssv_reg_write(struct ssv_dev *sd, u32 addr, u32 val)
{
	struct sdio_func *func = sd->func;
	int ret;

	sdio_claim_host(func);
	put_unaligned_le32(addr, sd->io_buf);
	put_unaligned_le32(val, sd->io_buf + 4);
	ret = sdio_memcpy_toio(func, sd->reg_port, sd->io_buf, 8);
	sdio_release_host(func);
	if (ret)
		dev_err_ratelimited(sd->dev, "write 0x%08x failed: %d\n", addr, ret);
	return ret;
}

int ssv_reg_set_bits(struct ssv_dev *sd, u32 addr, u32 set, u32 mask)
{
	u32 val;
	int ret;

	ret = ssv_reg_read(sd, addr, &val);
	if (ret)
		return ret;
	return ssv_reg_write(sd, addr, (val & ~mask) | (set & mask));
}

/*
 * Frames and host commands.  @buf must be DMA-safe and padded to the
 * SDIO block alignment (see sdio_align_size()).
 */
int ssv_write_data(struct ssv_dev *sd, const u8 *buf, size_t len)
{
	struct sdio_func *func = sd->func;
	int ret;

	sdio_claim_host(func);
	ret = sdio_memcpy_toio(func, sd->data_port, (void *)buf,
			       sdio_align_size(func, len));
	sdio_release_host(func);
	if (ret)
		dev_err_ratelimited(sd->dev, "data write (%zu) failed: %d\n", len, ret);
	return ret;
}

static void ssv_set_bus_clock(struct ssv_dev *sd, u32 hz)
{
	struct mmc_host *host = sd->func->card->host;

	/*
	 * The chip only keeps up with a high-speed bus once its PLL is set
	 * up, so the firmware is loaded at 25 MHz and the clock the MMC
	 * core negotiated is restored afterwards.
	 */
	hz = clamp(hz, host->f_min, host->f_max);
	if (hz == host->ios.clock)
		return;
	sdio_claim_host(sd->func);
	host->ios.clock = hz;
	host->ops->set_ios(host, &host->ios);
	sdio_release_host(sd->func);
	msleep(20);
}

/*
 * The chip identity is an ASCII string held big-endian in four registers,
 * the last register first, and padded with spaces.
 */
static int ssv_read_chip_id(struct ssv_dev *sd)
{
	u32 val;
	int i, ret;

	for (i = 0; i < 4; i++) {
		ret = ssv_reg_read(sd, ADR_CHIP_ID_3 - i * 4, &val);
		if (ret)
			return ret;
		put_unaligned_be32(val, sd->chip_id + i * 4);
	}
	sd->chip_id[16] = '\0';
	strim(sd->chip_id);
	return 0;
}

static int ssv_write_sram(struct ssv_dev *sd, u32 addr, const u8 *data, u32 len)
{
	struct sdio_func *func = sd->func;
	int ret;

	ret = ssv_reg_write(sd, ADR_SRAM_WRITE_ADDR, addr);
	if (ret)
		return ret;
	sdio_claim_host(func);
	sdio_writeb(func, 0x2, SDIO_REG_FN1_STATUS, &ret);
	if (!ret)
		ret = sdio_memcpy_toio(func, sd->data_port, (void *)data, len);
	if (!ret)
		sdio_writeb(func, 0, SDIO_REG_FN1_STATUS, &ret);
	sdio_release_host(func);
	return ret;
}

/* Hold the MCU in reset so that the SRAM can be rewritten. */
static int ssv_stop_mcu(struct ssv_dev *sd)
{
	int ret;

	ret = ssv_reg_set_bits(sd, ADR_PLATFORM_CLOCK_ENABLE, 0, RESET_N_CPUN10);
	ret = ret ?: ssv_reg_set_bits(sd, ADR_MANUAL_RESET_N, 0, CLK_EN_CPUN10);
	ret = ret ?: ssv_reg_set_bits(sd, ADR_BRG_SW_RST, 0, MCU_ENABLE);
	return ret ?: ssv_reg_set_bits(sd, ADR_BOOT, RG_REBOOT, RG_REBOOT);
}

static int ssv_start_mcu(struct ssv_dev *sd)
{
	int ret;

	ret = ssv_reg_set_bits(sd, ADR_N10CFG_DEF_IVB, 0, N10CFG_DEFAULT_IVB);
	ret = ret ?: ssv_reg_set_bits(sd, ADR_MANUAL_RESET_N, CLK_EN_CPUN10,
				      CLK_EN_CPUN10);
	return ret ?: ssv_reg_set_bits(sd, ADR_PLATFORM_CLOCK_ENABLE,
				       RESET_N_CPUN10, RESET_N_CPUN10);
}

static int ssv_upload_firmware(struct ssv_dev *sd, const struct firmware *fw)
{
	u32 checksum = FW_CHECKSUM_INIT, fw_checksum, blocks;
	u32 sram = 0, pos = 0;
	u8 *buf;
	int ret;

	buf = kmalloc(FW_BLOCK_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = ssv_stop_mcu(sd);
	if (ret)
		goto out;

	while (pos < fw->size) {
		u32 chunk = min_t(u32, fw->size - pos, FW_BLOCK_SIZE);
		u32 i;

		memset(buf, 0xa5, FW_BLOCK_SIZE);
		memcpy(buf, fw->data + pos, chunk);
		pos += chunk;
		/* the chip checksums whole 1 KiB blocks */
		chunk = round_up(chunk, FW_CHECKSUM_BLOCK);
		ret = ssv_write_sram(sd, sram, buf, chunk);
		if (ret)
			goto out;
		sram += chunk;
		for (i = 0; i < chunk; i += 4)
			checksum += get_unaligned_le32(buf + i);
	}

	checksum = ((checksum >> 24) + (checksum >> 16) + (checksum >> 8) +
		    checksum) & 0xff;
	checksum <<= 16;

	/* the firmware needs the larger instruction memory */
	ret = ssv_reg_set_bits(sd, ADR_SRAM_MODE, SRAM_MODE_ILM_160K,
			       SRAM_MODE_ILM_160K);
	blocks = DIV_ROUND_UP(sram, FW_CHECKSUM_BLOCK);
	ret = ret ?: ssv_reg_write(sd, ADR_TX_SEG, blocks << 16);
	ret = ret ?: ssv_start_mcu(sd);
	if (ret)
		goto out;
	msleep(50);

	ret = ssv_reg_read(sd, ADR_TX_SEG, &fw_checksum);
	if (ret)
		goto out;
	fw_checksum &= FW_STATUS_MASK;
	if (fw_checksum != checksum) {
		dev_err(sd->dev, "firmware checksum mismatch (0x%x != 0x%x)\n",
			fw_checksum, checksum);
		ret = -EIO;
		goto out;
	}
	ret = ssv_reg_write(sd, ADR_TX_SEG, ~checksum & FW_STATUS_MASK);
	msleep(50);
out:
	kfree(buf);
	return ret;
}

int ssv_load_firmware(struct ssv_dev *sd)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, SSV_FIRMWARE, sd->dev);
	if (ret) {
		dev_err(sd->dev, "cannot load %s: %d\n", SSV_FIRMWARE, ret);
		return ret;
	}
	ssv_set_bus_clock(sd, min(sd->bus_clock, SDIO_CLOCK_INIT));
	ret = ssv_upload_firmware(sd, fw);
	release_firmware(fw);
	if (!ret)
		ssv_set_bus_clock(sd, sd->bus_clock);
	return ret;
}

static void ssv_pmu_wakeup(struct ssv_dev *sd)
{
	int ret;

	sdio_claim_host(sd->func);
	sdio_writeb(sd->func, 1, SDIO_REG_PMU_WAKEUP, &ret);
	mdelay(10);
	sdio_writeb(sd->func, 0, SDIO_REG_PMU_WAKEUP, &ret);
	sdio_release_host(sd->func);
}

static int ssv_sdio_init(struct ssv_dev *sd)
{
	struct sdio_func *func = sd->func;
	u32 data = 0, reg = 0;
	int ret, i;

	sdio_claim_host(func);
	ret = sdio_enable_func(func);
	if (ret)
		goto out;
	for (i = 0; i < 3; i++) {
		data |= sdio_readb(func, SDIO_REG_DATA_PORT0 + i, &ret) << (8 * i);
		if (ret)
			goto out;
		reg |= sdio_readb(func, SDIO_REG_REG_PORT0 + i, &ret) << (8 * i);
		if (ret)
			goto out;
	}
	sd->data_port = data;
	sd->reg_port = reg;
	ret = sdio_set_block_size(func, SDIO_BLOCK_SIZE);
	if (ret)
		goto out;
	sdio_writeb(func, SDIO_OUTPUT_TIMING, SDIO_REG_OUTPUT_TIMING, &ret);
	if (!ret)
		sdio_writeb(func, 0, SDIO_REG_FN1_STATUS, &ret);
	/* the chip allocates TX buffers in units of 1 << 7 bytes */
	if (!ret)
		sdio_writeb(func, SDIO_TX_ALLOC_SHIFT | SDIO_TX_ALLOC_ENABLE,
			    SDIO_REG_TX_ALLOC, &ret);
out:
	sdio_release_host(func);
	return ret;
}

static int ssv_sdio_probe(struct sdio_func *func, const struct sdio_device_id *id)
{
	struct ssv_dev *sd;
	int ret;

	if (func->num != 1)
		return -ENODEV;

	sd = devm_kzalloc(&func->dev, sizeof(*sd), GFP_KERNEL);
	if (!sd)
		return -ENOMEM;
	sd->func = func;
	sd->dev = &func->dev;
	sd->io_buf = devm_kzalloc(&func->dev, IO_BUF_SIZE, GFP_KERNEL);
	if (!sd->io_buf)
		return -ENOMEM;
	sdio_set_drvdata(func, sd);

	func->card->quirks |= MMC_QUIRK_LENIENT_FN0 | MMC_QUIRK_BLKSZ_FOR_BYTE_MODE;
	sd->bus_clock = func->card->host->ios.clock;
	ssv_set_bus_clock(sd, min(sd->bus_clock, SDIO_CLOCK_INIT));
	ret = ssv_sdio_init(sd);
	if (ret) {
		dev_err(sd->dev, "SDIO init failed: %d\n", ret);
		return ret;
	}
	ssv_pmu_wakeup(sd);

	ret = ssv_read_chip_id(sd);
	if (ret)
		goto err;

	ret = ssv_hw_start(sd);
	if (ret)
		goto err;
	return 0;

err:
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
	return ret;
}

static void ssv_sdio_remove(struct sdio_func *func)
{
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
}

static const struct sdio_device_id ssv_sdio_ids[] = {
	{ SDIO_DEVICE(SDIO_VENDOR_ID_SSV, SDIO_DEVICE_ID_SSV_6256) },
	{ }
};
MODULE_DEVICE_TABLE(sdio, ssv_sdio_ids);

static struct sdio_driver ssv_sdio_driver = {
	.name = "ssv6256",
	.id_table = ssv_sdio_ids,
	.probe = ssv_sdio_probe,
	.remove = ssv_sdio_remove,
};
module_sdio_driver(ssv_sdio_driver);

MODULE_DESCRIPTION("iComm SSV6256 SDIO 802.11a/b/g/n driver");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(SSV_FIRMWARE);
