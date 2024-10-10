#pragma once

#include <boot/proto.h>
#include <parse/cfg.h>

#include "disk.h"


void parse_config(file_handle* file, boot_args* args);
