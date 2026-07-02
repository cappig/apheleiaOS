#include "ahci.h"

#include <arch/arch.h>
#include <base/macros.h>
#include <base/types.h>
#include <base/units.h>
#include <errno.h>
#include <limits.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/pci.h>
#include <sys/time.h>
#include <x86/asm.h>
#include <x86/mm/physical.h>
#if defined(__x86_64__)
#include <x86/paging64.h>
#else
#include <x86/paging32.h>
#endif

// osdevwiki provides extensive documentation on AHCI
// https://wiki.osdev.org/AHCI

typedef struct {
    ahci_device_t *primary;
    disk_dev_t *disk;
    bool loaded;
} ahci_driver_state_t;

static ahci_driver_state_t ahci_driver;

static bool ahci_port_stop(ahci_hba_port_t *port);
static void ahci_disable_dma(const ahci_device_t *dev);

const driver_desc_t ahci_driver_desc = {
    .name = "ahci",
    .deps = NULL,
    .stage = DRIVER_STAGE_STORAGE,
    .load = ahci_driver_load,
    .unload = ahci_driver_unload,
    .is_busy = ahci_driver_busy,
};

static inline u32 lo32(u64 v) {
    return (u32)(v & 0xffffffffULL);
}

static inline u32 hi32(u64 v) {
    return (u32)((v >> 32) & 0xffffffffULL);
}

static bool ahci_zero_phys(u64 paddr, size_t size) {
    void *map = arch_phys_map(paddr, size, 0);
    if (!map) {
        return false;
    }

    memset(map, 0, size);
    arch_phys_unmap(map, size);
    return true;
}

static inline u32 ahci_port_ack(ahci_hba_port_t *port) {
    if (!port) {
        return 0;
    }

    u32 is = port->is;
    port->is = is;
    return is;
}

static bool ahci_wait_port_ready(ahci_device_t *dev, u64 timeout_ticks) {
    if (!dev) {
        return false;
    }

    u64 start = arch_timer_ticks();
    size_t spins = 0;

    for (;;) {
        void *mmio = arch_phys_map(dev->abar_paddr, AHCI_MMIO_SIZE, PHYS_MAP_MMIO);
        if (!mmio) {
            return false;
        }

        ahci_hba_mem_t *hba = mmio;
        u32 tfd = hba->ports[dev->port_index].tfd;
        arch_phys_unmap(mmio, AHCI_MMIO_SIZE);

        if (!(tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ))) {
            return true;
        }

        if ((arch_timer_ticks() - start) >= timeout_ticks) {
            return false;
        }

        if (++spins > 1000000) {
            return false;
        }

        arch_cpu_relax();
    }
}

static bool ahci_disk_size(const ahci_device_t *dev, size_t *size_out) {
    if (!dev || !size_out || !dev->sector_size) {
        return false;
    }

    if (dev->sector_count > SIZE_MAX / dev->sector_size) {
        return false;
    }

    *size_out = dev->sector_count * dev->sector_size;
    return true;
}

static bool ahci_wait_cmd(ahci_device_t *dev, u32 slot_mask) {
    if (!dev) {
        return false;
    }

    u64 start = arch_timer_ticks();
    u64 timeout = ms_to_ticks(AHCI_CMD_TIMEOUT_MS);
    size_t spins = 0;

    for (;;) {
        void *mmio = arch_phys_map(dev->abar_paddr, AHCI_MMIO_SIZE, PHYS_MAP_MMIO);
        if (!mmio) {
            return false;
        }

        ahci_hba_mem_t *hba = mmio;
        ahci_hba_port_t *port = &hba->ports[dev->port_index];
        u32 pending = port->is;
        bool complete = !(port->ci & slot_mask);
        bool failed = (pending & AHCI_PxIS_TFES) != 0;

        if (complete || failed) {
            u32 status = ahci_port_ack(port);
            hba->is = 1U << dev->port_index;
            arch_phys_unmap(mmio, AHCI_MMIO_SIZE);
            return complete && !(status & AHCI_PxIS_TFES);
        }

        arch_phys_unmap(mmio, AHCI_MMIO_SIZE);

        if ((arch_timer_ticks() - start) >= timeout) {
            return false;
        }

        if (++spins > 1000000) {
            return false;
        }

        if (sched_is_running() && sched_current()) {
            sched_yield();
            continue;
        }

        arch_cpu_relax();
    }
}

