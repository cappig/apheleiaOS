#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

const char *account_uid_name(uid_t uid, char *buf, size_t len);
const char *account_gid_name(gid_t gid, char *buf, size_t len);
bool account_auth(const char *name, const char *password);
size_t account_groups(const char *name, gid_t primary_gid, gid_t *groups, size_t max_groups);
