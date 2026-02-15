#include "ahci.h"

#include <arch/arch.h>
#include <base/macros.h>
#include <base/types.h>
#include <base/units.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <stdlib.h>
#include <string.h>

#include "mm/physical.h"
#include "sys/disk.h"
#include "sys/pci.h"
#include "x86/asm.h"
#include "x86/irq.h"
#if defined(__x86_64__)
#include "x86/paging64.h"
#else
#include "x86/paging32.h"
#endif

// OSdevWiki provides extensive documentation on AHCI -- https://wiki.osdev.org/AHCI

static ahci_device_t* ahci_primary = NULL;
static u8 ahci_primary_irq_line = 0xff;
static bool ahci_warned_irq_fallback = false;

static u64 ahci_irq_timeout_ticks(void) {
    u32 hz = arch_timer_hz();
    if (!hz)
        return 1;

    u64 ticks = ((u64)hz * AHCI_IRQ_TIMEOUT_MS + 999ULL) / 1000ULL;
    return ticks ? ticks : 1;
}

static inline u32 lo32(u64 v) {
    return (u32)(v & 0xffffffffULL);
}

static inline u32 hi32(u64 v) {
    return (u32)((v >> 32) & 0xffffffffULL);
}

static bool ahci_zero_phys(u64 paddr, size_t size) {
    void* map = arch_phys_map(paddr, size);
    if (!map)
        return false;

    memset(map, 0, size);
    arch_phys_unmap(map, size);
    return true;
}

static bool ahci_irq_line_supported(u8 irq_line) {
    return irq_line == IRQ_OPEN_10 || irq_line == IRQ_OPEN_11;
}

static inline u32 ahci_port_ack(ahci_hba_port_t* port) {
    if (!port)
        return 0;

    u32 is = port->is;
    port->is = is;
    return is;
}

static void ahci_primary_irq(UNUSED int_state_t* s) {
    ahci_device_t* dev = ahci_primary;
    u8 irq_line = ahci_primary_irq_line;

    if (dev && dev->irq_enabled) {
        unsigned long flags = arch_irq_save();
        dev->irq_seq++;
        arch_irq_restore(flags);

        if (dev->irq_wait.list)
            sched_wake_all(&dev->irq_wait);
    }

    if (irq_line <= IRQ_SECONDARY_ATA)
        irq_ack(irq_line);
}

static u64 ahci_irq_snapshot(ahci_device_t* dev) {
    if (!dev)
        return 0;

    unsigned long flags = arch_irq_save();
    u64 seq = dev->irq_seq;
    arch_irq_restore(flags);
    return seq;
}

static bool ahci_wait_irq_event(ahci_device_t* dev, u64* seq) {
    if (!dev || !seq || !dev->irq_enabled)
        return false;

    u64 start = irq_ticks();
    u64 timeout = ahci_irq_timeout_ticks();

    for (;;) {
        unsigned long flags = arch_irq_save();

        u64 now = dev->irq_seq;
        if (now != *seq) {
            *seq = now;
            arch_irq_restore(flags);
            return true;
        }

        arch_irq_restore(flags);

        if ((irq_ticks() - start) >= timeout)
            return false;

        if (sched_is_running() && sched_current()) {
            sched_thread_t* current = sched_current();
            if (current && sched_signal_has_pending(current))
                return false;

            sched_yield();
            continue;
        }

        arch_cpu_wait();
    }
}

static bool ahci_wait_port_ready(ahci_hba_port_t* port, u64 timeout_ticks) {
    if (!port)
        return false;

    u64 start = irq_ticks();
    size_t spins = 0;

    while (port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) {
        if ((irq_ticks() - start) >= timeout_ticks)
            return false;

        if (++spins > 1000000)
            return false;

        arch_cpu_wait();
    }

    return true;
}