static void ahci_destroy_device(ahci_device_t *dev) {
    if (!dev) {
        return;
    }

    if (ahci_driver.primary == dev) {
        ahci_driver.primary = NULL;
    }

    if (dev->io_wait.list) {
        sched_waitq_destroy(&dev->io_wait);
    }

    if (dev->dma_paddr) {
        free_frames((void *)(uintptr_t)dev->dma_paddr, AHCI_DMA_PAGES);
    }

    if (dev->ct_paddr) {
        free_frames((void *)(uintptr_t)dev->ct_paddr, 1);
    }

    if (dev->fb_paddr) {
        free_frames((void *)(uintptr_t)dev->fb_paddr, 1);
    }

    if (dev->clb_paddr) {
        free_frames((void *)(uintptr_t)dev->clb_paddr, 1);
    }

    free(dev);
}

static bool ahci_hba_reset(ahci_hba_mem_t *hba) {
    if (!hba) {
        return false;
    }

    hba->ghc |= AHCI_HBA_HR;

    u64 start = arch_timer_ticks();
    u64 timeout = ms_to_ticks(AHCI_RESET_TIMEOUT_MS);

    for (size_t spins = 0; spins < 10000000; spins++) {
        if (!(hba->ghc & AHCI_HBA_HR)) {
            return true;
        }

        if (timeout && (arch_timer_ticks() - start) >= timeout) {
            break;
        }

        arch_cpu_relax();
    }

    return false;
}

static void ahci_stop_controller(ahci_device_t *dev) {
    if (!dev) {
        return;
    }

    void *mmio = arch_phys_map(dev->abar_paddr, AHCI_MMIO_SIZE, PHYS_MAP_MMIO);
    if (!mmio) {
        return;
    }

    ahci_hba_mem_t *hba = mmio;
    ahci_hba_port_t *port = &hba->ports[dev->port_index];

    port->ie = 0;
    bool stopped = ahci_port_stop(port);
    if (!stopped) {
        stopped = ahci_hba_reset(hba);
    }

    port->is = 0xffffffffU;
    hba->ghc &= ~AHCI_HBA_IE;

    arch_phys_unmap(mmio, AHCI_MMIO_SIZE);

    if (!stopped) {
        log_error("AHCI controller did not stop cleanly");
    }
}

static void ahci_discard_device(ahci_device_t *dev) {
    if (!dev) {
        return;
    }

    ahci_stop_controller(dev);
    ahci_disable_dma(dev);
    ahci_destroy_device(dev);
}

static void ahci_lock(ahci_device_t *dev) {
    if (!dev) {
        return;
    }

    for (;;) {
        u32 wait_seq = sched_wait_seq(&dev->io_wait);
        unsigned long flags = spin_lock_irqsave(&dev->io_lock);

        if (!dev->io_busy) {
            dev->io_busy = true;
            spin_unlock_irqrestore(&dev->io_lock, flags);
            return;
        }

        spin_unlock_irqrestore(&dev->io_lock, flags);

        if (sched_is_running() && sched_current() && dev->io_wait.list) {
            (void)sched_wait_for_change(&dev->io_wait, wait_seq);
            continue;
        }

        arch_cpu_relax();
    }
}

static void ahci_unlock(ahci_device_t *dev) {
    if (!dev) {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&dev->io_lock);
    dev->io_busy = false;
    spin_unlock_irqrestore(&dev->io_lock, flags);

    if (dev->io_wait.list) {
        sched_wake_one(&dev->io_wait);
    }
}

