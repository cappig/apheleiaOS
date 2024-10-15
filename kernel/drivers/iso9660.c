#include "iso9660.h"

#include <base/types.h>
#include <data/tree.h>
#include <fs/iso9660.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>

#include "mem/heap.h"
#include "vfs/driver.h"
#include "vfs/fs.h"


static iso_device_private* _parse_pvd(vfs_driver* dev, vfs_file_system* fs) {
    iso_volume_descriptor* pvd = kmalloc(sizeof(iso_volume_descriptor));

    for (u32 lba = ISO_VOLUME_START; lba < ISO_MAX_VOLUMES; lba++) {
        usize loc = fs->partition.offset + lba * ISO_SECTOR_SIZE;
        dev->interface->read(dev, pvd, loc, ISO_SECTOR_SIZE);

        if (pvd->type == ISO_PRIMARY) {
            iso_device_private* priv = kcalloc(sizeof(iso_device_private));
            priv->pvd = pvd;

            return priv;
        }

        if (pvd->type == ISO_TERMINATOR)
            break;
    }

    kfree(pvd);

    return NULL;
}

static vfs_node* _construct_vfs_node(vfs_file_system* fs, iso_dir* dir, vfs_driver* driver) {
    vfs_node* vnode = kcalloc(sizeof(vfs_node));

    vnode->name = strndup(dir->file_id, dir->file_id_len - 2);
    vnode->type = (dir->flags & ISO_DIR_SUBDIR) ? VFS_DIR : VFS_FILE;
    vnode->size = dir->extent_size.lsb;
    vnode->inode = dir->extent_location.lsb;
    vnode->interface = fs->interface;
    vnode->driver = driver;

    return vnode;
}

static void
_recursive_tree_build(vfs_driver* dev, vfs_file_system* fs, iso_dir* parent, tree_node* tree_parent) {
    u8* buffer = kmalloc(ISO_SECTOR_SIZE);

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

        vfs_node* vnode = _construct_vfs_node(fs, record, dev);

        tree_node* tree_child = tree_create_node(vnode);
        tree_insert_child(tree_parent, tree_child);

        if (record->flags & ISO_DIR_SUBDIR)
            _recursive_tree_build(dev, fs, record, tree_child);

    next:
        offset += record->length;
    }

    kfree(buffer);
}


// ISO files are just contiguous blocks of disk, nice and easy
static isize _read(vfs_node* node, void* buf, usize offset, usize len) {
    if (offset == len)
        return 0;
    if (offset > len)
        return -1;

    vfs_driver* driver = node->driver;

    usize size = min(len, node->size);
    usize loc = node->inode * ISO_SECTOR_SIZE;

    return driver->interface->read(driver, buf, loc + offset, size);
}

bool iso_init(vfs_driver* dev, vfs_file_system* fs) {
    iso_device_private* priv = _parse_pvd(dev, fs);

    if (!priv)
        return false;

    vfs_node* tree_root = vfs_create_node(fs->partition.name, VFS_MOUNT);
    fs->subtree = tree_create(tree_root);

    fs->interface = vfs_create_file_interface(_read, NULL);

    iso_dir* root = (iso_dir*)priv->pvd->root;
    _recursive_tree_build(dev, fs, root, fs->subtree->root);

    return true;
}
