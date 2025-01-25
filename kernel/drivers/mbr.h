#pragma once

#include <boot/mbr.h>

#include "sys/disk.h"


char* mbr_type_string(enum mbr_partition_type type);
void dump_mbr(mbr_table* table);

bool mbr_is_empty(mbr_table* table);

mbr_table* parse_mbr(disk_dev* dev);