static bool ahci_port_stop(ahci_hba_port_t *port) {
    if (!port) {
        return false;
    }

    port->cmd &= ~AHCI_PxCMD_ST;

    for (size_t i = 0; i < 1000000; i++) {
        if (!(port->cmd & AHCI_PxCMD_CR)) {
            break;
        }

        cpu_pause();
    }

    if (port->cmd & AHCI_PxCMD_CR) {
        return false;
    }

    port->cmd &= ~AHCI_PxCMD_FRE;

    for (size_t i = 0; i < 1000000; i++) {
        if (!(port->cmd & AHCI_PxCMD_FR)) {
            return true;
        }

        cpu_pause();
    }

    return false;
}

static bool ahci_port_start(ahci_hba_port_t *port) {
    if (!port) {
        return false;
    }

    for (size_t i = 0; i < 1000000; i++) {
        if (!(port->cmd & AHCI_PxCMD_CR)) {
            break;
        }

        cpu_pause();
    }

    if (port->cmd & AHCI_PxCMD_CR) {
        return false;
    }

    port->cmd |= AHCI_PxCMD_POD;
    port->cmd |= AHCI_PxCMD_SUD;
    port->cmd |= AHCI_PxCMD_FRE;
    port->cmd |= AHCI_PxCMD_ST;

    return true;
}

static void bios_handoff(ahci_hba_mem_t *hba) {
    if (!hba) {
        return;
    }

    if (!(hba->cap2 & AHCI_CAP2_BOH)) {
        return;
    }

    hba->bohc |= AHCI_BOHC_OOS;

    u64 start = arch_timer_ticks();
    u64 timeout = ms_to_ticks(AHCI_CMD_TIMEOUT_MS);

    while ((hba->bohc & AHCI_BOHC_BOS) && (arch_timer_ticks() - start) < timeout) {
        arch_cpu_relax();
    }
}

static bool ahci_port_present(ahci_hba_port_t *port) {
    if (!port) {
        return false;
    }

    u32 ssts = port->ssts;
    u8 det = (u8)(ssts & AHCI_SSTS_DET_MASK);

    if (det != AHCI_SSTS_DET_PRESENT) {
        return false;
    }

    u32 sig = port->sig;

    return sig == AHCI_SIG_ATA || sig == 0;
}

typedef struct {
    ahci_device_t *dev;
    u8 command;
    u64 lba;
    u16 sectors;
    bool write;
    size_t bytes;
} ahci_cmd_t;

static bool ahci_cmd_valid(const ahci_cmd_t *cmd) {
    if (!cmd || !cmd->dev) {
        return false;
    }

    return !cmd->bytes || cmd->sectors;
}

static bool ahci_build_header(const ahci_cmd_t *cmd) {
    ahci_device_t *dev = cmd->dev;
    bool data_cmd = cmd->bytes != 0;

    void *cl_map = arch_phys_map(dev->clb_paddr, PAGE_4KIB, 0);
    if (!cl_map) {
        return false;
    }

    ahci_cmd_header_t *cl = cl_map;
    ahci_cmd_header_t *hdr = &cl[AHCI_CMD_SLOT];
    memset(hdr, 0, sizeof(*hdr));

    const u8 fis_dword_count = (u8)(sizeof(ahci_fis_reg_h2d_t) / sizeof(u32));
    hdr->flags = fis_dword_count & AHCI_CMDH_CFL_MASK;

    if (cmd->write) {
        hdr->flags |= AHCI_CMDH_W;
    }

    hdr->prdtl = data_cmd ? AHCI_PRDTL : 0;
    hdr->ctba = lo32(dev->ct_paddr);
    hdr->ctbau = hi32(dev->ct_paddr);

    arch_phys_unmap(cl_map, PAGE_4KIB);
    return true;
}

static void ahci_fill_fis(const ahci_cmd_t *cmd, ahci_fis_reg_h2d_t *fis) {
    memset(fis, 0, sizeof(*fis));

    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = AHCI_FIS_H2D_C;
    fis->command = cmd->command;
    fis->device = (cmd->command == ATA_CMD_IDENTIFY) ? 0 : (1U << 6);

    fis->lba0 = (u8)(cmd->lba & 0xff);
    fis->lba1 = (u8)((cmd->lba >> 8) & 0xff);
    fis->lba2 = (u8)((cmd->lba >> 16) & 0xff);
    fis->lba3 = (u8)((cmd->lba >> 24) & 0xff);
    fis->lba4 = (u8)((cmd->lba >> 32) & 0xff);
    fis->lba5 = (u8)((cmd->lba >> 40) & 0xff);

    if (cmd->command == ATA_CMD_IDENTIFY) {
        fis->countl = 1;
        fis->counth = 0;
    } else {
        fis->countl = (u8)(cmd->sectors & 0xff);
        fis->counth = (u8)((cmd->sectors >> 8) & 0xff);
    }
}

