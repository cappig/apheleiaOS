#include "config.h"

#include <common/boot_config.h>
#include <log/log.h>
#include <stdlib.h>

#include "disk.h"

void parse_config(kernel_args_t *args, bool log_color) {
    boot_config_init(args, log_color);

    char *config = read_rootfs("/boot/loader.conf", NULL);
    if (!config) {
        log_warn("/boot/loader.conf not found, using defaults");
        return;
    }

    boot_config_parse(config, args);
    free(config);
}
