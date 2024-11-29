#include "ide.h"

#include <base/types.h>
#include <boot/mbr.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <x86/asm.h>

#include "arch/idt.h"
#include "arch/irq.h"
#include "drivers/pci.h"
#include "mem/heap.h"
#include "vfs/driver.h"
#include "vfs/fs.h"


static void ata_irq_handler(int_state* s) {
    log_warn("ATA irq :: %#lx\n", s->int_num);

    irq_ack(IRQ_PRIMARY_ATA);
}

// Waste ~400 nanoseconds so that we can give the disk IO a little rest
static void ata_wait(ide_channel* channel) {
    for (usize i = 0; i < 4; i++)
        inb(channel->base + ATA_REG_ALTSTATUS);
}

// Wait until the drive is ready to execute further commands
static bool ata_wait_for_ready(ide_channel* channel) {
    ata_wait(channel);

    // Wait until the busy bit clears
    for (usize timeout = 10000; timeout > 0; timeout--) {
        u8 status = inb(channel->base + ATA_REG_STATUS);

        if (status & ATA_SR_ERROR)
            return 1;

        if (status & ATA_SR_DRIVE_FAULT)
            return 1;

        if (!(status & ATA_SR_BUSY))
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

static u64 set_ata_size(ide_device* device) {
    ata_identify* id = device->identify;

    u64 blocks = id->long_lba_sectors;

    // Fall back to short sectors if the drive doesn't support lba48
    if (blocks == 0)
        blocks = id->short_lba_sectors;

    device->sectors = blocks;

    u32 block_size = id->logical_sector_size * 2;

    // The block size field is optional, fall back to 512 if 0
    // TODO: compute the physical sector size, it's a good performance hint
    if (block_size == 0)
        block_size = ATA_SECTOR_SIZE;

    device->sector_size = block_size;

    return blocks;
}

static u64 set_atapi_size(ide_device* device) {
    ide_channel* channel = device->channel;

    outb(channel->base + ATA_REG_LBA1, 0x8);
    outb(channel->base + ATA_REG_LBA2, 0x8);

    outb(channel->base + ATA_REG_COMMAND, ATA_CMD_PACKET);

    if (ata_wait_for_ready(channel))
        return 0;

    atapi_packet packet = {{
        [0] = ATAPI_CMD_CAPACITY,
    }};

    for (int i = 0; i < 6; i++)
        outw(channel->base, packet.words[i]);

    if (ata_wait_for_ready(channel))
        return 0;

    // These values are big endian. Fucking kill me now
    u16 capacity[4];
    for (usize i = 0; i < 8 / 2; i++)
        capacity[i] = inw(channel->base + ATA_REG_DATA);

    u32 blocks = 0;
    memcpy(&blocks, capacity, sizeof(u32));

    u32 block_size = 0;
    memcpy(&block_size, &capacity[2], sizeof(u32));

    block_size = bswapl(block_size);

    // Just in case. we don't want division by zero
    if (block_size == 0)
        block_size = ATAPI_SECTOR_SIZE;

    device->sectors = bswapl(blocks);
    device->sector_size = block_size;

    return device->sectors;
}

// https://wiki.osdev.org/ATA_PIO_Mode
// NOTE: returns true if this slot is valid
static bool ata_probe_device(ide_device* device) {
    ide_channel* channel = device->channel;

    // Select the device
    u8 disk = ATA_DEVICE_LBA | (!device->is_master << 4);
    outb(channel->base + ATA_REG_DEVICE, disk);
    ata_wait(channel);

    // Assume that this is a non packet device, if it is a packet device it will abort
    // signature will be placed in the lba registers after it is executed
    outb(channel->base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_wait(channel);

    // TODO: filter other garbage values
    if (inb(channel->base + ATA_REG_STATUS) == 0)
        return false;

    // This may be a packet device since the command got aborted
    if (ata_wait_for_ready(channel)) {
        u8 cl = inb(channel->base + ATA_REG_LBA1);
        u8 ch = inb(channel->base + ATA_REG_LBA2);

        if (!TYPE_IS_ATAPI((ch << 8) | cl))
            return false;

        device->is_atapi = true;

        // Issue the correct command
        outb(channel->base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
        ata_wait(channel);
    }

    // At this point we most likely have a valid device
    device->identify = kmalloc(sizeof(ata_identify));

    for (usize i = 0; i < 256; i++)
        device->identify->raw[i] = inw(channel->base + ATA_REG_DATA);

    u64 sectors = 0;
    if (device->is_atapi)
        sectors = set_atapi_size(device);
    else
        sectors = set_ata_size(device);

    // If the disk reports 0 sectors or if an error occurred the disk isn't valid
    // If we follow this branch the identify field is left dangling but that is fine
    // since the exists field equals false rendering all other fields irrelevant
    if (sectors == 0) {
        kfree(device->identify);
        return false;
    }

    device->exists = true;

    return true;
}

static bool ata_mount_disk(virtual_fs* vfs, vfs_drive_interface* interface, ide_device* disk) {
    if (!disk->exists)
        return false;

    static u8 hd_index = 0;
    static u8 cd_index = 0;

    usize next_index = disk->is_atapi ? cd_index++ : hd_index++;

    // Name our dev file cdX if it's an optical drive or hdX if it's a regular hdd
    char name[4] = {
        disk->is_atapi ? 'c' : 'h',
        'd',
        'a' + next_index,
    };

    disk->read_buffer = kmalloc(disk->sector_size);

    vfs_driver* dev = vfs_create_device(name, disk->sector_size, disk->sectors);

    dev->private = disk;
    dev->interface = interface;
    dev->type = disk->is_atapi ? VFS_DRIVER_OPTICAL : VFS_DRIVER_HARD;

    vfs_register(vfs, "/dev", dev);

    return true;
}

// IDE IO ports are *usually* located on the standard base but this is not always the case
// The PCI BAR registers contain the actual offsets so we read them just to be sure
static void init_controller(pci_device* pci, ide_controller* controller) {
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
}

static void ata_probe(virtual_fs* vfs, vfs_drive_interface* interface, ide_controller* controller) {
    // Disable interrupts for now TODO:
    outb(controller->primary.control, ATA_CNT_NO_INT);
    outb(controller->secondary.control, ATA_CNT_NO_INT);

    // We can have four disks per controller, two on each channel. Probe Them all
    for (usize num = 0; num < 4; num++) {
        ide_channel* channel = num >= 2 ? &controller->secondary : &controller->primary;
        ide_device* device = &controller->devices[num];

        device->channel = channel;
        device->is_master = !(num & 1);

        bool valid = ata_probe_device(device);

        if (valid) {
            ata_mount_disk(vfs, interface, device);
            controller->disks++;
        }
    }
}

static bool ata_read_sector_pio(ide_device* device, usize lba, u16* buffer) {
    ide_channel* channel = device->channel;

    u8 disk = ATA_DEVICE_LBA | (!device->is_master << 4);
    outb(channel->base + ATA_REG_DEVICE, disk | (lba & 0x0f000000) >> 24);

    ata_wait(channel);

    outb(channel->base + ATA_REG_SECCOUNT0, 1);
    outb(channel->base + ATA_REG_LBA0, lba & 0x000000ff);
    outb(channel->base + ATA_REG_LBA1, (lba & 0x0000ff00) >> 8);
    outb(channel->base + ATA_REG_LBA2, (lba & 0x00ff0000) >> 16);

    outb(channel->base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    if (ata_wait_for_ready(channel))
        return false;

    // FIXME: PIO mode is mega dogshit!! Implement irqs asap
    for (usize i = 0; i < ATA_SECTOR_SIZE / 2; i++)
        buffer[i] = inw(channel->base + ATA_REG_DATA);

    return true;
}

static bool atapi_read_sector_pio(ide_device* device, usize lba, u16* buffer) {
    ide_channel* channel = device->channel;

    u8 disk = ATA_DEVICE_LBA | (!device->is_master << 4);
    outb(channel->base + ATA_REG_DEVICE, disk);

    ata_wait(channel);

    outb(channel->base + ATA_REG_LBA1, ATAPI_SECTOR_SIZE & 0xff);
    outb(channel->base + ATA_REG_LBA2, ATAPI_SECTOR_SIZE >> 8);

    outb(channel->base + ATA_REG_COMMAND, ATA_CMD_PACKET);

    if (ata_wait_for_ready(channel))
        return false;

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
        return false;

    for (usize i = 0; i < ATAPI_SECTOR_SIZE / 2; i++)
        buffer[i] = inw(channel->base + ATA_REG_DATA);

    return true;
}

static bool ide_read_sector_pio(vfs_driver* dev, usize lba, void* buffer) {
    ide_device* device = dev->private;

    if (!device->exists)
        return false;

    if (device->is_atapi)
        return atapi_read_sector_pio(device, lba, buffer);
    else
        return ata_read_sector_pio(device, lba, buffer);
}

// isize (*read)(disk_device* dev, void* dest, usize offset, usize bytes);
static isize ide_read_wrapper(vfs_driver* dev, void* dest, usize offset, usize bytes) {
    ide_device* device = dev->private;

    usize sectors = DIV_ROUND_UP(bytes, dev->sector_size);
    usize remainder = bytes % dev->sector_size;

    usize lba = DIV_ROUND_UP(offset, dev->sector_size);

    usize read = 0;
    for (usize i = 0; i < sectors; i++) {
        if (!ide_read_sector_pio(dev, lba + i, device->read_buffer)) {
            log_warn("Error reading disk");
            break;
        }

        usize len = dev->sector_size;

        if (i + 1 == sectors && remainder)
            len = remainder;

        memcpy(dest + i * dev->sector_size, device->read_buffer, len);

        read += len;
    }

#ifdef DISK_DEBUG
    log_debug("[DISK_DEBUG] IDE read: lba = %zd, bytes = %zd", lba, bytes);
#endif

    return read;
}

// TODO: keep a global linked list of detected disks and push these on
bool ide_disk_init(virtual_fs* vfs) {
    pci_device* ide_controller_pci = pci_find_device(PCI_MASS_STORAGE, PCI_MS_IDE, NULL);

    // No IDE controller found
    if (!ide_controller_pci) {
        log_error("IDE disk controller not found!");
        return false;
    }

    irq_register(IRQ_PRIMARY_ATA, ata_irq_handler);
    irq_register(IRQ_SECONDARY_ATA, ata_irq_handler);

    vfs_drive_interface* interface = kcalloc(sizeof(vfs_drive_interface));
    interface->read = ide_read_wrapper;
    interface->write = NULL;

    ide_controller* controller = kcalloc(sizeof(ide_controller));
    init_controller(ide_controller_pci, controller);
    pci_destroy_device(ide_controller_pci);

    ata_probe(vfs, interface, controller);

    log_info("Found and mounted %lu IDE disks", controller->disks);

    return (controller->disks > 0);
}
