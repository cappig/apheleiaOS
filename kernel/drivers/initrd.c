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
#include "sys/panic.h"
#include "vfs/driver.h"
#include "vfs/fs.h"


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
_construct_vfs_node(vfs_file_system* fs, vfs_driver* driver, ustar_header* header, usize offset) {
    vfs_node_type type = _get_type(header->type);

    if (type == VFS_INVALID_TYPE)
        return NULL;

    vfs_node* vnode = kcalloc(sizeof(vfs_node));

    vnode->name = basename(header->name);
    vnode->type = type;

    vnode->size = ustar_to_num(header->size, 11);
    vnode->inode = (u64)offset; // this points to the ustar header
    vnode->interface = fs->interface;
    vnode->driver = driver;

    return vnode;
}

static void _tree_build(tree_node* mount, vfs_driver* dev, vfs_file_system* fs) {
    ustar_header* head = kmalloc(sizeof(ustar_header));

    usize offset = 0;

    for (;;) {
        isize read = dev->interface->read(dev, head, offset, sizeof(ustar_header));

        if (read < (isize)sizeof(ustar_header))
            break;

        if (strncmp(head->ustar, "ustar", 5))
            break;

        // Locate the parent node for this child
        // TODO: what if the tar file doesn't declare a parent folder before it starts declating
        // children? Is that even possible? That would mount the file in the root of the mount
        char* dir_name = dirname(head->name);

        tree_node* tnode_parent = mount;

        if (dir_name) {
            tnode_parent = vfs_lookup_tree_from(mount, dir_name);
            kfree(dir_name);
        }

        vfs_node* vnode = _construct_vfs_node(fs, dev, head, offset);

        if (!vnode) {
            log_warn("initrd has invalid node!");
            goto next;
        }

        tree_node* tnode = tree_create_node(vnode);
        tree_insert_child(tnode_parent, tnode);

    next:
        usize file_size = ustar_to_num(head->size, 11);

        offset += (((file_size + 511) / 512) + 1) * 512;
    }
}


static isize _read(vfs_node* node, void* buf, usize offset, usize len) {
    if (offset == len)
        return 0;
    if (offset > len)
        return -1;

    vfs_driver* driver = node->driver;

    usize size = min(len, node->size);
    usize loc = node->inode + sizeof(ustar_header) + offset;

    return driver->interface->read(driver, buf, loc, size);
}

void initrd_mount(boot_handoff* handoff) {
    void* vaddr = (void*)ID_MAPPED_VADDR(handoff->initrd_loc);
    usize size = handoff->initrd_size;

    vfs_driver* dev = ramdisk_init("initrd", vaddr, size, false);

    vfs_node* tree_root = vfs_create_node("initrd", VFS_DIR);

    vfs_file_system* fs = kcalloc(sizeof(vfs_file_system));
    fs->interface = vfs_create_file_interface(_read, NULL);
    fs->subtree = tree_create(tree_root);

    _tree_build(fs->subtree->root, dev, fs);

    tree_node* mount = vfs_mount("/mnt", fs->subtree->root);
    assert(mount); // this is fatal since critical system files live on the initrd
}
