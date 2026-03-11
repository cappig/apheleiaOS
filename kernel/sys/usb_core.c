#include "usb.h"

#include <arch/arch.h>
#include <data/vector.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>
#include <sys/time.h>

#include "usb_internal.h"

typedef struct {
    size_t id;
    usb_hcd_info_t info;
    bool has_ops;
    usb_hcd_ops_t ops;
    u8 next_address;
} usb_hcd_state_t;

struct usb_device {
    size_t id;
    size_t hcd_id;
    size_t port;
    bool connected;
    u64 generation;
    usb_speed_t speed;
    bool identity_valid;
    usb_device_identity_t identity;
    void *hcd_device;
    const usb_class_driver_t *bound_driver;
    bool bind_attempted;
    bool enum_attempted;
    bool enum_queued;
};

typedef struct {
    size_t hcd_id;
    size_t port;
    u64 generation;
} usb_enum_task_t;

typedef struct {
    const usb_class_driver_t *driver;
    usb_device_handle_t dev;
} usb_detach_task_t;

typedef struct {
    void (*release)(size_t hcd_id, size_t port, void *ctx);
    size_t hcd_id;
    size_t port;
    void *ctx;
} usb_release_task_t;

static vector_t *usb_hcds = NULL;
static vector_t *usb_devices = NULL;
static vector_t *usb_class_drivers = NULL;
static vector_t *usb_enum_tasks = NULL;

static size_t next_hcd_id = 1;
static size_t next_device_id = 1;

static spinlock_t usb_state_lock = SPINLOCK_INIT;
static sched_wait_queue_t usb_enum_wait = {0};
static bool usb_enum_wait_ready = false;
static sched_thread_t *usb_enum_thread = NULL;
static bool usb_core_ready = false;


static const char *_usb_hcd_kind_name(usb_hcd_kind_t kind) {
    switch (kind) {
    case USB_HCD_XHCI:
        return "xHCI";
    default:
        return "USB-HCD";
    }
}

static const char *_usb_speed_name(usb_speed_t speed) {
    switch (speed) {
    case USB_SPEED_LOW:
        return "low";
    case USB_SPEED_FULL:
        return "full";
    case USB_SPEED_HIGH:
        return "high";
    case USB_SPEED_SUPER:
        return "super";
    default:
        return "?";
    }
}

static bool _usb_enum_candidate_better(
    usb_device_handle_t dev,
    usb_device_handle_t candidate
) {
    if (!dev) {
        return false;
    }

    if (!candidate) {
        return true;
    }

    if (dev->speed != candidate->speed) {
        return dev->speed > candidate->speed;
    }

    if (dev->hcd_id != candidate->hcd_id) {
        return dev->hcd_id < candidate->hcd_id;
    }

    return dev->port < candidate->port;
}

static inline void _usb_log_attach_failed(
    const char *driver_name,
    size_t hcd_id,
    size_t port
) {
    log_warn(
        "USB class driver '%s' failed while attaching (hcd=%zu port=%zu)",
        driver_name,
        hcd_id,
        port
    );
}

static inline void _usb_identity_class_triplet(
    const usb_device_identity_t *identity,
    u8 *out_class,
    u8 *out_subclass,
    u8 *out_protocol
) {
    if (!identity || !out_class || !out_subclass || !out_protocol) {
        return;
    }

    *out_class = identity->interface_class ? identity->interface_class : identity->device_class;
    *out_subclass =
        identity->interface_class ? identity->interface_subclass : identity->device_subclass;
    *out_protocol =
        identity->interface_class ? identity->interface_protocol : identity->device_protocol;
}

static bool _usb_ensure_state(void) {
    if (!usb_hcds) {
        usb_hcds = vec_create(sizeof(usb_hcd_state_t));
    }

    if (!usb_devices) {
        usb_devices = vec_create(sizeof(usb_device_handle_t));
    }

    if (!usb_class_drivers) {
        usb_class_drivers = vec_create(sizeof(usb_class_driver_t *));
    }

    if (!usb_enum_tasks) {
        usb_enum_tasks = vec_create(sizeof(usb_enum_task_t));
    }

    return usb_hcds && usb_devices && usb_class_drivers && usb_enum_tasks;
}

static usb_hcd_state_t *_usb_hcd_find_locked(size_t hcd_id) {
    if (!usb_hcds || !hcd_id) {
        return NULL;
    }

    for (size_t i = 0; i < usb_hcds->size; i++) {
        usb_hcd_state_t *hcd = vec_at(usb_hcds, i);

        if (hcd && hcd->id == hcd_id) {
            return hcd;
        }
    }

    return NULL;
}

static usb_device_handle_t _usb_device_find_locked(size_t hcd_id, size_t port) {
    if (!usb_devices || !hcd_id || !port) {
        return NULL;
    }

    for (size_t i = 0; i < usb_devices->size; i++) {
        usb_device_handle_t *slot = vec_at(usb_devices, i);
        if (!slot || !*slot) {
            continue;
        }

        usb_device_handle_t dev = *slot;
        if (dev->hcd_id == hcd_id && dev->port == port) {
            return dev;
        }
    }

    return NULL;
}

static usb_device_handle_t _usb_device_create_locked(size_t hcd_id, size_t port) {
    usb_device_handle_t dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return NULL;
    }

    dev->id = next_device_id++;
    dev->hcd_id = hcd_id;
    dev->port = port;

    if (!vec_push(usb_devices, &dev)) {
        free(dev);
        return NULL;
    }

    return dev;
}