static bool ahci_wait_cmd_poll(ahci_device_t* dev, ahci_hba_port_t* port, u32 slot_mask) {
    if (!dev || !port)
        return false;

    u64 start = irq_ticks();
    u64 timeout = ahci_irq_timeout_ticks();
    size_t spins = 0;

    for (;;) {
        u32 pending = port->is;
        if (pending & AHCI_PxIS_TFES)
            return false;

        if (!(port->ci & slot_mask))
            return true;

        if ((irq_ticks() - start) >= timeout)
            return false;

        if (++spins > 1000000)
            return false;

        arch_cpu_wait();
    }
}

static void ahci_destroy_device(ahci_device_t* dev) {
    if (!dev)
        return;

    if (ahci_primary == dev)
        ahci_primary = NULL;

    if (dev->io_wait.list)
        sched_wait_queue_destroy(&dev->io_wait);
    if (dev->irq_wait.list)
        sched_wait_queue_destroy(&dev->irq_wait);

    if (dev->dma_paddr)
        free_frames((void*)(uintptr_t)dev->dma_paddr, AHCI_DMA_PAGES);

    if (dev->ct_paddr)
        free_frames((void*)(uintptr_t)dev->ct_paddr, 1);

    if (dev->fb_paddr)
        free_frames((void*)(uintptr_t)dev->fb_paddr, 1);

    if (dev->clb_paddr)
        free_frames((void*)(uintptr_t)dev->clb_paddr, 1);

    free(dev);
}

static void ahci_lock(ahci_device_t* dev) {
    if (!dev)
        return;

    for (;;) {
        unsigned long flags = arch_irq_save();

        if (!dev->io_busy) {
            dev->io_busy = true;
            arch_irq_restore(flags);
            return;
        }

        arch_irq_restore(flags);

        if (sched_is_running() && sched_current() && dev->io_wait.list) {
            sched_block(&dev->io_wait);
            continue;
        }

        arch_cpu_wait();
    }
}

static void ahci_unlock(ahci_device_t* dev) {
    if (!dev)
        return;

    unsigned long flags = arch_irq_save();
    dev->io_busy = false;
    arch_irq_restore(flags);

    if (dev->io_wait.list)
        sched_wake_one(&dev->io_wait);
}

static bool ahci_port_stop(ahci_hba_port_t* port) {
    if (!port)
        return false;

    port->cmd &= ~AHCI_PxCMD_ST;
    port->cmd &= ~AHCI_PxCMD_FRE;

    for (size_t i = 0; i < 1000000; i++) {
        if (!(port->cmd & (AHCI_PxCMD_CR | AHCI_PxCMD_FR)))
            return true;

        cpu_pause();
    }

    return false;
}

static bool ahci_port_start(ahci_hba_port_t* port) {
    if (!port)
        return false;

    for (size_t i = 0; i < 1000000; i++) {
        if (!(port->cmd & AHCI_PxCMD_CR))
            break;

        cpu_pause();
    }

    if (port->cmd & AHCI_PxCMD_CR)
        return false;

    port->cmd |= AHCI_PxCMD_POD;
    port->cmd |= AHCI_PxCMD_SUD;
    port->cmd |= AHCI_PxCMD_FRE;
    port->cmd |= AHCI_PxCMD_ST;

    return true;
}

static void ahci_request_bios_handoff(ahci_hba_mem_t* hba) {
    if (!hba)
        return;

    if (!(hba->cap2 & AHCI_CAP2_BOH))
        return;

    hba->bohc |= AHCI_BOHC_OOS;

    u64 start = irq_ticks();
    u64 timeout = ahci_irq_timeout_ticks();

    while ((hba->bohc & AHCI_BOHC_BOS) && (irq_ticks() - start) < timeout) {
        arch_cpu_wait();
    }
}

static bool ahci_port_present(ahci_hba_port_t* port) {
    if (!port)
        return false;

    u32 ssts = port->ssts;
    u8 det = (u8)(ssts & AHCI_SSTS_DET_MASK);
    u8 ipm = (u8)((ssts >> 8) & AHCI_SSTS_IPM_MASK);

    if (det != AHCI_SSTS_DET_PRESENT)
        return false;

    if (ipm != AHCI_SSTS_IPM_ACTIVE)
        return false;

    return port->sig == AHCI_SIG_ATA;
}

