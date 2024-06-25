#pragma once

#include <boot/mbr.h>

#include "vfs/driver.h"


char* mbr_type_string(enum mbr_partition_type type);
void dump_mbr(mbr_table* table);

void mbr_parse(vfs_driver* dev, mbr_table* table);

bool validate_mbr(mbr_table* table);
