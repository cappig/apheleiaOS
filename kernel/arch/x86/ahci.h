#pragma once

#include <base/macros.h>
#include <base/types.h>
#include <sched/scheduler.h>
#include <stdbool.h>
#include <stddef.h>

#define AHCI_PCI_BAR 5

#define AHCI_PORT_COUNT 32
#define AHCI_MMIO_SIZE  0x2000

#define AHCI_SIG_ATA 0x00000101U

#define AHCI_PxCMD_ST  (1U << 0)
#define AHCI_PxCMD_SUD (1U << 1)
#define AHCI_PxCMD_POD (1U << 2)
#define AHCI_PxCMD_FRE (1U << 4)
#define AHCI_PxCMD_FR  (1U << 14)
#define AHCI_PxCMD_CR  (1U << 15)

#define AHCI_PxIS_TFES (1U << 30)

#define AHCI_HBA_IE   (1U << 1)
#define AHCI_HBA_AE   (1U << 31)
#define AHCI_CAP2_BOH (1U << 0)
#define AHCI_BOHC_BOS (1U << 0)
#define AHCI_BOHC_OOS (1U << 1)

#define AHCI_SSTS_DET_MASK    0x0fU
#define AHCI_SSTS_IPM_MASK    0x0fU
#define AHCI_SSTS_DET_PRESENT 0x03U
#define AHCI_SSTS_IPM_ACTIVE  0x01U

#define FIS_TYPE_REG_H2D 0x27
#define AHCI_FIS_H2D_C   (1U << 7)

#define ATA_CMD_IDENTIFY      0xec
#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35

#define ATA_DEV_BUSY 0x80
#define ATA_DEV_DRQ  0x08

#define AHCI_SECTOR_SIZE 512

#define AHCI_CMD_SLOT      0
#define AHCI_PRDTL         1
#define AHCI_CMDH_CFL_MASK 0x1fU
#define AHCI_CMDH_W        (1U << 6)
#define AHCI_PRDT_DBC_MASK 0x003fffffU
#define AHCI_PRDT_I        (1U << 31)

#define AHCI_DMA_PAGES         16
#define AHCI_PAGE_SIZE         4096U
#define AHCI_DMA_SIZE_BYTES    (AHCI_DMA_PAGES * AHCI_PAGE_SIZE)
#define AHCI_MAX_SECTORS       (AHCI_DMA_SIZE_BYTES / AHCI_SECTOR_SIZE)
#define AHCI_IRQ_TIMEOUT_MS    100

typedef volatile struct {
    u32 clb;
    u32 clbu;
    u32 fb;
    u32 fbu;
    u32 is;
    u32 ie;
    u32 cmd;
    u32 rsv0;
    u32 tfd;
    u32 sig;
    u32 ssts;
    u32 sctl;
    u32 serr;
    u32 sact;
    u32 ci;
    u32 sntf;
    u32 fbs;
    u32 rsv1[11];
    u32 vendor[4];
} ahci_hba_port_t;

typedef volatile struct {
    u32 cap;
    u32 ghc;
    u32 is;
    u32 pi;
    u32 vs;
    u32 ccc_ctl;
    u32 ccc_pts;
    u32 em_loc;
    u32 em_ctl;
    u32 cap2;
    u32 bohc;
    u8 rsv[0xa0 - 0x2c];
    u8 vendor[0x100 - 0xa0];
    ahci_hba_port_t ports[32];
} ahci_hba_mem_t;

typedef struct PACKED {
    u8 flags;
    u8 flags2;

    u16 prdtl;
    volatile u32 prdbc;

    u32 ctba;
    u32 ctbau;
    u32 rsv1[4];
} ahci_cmd_header_t;

typedef struct PACKED {
    u32 dba;
    u32 dbau;
    u32 rsv0;
    u32 dbc_i;
} ahci_prdt_entry_t;

typedef struct PACKED {
    u8 fis_type;

    u8 pmport_c;

    u8 command;
    u8 featurel;

    u8 lba0;
    u8 lba1;
    u8 lba2;
    u8 device;

    u8 lba3;
    u8 lba4;
    u8 lba5;
    u8 featureh;

    u8 countl;
    u8 counth;
    u8 icc;
    u8 control;

    u8 rsv1[4];
} ahci_fis_reg_h2d_t;

typedef struct PACKED {
    u8 cfis[64];
    u8 acmd[16];
    u8 rsv[48];
    ahci_prdt_entry_t prdt_entry[1];
} ahci_cmd_tbl_t;

typedef struct {
    u64 abar_paddr;
    u32 port_index;

    u64 clb_paddr;
    u64 fb_paddr;
    u64 ct_paddr;
    u64 dma_paddr;

    u8 irq_line;
    bool irq_enabled;
    volatile u64 irq_seq;

    size_t sector_size;
    size_t sector_count;

    volatile bool io_busy;
    sched_wait_queue_t io_wait;
    sched_wait_queue_t irq_wait;
} ahci_device_t;

typedef ahci_hba_port_t hba_port_t;
typedef ahci_hba_mem_t hba_mem_t;
typedef ahci_cmd_header_t hba_cmd_header_t;
typedef ahci_prdt_entry_t hba_prdt_entry_t;
typedef ahci_fis_reg_h2d_t fis_reg_h2d_t;
typedef ahci_cmd_tbl_t hba_cmd_tbl_t;

bool ahci_disk_init(void);
