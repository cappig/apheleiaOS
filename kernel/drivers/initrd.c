#include "initrd.h"

#include <base/addr.h>
#include <base/types.h>
#include <boot/proto.h>
#include <data/tree.h>
#include <fs/ustar.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/ramdisk.h"
#include "mem/heap.h"
#include "sys/disk.h"
#include "vfs/fs.h"

static file_system fs = {.name = "ustar"};


static vfs_node_type _get_type(char ustar_type) {
    switch (ustar_type) {
    case USTAR_TYPE_NORMAL_FILE:
    case USTAR_TYPE_CONTIGUOUS_FILE:
    case USTAR_TYPE_HARD_LINK:
        return VFS_FILE;

    case USTAR_TYPE_SYM_LINK:
        return VFS_SYMLINK;

    case USTAR_TYPE_CHAR_DEV:
    case USTAR_TYPE_FIFO:
        return VFS_CHARDEV;

    case USTAR_TYPE_BLOCK_DEV:
        return VFS_BLOCKDEV;

    case USTAR_TYPE_DIR:
        return VFS_DIR;

    default:
        return VFS_INVALID_TYPE;
    }
}

static vfs_node*
_construct_vfs_node(file_system_instance* instance, ustar_header* header, usize offset) {
    vfs_node_type type = _get_type(header->type);

    if (type == VFS_INVALID_TYPE)
        return NULL;

    // This basename is fine since the header getw overwritten on the next iteration
    char* name = basename(header->name);

    vfs_node* vnode = vfs_create_node(name, type);

    vnode->size = ustar_to_num(header->size, 11);
    vnode->inode = (u64)offset; // this points to the ustar header
    vnode->interface = fs.node_interface;
    vnode->fs = instance;

    return vnode;
}

static void _tree_build(file_system_instance* instance, vfs_node* mount) {
    ustar_header* head = kmalloc(sizeof(ustar_header));

    usize offset = 0;

    disk_dev* dev = instance->partition->disk;

    for (;;) {
        isize read = dev->interface->read(dev, head, offset, sizeof(ustar_header));

        if (read < (isize)sizeof(ustar_header))
            break;

        if (strncmp(head->ustar, "ustar", 5))
            break;

        char dir_name[101] = {0};
        strncpy(dir_name, head->name, 100);

        char* path = dirname(dir_name);
        vfs_node* parent = vfs_lookup_from(mount, path);

        vfs_node* child = _construct_vfs_node(instance, head, offset);

        if (child)
            vfs_insert_child(parent, child);
        else
            log_warn("initrd has invalid node!");

        usize file_size = ustar_to_num(head->size, 11);

        offset += (((file_size + 511) / 512) + 1) * 512;
    }
}


static isize _read(vfs_node* node, void* buf, usize offset, usize len) {
    if (offset > node->size)
        return -1;

    len = min(len, node->size - offset);

    disk_dev* dev = node->fs->partition->disk;

    usize size = min(len, node->size);
    usize loc = node->inode + sizeof(ustar_header) + offset;

    return dev->interface->read(dev, buf, loc, size);
}


static file_system_instance* _probe(disk_partition* part) {
    disk_dev* dev = part->disk;

    ustar_header* head = kmalloc(sizeof(ustar_header));

    dev->interface->read(dev, head, 0, sizeof(ustar_header));

    if (strncmp(head->ustar, "ustar", 5)) {
        kfree(head);
        return NULL;
    }

    file_system_instance* instance = kcalloc(sizeof(file_system_instance));

    instance->fs = &fs;

    return instance;
}

static bool _mount(file_system_instance* instance, vfs_node* mount) {
    if (instance->fs->id != fs.id)
        return false;

    instance->mount = mount;

    _tree_build(instance, mount);

    instance->tree_build = true;

    return true;
}


void initrd_mount(boot_handoff* handoff) {
    void* vaddr = (void*)ID_MAPPED_VADDR(handoff->initrd_loc);
    usize size = handoff->initrd_size;

    file_system_interface* fs_interface = kcalloc(sizeof(file_system_interface));

    fs_interface->probe = _probe;
    fs_interface->mount = _mount;

    fs.fs_interface = fs_interface;

    vfs_node_interface* node_interface = kcalloc(sizeof(vfs_node_interface));

    node_interface->read = _read;
    node_interface->write = NULL;

    fs.node_interface = node_interface;

    disk_dev* dev = ramdisk_init("initrd", vaddr, size, false);

    file_system_register(&fs);

    disk_register(dev);
}
