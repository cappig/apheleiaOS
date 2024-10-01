#include "zero.h"

#include <base/types.h>
#include <string.h>

#include "vfs/fs.h"


static isize _read_zero(vfs_node* node, void* buf, usize offset, usize len) {
    if (!buf)
        return -1;

    memset(buf, 0x00, len);
    return len;
}

static isize _write_zero(vfs_node* node, void* buf, usize offset, usize len) {
    return len;
}

static isize _read_null(vfs_node* node, void* buf, usize offset, usize len) {
    return VFS_EOF;
}

static isize _write_null(vfs_node* node, void* buf, usize offset, usize len) {
    return len;
}


void init_zero_devs(virtual_fs* vfs) {
    vfs_node* zero_dev = vfs_create_node("zero", VFS_CHARDEV);
    zero_dev->interface = vfs_create_file_interface(_read_zero, _write_zero);

    vfs_mount(vfs, "/dev", tree_create_node(zero_dev));

    vfs_node* null_dev = vfs_create_node("null", VFS_CHARDEV);
    null_dev->interface = vfs_create_file_interface(_read_null, _write_null);

    vfs_mount(vfs, "/dev", tree_create_node(null_dev));
}
