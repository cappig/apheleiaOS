#include "ide.h"

#include <base/types.h>
#include <boot/mbr.h>
#include <log/log.h>
#include <string.h>
#include <x86/asm.h>

#include "arch/idt.h"
#include "mem/heap.h"
#include "pci.h"
#include "vfs/driver.h"
#include "vfs/fs.h"

// If we read sequential sectors from an ATAPI device with sector
// size 2048 we would end up reading the same sector 4 times
// To optimize this we cache ATAPI reads
// TODO: invalidate a cache blocks when we write
static atapi_cache cache = {0};


static bool cache_has_sector(usize lba) {
    for (usize i = 0; i < 4; i++)
        if (cache.lba[i] == lba && cache.valid[i])
            return true;

    return false;
}

static inline u16* get_cache_sector(usize lba) {
    return &cache.buffer[(lba % 4) * ATA_SECTOR_SIZE];
}

static void cache_sector(usize lba) {
    for (usize i = 0; i < 4; i++) {
        cache.lba[i] = lba + i;
        cache.valid[i] = true;
    }
}


static void ata_irq_handler(int_state* s) {
    log_warn("ATA irq :: %#lx\n", s->int_num);
}

// Waste 400 nanoseconds so that we can give the disk IO a little rest
static void ata_wait(ide_channel* channel) {
    for (usize i = 0; i < 4; i++)
        inb(channel->base + ATA_REG_ALTSTATUS);
}

// Wait until the drive is ready to execute further commands
static bool ata_wait_for_ready(ide_channel* channel) {
    ata_wait(channel);

    // Wait until the busy bit clears i.e. the disk is ready to be read
    for (usize timeout = 100000; timeout > 0; timeout--) {
        u8 status = inb(channel->base + ATA_REG_STATUS);

        if (status & ATA_SR_ERROR)
            return 1;

        if (!(status & ATA_SR_BUSY))
            // if (!(status & ATA_SR_BUSY) && (status & ATA_SR_DATA_REQEST_READY))
            return 0;
    }

    return 1;
}

// For some unbelievably retarded reason ATA strings swap each pair of characters.
// This is defined in section 3.3.10 of the ATA spec
static void fix_ata_string(char* string, usize len) {
    for (usize i = 0; i < len; i += 2) {
        char tmp = string[i + 1];
        string[i + 1] = string[i];
        string[i] = tmp;
    }
}

// https://wiki.osdev.org/ATA_PIO_Mode
static bool ata_probe_device(ide_device* device) {
    ide_channel* channel = device->channel;

    // Select the device
    u8 disk = device->is_master ? ATA_MASTER : ATA_SLAVE;
    outb(channel->base + ATA_REG_HDDEVSEL, disk);

    ata_wait(channel);

    // Perform a software reset so that we can read the
    // initial device state (as defined in table 206)
    outb(channel->control, ATA_CNT_RESET);

    // Disable interrupts because we use PIO mode to read the ID packet
    outb(channel->control, ATA_CNT_NO_INT);

    if (ata_wait_for_ready(channel)) {
        log_error("IDE disk error while waiting for data!");
        return 0;
    }

    // Sanity check
    if (inb(channel->base + ATA_REG_SECCOUNT0) != 0x1)
        return 0;

    // Check if the drive supports the PACKET command i.e. if it's a optical drive
    // This is defined in section 9.12 of the spec
    u8 cl = inb(channel->base + ATA_REG_LBA1);
    u8 ch = inb(channel->base + ATA_REG_LBA2);

    device->type = (ata_device_type)((ch << 8) | cl);

    // Defined in standard Table 206 (page 347)
    u8 id_cmd;
    if (TYPE_IS_ATAPI(device->type))
        id_cmd = ATA_CMD_IDENTIFY_PACKET;
    else if (TYPE_IS_ATA(device->type))
        id_cmd = ATA_CMD_IDENTIFY;
    else
        return 0;

    // At this point we have a valid device
    outb(channel->base + ATA_REG_COMMAND, id_cmd);

    if (ata_wait_for_ready(channel)) {
        log_error("IDE disk error while waiting for data!");
        return 0;
    }

    // Read the IDENTIFY packet
    ata_identify* id = kmalloc(sizeof(ata_identify));
    for (usize i = 0; i < 256; i++)
        id->raw[i] = inw(channel->base + ATA_REG_DATA);

    // The only string we care about is the model ID, fix it up
    fix_ata_string(id->model, 40);

    device->exists = true;
    device->identify = id;

    return 1;
}

