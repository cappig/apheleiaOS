#pragma once

#include <lib/boot.h>
#include <stdbool.h>

void boot_config_init(kernel_args_t *args, bool log_color);
void boot_config_parse(char *text, kernel_args_t *args);
