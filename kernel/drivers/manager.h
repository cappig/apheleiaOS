#pragma once

#include <base/attributes.h>
#include <base/types.h>

typedef enum {
    DRIVER_OK = 0,
    DRIVER_ERR_NOT_FOUND,
    DRIVER_ERR_ALREADY_LOADED,
    DRIVER_ERR_NOT_LOADED,
    DRIVER_ERR_DEPENDENCY,
    DRIVER_ERR_BUSY,
    DRIVER_ERR_INIT_FAILED,
    DRIVER_ERR_UNSUPPORTED,
} driver_err_t;

typedef enum {
    DRIVER_STAGE_ARCH_EARLY = 0,
    DRIVER_STAGE_STORAGE,
    DRIVER_STAGE_DEVFS,
} driver_stage_t;

typedef struct {
    const char *name;
    const char *const *deps;
    driver_stage_t stage;
    driver_err_t (*load)(void);
    driver_err_t (*unload)(void);
    bool (*is_busy)(void);
} driver_desc_t;

MUST_USE bool driver_registry_init(void);
MUST_USE driver_err_t driver_register(const driver_desc_t *desc);
MUST_USE driver_err_t driver_unregister(const char *name);
MUST_USE driver_err_t driver_load(const char *name);
MUST_USE driver_err_t driver_unload(const char *name);
void driver_load_stage(driver_stage_t stage);
const char *driver_err_string(driver_err_t err);