static bool
ahci_exec_cmd(ahci_device_t* dev, u8 command, u64 lba, u16 sectors, bool write, size_t bytes) {
    if (!dev || !dev->irq_enabled || !sectors || !bytes)
        return false;

    void* cl_map = arch_phys_map(dev->clb_paddr, PAGE_4KIB);
    if (!cl_map)
        return false;

    ahci_cmd_header_t* cl = cl_map;
    ahci_cmd_header_t* hdr = &cl[AHCI_CMD_SLOT];
    memset(hdr, 0, sizeof(*hdr));

    const u8 fis_dword_count = (u8)(sizeof(ahci_fis_reg_h2d_t) / sizeof(u32));
    hdr->flags = fis_dword_count & AHCI_CMDH_CFL_MASK;

    if (write)
        hdr->flags |= AHCI_CMDH_W;

    hdr->prdtl = AHCI_PRDTL;
    hdr->ctba = lo32(dev->ct_paddr);
    hdr->ctbau = hi32(dev->ct_paddr);

    arch_phys_unmap(cl_map, PAGE_4KIB);

    void* ct_map = arch_phys_map(dev->ct_paddr, PAGE_4KIB);
    if (!ct_map)
        return false;

    ahci_cmd_tbl_t* tbl = ct_map;
    memset(tbl, 0, PAGE_4KIB);

    tbl->prdt_entry[0].dba = lo32(dev->dma_paddr);
    tbl->prdt_entry[0].dbau = hi32(dev->dma_paddr);
    tbl->prdt_entry[0].dbc_i = ((u32)(bytes - 1) & AHCI_PRDT_DBC_MASK) | AHCI_PRDT_I;

    ahci_fis_reg_h2d_t* fis = (ahci_fis_reg_h2d_t*)tbl->cfis;
    memset(fis, 0, sizeof(*fis));

    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = AHCI_FIS_H2D_C;
    fis->command = command;
    fis->device = (command == ATA_CMD_IDENTIFY) ? 0 : (1U << 6);

    fis->lba0 = (u8)(lba & 0xff);
    fis->lba1 = (u8)((lba >> 8) & 0xff);
    fis->lba2 = (u8)((lba >> 16) & 0xff);
    fis->lba3 = (u8)((lba >> 24) & 0xff);
    fis->lba4 = (u8)((lba >> 32) & 0xff);
    fis->lba5 = (u8)((lba >> 40) & 0xff);

    if (command == ATA_CMD_IDENTIFY) {
        fis->countl = 1;
        fis->counth = 0;
    } else {
        fis->countl = (u8)(sectors & 0xff);
        fis->counth = (u8)((sectors >> 8) & 0xff);
    }

    arch_phys_unmap(ct_map, PAGE_4KIB);

    void* mmio_map = arch_phys_map(dev->abar_paddr, AHCI_MMIO_SIZE);
    if (!mmio_map)
        return false;

    ahci_hba_mem_t* hba = mmio_map;
    ahci_hba_port_t* port = &hba->ports[dev->port_index];

    if (!ahci_wait_port_ready(port, ahci_irq_timeout_ticks())) {
        arch_phys_unmap(mmio_map, AHCI_MMIO_SIZE);
        return false;
    }

    const u32 slot_mask = (1U << AHCI_CMD_SLOT);
    const u32 port_mask = (1U << dev->port_index);

    if (port->ci & slot_mask) {
        log_warn("ahci: slot %u still busy before issue (ci=%#x)", AHCI_CMD_SLOT, port->ci);
        arch_phys_unmap(mmio_map, AHCI_MMIO_SIZE);
        return false;
    }

    u64 irq_seq = ahci_irq_snapshot(dev);

    hba->is = port_mask;
    port->is = 0xffffffffU;
    port->ci = slot_mask;

    bool ok = false;
    bool need_poll_fallback = false;

    for (;;) {
        u32 pending = port->is;
        if (pending & AHCI_PxIS_TFES)
            break;

        if (!(port->ci & slot_mask)) {
            ok = true;
            break;
        }

        if (!ahci_wait_irq_event(dev, &irq_seq)) {
            need_poll_fallback = true;
            break;
        }
    }

    if (need_poll_fallback) {
        if (!ahci_warned_irq_fallback) {
            log_warn("ahci: IRQ timeout on port %u, falling back to completion polling", dev->port_index);
            ahci_warned_irq_fallback = true;
        }

        ok = ahci_wait_cmd_poll(dev, port, slot_mask);
    }

    u32 complete_is = ahci_port_ack(port);

    hba->is = port_mask;

    if (complete_is & AHCI_PxIS_TFES)
        ok = false;

    if (!ok) {
        log_debug(
            "ahci: cmd=%#x port=%u tfd=%#x is=%#x ci=%#x sact=%#x serr=%#x cmdreg=%#x ssts=%#x",
            command,
            dev->port_index,
            port->tfd,
            complete_is,
            port->ci,
            port->sact,
            port->serr,
            port->cmd,
            port->ssts
        );
    }

    arch_phys_unmap(mmio_map, AHCI_MMIO_SIZE);
    return ok;
}