static u8 _usb_alloc_address_locked(usb_hcd_state_t *hcd) {
    if (!hcd) {
        return 1;
    }

    if (!hcd->next_address || hcd->next_address > 127) {
        hcd->next_address = 1;
    }

    u8 address = hcd->next_address;
    hcd->next_address++;

    if (hcd->next_address > 127) {
        hcd->next_address = 1;
    }

    return address;
}

static void _usb_log_no_driver(const usb_device_handle_t dev) {
    if (!dev || !dev->identity_valid) {
        return;
    }

    u8 class_code = 0;
    u8 sub = 0;
    u8 proto = 0;
    _usb_identity_class_triplet(&dev->identity, &class_code, &sub, &proto);

    log_info(
        "USB device hcd=%zu port=%zu has no class driver (%#x/%#x/%#x)",
        dev->hcd_id,
        dev->port,
        class_code,
        sub,
        proto
    );
}

static void _usb_plan_bind_locked(
    usb_device_handle_t dev,
    const usb_class_driver_t **out_driver,
    bool *out_log_no_driver
) {
    if (out_driver) {
        *out_driver = NULL;
    }

    if (out_log_no_driver) {
        *out_log_no_driver = false;
    }

    if (
        !dev ||
        !dev->connected ||
        !dev->identity_valid ||
        dev->bound_driver ||
        !usb_class_drivers
    ) {
        return;
    }

    for (size_t i = 0; i < usb_class_drivers->size; i++) {
        usb_class_driver_t **slot = vec_at(usb_class_drivers, i);
        if (!slot || !*slot) {
            continue;
        }

        usb_class_driver_t *driver = *slot;
        if (!driver->match || !driver->match(&dev->identity)) {
            continue;
        }

        dev->bound_driver = driver;
        dev->bind_attempted = true;

        if (out_driver) {
            *out_driver = driver;
        }

        return;
    }

    if (!dev->bind_attempted && out_log_no_driver) {
        *out_log_no_driver = true;
    }

    dev->bind_attempted = true;
}

static bool _usb_queue_enum_locked(size_t hcd_id, size_t port, u64 generation) {
    if (!usb_enum_tasks || !hcd_id || !port) {
        return false;
    }

    for (size_t i = 0; i < usb_enum_tasks->size; i++) {
        usb_enum_task_t *task = vec_at(usb_enum_tasks, i);
        if (!task) {
            continue;
        }

        if (task->hcd_id != hcd_id || task->port != port) {
            continue;
        }

        if (generation > task->generation) {
            task->generation = generation;
        }

        return true;
    }

    usb_enum_task_t task = {
        .hcd_id = hcd_id,
        .port = port,
        .generation = generation,
    };

    return vec_push(usb_enum_tasks, &task);
}

static void _usb_drop_enum_tasks_for_hcd_locked(size_t hcd_id) {
    if (!usb_enum_tasks || !hcd_id) {
        return;
    }

    for (size_t i = 0; i < usb_enum_tasks->size;) {
        usb_enum_task_t *task = vec_at(usb_enum_tasks, i);
        if (!task || task->hcd_id != hcd_id) {
            i++;
            continue;
        }

        vec_remove_at(usb_enum_tasks, i, NULL);
    }
}

static void _usb_wake_enum_worker(void) {
    if (!usb_enum_wait_ready || !sched_is_running()) {
        return;
    }

    sched_wake_one(&usb_enum_wait);
}

static bool _usb_hcd_has_enum_ops(const usb_hcd_state_t *hcd) {
    return (
        hcd &&
        hcd->has_ops &&
        hcd->ops.port_reset &&
        hcd->ops.device_open &&
        hcd->ops.device_close &&
        hcd->ops.set_address &&
        hcd->ops.control_transfer
    );
}

static bool _usb_enum_candidate_eligible_locked(
    usb_device_handle_t dev,
    bool defer_slow
) {
    if (
        !dev ||
        !dev->connected ||
        dev->identity_valid ||
        dev->enum_attempted ||
        dev->enum_queued
    ) {
        return false;
    }

    usb_hcd_state_t *hcd = _usb_hcd_find_locked(dev->hcd_id);
    if (!_usb_hcd_has_enum_ops(hcd)) {
        return false;
    }

    return !defer_slow || dev->speed >= USB_SPEED_HIGH;
}

static usb_device_handle_t _usb_pick_boot_enum_candidate_locked(
    bool defer_slow,
    bool *out_fast_identity_present
) {
    usb_device_handle_t candidate = NULL;
    bool fast_identity_present = false;

    if (!usb_devices) {
        if (out_fast_identity_present) {
            *out_fast_identity_present = false;
        }

        return NULL;
    }

    for (size_t i = 0; i < usb_devices->size; i++) {
        usb_device_handle_t *slot = vec_at(usb_devices, i);
        if (!slot || !*slot) {
            continue;
        }

        usb_device_handle_t dev = *slot;

        if (dev->connected && dev->identity_valid && dev->speed >= USB_SPEED_HIGH) {
            fast_identity_present = true;
        }

        if (!_usb_enum_candidate_eligible_locked(dev, defer_slow)) {
            continue;
        }

        if (_usb_enum_candidate_better(dev, candidate)) {
            candidate = dev;
        }
    }

    if (out_fast_identity_present) {
        *out_fast_identity_present = fast_identity_present;
    }

    return candidate;
}

