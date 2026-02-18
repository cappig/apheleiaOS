#pragma once

#include <stddef.h>
#include <sys/types.h>

const char *account_uid_name(uid_t uid, char *buf, size_t len);
const char *account_gid_name(gid_t gid, char *buf, size_t len);
