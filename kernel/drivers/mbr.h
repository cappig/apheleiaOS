#pragma once

#include <boot/mbr.h>

#include "vfs/driver.h"


char* mbr_type_string(enum mbr_partition_type type);
void dump_mbr(mbr_table* table);

mbr_table* parse_mbr(vfs_driver* dev);