static bool _usb_boot_enumeration_settled_locked(bool queued) {
    if (queued) {
        return false;
    }

    if (usb_enum_tasks && usb_enum_tasks->size) {
        return false;
    }

    if (!usb_devices) {
        return true;
    }

    for (size_t i = 0; i < usb_devices->size; i++) {
        usb_device_handle_t *slot = vec_at(usb_devices, i);
        if (!slot || !*slot) {
            continue;
        }

        usb_device_handle_t dev = *slot;
        if (dev->connected && !dev->identity_valid && dev->enum_queued) {
            return false;
        }
    }

    return true;
}

static void _usb_process_enum_task(const usb_enum_task_t *task) {
    if (!task || !task->hcd_id || !task->port) {
        return;
    }

    usb_enum_request_t req = {0};
    bool can_enum = false;

    unsigned long flags = spin_lock_irqsave(&usb_state_lock);

    usb_hcd_state_t *hcd = _usb_hcd_find_locked(task->hcd_id);
    usb_device_handle_t dev = _usb_device_find_locked(task->hcd_id, task->port);

    if (
        hcd &&
        dev &&
        dev->connected &&
        dev->generation == task->generation &&
        _usb_hcd_has_enum_ops(hcd)
    ) {
        req = (usb_enum_request_t){
            .hcd_id = task->hcd_id,
            .port = task->port,
            .speed = dev->speed,
            .address = _usb_alloc_address_locked(hcd),
            .ops = &hcd->ops,
        };

        dev->enum_queued = false;
        can_enum = true;
    } else if (dev && dev->generation == task->generation) {
        dev->enum_queued = false;
        dev->enum_attempted = false;
    }

    spin_unlock_irqrestore(&usb_state_lock, flags);

    if (!can_enum) {
        return;
    }

    usb_enum_result_t enum_result = {0};
    bool enum_ok = usb_enum_run(&req, &enum_result);

    const usb_class_driver_t *attach_driver = NULL;
    bool log_no_driver = false;

    void *stale_release_ctx = NULL;
    void *reject_release_ctx = NULL;

    flags = spin_lock_irqsave(&usb_state_lock);
    hcd = _usb_hcd_find_locked(task->hcd_id);
    dev = _usb_device_find_locked(task->hcd_id, task->port);

    bool stale = (
        !hcd ||
        !dev ||
        !dev->connected ||
        dev->generation != task->generation
    );

    if (!stale && enum_ok) {
        if (dev->hcd_device && _usb_hcd_has_enum_ops(hcd)) {
            stale_release_ctx = dev->hcd_device;
        }

        dev->identity = enum_result.identity;
        dev->identity_valid = true;
        dev->hcd_device = enum_result.device_ctx;
        dev->bind_attempted = false;
        dev->enum_attempted = true;
        dev->enum_queued = false;

        _usb_plan_bind_locked(dev, &attach_driver, &log_no_driver);
    } else if (enum_ok) {
        reject_release_ctx = enum_result.device_ctx;
    }

    if (!stale && dev && !enum_ok) {
        dev->enum_attempted = true;
        dev->enum_queued = false;
    }

    spin_unlock_irqrestore(&usb_state_lock, flags);

    if (stale_release_ctx && req.ops && req.ops->device_close) {
        req.ops->device_close(task->hcd_id, task->port, stale_release_ctx);
    }

    if (reject_release_ctx && req.ops && req.ops->device_close) {
        req.ops->device_close(task->hcd_id, task->port, reject_release_ctx);
    }

    if (!stale && enum_ok) {
        log_info(
            "USB device hcd=%zu port=%zu vid=%#x pid=%#x",
            task->hcd_id,
            task->port,
            enum_result.identity.vendor_id,
            enum_result.identity.product_id
        );

        if (log_no_driver) {
            flags = spin_lock_irqsave(&usb_state_lock);

            dev = _usb_device_find_locked(task->hcd_id, task->port);
            if (dev) {
                _usb_log_no_driver(dev);
            }

            spin_unlock_irqrestore(&usb_state_lock, flags);
        }

        if (attach_driver && attach_driver->attach) {
            if (!attach_driver->attach(dev)) {
                _usb_log_attach_failed(attach_driver->name, task->hcd_id, task->port);
            }
        }

        return;
    }

    if (!stale && !enum_ok) {
        log_warn(
            "USB enumeration failed while probing device (hcd=%zu port=%zu)",
            task->hcd_id,
            task->port
        );
    }
}

static void _usb_enum_worker(void *arg) {
    (void)arg;

    for (;;) {
        usb_enum_task_t task = {0};
        bool have_task = false;
        bool can_block = false;
        u32 wait_seq = 0;

        unsigned long flags = spin_lock_irqsave(&usb_state_lock);
        if (usb_enum_tasks && usb_enum_tasks->size) {
            have_task = vec_pop(usb_enum_tasks, &task);
        } else if (usb_enum_wait_ready) {
            wait_seq = sched_wait_seq(&usb_enum_wait);
            can_block = true;
        }
        spin_unlock_irqrestore(&usb_state_lock, flags);

        if (!have_task) {
            if (can_block) {
                (void)sched_wait_on_queue(
                    &usb_enum_wait,
                    wait_seq,
                    0,
                    SCHED_WAIT_INTERRUPTIBLE
                );
            } else {
                if (sched_is_running() && sched_current()) {
                    sched_yield();
                } else {
                    arch_cpu_wait();
                }
            }
            continue;
        }

        _usb_process_enum_task(&task);
    }
}

