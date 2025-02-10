#include "iso9660.h"

#include <base/types.h>
#include <data/tree.h>
#include <fs/iso9660.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>

#include "mem/heap.h"
#include "sys/disk.h"
#include "vfs/fs.h"

static file_system fs = {.name = "iso9660"};


static iso_volume_descriptor* _parse_pvd(disk_partition* part) {
    iso_volume_descriptor* pvd = kmalloc(sizeof(iso_volume_descriptor));

    disk_dev* dev = part->disk;

    for (u32 lba = ISO_VOLUME_START; lba < ISO_MAX_VOLUMES; lba++) {
        usize loc = part->offset + lba * ISO_SECTOR_SIZE;
        dev->interface->read(dev, pvd, loc, ISO_SECTOR_SIZE);

        if (pvd->type == ISO_PRIMARY) {
            if (strncmp(pvd->id, "CD001", 5))
                continue;

            return pvd;
        }

        if (pvd->type == ISO_TERMINATOR)
            break;
    }

    kfree(pvd);

    return NULL;
}

static vfs_node* _construct_vfs_node(file_system_instance* instance, iso_dir* dir) {
    vfs_node* vnode = vfs_create_node(NULL, 0);

    vnode->name = strndup(dir->file_id, dir->file_id_len - 2);
    vnode->type = (dir->flags & ISO_DIR_SUBDIR) ? VFS_DIR : VFS_FILE;
    vnode->size = dir->extent_size.lsb;
    vnode->inode = dir->extent_location.lsb;
    vnode->interface = fs.node_interface;
    vnode->fs = instance;

    return vnode;
}

static void
_recursive_tree_build(file_system_instance* instance, iso_dir* parent, vfs_node* parent_node) {
    u8* buffer = kmalloc(ISO_SECTOR_SIZE);

    disk_dev* dev = instance->partition->disk;

    usize loc = parent->extent_location.lsb * ISO_SECTOR_SIZE;
    dev->interface->read(dev, buffer, loc, parent->extent_size.lsb);

    usize offset = 0;
    while (offset < parent->extent_size.lsb) {
        iso_dir* record = (iso_dir*)(buffer + offset);

        if (!record->length)
            break;

        usize name_len = record->file_id_len;
        char* name = record->file_id;

        // Ignore the '.' and '..' pseudo directories
        bool is_dot = !strncmp(name, "", name_len);
        bool is_dot_dot = !strncmp(name, "\1", name_len);

        if (is_dot || is_dot_dot)
            goto next;


        vfs_node* child_node = _construct_vfs_node(instance, record);

        vfs_insert_child(parent_node, child_node);

        if (record->flags & ISO_DIR_SUBDIR)
            _recursive_tree_build(instance, record, child_node);

    next:
        offset += record->length;
    }

    kfree(buffer);
}


// ISO files are just contiguous blocks, nice and easy
static isize _read(vfs_node* node, void* buf, usize offset, usize len, u32 flags) {
    if (offset > node->size)
        return -1;

    len = min(len, node->size - offset);

    disk_dev* dev = node->fs->partition->disk;

    usize size = min(len, node->size);
    usize loc = node->inode * ISO_SECTOR_SIZE;

    return dev->interface->read(dev, buf, loc + offset, size);
}


static file_system_instance* _probe(disk_partition* part) {
    iso_volume_descriptor* pvd = _parse_pvd(part);

    if (!pvd)
        return NULL;

    file_system_instance* instance = kcalloc(sizeof(file_system_instance));

    instance->fs = &fs;
    instance->private = pvd;
    instance->partition = part;

    return instance;
}

static bool _build_tree(file_system_instance* instance) {
    if (!instance || !instance->fs)
        return false;

    if (instance->fs->id != fs.id)
        return false;

    iso_volume_descriptor* pvd = instance->private;
    iso_dir* root = (iso_dir*)pvd->root;

    vfs_node* vroot = vfs_create_node(instance->partition->name, VFS_DIR);
    vroot->fs = instance;

    instance->subtree = tree_create_rooted(vroot->tree_entry);

    _recursive_tree_build(instance, root, vroot);

    instance->tree_built = true;

    return true;
}

static bool _destroy_tree(file_system_instance* instance) {
    if (!instance || !instance->fs)
        return false;

    if (instance->fs->id != fs.id)
        return false;

    if (!instance->tree_built)
        return false;

    // TODO: implement this

    return true;
}


bool iso_init() {
    file_system_interface* fs_interface = kcalloc(sizeof(file_system_interface));

    fs_interface->probe = _probe;
    fs_interface->build_tree = _build_tree;
    fs_interface->destroy_tree = _destroy_tree;

    fs.fs_interface = fs_interface;

    vfs_node_interface* node_interface = kcalloc(sizeof(vfs_node_interface));

    node_interface->read = _read;
    node_interface->write = NULL;

    fs.node_interface = node_interface;

    file_system_register(&fs);

    return true;
}