// NOTE: the device must be initialized when this gets called
static usize compute_disk_size(ide_device* dev) {
    if (!dev->exists)
        return 0;

    u64 blocks = dev->identify->long_lba_sectors;
    if (blocks == 0)
        blocks = dev->identify->short_lba_sectors;

    return blocks * (TYPE_IS_ATA(dev->type) ? ATA_SECTOR_SIZE : ATAPI_SECTOR_SIZE);
}

static void ata_probe_controller(pci_device* pci, ide_controller* controller) {
    // IDE IO ports are *usually* located on the standard base but this is not always the case
    // The PCI BAR registers contain the actual offsets so we read them just to be sure
    controller->primary = (ide_channel){
        .base = (pci->generic.bar0 & 0xfffffffc) + ATA_PRIMARY_BASE * (!pci->generic.bar0),
        .control = (pci->generic.bar1 & 0xfffffffc) + ATA_PRIMARY_CONTROL * (!pci->generic.bar1),
        .bus_master = (pci->generic.bar4 & 0xfffffffc),
    };
    controller->secondary = (ide_channel){
        .base = (pci->generic.bar2 & 0xfffffffc) + ATA_SECONDARY_BASE * (!pci->generic.bar2),
        .control = (pci->generic.bar3 & 0xfffffffc) + ATA_SECONDARY_CONTROL * (!pci->generic.bar3),
        .bus_master = (pci->generic.bar4 & 0xfffffffc) + 8,
    };

    // We can have four disks per controller, two on each channel. Probe Them all
    for (usize num = 0; num < 4; num++) {
        ide_channel* channel = num >= 2 ? &controller->secondary : &controller->primary;
        ide_device* device = &controller->devices[num];

        device->channel = channel;
        device->is_master = !(num & 1);

        if (ata_probe_device(device))
            controller->disks++;
    }
}