static void _usb_start_enum_worker(void) {
    if (!usb_enum_wait_ready) {
        sched_wait_queue_init(&usb_enum_wait);
        sched_wait_queue_set_name(&usb_enum_wait, "usb_enum_wait");
        usb_enum_wait_ready = true;
    }

    if (usb_enum_thread) {
        return;
    }

    usb_enum_thread = sched_create_kernel_thread("usb-enum", _usb_enum_worker, NULL);
    if (!usb_enum_thread) {
        log_warn("USB core failed while creating enumeration worker");
        return;
    }

    sched_make_runnable(usb_enum_thread);
}

bool usb_register_class_driver(const usb_class_driver_t *driver) {
    if (!driver || !driver->name || !driver->name[0] || !driver->match) {
        return false;
    }

    if (!_usb_ensure_state()) {
        return false;
    }

    unsigned long flags = spin_lock_irqsave(&usb_state_lock);

    for (size_t i = 0; i < usb_class_drivers->size; i++) {
        usb_class_driver_t **slot = vec_at(usb_class_drivers, i);
        if (!slot || !*slot) {
            continue;
        }

        if (*slot == driver || !strcmp((*slot)->name, driver->name)) {
            spin_unlock_irqrestore(&usb_state_lock, flags);
            return true;
        }
    }

    usb_class_driver_t *driver_ptr = (usb_class_driver_t *)driver;
    bool ok = vec_push(usb_class_drivers, &driver_ptr);

    if (!ok) {
        spin_unlock_irqrestore(&usb_state_lock, flags);
        return false;
    }

    vector_t *attach_list = vec_create(sizeof(usb_device_handle_t));
    bool attach_queue_failed = false;

    if (attach_list) {
        for (size_t i = 0; i < usb_devices->size; i++) {
            usb_device_handle_t *slot = vec_at(usb_devices, i);
            if (!slot || !*slot) {
                continue;
            }

            usb_device_handle_t dev = *slot;

            if (
                !dev->connected ||
                !dev->identity_valid ||
                dev->bound_driver
            ) {
                continue;
            }

            if (!driver->match(&dev->identity)) {
                continue;
            }

            dev->bound_driver = driver;
            dev->bind_attempted = true;

            if (!vec_push(attach_list, &dev)) {
                dev->bound_driver = NULL;
                dev->bind_attempted = false;
                attach_queue_failed = true;
                break;
            }
        }
    } else if (usb_devices->size > 0) {
        attach_queue_failed = true;
    }

    spin_unlock_irqrestore(&usb_state_lock, flags);

    log_debug("registered USB class driver '%s'", driver->name);

    if (attach_list && driver->attach) {
        for (size_t i = 0; i < attach_list->size; i++) {
            usb_device_handle_t *slot = vec_at(attach_list, i);
            if (!slot || !*slot) {
                continue;
            }

            if (!driver->attach(*slot)) {
                _usb_log_attach_failed(
                    driver->name,
                    usb_device_hcd_id(*slot),
                    usb_device_port(*slot)
                );
            }
        }
    }

    if (attach_list) {
        vec_destroy(attach_list);
    }

    if (attach_queue_failed) {
        log_warn(
            "USB class driver '%s' registered without immediate attach due low memory",
            driver->name
        );
    }

    return true;
}

bool usb_unregister_class_driver(const char *name) {
    if (!name || !name[0]) {
        return false;
    }

    if (!_usb_ensure_state()) {
        return false;
    }

    const usb_class_driver_t *driver = NULL;
    vector_t *detach_list = vec_create(sizeof(usb_detach_task_t));
    size_t dropped_detach_callbacks = 0;

    unsigned long flags = spin_lock_irqsave(&usb_state_lock);

    size_t driver_index = (size_t)-1;
    for (size_t i = 0; i < usb_class_drivers->size; i++) {
        usb_class_driver_t **slot = vec_at(usb_class_drivers, i);
        if (!slot || !*slot) {
            continue;
        }

        if (strcmp((*slot)->name, name)) {
            continue;
        }

        driver = *slot;
        driver_index = i;
        break;
    }

    if (!driver) {
        spin_unlock_irqrestore(&usb_state_lock, flags);
        if (detach_list) {
            vec_destroy(detach_list);
        }
        return false;
    }

    for (size_t i = 0; i < usb_devices->size; i++) {
        usb_device_handle_t *slot = vec_at(usb_devices, i);
        if (!slot || !*slot) {
            continue;
        }

        usb_device_handle_t dev = *slot;
        if (dev->bound_driver != driver) {
            continue;
        }

        if (detach_list) {
            usb_detach_task_t task = {
                .driver = driver,
                .dev = dev,
            };
            if (!vec_push(detach_list, &task)) {
                dropped_detach_callbacks++;
            }
        } else {
            dropped_detach_callbacks++;
        }

        dev->bound_driver = NULL;
        dev->bind_attempted = false;
    }

    vec_remove_at(usb_class_drivers, driver_index, NULL);
    spin_unlock_irqrestore(&usb_state_lock, flags);

    if (driver->detach && detach_list) {
        for (size_t i = 0; i < detach_list->size; i++) {
            usb_detach_task_t *task = vec_at(detach_list, i);
            if (!task || !task->driver || !task->driver->detach || !task->dev) {
                continue;
            }

            task->driver->detach(task->dev);
        }
    }

    if (detach_list) {
        vec_destroy(detach_list);
    }

    if (dropped_detach_callbacks > 0) {
        log_warn(
            "USB class driver '%s' detached %zu device(s) without callback due low memory",
            driver->name,
            dropped_detach_callbacks
        );
    }

    return true;
}