static bool ahci_build_table(const ahci_cmd_t *cmd) {
    ahci_device_t *dev = cmd->dev;

    void *ct_map = arch_phys_map(dev->ct_paddr, PAGE_4KIB, 0);
    if (!ct_map) {
        return false;
    }

    ahci_cmd_tbl_t *tbl = ct_map;
    memset(tbl, 0, PAGE_4KIB);

    if (cmd->bytes) {
        tbl->prdt_entry[0].dba = lo32(dev->dma_paddr);
        tbl->prdt_entry[0].dbau = hi32(dev->dma_paddr);
        tbl->prdt_entry[0].dbc_i = (u32)(cmd->bytes - 1) & AHCI_PRDT_DBC_MASK;
    }

    ahci_fis_reg_h2d_t *fis = (ahci_fis_reg_h2d_t *)tbl->cfis;
    ahci_fill_fis(cmd, fis);

    arch_phys_unmap(ct_map, PAGE_4KIB);
    return true;
}

static bool ahci_issue_cmd(const ahci_cmd_t *cmd) {
    ahci_device_t *dev = cmd->dev;

    if (!ahci_wait_port_ready(dev, ms_to_ticks(AHCI_CMD_TIMEOUT_MS))) {
        return false;
    }

    void *mmio = arch_phys_map(dev->abar_paddr, AHCI_MMIO_SIZE, PHYS_MAP_MMIO);
    if (!mmio) {
        return false;
    }

    ahci_hba_mem_t *hba = mmio;
    ahci_hba_port_t *port = &hba->ports[dev->port_index];

    const u32 slot_mask = (1U << AHCI_CMD_SLOT);
    const u32 port_mask = (1U << dev->port_index);

    if (port->ci & slot_mask) {
        log_warn("AHCI slot %u still busy before issue (ci=%#x)", AHCI_CMD_SLOT, (unsigned int)port->ci);
        arch_phys_unmap(mmio, AHCI_MMIO_SIZE);
        return false;
    }

    hba->is = port_mask;
    port->is = 0xffffffffU;
    port->ci = slot_mask;
    arch_phys_unmap(mmio, AHCI_MMIO_SIZE);

    bool complete = ahci_wait_cmd(dev, slot_mask);
    if (!complete) {
        log_debug("AHCI command %#x failed on port %u", (unsigned int)cmd->command, (unsigned int)dev->port_index);
    }

    return complete;
}

static bool ahci_exec_cmd(ahci_device_t *dev, u8 command, u64 lba, u16 sectors, bool write, size_t bytes) {
    ahci_cmd_t cmd = {
        .dev = dev,
        .command = command,
        .lba = lba,
        .sectors = sectors,
        .write = write,
        .bytes = bytes,
    };

    if (!ahci_cmd_valid(&cmd)) {
        return false;
    }

    if (!ahci_build_header(&cmd) || !ahci_build_table(&cmd)) {
        return false;
    }

    return ahci_issue_cmd(&cmd);
}

static bool ahci_flush(ahci_device_t *dev) {
    return ahci_exec_cmd(dev, ATA_CMD_FLUSH_EXT, 0, 0, false, 0);
}

static bool ahci_identify(ahci_device_t *dev, u16 *identify) {
    if (!dev || !identify) {
        return false;
    }

    if (!ahci_exec_cmd(dev, ATA_CMD_IDENTIFY, 0, 1, false, AHCI_SECTOR_SIZE)) {
        return false;
    }

    void *dma = arch_phys_map(dev->dma_paddr, AHCI_SECTOR_SIZE, 0);
    if (!dma) {
        return false;
    }

    memcpy(identify, dma, AHCI_SECTOR_SIZE);
    arch_phys_unmap(dma, AHCI_SECTOR_SIZE);

    return true;
}