static bool ata_read_sector_pio(ide_device* device, usize lba, u16* buffer) {
    ide_channel* channel = device->channel;

    u8 disk = device->is_master ? ATA_MASTER : ATA_SLAVE;
    outb(channel->base + ATA_REG_HDDEVSEL, disk | (u8)(lba >> 24));

    ata_wait(channel);

    outb(channel->base + ATA_REG_SECCOUNT0, 1);
    outb(channel->base + ATA_REG_FEATURES, 0x00);
    outb(channel->base + ATA_REG_LBA0, (u8)(lba));
    outb(channel->base + ATA_REG_LBA1, (u8)(lba >> 8));
    outb(channel->base + ATA_REG_LBA2, (u8)(lba >> 16));

    outb(channel->base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    if (ata_wait_for_ready(channel))
        return 1;

    // FIXME: PIO mode is mega dogshit!! Implement irqs asap
    for (usize i = 0; i < ATA_SECTOR_SIZE / 2; i++)
        buffer[i] = inw(channel->base + ATA_REG_DATA);

    return 0;
}

static bool atapi_read_sector_pio(ide_device* device, usize lba, u16* buffer) {
    ide_channel* channel = device->channel;

    u8 disk = device->is_master ? ATA_MASTER : ATA_SLAVE;
    outb(channel->base + ATA_REG_HDDEVSEL, disk);

    ata_wait(channel);

    outb(channel->base + ATA_REG_FEATURES, 0x00);
    outb(channel->base + ATA_REG_LBA1, ATAPI_SECTOR_SIZE & 0xff);
    outb(channel->base + ATA_REG_LBA2, ATAPI_SECTOR_SIZE >> 8);

    outb(channel->base + ATA_REG_COMMAND, ATA_CMD_PACKET);

    atapi_packet packet = {{
        [0] = ATAPI_CMD_READ,
        [2] = (lba >> 24) & 0xff,
        [3] = (lba >> 16) & 0xff,
        [4] = (lba >> 8) & 0xff,
        [5] = (lba >> 0) & 0xff,
        [9] = 1,
    }};

    for (int i = 0; i < 6; i++)
        outw(channel->base, packet.words[i]);

    if (ata_wait_for_ready(channel))
        return 1;

    for (usize i = 0; i < ATAPI_SECTOR_SIZE / 2; i++)
        buffer[i] = inw(channel->base + ATA_REG_DATA);

    return 0;
}

static bool ide_read_sector_pio(vfs_driver* dev, usize lba, void* buffer) {
    ide_device* device = dev->private;

    if (!device->exists)
        return 1;

    // Is the requested sector in the cache
    if (cache_has_sector(lba))
        return get_cache_sector(lba);

    if (TYPE_IS_ATA(device->type)) {
        return ata_read_sector_pio(device, lba, (u16*)buffer);
    } else {
        if (atapi_read_sector_pio(device, lba / 4, cache.buffer))
            return 1;

        // We only cache large ATAPI sectors
        cache_sector(lba);

        memcpy(buffer, get_cache_sector(lba), ATA_SECTOR_SIZE);

        return 0;
    }
}

// isize (*read)(disk_device* dev, void* dest, usize offset, usize bytes);
static isize ide_read_wrapper(vfs_driver* dev, void* dest, usize offset, usize bytes) {
    usize sectors = DIV_ROUND_UP(bytes, dev->sector_size);
    usize lba = DIV_ROUND_UP(offset, dev->sector_size);

    void* buffer = kmalloc(sectors * dev->sector_size);

    for (usize i = 0; i < sectors; i++)
        ide_read_sector_pio(dev, lba, buffer + i * dev->sector_size);

    memcpy(dest, buffer + offset, bytes);

    return 1;
}

// TODO: keep a global linked list of detected disks and push these on
bool ide_disk_init(virtual_fs* vfs) {
    pci_device* ide_controller_pci = pci_find_device(PCI_MASS_STORAGE, PCI_MS_IDE, NULL);

    // No IDE controller found
    if (ide_controller_pci == NULL) {
        log_error("IDE disk controller not found!");
        return 1;
    }

    set_int_handler(IRQ_NUMBER(IRQ_PRIMARY_ATA), ata_irq_handler);
    set_int_handler(IRQ_NUMBER(IRQ_SECONDARY_ATA), ata_irq_handler);

    ide_controller* controller = kcalloc(sizeof(ide_controller));
    ata_probe_controller(ide_controller_pci, controller);

    cache.buffer = kmalloc(ATAPI_SECTOR_SIZE);

    vfs_drive_interface* interface = kcalloc(sizeof(vfs_drive_interface));
    interface->read = ide_read_wrapper;
    interface->write = NULL;
    interface->init = NULL;

    usize hd_index = 0;
    usize cd_index = 0;

    for (usize i = 0; i < 4; i++) {
        ide_device* disk = &controller->devices[i];

        if (!disk->exists)
            continue;

        log_debug("IDE disk %zd has model id: %.40s", i, disk->identify->model);

        bool is_cd = TYPE_IS_ATAPI(disk->type);
        usize next_index = is_cd ? cd_index++ : hd_index++;

        // Name our dev file cdX if it's an optical drive or hdX if it's a regular hdd
        char name[4] = {
            is_cd ? 'c' : 'h',
            'd',
            'a' + next_index,
        };

        ide_device* ide_dev = &controller->devices[i];
        usize disk_size = compute_disk_size(ide_dev);

        vfs_driver* dev = vfs_create_device(name, ATA_SECTOR_SIZE, disk_size);

        dev->private = ide_dev;
        dev->back_interface = interface;

        vfs_mount(vfs, "/dev", vfs_create_node(name, VFS_BLOCKDEV));
        log_debug("Mounted /dev/%s", name);
    }

    log_info("Found and mounted %lu IDE disks", controller->disks);

    pci_destroy_device(ide_controller_pci);

    return !(controller->disks > 0);
}