bool usb_register_hcd(
    const usb_hcd_info_t *info,
    const usb_hcd_ops_t *ops,
    size_t *out_hcd_id
) {
    if (!info || !info->max_ports || info->kind == USB_HCD_UNKNOWN) {
        return false;
    }

    if (!_usb_ensure_state()) {
        return false;
    }

    unsigned long flags = spin_lock_irqsave(&usb_state_lock);

    for (size_t i = 0; i < usb_hcds->size; i++) {
        usb_hcd_state_t *hcd = vec_at(usb_hcds, i);
        if (!hcd) {
            continue;
        }

        if (
            hcd->info.kind == info->kind &&
            hcd->info.pci_bus == info->pci_bus &&
            hcd->info.pci_slot == info->pci_slot &&
            hcd->info.pci_func == info->pci_func
        ) {
            if (ops) {
                hcd->ops = *ops;
                hcd->has_ops = true;
            }

            size_t id = hcd->id;
            spin_unlock_irqrestore(&usb_state_lock, flags);

            if (out_hcd_id) {
                *out_hcd_id = id;
            }

            return true;
        }
    }

    usb_hcd_state_t hcd = {
        .id = next_hcd_id++,
        .info = *info,
        .has_ops = ops != NULL,
        .ops = {0},
        .next_address = 1,
    };

    if (ops) {
        hcd.ops = *ops;
    }

    bool ok = vec_push(usb_hcds, &hcd);
    spin_unlock_irqrestore(&usb_state_lock, flags);

    if (!ok) {
        return false;
    }

    if (out_hcd_id) {
        *out_hcd_id = hcd.id;
    }

    log_info(
        "%s id=%zu ports=%zu%s%s",
        _usb_hcd_kind_name(info->kind),
        hcd.id,
        info->max_ports,
        info->irq_driven ? ", irq" : "",
        info->msi_enabled ? ", msi" : ""
    );

    return true;
}

bool usb_unregister_hcd(size_t hcd_id) {
    if (!hcd_id) {
        return false;
    }

    if (!_usb_ensure_state()) {
        return false;
    }

    vector_t *detach_list = vec_create(sizeof(usb_detach_task_t));
    vector_t *release_list = vec_create(sizeof(usb_release_task_t));
    vector_t *free_list = vec_create(sizeof(usb_device_handle_t));
    size_t dropped_detach_callbacks = 0;
    size_t inline_release_count = 0;
    size_t inline_free_count = 0;

    unsigned long flags = spin_lock_irqsave(&usb_state_lock);
    size_t hcd_index = (size_t)-1;
    usb_hcd_state_t hcd_copy = {0};

    for (size_t i = 0; i < usb_hcds->size; i++) {
        usb_hcd_state_t *hcd = vec_at(usb_hcds, i);
        if (!hcd || hcd->id != hcd_id) {
            continue;
        }

        hcd_index = i;
        hcd_copy = *hcd;
        break;
    }

    if (hcd_index == (size_t)-1) {
        spin_unlock_irqrestore(&usb_state_lock, flags);
        if (detach_list) {
            vec_destroy(detach_list);
        }
        if (release_list) {
            vec_destroy(release_list);
        }
        if (free_list) {
            vec_destroy(free_list);
        }
        return false;
    }

    for (size_t i = 0; i < usb_devices->size; i++) {
        usb_device_handle_t *slot = vec_at(usb_devices, i);
        if (!slot || !*slot) {
            continue;
        }

        usb_device_handle_t dev = *slot;
        if (dev->hcd_id != hcd_id) {
            continue;
        }

        if (
            dev->connected &&
            dev->bound_driver &&
            dev->bound_driver->detach &&
            dev->identity_valid
        ) {
            usb_detach_task_t task = {
                .driver = dev->bound_driver,
                .dev = dev,
            };

            if (detach_list) {
                if (!vec_push(detach_list, &task)) {
                    dropped_detach_callbacks++;
                }
            } else {
                dropped_detach_callbacks++;
            }
        }

        if (
            dev->hcd_device &&
            _usb_hcd_has_enum_ops(&hcd_copy) &&
            hcd_copy.ops.device_close
        ) {
            usb_release_task_t task = {
                .release = hcd_copy.ops.device_close,
                .hcd_id = hcd_id,
                .port = dev->port,
                .ctx = dev->hcd_device,
            };

            if (release_list) {
                if (!vec_push(release_list, &task)) {
                    task.release(task.hcd_id, task.port, task.ctx);
                    inline_release_count++;
                }
            } else {
                task.release(task.hcd_id, task.port, task.ctx);
                inline_release_count++;
            }
        }

        dev->connected = false;
        dev->generation++;
        dev->identity_valid = false;
        dev->identity = (usb_device_identity_t){0};
        dev->hcd_device = NULL;
        dev->bound_driver = NULL;
        dev->bind_attempted = false;
        dev->enum_attempted = false;
        dev->enum_queued = false;

        if (free_list) {
            if (!vec_push(free_list, &dev)) {
                free(dev);
                inline_free_count++;
            }
        } else {
            free(dev);
            inline_free_count++;
        }

        *slot = NULL;
    }

    _usb_drop_enum_tasks_for_hcd_locked(hcd_id);
    vec_remove_at(usb_hcds, hcd_index, NULL);
    spin_unlock_irqrestore(&usb_state_lock, flags);

    if (detach_list) {
        for (size_t i = 0; i < detach_list->size; i++) {
            usb_detach_task_t *task = vec_at(detach_list, i);
            if (!task || !task->driver || !task->driver->detach || !task->dev) {
                continue;
            }

            task->driver->detach(task->dev);
        }
    }

    if (release_list) {
        for (size_t i = 0; i < release_list->size; i++) {
            usb_release_task_t *task = vec_at(release_list, i);
            if (!task || !task->release || !task->ctx) {
                continue;
            }

            task->release(task->hcd_id, task->port, task->ctx);
        }
    }

    if (free_list) {
        for (size_t i = 0; i < free_list->size; i++) {
            usb_device_handle_t *slot = vec_at(free_list, i);
            if (!slot || !*slot) {
                continue;
            }

            free(*slot);
        }
    }

    if (detach_list) {
        vec_destroy(detach_list);
    }

    if (release_list) {
        vec_destroy(release_list);
    }

    if (free_list) {
        vec_destroy(free_list);
    }

    if (dropped_detach_callbacks > 0) {
        log_warn(
            "USB HCD id=%zu dropped %zu class detach callback(s) due low memory",
            hcd_id,
            dropped_detach_callbacks
        );
    }

    if (inline_release_count > 0) {
        log_warn(
            "USB HCD id=%zu released %zu device context(s) inline due low memory",
            hcd_id,
            inline_release_count
        );
    }

    if (inline_free_count > 0) {
        log_warn(
            "USB HCD id=%zu freed %zu device record(s) inline due low memory",
            hcd_id,
            inline_free_count
        );
    }

    return true;
}