static bool ahci_transfer(ahci_device_t *dev, u64 lba, u16 sectors, void *buf, bool write) {
    if (!dev || !buf || !sectors) {
        return false;
    }

    size_t bytes = (size_t)sectors * AHCI_SECTOR_SIZE;

    if (write) {
        void *dma = arch_phys_map(dev->dma_paddr, bytes, 0);
        if (!dma) {
            return false;
        }

        memcpy(dma, buf, bytes);
        arch_phys_unmap(dma, bytes);
    }

    u8 cmd = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    if (!ahci_exec_cmd(dev, cmd, lba, sectors, write, bytes)) {
        return false;
    }

    if (!write) {
        void *dma = arch_phys_map(dev->dma_paddr, bytes, 0);
        if (!dma) {
            return false;
        }

        memcpy(buf, dma, bytes);
        arch_phys_unmap(dma, bytes);
    }

    return true;
}

static ssize_t ahci_read(disk_dev_t *disk, void *dest, size_t offset, size_t bytes) {
    if (!disk || !dest || !disk->private) {
        return -1;
    }

    ahci_device_t *dev = disk->private;
    ahci_lock(dev);

    ssize_t result = -1;
    size_t disk_size = 0;

    if (!ahci_disk_size(dev, &disk_size)) {
        result = -EOVERFLOW;
        goto done;
    }

    if (offset >= disk_size) {
        goto done_empty;
    }

    size_t left = disk_size - offset;
    if (bytes > left) {
        bytes = left;
    }

    if (!bytes) {
        goto done_empty;
    }

    u8 *out = dest;

    u64 lba = offset / dev->sector_size;
    size_t sector_off = offset % dev->sector_size;

    size_t remaining = bytes;
    u8 bounce[AHCI_SECTOR_SIZE];

    if (sector_off) {
        if (!ahci_transfer(dev, lba, 1, bounce, false)) {
            goto done;
        }

        size_t avail = dev->sector_size - sector_off;
        size_t chunk = remaining < avail ? remaining : avail;

        memcpy(out, bounce + sector_off, chunk);

        out += chunk;
        remaining -= chunk;
        lba++;
    }

    while (remaining >= dev->sector_size) {
        size_t full = remaining / dev->sector_size;
        size_t batch = full;

        if (batch > AHCI_MAX_SECTORS) {
            batch = AHCI_MAX_SECTORS;
        }

        size_t chunk = batch * dev->sector_size;
        if (!ahci_transfer(dev, lba, (u16)batch, out, false)) {
            goto done;
        }

        out += chunk;
        remaining -= chunk;
        lba += batch;
    }

    if (remaining) {
        if (!ahci_transfer(dev, lba, 1, bounce, false)) {
            goto done;
        }

        memcpy(out, bounce, remaining);
    }

    result = (ssize_t)bytes;
    goto done;

done_empty:
    result = 0;
done:
    ahci_unlock(dev);
    return result;
}

