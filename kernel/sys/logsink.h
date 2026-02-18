#pragma once

#include <base/types.h>

void logsink_reset(void);
void logsink_add_target(const char *path);

void logsink_bind_devices(void);
void logsink_unbind_devices(void);
bool logsink_is_bound(void);
bool logsink_has_targets(void);

void logsink_write(const char *s, size_t len);