bool usb_report_port_state(
    size_t hcd_id,
    size_t port,
    bool connected,
    usb_speed_t speed
) {
    if (!hcd_id || !port) {
        return false;
    }

    if (!_usb_ensure_state()) {
        return false;
    }

    const usb_class_driver_t *detach_driver = NULL;
    usb_device_handle_t detached_dev = NULL;

    void (*release_fn)(size_t, size_t, void *) = NULL;
    void *release_ctx = NULL;

    const usb_class_driver_t *attach_driver = NULL;
    usb_device_handle_t attach_dev = NULL;
    bool log_no_driver = false;

    bool queue_enum = false;

    bool log_connected = false;
    bool log_disconnected = false;
    const char *hcd_name = "USB-HCD";

    unsigned long flags = spin_lock_irqsave(&usb_state_lock);

    usb_hcd_state_t *hcd = _usb_hcd_find_locked(hcd_id);
    if (!hcd) {
        spin_unlock_irqrestore(&usb_state_lock, flags);
        return false;
    }

    hcd_name = _usb_hcd_kind_name(hcd->info.kind);

    usb_device_handle_t dev = _usb_device_find_locked(hcd_id, port);

    if (!connected) {
        if (dev && dev->connected) {
            log_disconnected = true;

            if (dev->bound_driver && dev->bound_driver->detach && dev->identity_valid) {
                detach_driver = dev->bound_driver;
                detached_dev = dev;
            }

            if (dev->hcd_device && _usb_hcd_has_enum_ops(hcd)) {
                release_fn = hcd->ops.device_close;
                release_ctx = dev->hcd_device;
            }

            dev->connected = false;
            dev->generation++;
            dev->identity_valid = false;
            dev->identity = (usb_device_identity_t){0};
            dev->hcd_device = NULL;
            dev->bound_driver = NULL;
            dev->bind_attempted = false;
            dev->enum_attempted = false;
            dev->enum_queued = false;
        }

        spin_unlock_irqrestore(&usb_state_lock, flags);

        if (detach_driver) {
            detach_driver->detach(detached_dev);
        }

        if (release_fn && release_ctx) {
            release_fn(hcd_id, port, release_ctx);
        }

        if (log_disconnected) {
            log_info("%s id=%zu port %zu disconnected", hcd_name, hcd_id, port);
        }

        return true;
    }

    if (!dev) {
        dev = _usb_device_create_locked(hcd_id, port);
        if (!dev) {
            spin_unlock_irqrestore(&usb_state_lock, flags);
            return false;
        }
    }

    bool was_connected = dev->connected;
    dev->connected = true;
    dev->speed = speed;

    if (!was_connected) {
        log_connected = true;
        dev->generation++;

        if (dev->hcd_device && _usb_hcd_has_enum_ops(hcd)) {
            release_fn = hcd->ops.device_close;
            release_ctx = dev->hcd_device;
        }

        dev->identity_valid = false;
        dev->identity = (usb_device_identity_t){0};
        dev->hcd_device = NULL;
        dev->bound_driver = NULL;
        dev->bind_attempted = false;
        dev->enum_attempted = false;
        dev->enum_queued = false;
    }

    if (
        !dev->identity_valid &&
        !dev->enum_attempted &&
        _usb_hcd_has_enum_ops(hcd)
    ) {
        if (sched_is_running()) {
            dev->enum_attempted = true;
            dev->enum_queued = _usb_queue_enum_locked(hcd_id, port, dev->generation);
            if (!dev->enum_queued) {
                dev->enum_attempted = false;
            } else {
                queue_enum = true;
            }
        } else {
            dev->enum_attempted = false;
        }
    } else {
        _usb_plan_bind_locked(dev, &attach_driver, &log_no_driver);
        attach_dev = dev;
    }

    spin_unlock_irqrestore(&usb_state_lock, flags);

    if (release_fn && release_ctx) {
        release_fn(hcd_id, port, release_ctx);
    }

    if (log_connected) {
        log_info(
            "%s id=%zu port %zu connected (%s-speed)",
            hcd_name,
            hcd_id,
            port,
            _usb_speed_name(speed)
        );
    }

    if (queue_enum) {
        _usb_wake_enum_worker();
    }

    if (log_no_driver) {
        flags = spin_lock_irqsave(&usb_state_lock);
        dev = _usb_device_find_locked(hcd_id, port);
        if (dev) {
            _usb_log_no_driver(dev);
        }
        spin_unlock_irqrestore(&usb_state_lock, flags);
    }

    if (attach_driver && attach_driver->attach) {
        if (!attach_driver->attach(attach_dev)) {
            _usb_log_attach_failed(attach_driver->name, hcd_id, port);
        }
    }

    return true;
}