static ssize_t ahci_write(disk_dev_t *disk, void *src, size_t offset, size_t bytes) {
    if (!disk || !src || !disk->private) {
        return -1;
    }

    ahci_device_t *dev = disk->private;
    ahci_lock(dev);

    ssize_t result = -1;
    size_t disk_size = 0;

    if (!ahci_disk_size(dev, &disk_size)) {
        result = -EOVERFLOW;
        goto done;
    }

    if (offset >= disk_size) {
        goto done_empty;
    }

    size_t left = disk_size - offset;
    if (bytes > left) {
        bytes = left;
    }

    if (!bytes) {
        goto done_empty;
    }

    u8 *in = src;

    u64 lba = offset / dev->sector_size;
    size_t sector_off = offset % dev->sector_size;

    size_t remaining = bytes;
    u8 bounce[AHCI_SECTOR_SIZE];

    while (remaining) {
        size_t chunk = dev->sector_size;
        bool partial = sector_off != 0 || remaining < dev->sector_size;

        if (partial) {
            if (!ahci_transfer(dev, lba, 1, bounce, false)) {
                goto done;
            }

            chunk = dev->sector_size - sector_off;
            if (chunk > remaining) {
                chunk = remaining;
            }

            memcpy(bounce + sector_off, in, chunk);

            if (!ahci_transfer(dev, lba, 1, bounce, true)) {
                goto done;
            }
        } else {
            size_t full = remaining / dev->sector_size;
            size_t batch = full;

            if (batch > AHCI_MAX_SECTORS) {
                batch = AHCI_MAX_SECTORS;
            }

            chunk = batch * dev->sector_size;
            if (!ahci_transfer(dev, lba, (u16)batch, in, true)) {
                goto done;
            }

            lba += batch;
            in += chunk;
            remaining -= chunk;

            continue;
        }

        in += chunk;
        remaining -= chunk;
        lba++;
        sector_off = 0;
    }

    if (!ahci_flush(dev)) {
        result = -EIO;
        goto done;
    }

    result = (ssize_t)bytes;
    goto done;

done_empty:
    result = 0;
done:
    ahci_unlock(dev);
    return result;
}

static bool ahci_setup_port(ahci_device_t *dev) {
    if (!dev) {
        return false;
    }

    dev->clb_paddr = (u64)(uintptr_t)alloc_frames(1);
    dev->fb_paddr = (u64)(uintptr_t)alloc_frames(1);
    dev->ct_paddr = (u64)(uintptr_t)alloc_frames(1);
    dev->dma_paddr = (u64)(uintptr_t)alloc_frames(AHCI_DMA_PAGES);

    if (!dev->clb_paddr || !dev->fb_paddr || !dev->ct_paddr || !dev->dma_paddr) {
        return false;
    }

    bool list_zeroed = ahci_zero_phys(dev->clb_paddr, PAGE_4KIB);
    bool fis_zeroed = ahci_zero_phys(dev->fb_paddr, PAGE_4KIB);
    bool table_zeroed = ahci_zero_phys(dev->ct_paddr, PAGE_4KIB);
    bool dma_zeroed = ahci_zero_phys(dev->dma_paddr, AHCI_DMA_SIZE_BYTES);

    if (!list_zeroed || !fis_zeroed || !table_zeroed || !dma_zeroed) {
        return false;
    }

    void *mmio_map = arch_phys_map(dev->abar_paddr, AHCI_MMIO_SIZE, PHYS_MAP_MMIO);

    if (!mmio_map) {
        return false;
    }

    ahci_hba_mem_t *hba = mmio_map;
    bios_handoff(hba);

    hba->ghc |= AHCI_HBA_AE;
    hba->ghc &= ~AHCI_HBA_IE;

    ahci_hba_port_t *port = &hba->ports[dev->port_index];

    if (!ahci_port_stop(port)) {
        arch_phys_unmap(mmio_map, AHCI_MMIO_SIZE);
        return false;
    }

    port->sact = 0;
    port->ci = 0;

    port->clb = lo32(dev->clb_paddr);
    port->clbu = hi32(dev->clb_paddr);
    port->fb = lo32(dev->fb_paddr);
    port->fbu = hi32(dev->fb_paddr);
    port->serr = 0xffffffffU;
    port->is = 0xffffffffU;
    port->ie = 0;

    if (!ahci_port_start(port)) {
        arch_phys_unmap(mmio_map, AHCI_MMIO_SIZE);
        return false;
    }

    arch_phys_unmap(mmio_map, AHCI_MMIO_SIZE);
    return true;
}