static bool ahci_identify(ahci_device_t* dev, u16* identify) {
    if (!dev || !identify)
        return false;

    if (!ahci_exec_cmd(dev, ATA_CMD_IDENTIFY, 0, 1, false, AHCI_SECTOR_SIZE))
        return false;

    void* dma = arch_phys_map(dev->dma_paddr, AHCI_SECTOR_SIZE);
    if (!dma)
        return false;

    memcpy(identify, dma, AHCI_SECTOR_SIZE);
    arch_phys_unmap(dma, AHCI_SECTOR_SIZE);

    return true;
}

static bool ahci_transfer(ahci_device_t* dev, u64 lba, u16 sectors, void* buf, bool write) {
    if (!dev || !buf || !sectors)
        return false;

    size_t bytes = (size_t)sectors * AHCI_SECTOR_SIZE;

    if (write) {
        void* dma = arch_phys_map(dev->dma_paddr, bytes);
        if (!dma)
            return false;

        memcpy(dma, buf, bytes);
        arch_phys_unmap(dma, bytes);
    }

    u8 cmd = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    if (!ahci_exec_cmd(dev, cmd, lba, sectors, write, bytes))
        return false;

    if (!write) {
        void* dma = arch_phys_map(dev->dma_paddr, bytes);
        if (!dma)
            return false;

        memcpy(buf, dma, bytes);
        arch_phys_unmap(dma, bytes);
    }

    return true;
}

static ssize_t ahci_read(disk_dev_t* disk, void* dest, size_t offset, size_t bytes) {
    if (!disk || !dest || !disk->private)
        return -1;

    ahci_device_t* dev = disk->private;
    ahci_lock(dev);

    ssize_t ret = -1;
    size_t disk_size = dev->sector_count * dev->sector_size;

    if (offset >= disk_size)
        goto done_empty;

    if (offset + bytes > disk_size)
        bytes = disk_size - offset;

    if (!bytes)
        goto done_empty;

    u8* out = dest;
    u64 lba = offset / dev->sector_size;
    size_t sector_off = offset % dev->sector_size;
    size_t remaining = bytes;
    u8 bounce[AHCI_SECTOR_SIZE];

    if (sector_off) {
        if (!ahci_transfer(dev, lba, 1, bounce, false))
            goto done;

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

        if (batch > AHCI_MAX_SECTORS)
            batch = AHCI_MAX_SECTORS;

        size_t chunk = batch * dev->sector_size;
        if (!ahci_transfer(dev, lba, (u16)batch, out, false))
            goto done;

        out += chunk;
        remaining -= chunk;
        lba += batch;
    }

    if (remaining) {
        if (!ahci_transfer(dev, lba, 1, bounce, false))
            goto done;

        memcpy(out, bounce, remaining);
    }

    ret = (ssize_t)bytes;
    goto done;

done_empty:
    ret = 0;
done:
    ahci_unlock(dev);
    return ret;
}

