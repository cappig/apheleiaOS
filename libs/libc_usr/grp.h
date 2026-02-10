#pragma once

#include <grp.h>

int getgrgid(gid_t gid, group_t* out);