static bool ahci_find_controller(ahci_device_t *dev) {
    if (!dev) {
        return false;
    }

    static const u8 subclasses[] = { PCI_MS_SATA, PCI_MS_RAID, PCI_MS_ATA };

    for (size_t s = 0; s < ARRAY_LEN(subclasses); s++) {
        pci_found_t *cursor = NULL;

        for (;;) {
            pci_found_t *node = pci_find_node(PCI_MASS_STORAGE, subclasses[s], cursor);

            if (!node) {
                break;
            }

            cursor = node;

            u32 bar5_lo = pci_read_config(node->bus, node->slot, node->func, PCI_CFG_BAR5, 4);
            log_debug("PCI command=%#x status=%#x", node->header.command, node->header.status);

            if (!bar5_lo || bar5_lo == 0xffffffffU || (bar5_lo & 1U)) {
                continue;
            }

            u64 abar = (u64)(bar5_lo & ~0x0fU);

            if ((bar5_lo & 0x6U) == 0x4U) {
                u32 bar5_hi = pci_read_config(node->bus, node->slot, node->func, PCI_CFG_BAR5 + 4, 4);
                abar |= ((u64)bar5_hi << 32);
            }

            // the current MMIO mapping path is limited to 32-bit ABARs
            if (!abar || abar > 0xffffffffULL) {
                continue;
            }

            dev->abar_paddr = abar;
            dev->bus = node->bus;
            dev->slot = node->slot;
            dev->func = node->func;

            void *mmio_map = arch_phys_map(dev->abar_paddr, AHCI_MMIO_SIZE, PHYS_MAP_MMIO);

            if (!mmio_map) {
                continue;
            }

            ahci_hba_mem_t *hba = mmio_map;
            if (!hba->cap || hba->cap == 0xffffffffU) {
                arch_phys_unmap(mmio_map, AHCI_MMIO_SIZE);
                continue;
            }

            bios_handoff(hba);
            hba->ghc |= AHCI_HBA_AE;
            hba->ghc &= ~AHCI_HBA_IE;

            u32 pi = hba->pi;

            bool found_port = false;
            u32 fallback_port = AHCI_PORT_COUNT;
            u32 first_impl_port = AHCI_PORT_COUNT;

            for (u32 i = 0; i < AHCI_PORT_COUNT; i++) {
                if (!(pi & (1U << i))) {
                    continue;
                }

                if (first_impl_port == AHCI_PORT_COUNT) {
                    first_impl_port = i;
                }

                if (ahci_port_present(&hba->ports[i])) {
                    dev->port_index = i;
                    found_port = true;
                    break;
                }

                u32 ssts = hba->ports[i].ssts;
                u8 det = (u8)(ssts & AHCI_SSTS_DET_MASK);

                if (det == AHCI_SSTS_DET_PRESENT && fallback_port == AHCI_PORT_COUNT) {
                    fallback_port = i;
                }
            }

            if (!found_port) {
                if (fallback_port != AHCI_PORT_COUNT) {
                    dev->port_index = fallback_port;
                    found_port = true;
                } else if (first_impl_port != AHCI_PORT_COUNT) {
                    dev->port_index = first_impl_port;
                    found_port = true;
                }
            }

            arch_phys_unmap(mmio_map, AHCI_MMIO_SIZE);
            if (found_port) {
                return true;
            }
        }
    }

    return false;
}

static void ahci_disable_irqs(const ahci_device_t *dev) {
    if (!dev) {
        return;
    }

    (void)pci_disable_msi(dev->bus, dev->slot, dev->func);

    u16 msix = pci_find_capability(dev->bus, dev->slot, dev->func, PCI_CAP_MSIX);
    if (msix) {
        u16 control = (u16)pci_read_config(dev->bus, dev->slot, dev->func, msix + 2, 2);
        control &= (u16) ~(1U << 15);
        control |= (u16)(1U << 14);
        pci_write_config(dev->bus, dev->slot, dev->func, msix + 2, control, 2);
    }

    u16 command = (u16)pci_read_config(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND, 2);
    command |= PCI_COMMAND_INT_DIS;
    pci_write_config(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND, command, 2);
}

static void ahci_disable_dma(const ahci_device_t *dev) {
    if (!dev) {
        return;
    }

    u16 command = (u16)pci_read_config(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND, 2);
    command &= (u16)~PCI_COMMAND_BUS_MASTER;
    pci_write_config(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND, command, 2);
}