bool usb_schedule_port_enumeration(size_t hcd_id, size_t port) {
    if (!hcd_id || !port) {
        return false;
    }

    if (!_usb_ensure_state()) {
        return false;
    }

    bool queued = false;
    unsigned long flags = spin_lock_irqsave(&usb_state_lock);

    usb_hcd_state_t *hcd = _usb_hcd_find_locked(hcd_id);
    usb_device_handle_t dev = _usb_device_find_locked(hcd_id, port);

    if (
        hcd &&
        dev &&
        dev->connected &&
        !dev->enum_attempted &&
        _usb_hcd_has_enum_ops(hcd)
    ) {
        dev->enum_attempted = true;
        queued = _usb_queue_enum_locked(hcd_id, port, dev->generation);
        dev->enum_queued = queued;

        if (!queued) {
            dev->enum_attempted = false;
        }
    }

    spin_unlock_irqrestore(&usb_state_lock, flags);

    if (queued) {
        _usb_wake_enum_worker();
    }

    return queued;
}

bool usb_wait_for_boot_enumeration(u32 timeout_ms) {
    if (!_usb_ensure_state()) {
        return false;
    }

    if (sched_is_running()) {
        return true;
    }

    u32 wait_ms = timeout_ms ? timeout_ms : 1500;
    u64 start = arch_timer_ticks();
    u64 timeout = ms_to_ticks(wait_ms);

    for (;;) {
        usb_enum_task_t task = {0};
        bool have_task = false;

        unsigned long flags = spin_lock_irqsave(&usb_state_lock);
        if (usb_enum_tasks && usb_enum_tasks->size) {
            have_task = vec_pop(usb_enum_tasks, &task);
        }
        spin_unlock_irqrestore(&usb_state_lock, flags);

        if (have_task) {
            _usb_process_enum_task(&task);
            continue;
        }

        bool queued = false;
        flags = spin_lock_irqsave(&usb_state_lock);

        usb_device_handle_t candidate = NULL;
        bool fast_identity_present = false;
        u64 elapsed = arch_timer_ticks() - start;
        bool defer_slow = elapsed < (timeout / 3U);

        candidate = _usb_pick_boot_enum_candidate_locked(
            defer_slow,
            &fast_identity_present
        );

        if (!candidate && defer_slow) {
            candidate = _usb_pick_boot_enum_candidate_locked(false, NULL);
        }

        if (fast_identity_present && candidate && candidate->speed < USB_SPEED_HIGH) {
            candidate = NULL;
        }

        if (candidate) {
            candidate->enum_attempted = true;
            queued = _usb_queue_enum_locked(
                candidate->hcd_id,
                candidate->port,
                candidate->generation
            );
            candidate->enum_queued = queued;

            if (!queued) {
                candidate->enum_attempted = false;
            }
        }

        bool settled = _usb_boot_enumeration_settled_locked(queued);

        spin_unlock_irqrestore(&usb_state_lock, flags);

        if (queued) {
            continue;
        }

        if (settled) {
            return true;
        }

        if ((arch_timer_ticks() - start) >= timeout) {
            return false;
        }

        if (sched_is_running() && sched_current()) {
            sched_yield();
        }
    }
}

size_t usb_device_hcd_id(usb_device_handle_t dev) {
    return dev ? dev->hcd_id : 0;
}

size_t usb_device_port(usb_device_handle_t dev) {
    return dev ? dev->port : 0;
}

const usb_device_identity_t *usb_device_identity(usb_device_handle_t dev) {
    if (!dev || !dev->identity_valid) {
        return NULL;
    }

    return &dev->identity;
}

bool usb_identify_device(
    size_t hcd_id,
    size_t port,
    const usb_device_identity_t *identity
) {
    if (!hcd_id || !port || !identity) {
        return false;
    }

    if (!_usb_ensure_state()) {
        return false;
    }

    const usb_class_driver_t *detach_driver = NULL;
    usb_device_handle_t detached_dev = NULL;

    const usb_class_driver_t *attach_driver = NULL;
    usb_device_handle_t attach_dev = NULL;
    bool log_no_driver = false;

    unsigned long flags = spin_lock_irqsave(&usb_state_lock);
    usb_device_handle_t dev = _usb_device_find_locked(hcd_id, port);

    if (!dev || !dev->connected) {
        spin_unlock_irqrestore(&usb_state_lock, flags);
        return false;
    }

    if (dev->bound_driver && dev->bound_driver->detach && dev->identity_valid) {
        detach_driver = dev->bound_driver;
        detached_dev = dev;
    }

    dev->bound_driver = NULL;
    dev->bind_attempted = false;

    dev->identity = *identity;
    dev->identity_valid = true;
    dev->enum_attempted = true;

    _usb_plan_bind_locked(dev, &attach_driver, &log_no_driver);
    attach_dev = dev;

    spin_unlock_irqrestore(&usb_state_lock, flags);

    if (detach_driver) {
        detach_driver->detach(detached_dev);
    }

    if (log_no_driver) {
        flags = spin_lock_irqsave(&usb_state_lock);
        dev = _usb_device_find_locked(hcd_id, port);
        if (dev) {
            _usb_log_no_driver(dev);
        }
        spin_unlock_irqrestore(&usb_state_lock, flags);
    }

    if (attach_driver && attach_driver->attach) {
        if (!attach_driver->attach(attach_dev)) {
            _usb_log_attach_failed(attach_driver->name, hcd_id, port);
        }
    }

    return true;
}

