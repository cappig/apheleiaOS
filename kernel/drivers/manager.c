#include "manager.h"

#include <log/log.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

#include "registry.h"

#define DRIVER_REGISTRY_MAX 64

typedef struct {
    const driver_desc_t *desc;
    bool loaded;
    bool loading;
} driver_entry_t;

static driver_entry_t driver_registry[DRIVER_REGISTRY_MAX];
static size_t driver_registry_count = 0;
static bool driver_registry_ready = false;

static ssize_t _driver_find(const char *name) {
    if (!name || !name[0]) {
        return -1;
    }

    for (size_t i = 0; i < driver_registry_count; i++) {
        const driver_desc_t *desc = driver_registry[i].desc;
        if (!desc || !desc->name) {
            continue;
        }

        if (!strcmp(desc->name, name)) {
            return (ssize_t)i;
        }
    }

    return -1;
}

static bool _driver_depends_on(const driver_desc_t *desc, const char *name) {
    if (!desc || !desc->deps || !name || !name[0]) {
        return false;
    }

    for (size_t i = 0; desc->deps[i]; i++) {
        if (!strcmp(desc->deps[i], name)) {
            return true;
        }
    }

    return false;
}

static driver_err_t _driver_load_entry(size_t index) {
    if (index >= driver_registry_count) {
        return DRIVER_ERR_NOT_FOUND;
    }

    driver_entry_t *entry = &driver_registry[index];
    const driver_desc_t *desc = entry->desc;

    if (!desc || !desc->name) {
        return DRIVER_ERR_NOT_FOUND;
    }

    if (entry->loaded) {
        return DRIVER_OK;
    }

    if (entry->loading) {
        return DRIVER_ERR_DEPENDENCY;
    }

    entry->loading = true;

    if (desc->deps) {
        for (size_t i = 0; desc->deps[i]; i++) {
            driver_err_t dep_err = driver_load(desc->deps[i]);
            if (dep_err != DRIVER_OK && dep_err != DRIVER_ERR_ALREADY_LOADED) {
                entry->loading = false;
                return dep_err;
            }
        }
    }

    driver_err_t err = DRIVER_OK;
    if (desc->load) {
        err = desc->load();
        if (err == DRIVER_OK) {
            entry->loaded = true;
        }
    } else {
        err = DRIVER_ERR_UNSUPPORTED;
    }

    entry->loading = false;

    if (err == DRIVER_OK) {
        log_debug("driver loaded %s", desc->name);
    }

    return err;
}

bool driver_registry_init(void) {
    if (driver_registry_ready) {
        return true;
    }

    memset(driver_registry, 0, sizeof(driver_registry));
    driver_registry_count = 0;

    if (!register_drivers()) {
        return false;
    }

    driver_registry_ready = true;
    return true;
}

driver_err_t driver_register(const driver_desc_t *desc) {
    if (!desc || !desc->name || !desc->name[0]) {
        return DRIVER_ERR_INIT_FAILED;
    }

    if (_driver_find(desc->name) >= 0) {
        return DRIVER_OK;
    }

    if (driver_registry_count >= DRIVER_REGISTRY_MAX) {
        return DRIVER_ERR_INIT_FAILED;
    }

    driver_registry[driver_registry_count++] = (driver_entry_t){
        .desc = desc,
        .loaded = false,
        .loading = false,
    };

    return DRIVER_OK;
}

driver_err_t driver_unregister(const char *name) {
    ssize_t index = _driver_find(name);
    if (index < 0) {
        return DRIVER_ERR_NOT_FOUND;
    }

    if (driver_registry[(size_t)index].loaded) {
        return DRIVER_ERR_BUSY;
    }

    for (size_t i = (size_t)index; i + 1 < driver_registry_count; i++) {
        driver_registry[i] = driver_registry[i + 1];
    }

    driver_registry_count--;
    memset(&driver_registry[driver_registry_count], 0, sizeof(driver_registry[0]));

    return DRIVER_OK;
}

driver_err_t driver_load(const char *name) {
    ssize_t index = _driver_find(name);
    if (index < 0) {
        return DRIVER_ERR_NOT_FOUND;
    }

    driver_entry_t *entry = &driver_registry[(size_t)index];

    if (entry->loaded) {
        return DRIVER_ERR_ALREADY_LOADED;
    }

    return _driver_load_entry((size_t)index);
}

driver_err_t driver_unload(const char *name) {
    ssize_t index = _driver_find(name);
    if (index < 0) {
        return DRIVER_ERR_NOT_FOUND;
    }

    driver_entry_t *entry = &driver_registry[(size_t)index];
    const driver_desc_t *desc = entry->desc;

    if (!entry->loaded) {
        return DRIVER_ERR_NOT_LOADED;
    }

    for (size_t i = 0; i < driver_registry_count; i++) {
        if (i == (size_t)index || !driver_registry[i].loaded) {
            continue;
        }

        if (_driver_depends_on(driver_registry[i].desc, desc->name)) {
            return DRIVER_ERR_DEPENDENCY;
        }
    }

    if (desc->is_busy && desc->is_busy()) {
        return DRIVER_ERR_BUSY;
    }

    if (!desc->unload) {
        return DRIVER_ERR_UNSUPPORTED;
    }

    driver_err_t err = desc->unload();
    if (err != DRIVER_OK) {
        return err;
    }

    entry->loaded = false;
    log_debug("driver unloaded %s", desc->name);

    return DRIVER_OK;
}

static const char *_driver_stage_name(driver_stage_t stage) {
    switch (stage) {
    case DRIVER_STAGE_ARCH_EARLY:
        return "arch-early";
    case DRIVER_STAGE_STORAGE:
        return "storage";
    case DRIVER_STAGE_DEVFS:
        return "devfs";
    default:
        return "unknown";
    }
}

void driver_load_stage(driver_stage_t stage) {
    for (size_t i = 0; i < driver_registry_count; i++) {
        const driver_desc_t *desc = driver_registry[i].desc;
        if (!desc || desc->stage != stage) {
            continue;
        }

        driver_err_t err = driver_load(desc->name);

        if (err == DRIVER_OK || err == DRIVER_ERR_ALREADY_LOADED) {
            continue;
        }

        log_warn(
            "driver stage load failed (%s/%s) error=%s",
            _driver_stage_name(stage),
            desc->name,
            driver_err_string(err)
        );
    }
}

const char *driver_err_string(driver_err_t err) {
    switch (err) {
    case DRIVER_OK:
        return "ok";
    case DRIVER_ERR_NOT_FOUND:
        return "not-found";
    case DRIVER_ERR_ALREADY_LOADED:
        return "already-loaded";
    case DRIVER_ERR_NOT_LOADED:
        return "not-loaded";
    case DRIVER_ERR_DEPENDENCY:
        return "dependency";
    case DRIVER_ERR_BUSY:
        return "busy";
    case DRIVER_ERR_INIT_FAILED:
        return "init-failed";
    case DRIVER_ERR_UNSUPPORTED:
        return "unsupported";
    default:
        return "unknown";
    }
}