static bool ahci_disk_init(void) {
    ahci_device_t *dev = calloc(1, sizeof(ahci_device_t));
    if (!dev) {
        return false;
    }

    sched_waitq_init(&dev->io_wait);
    spinlock_init(&dev->io_lock);
    dev->sector_size = AHCI_SECTOR_SIZE;

    if (!ahci_find_controller(dev)) {
        ahci_destroy_device(dev);
        return false;
    }

    ahci_driver.primary = dev;

    // The physical MMIO window is transient, so this driver deliberately uses
    // polling and leaves both PCI and HBA interrupt delivery disabled.
    ahci_disable_irqs(dev);
    pci_enable_bus_master(dev->bus, dev->slot, dev->func);

    if (!ahci_setup_port(dev)) {
        log_error("AHCI failed to setup port %u", (unsigned int)dev->port_index);
        ahci_discard_device(dev);
        return false;
    }

    u16 identify[256] = { 0 };

    if (!ahci_identify(dev, identify)) {
        log_error("AHCI identify failed on port %u", (unsigned int)dev->port_index);
        ahci_discard_device(dev);
        return false;
    }

    if ((identify[83] & (1U << 10)) == 0) {
        log_warn("AHCI disk does not support LBA48");
        ahci_discard_device(dev);
        return false;
    }

    u64 sector_count = (u64)identify[100] | ((u64)identify[101] << 16) | ((u64)identify[102] << 32) |
                       ((u64)identify[103] << 48);

    if (!sector_count) {
        log_error("AHCI identify reported zero sectors");
        ahci_discard_device(dev);
        return false;
    }

    if (sector_count > (u64)(SIZE_MAX / AHCI_SECTOR_SIZE)) {
        log_error("AHCI disk is too large for this build");
        ahci_discard_device(dev);
        return false;
    }

    dev->sector_count = (size_t)sector_count;

    static disk_interface_t ahci_interface = {
        .read = ahci_read,
        .write = ahci_write,
    };

    disk_dev_t *disk = calloc(1, sizeof(disk_dev_t));
    if (!disk) {
        ahci_discard_device(dev);
        return false;
    }

    disk->name = strdup("sda");
    disk->type = DISK_HARD;
    disk->sector_size = dev->sector_size;
    disk->sector_count = dev->sector_count;
    disk->interface = &ahci_interface;
    disk->private = dev;

    if (!disk->name || !disk_register(disk)) {
        free(disk->name);
        free(disk);
        ahci_discard_device(dev);
        return false;
    }

    ahci_driver.disk = disk;

    size_t disk_size = 0;
    size_t disk_mib = ahci_disk_size(dev, &disk_size) ? disk_size / MIB : 0;

    log_info(
        "AHCI initialized port %u in polling mode (%zu sectors, %zu MiB)",
        (unsigned)dev->port_index,
        dev->sector_count,
        disk_mib
    );

    return true;
}

bool ahci_driver_busy(void) {
    return ahci_driver.disk && disk_is_busy(ahci_driver.disk);
}

driver_err_t ahci_driver_load(void) {
    if (ahci_driver.loaded) {
        return DRIVER_OK;
    }

    if (!ahci_disk_init()) {
        return DRIVER_ERR_INIT_FAILED;
    }

    ahci_driver.loaded = true;
    return DRIVER_OK;
}

driver_err_t ahci_driver_unload(void) {
    if (!ahci_driver.loaded) {
        return DRIVER_OK;
    }

    if (ahci_driver_busy()) {
        return DRIVER_ERR_BUSY;
    }

    ahci_device_t *dev = ahci_driver.primary;

    if (ahci_driver.disk) {
        if (!disk_unregister(ahci_driver.disk)) {
            return DRIVER_ERR_BUSY;
        }
    }

    if (dev) {
        ahci_stop_controller(dev);
        ahci_disable_dma(dev);
    }

    if (ahci_driver.disk) {
        free(ahci_driver.disk->name);
        free(ahci_driver.disk);
        ahci_driver.disk = NULL;
    }

    if (dev) {
        ahci_destroy_device(dev);
    }

    ahci_driver.primary = NULL;
    ahci_driver.loaded = false;

    return DRIVER_OK;
}
