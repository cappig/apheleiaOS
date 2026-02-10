#pragma once

#include <pwd.h>

int getpwnam(const char* name, passwd_t* out);
int getpwuid(uid_t uid, passwd_t* out);
