/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SSV6256 (iComm "Turismo") register map: only what the driver uses.
 *
 * From the vendor driver:
 * Copyright (c) 2015 South Silicon Valley Microelectronics Inc.
 * Copyright (c) 2015 iComm Corporation
 */
#ifndef SSV6256_REG_H
#define SSV6256_REG_H

/* Register blocks */
#define SYS_REG_BASE				0xc0000000
#define SPI_REG_BASE				0xc0000a00

/* Registers */
#define ADR_BRG_SW_RST				(SYS_REG_BASE + 0x0)
#define ADR_BOOT				(SYS_REG_BASE + 0x4)
#define ADR_CHIP_ID_3				(SYS_REG_BASE + 0x14)
#define ADR_PLATFORM_CLOCK_ENABLE		(SYS_REG_BASE + 0x1c)
#define ADR_SRAM_MODE				(SYS_REG_BASE + 0x128)
#define ADR_MANUAL_RESET_N			(SYS_REG_BASE + 0xb4)
#define ADR_N10CFG_DEF_IVB			(SYS_REG_BASE + 0xe8)
#define ADR_SRAM_WRITE_ADDR			(SYS_REG_BASE + 0x860)
#define ADR_TX_SEG				(SPI_REG_BASE + 0x10)

/* Register fields */
#define MCU_ENABLE				BIT(0)
#define RG_REBOOT				BIT(0)
#define CLK_EN_CPUN10				BIT(1)
#define SRAM_MODE_ILM_160K			BIT(1)
#define RESET_N_CPUN10				BIT(24)
#define N10CFG_DEFAULT_IVB			GENMASK(15, 0)

#endif