bool usb_device_control_transfer(
    usb_device_handle_t dev,
    const usb_setup_packet_t *setup,
    void *buffer,
    size_t length,
    u32 timeout_ms,
    size_t *out_actual
) {
    if (!dev || !setup) {
        return false;
    }

    if (setup->length && !buffer && length) {
        return false;
    }

    bool (*transfer_fn)(size_t, size_t, void *, const usb_transfer_t *, size_t *) = NULL;
    void *device_ctx = NULL;

    unsigned long flags = spin_lock_irqsave(&usb_state_lock);

    usb_hcd_state_t *hcd = _usb_hcd_find_locked(dev->hcd_id);
    if (hcd && dev->connected && hcd->has_ops && hcd->ops.control_transfer) {
        transfer_fn = hcd->ops.control_transfer;
        device_ctx = dev->hcd_device;
    }

    spin_unlock_irqrestore(&usb_state_lock, flags);

    if (!transfer_fn) {
        return false;
    }

    usb_transfer_t transfer = {
        .endpoint = {
            .address = 0,
            .transfer_type = USB_XFER_CONTROL,
            .max_packet_size = 0,
            .interval = 0,
        },
        .setup = setup,
        .buffer = buffer,
        .length = length,
        .timeout_ms = timeout_ms,
    };

    return transfer_fn(dev->hcd_id, dev->port, device_ctx, &transfer, out_actual);
}

bool usb_device_bulk_transfer(
    usb_device_handle_t dev,
    u8 endpoint,
    void *buffer,
    size_t length,
    u32 timeout_ms,
    size_t *out_actual
) {
    if (!dev || !(endpoint & USB_ENDPOINT_NUM_MASK)) {
        return false;
    }

    if (length && !buffer) {
        return false;
    }

    bool (*transfer_fn)(size_t, size_t, void *, const usb_transfer_t *, size_t *) = NULL;
    void *device_ctx = NULL;

    unsigned long flags = spin_lock_irqsave(&usb_state_lock);

    usb_hcd_state_t *hcd = _usb_hcd_find_locked(dev->hcd_id);
    if (hcd && dev->connected && hcd->has_ops && hcd->ops.bulk_transfer) {
        transfer_fn = hcd->ops.bulk_transfer;
        device_ctx = dev->hcd_device;
    }

    spin_unlock_irqrestore(&usb_state_lock, flags);

    if (!transfer_fn) {
        return false;
    }

    usb_transfer_t transfer = {
        .endpoint = {
            .address = endpoint,
            .transfer_type = USB_XFER_BULK,
            .max_packet_size = 0,
            .interval = 0,
        },
        .setup = NULL,
        .buffer = buffer,
        .length = length,
        .timeout_ms = timeout_ms,
    };

    return transfer_fn(dev->hcd_id, dev->port, device_ctx, &transfer, out_actual);
}

bool usb_control_transfer(
    size_t hcd_id,
    size_t port,
    const usb_setup_packet_t *setup,
    void *buffer,
    size_t length,
    u32 timeout_ms,
    size_t *out_actual
) {
    if (!hcd_id || !port) {
        return false;
    }

    unsigned long flags = spin_lock_irqsave(&usb_state_lock);
    usb_device_handle_t dev = _usb_device_find_locked(hcd_id, port);
    spin_unlock_irqrestore(&usb_state_lock, flags);

    return usb_device_control_transfer(
        dev,
        setup,
        buffer,
        length,
        timeout_ms,
        out_actual
    );
}

bool usb_bulk_transfer(
    size_t hcd_id,
    size_t port,
    u8 endpoint,
    void *buffer,
    size_t length,
    u32 timeout_ms,
    size_t *out_actual
) {
    if (!hcd_id || !port) {
        return false;
    }

    unsigned long flags = spin_lock_irqsave(&usb_state_lock);
    usb_device_handle_t dev = _usb_device_find_locked(hcd_id, port);
    spin_unlock_irqrestore(&usb_state_lock, flags);

    return usb_device_bulk_transfer(
        dev,
        endpoint,
        buffer,
        length,
        timeout_ms,
        out_actual
    );
}

size_t usb_connected_device_count(void) {
    if (!_usb_ensure_state()) {
        return 0;
    }

    size_t connected = 0;
    unsigned long flags = spin_lock_irqsave(&usb_state_lock);

    for (size_t i = 0; i < usb_devices->size; i++) {
        usb_device_handle_t *slot = vec_at(usb_devices, i);
        if (slot && *slot && (*slot)->connected) {
            connected++;
        }
    }

    spin_unlock_irqrestore(&usb_state_lock, flags);
    return connected;
}

bool usb_core_init(void) {
    if (usb_core_ready) {
        return true;
    }

    if (!_usb_ensure_state()) {
        log_warn("USB core failed while allocating state");
        return false;
    }

    _usb_start_enum_worker();
    usb_core_ready = true;
    return true;
}

bool usb_core_is_ready(void) {
    return usb_core_ready;
}

bool usb_init(void) {
    return usb_core_init();
}
