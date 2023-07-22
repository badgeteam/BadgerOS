
// SPDX-License-Identifier: MIT

#pragma once

#include "port/hardware.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PMU_HP_ACTIVE_DIG_POWER_REG      (PMU_BASE + 0x0)
#define PMU_HP_ACTIVE_ICG_HP_FUNC_REG    (PMU_BASE + 0x4)
#define PMU_HP_ACTIVE_ICG_HP_APB_REG     (PMU_BASE + 0x8)
#define PMU_HP_ACTIVE_ICG_MODEM_REG      (PMU_BASE + 0xc)
#define PMU_HP_ACTIVE_HP_SYS_CNTL_REG    (PMU_BASE + 0x10)
#define PMU_HP_ACTIVE_HP_CK_POWER_REG    (PMU_BASE + 0x14)
#define PMU_HP_ACTIVE_BIAS_REG           (PMU_BASE + 0x18)
#define PMU_HP_ACTIVE_BACKUP_REG         (PMU_BASE + 0x1c)
#define PMU_HP_ACTIVE_BACKUP_CLK_REG     (PMU_BASE + 0x20)
#define PMU_HP_ACTIVE_SYSCLK_REG         (PMU_BASE + 0x24)
#define PMU_HP_ACTIVE_HP_REGULATOR0_REG  (PMU_BASE + 0x28)
#define PMU_HP_ACTIVE_HP_REGULATOR1_REG  (PMU_BASE + 0x2c)
#define PMU_HP_ACTIVE_XTAL_REG           (PMU_BASE + 0x30)
#define PMU_HP_MODEM_DIG_POWER_REG       (PMU_BASE + 0x34)
#define PMU_HP_MODEM_ICG_HP_FUNC_REG     (PMU_BASE + 0x38)
#define PMU_HP_MODEM_ICG_HP_APB_REG      (PMU_BASE + 0x3c)
#define PMU_HP_MODEM_ICG_MODEM_REG       (PMU_BASE + 0x40)
#define PMU_HP_MODEM_HP_SYS_CNTL_REG     (PMU_BASE + 0x44)
#define PMU_HP_MODEM_HP_CK_POWER_REG     (PMU_BASE + 0x48)
#define PMU_HP_MODEM_BIAS_REG            (PMU_BASE + 0x4c)
#define PMU_HP_MODEM_BACKUP_REG          (PMU_BASE + 0x50)
#define PMU_HP_MODEM_BACKUP_CLK_REG      (PMU_BASE + 0x54)
#define PMU_HP_MODEM_SYSCLK_REG          (PMU_BASE + 0x58)
#define PMU_HP_MODEM_HP_REGULATOR0_REG   (PMU_BASE + 0x5c)
#define PMU_HP_MODEM_HP_REGULATOR1_REG   (PMU_BASE + 0x60)
#define PMU_HP_MODEM_XTAL_REG            (PMU_BASE + 0x64)
#define PMU_HP_SLEEP_DIG_POWER_REG       (PMU_BASE + 0x68)
#define PMU_HP_SLEEP_ICG_HP_FUNC_REG     (PMU_BASE + 0x6c)
#define PMU_HP_SLEEP_ICG_HP_APB_REG      (PMU_BASE + 0x70)
#define PMU_HP_SLEEP_ICG_MODEM_REG       (PMU_BASE + 0x74)
#define PMU_HP_SLEEP_HP_SYS_CNTL_REG     (PMU_BASE + 0x78)
#define PMU_HP_SLEEP_HP_CK_POWER_REG     (PMU_BASE + 0x7c)
#define PMU_HP_SLEEP_BIAS_REG            (PMU_BASE + 0x80)
#define PMU_HP_SLEEP_BACKUP_REG          (PMU_BASE + 0x84)
#define PMU_HP_SLEEP_BACKUP_CLK_REG      (PMU_BASE + 0x88)
#define PMU_HP_SLEEP_SYSCLK_REG          (PMU_BASE + 0x8c)
#define PMU_HP_SLEEP_HP_REGULATOR0_REG   (PMU_BASE + 0x90)
#define PMU_HP_SLEEP_HP_REGULATOR1_REG   (PMU_BASE + 0x94)
#define PMU_HP_SLEEP_XTAL_REG            (PMU_BASE + 0x98)
#define PMU_HP_SLEEP_LP_REGULATOR0_REG   (PMU_BASE + 0x9c)
#define PMU_HP_SLEEP_LP_REGULATOR1_REG   (PMU_BASE + 0xa0)
#define PMU_HP_SLEEP_LP_DCDC_RESERVE_REG (PMU_BASE + 0xa4)
#define PMU_HP_SLEEP_LP_DIG_POWER_REG    (PMU_BASE + 0xa8)
#define PMU_HP_SLEEP_LP_CK_POWER_REG     (PMU_BASE + 0xac)
#define PMU_LP_SLEEP_LP_BIAS_RESERVE_REG (PMU_BASE + 0xb0)
#define PMU_LP_SLEEP_LP_REGULATOR0_REG   (PMU_BASE + 0xb4)
#define PMU_LP_SLEEP_LP_REGULATOR1_REG   (PMU_BASE + 0xb8)
#define PMU_LP_SLEEP_XTAL_REG            (PMU_BASE + 0xbc)
#define PMU_LP_SLEEP_LP_DIG_POWER_REG    (PMU_BASE + 0xc0)
#define PMU_LP_SLEEP_LP_CK_POWER_REG     (PMU_BASE + 0xc4)
#define PMU_LP_SLEEP_BIAS_REG            (PMU_BASE + 0xc8)
#define PMU_IMM_HP_CK_POWER_REG          (PMU_BASE + 0xcc)
#define PMU_IMM_SLEEP_SYSCLK_REG         (PMU_BASE + 0xd0)
#define PMU_IMM_HP_FUNC_ICG_REG          (PMU_BASE + 0xd4)
#define PMU_IMM_HP_APB_ICG_REG           (PMU_BASE + 0xd8)
#define PMU_IMM_MODEM_ICG_REG            (PMU_BASE + 0xdc)
#define PMU_IMM_LP_ICG_REG               (PMU_BASE + 0xe0)
#define PMU_IMM_PAD_HOLD_ALL_REG         (PMU_BASE + 0xe4)
#define PMU_IMM_I2C_ISO_REG              (PMU_BASE + 0xe8)
#define PMU_POWER_WAIT_TIMER0_REG        (PMU_BASE + 0xec)
#define PMU_POWER_WAIT_TIMER1_REG        (PMU_BASE + 0xf0)
#define PMU_POWER_PD_TOP_CNTL_REG        (PMU_BASE + 0xf4)
#define PMU_POWER_PD_HPAON_CNTL_REG      (PMU_BASE + 0xf8)
#define PMU_POWER_PD_HPCPU_CNTL_REG      (PMU_BASE + 0xfc)
#define PMU_POWER_PD_HPPERI_RESERVE_REG  (PMU_BASE + 0x100)
#define PMU_POWER_PD_HPWIFI_CNTL_REG     (PMU_BASE + 0x104)
#define PMU_POWER_PD_LPPERI_CNTL_REG     (PMU_BASE + 0x108)
#define PMU_POWER_PD_MEM_CNTL_REG        (PMU_BASE + 0x10c)
#define PMU_POWER_PD_MEM_MASK_REG        (PMU_BASE + 0x110)
#define PMU_POWER_HP_PAD_REG             (PMU_BASE + 0x114)
#define PMU_POWER_VDD_SPI_CNTL_REG       (PMU_BASE + 0x118)
#define PMU_POWER_CK_WAIT_CNTL_REG       (PMU_BASE + 0x11c)
#define PMU_SLP_WAKEUP_CNTL0_REG         (PMU_BASE + 0x120)
#define PMU_SLP_WAKEUP_CNTL1_REG         (PMU_BASE + 0x124)
#define PMU_SLP_WAKEUP_CNTL2_REG         (PMU_BASE + 0x128)
#define PMU_SLP_WAKEUP_CNTL3_REG         (PMU_BASE + 0x12c)
#define PMU_SLP_WAKEUP_CNTL4_REG         (PMU_BASE + 0x130)
#define PMU_SLP_WAKEUP_CNTL5_REG         (PMU_BASE + 0x134)
#define PMU_SLP_WAKEUP_CNTL6_REG         (PMU_BASE + 0x138)
#define PMU_SLP_WAKEUP_CNTL7_REG         (PMU_BASE + 0x13c)
#define PMU_SLP_WAKEUP_STATUS0_REG       (PMU_BASE + 0x140)
#define PMU_SLP_WAKEUP_STATUS1_REG       (PMU_BASE + 0x144)
#define PMU_HP_CK_POWERON_REG            (PMU_BASE + 0x148)
#define PMU_HP_CK_CNTL_REG               (PMU_BASE + 0x14c)
#define PMU_POR_STATUS_REG               (PMU_BASE + 0x150)
#define PMU_RF_PWC_REG                   (PMU_BASE + 0x154)
#define PMU_BACKUP_CFG_REG               (PMU_BASE + 0x158)
#define PMU_INT_RAW_REG                  (PMU_BASE + 0x15c)
#define PMU_HP_INT_ST_REG                (PMU_BASE + 0x160)
#define PMU_HP_INT_ENA_REG               (PMU_BASE + 0x164)
#define PMU_HP_INT_CLR_REG               (PMU_BASE + 0x168)
#define PMU_LP_INT_RAW_REG               (PMU_BASE + 0x16c)
#define PMU_LP_INT_ST_REG                (PMU_BASE + 0x170)
#define PMU_LP_INT_ENA_REG               (PMU_BASE + 0x174)
#define PMU_LP_INT_CLR_REG               (PMU_BASE + 0x178)
#define PMU_LP_CPU_PWR0_REG              (PMU_BASE + 0x17c)
#define PMU_LP_CPU_PWR1_REG              (PMU_BASE + 0x180)
#define PMU_HP_LP_CPU_COMM_REG           (PMU_BASE + 0x184)
#define PMU_HP_REGULATOR_CFG_REG         (PMU_BASE + 0x188)
#define PMU_MAIN_STATE_REG               (PMU_BASE + 0x18c)
#define PMU_PWR_STATE_REG                (PMU_BASE + 0x190)
#define PMU_CLK_STATE0_REG               (PMU_BASE + 0x194)
#define PMU_CLK_STATE1_REG               (PMU_BASE + 0x198)
#define PMU_CLK_STATE2_REG               (PMU_BASE + 0x19c)
#define PMU_VDD_SPI_STATUS_REG           (PMU_BASE + 0x1a0)
#define PMU_DATE_REG                     (PMU_BASE + 0x3fc)