static ssize_t ahci_write(disk_dev_t* disk, void* src, size_t offset, size_t bytes) {
    if (!disk || !src || !disk->private)
        return -1;

    ahci_device_t* dev = disk->private;
    ahci_lock(dev);

    ssize_t ret = -1;
    size_t disk_size = dev->sector_count * dev->sector_size;

    if (offset >= disk_size)
        goto done_empty;

    if (offset + bytes > disk_size)
        bytes = disk_size - offset;

    if (!bytes)
        goto done_empty;

    u8* in = src;
    u64 lba = offset / dev->sector_size;
    size_t sector_off = offset % dev->sector_size;
    size_t remaining = bytes;
    u8 bounce[AHCI_SECTOR_SIZE];

    while (remaining) {
        size_t chunk = dev->sector_size;
        bool partial = sector_off != 0 || remaining < dev->sector_size;

        if (partial) {
            if (!ahci_transfer(dev, lba, 1, bounce, false))
                goto done;

            chunk = dev->sector_size - sector_off;
            if (chunk > remaining)
                chunk = remaining;

            memcpy(bounce + sector_off, in, chunk);

            if (!ahci_transfer(dev, lba, 1, bounce, true))
                goto done;
        } else {
            size_t full = remaining / dev->sector_size;
            size_t batch = full;

            if (batch > AHCI_MAX_SECTORS)
                batch = AHCI_MAX_SECTORS;

            chunk = batch * dev->sector_size;
            if (!ahci_transfer(dev, lba, (u16)batch, in, true))
                goto done;

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

    ret = (ssize_t)bytes;
    goto done;

done_empty:
    ret = 0;
done:
    ahci_unlock(dev);
    return ret;
}

static bool ahci_setup_port(ahci_device_t* dev) {
    if (!dev)
        return false;

    dev->clb_paddr = (u64)(uintptr_t)alloc_frames(1);
    dev->fb_paddr = (u64)(uintptr_t)alloc_frames(1);
    dev->ct_paddr = (u64)(uintptr_t)alloc_frames(1);
    dev->dma_paddr = (u64)(uintptr_t)alloc_frames(AHCI_DMA_PAGES);

    if (!dev->clb_paddr || !dev->fb_paddr || !dev->ct_paddr || !dev->dma_paddr)
        return false;

    if (!ahci_zero_phys(dev->clb_paddr, PAGE_4KIB) || !ahci_zero_phys(dev->fb_paddr, PAGE_4KIB) ||
        !ahci_zero_phys(dev->ct_paddr, PAGE_4KIB) ||
        !ahci_zero_phys(dev->dma_paddr, AHCI_DMA_SIZE_BYTES)) {
        return false;
    }

    void* mmio_map = arch_phys_map(dev->abar_paddr, AHCI_MMIO_SIZE);
    if (!mmio_map)
        return false;

    ahci_hba_mem_t* hba = mmio_map;
    ahci_request_bios_handoff(hba);

    hba->ghc |= AHCI_HBA_AE;

    if (dev->irq_enabled)
        hba->ghc |= AHCI_HBA_IE;

    ahci_hba_port_t* port = &hba->ports[dev->port_index];

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
    port->ie = dev->irq_enabled ? 0xffffffffU : 0;

    if (!ahci_port_start(port)) {
        arch_phys_unmap(mmio_map, AHCI_MMIO_SIZE);
        return false;
    }

    arch_phys_unmap(mmio_map, AHCI_MMIO_SIZE);
    return true;
}

static bool ahci_find_controller(ahci_device_t* dev) {
    if (!dev)
        return false;

    pci_device_t* cursor = NULL;

    for (;;) {
        pci_device_t* current = pci_find_device(PCI_MASS_STORAGE, PCI_MS_SATA, cursor);

        if (cursor)
            pci_destroy_device(cursor);

        cursor = NULL;

        if (!current)
            return false;

        if (current->header.prog_if != 0x01) {
            cursor = current;
            continue;
        }

        log_debug(
            "ahci: pci command=%#x status=%#x int_line=%u",
            current->header.command,
            current->header.status,
            current->generic.int_line
        );

        u32 abar = current->generic.bar5 & ~0x0fU;
        if (!abar) {
            cursor = current;
            continue;
        }

        dev->abar_paddr = abar;
        dev->irq_line = current->generic.int_line;

        void* mmio_map = arch_phys_map(dev->abar_paddr, AHCI_MMIO_SIZE);
        if (!mmio_map) {
            cursor = current;
            continue;
        }

        ahci_hba_mem_t* hba = mmio_map;
        u32 pi = hba->pi;

        bool found_port = false;

        for (u32 i = 0; i < AHCI_PORT_COUNT; i++) {
            if (!(pi & (1U << i)))
                continue;

            if (ahci_port_present(&hba->ports[i])) {
                dev->port_index = i;
                found_port = true;
                break;
            }
        }

        arch_phys_unmap(mmio_map, AHCI_MMIO_SIZE);
        if (found_port) {
            pci_destroy_device(current);
            return true;
        }

        cursor = current;
    }
}

bool ahci_disk_init(void) {
    ahci_device_t* dev = calloc(1, sizeof(ahci_device_t));
    if (!dev)
        return false;

    sched_wait_queue_init(&dev->io_wait);
    sched_wait_queue_init(&dev->irq_wait);
    dev->sector_size = AHCI_SECTOR_SIZE;

    if (!ahci_find_controller(dev)) {
        ahci_destroy_device(dev);
        return false;
    }

    if (!ahci_irq_line_supported(dev->irq_line)) {
        log_error("ahci: unsupported IRQ line %u", dev->irq_line);
        ahci_destroy_device(dev);
        return false;
    }

    ahci_primary = dev;
    ahci_primary_irq_line = dev->irq_line;
    dev->irq_enabled = true;

    irq_register(dev->irq_line, ahci_primary_irq);

    if (!ahci_setup_port(dev)) {
        log_error("ahci: failed to setup port %u", dev->port_index);
        ahci_destroy_device(dev);
        return false;
    }

    u16 identify[256] = {0};

    if (!ahci_identify(dev, identify)) {
        log_error("ahci: identify failed on port %u", dev->port_index);
        ahci_destroy_device(dev);
        return false;
    }

    u64 sector_count = 0;
    if (identify[83] & (1U << 10))
        sector_count = (u64)identify[100] | ((u64)identify[101] << 16) | ((u64)identify[102] << 32) | ((u64)identify[103] << 48);

    if (!sector_count)
        sector_count = (u64)identify[60] | ((u64)identify[61] << 16);

    if (!sector_count) {
        log_error("ahci: identify reported zero sectors");
        ahci_destroy_device(dev);
        return false;
    }

    dev->sector_count = (size_t)sector_count;

    static disk_interface_t ahci_interface = {
        .read = ahci_read,
        .write = ahci_write,
    };

    disk_dev_t* disk = calloc(1, sizeof(disk_dev_t));
    if (!disk) {
        ahci_destroy_device(dev);
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
        ahci_destroy_device(dev);
        return false;
    }

    log_info(
        "ahci: initialized port %u irq=%u (%zu sectors, %zu MiB)",
        dev->port_index,
        dev->irq_line,
        dev->sector_count,
        (size_t)((dev->sector_count * dev->sector_size) / MIB)
    );

    return true;
}
