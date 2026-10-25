/* =========================================================================
 * object.h - Zircon Kernel Objects (Dispatcher), StateTracker, Event & EventPair
 * References:
 *   - zircon_org/kernel/object/include/object/dispatcher.h
 *   - zircon_org/kernel/object/include/object/state_tracker.h
 *   - zircon_org/kernel/object/include/object/event_dispatcher.h
 *   - zircon_org/kernel/object/include/object/event_pair_dispatcher.h
 * ========================================================================= */
#ifndef ABOS_OBJECT_H
#define ABOS_OBJECT_H

#include <stdint.h>
#include <stddef.h>
#include "spinlock.h"
#include "mmu.h"
#include "thread.h"
#include "wait.h"

typedef int32_t  ab_status_t;
typedef uint32_t ab_handle_t;
typedef uint64_t ab_koid_t;
typedef uint32_t ab_rights_t;
typedef uint32_t ab_signals_t;

#define AB_HANDLE_INVALID       ((ab_handle_t)0U)
#define AB_KOID_INVALID         ((ab_koid_t)0ULL)
#define AB_KOID_KERNEL          ((ab_koid_t)1ULL)

/* Zircon-style status codes (zircon_org/system/public/zircon/errors.h) */
#define AB_OK                    0
#define AB_ERR_INTERNAL         -1
#define AB_ERR_NOT_SUPPORTED    -2
#define AB_ERR_NO_RESOURCES     -3
#define AB_ERR_NO_MEMORY        -4
#define AB_ERR_INVALID_ARGS     -10
#define AB_ERR_BAD_HANDLE       -11
#define AB_ERR_WRONG_TYPE       -12
#define AB_ERR_BAD_SYSCALL      -13
#define AB_ERR_OUT_OF_RANGE     -14
#define AB_ERR_BUFFER_TOO_SMALL -15
#define AB_ERR_BAD_STATE        -20
#define AB_ERR_TIMED_OUT        -21
#define AB_ERR_SHOULD_WAIT      -22
#define AB_ERR_CANCELED         -23
#define AB_ERR_PEER_CLOSED      -24
#define AB_ERR_NOT_FOUND        -25
#define AB_ERR_ACCESS_DENIED    -30

/* Zircon Object Types (zircon_org/system/public/zircon/syscalls/object.h) */
typedef enum {
    AB_OBJ_TYPE_NONE = 0,
    AB_OBJ_TYPE_PROCESS,
    AB_OBJ_TYPE_THREAD,
    AB_OBJ_TYPE_VMO,
    AB_OBJ_TYPE_CHANNEL,
    AB_OBJ_TYPE_EVENT,
    AB_OBJ_TYPE_EVENTPAIR,
    AB_OBJ_TYPE_PORT,
    AB_OBJ_TYPE_INTERRUPT,
    AB_OBJ_TYPE_RESOURCE,
    AB_OBJ_TYPE_LOG,
    AB_OBJ_TYPE_JOB,
    AB_OBJ_TYPE_VMAR,
} ab_obj_type_t;

/* Capability Rights Bitmask (zircon_org/system/public/zircon/rights.h) */
#define AB_RIGHT_NONE           ((ab_rights_t)0U)
#define AB_RIGHT_DUPLICATE      ((ab_rights_t)(1U << 0))
#define AB_RIGHT_TRANSFER       ((ab_rights_t)(1U << 1))
#define AB_RIGHT_READ           ((ab_rights_t)(1U << 2))
#define AB_RIGHT_WRITE          ((ab_rights_t)(1U << 3))
#define AB_RIGHT_EXECUTE        ((ab_rights_t)(1U << 4))
#define AB_RIGHT_MAP            ((ab_rights_t)(1U << 5))
#define AB_RIGHT_GET_PROPERTY   ((ab_rights_t)(1U << 6))
#define AB_RIGHT_SET_PROPERTY   ((ab_rights_t)(1U << 7))
#define AB_RIGHT_SIGNAL         ((ab_rights_t)(1U << 8))
#define AB_RIGHT_SIGNAL_PEER    ((ab_rights_t)(1U << 9))
#define AB_RIGHT_WAIT           ((ab_rights_t)(1U << 10))
#define AB_RIGHT_INSPECT        ((ab_rights_t)(1U << 11))
#define AB_RIGHT_MANAGE_PROCESS ((ab_rights_t)(1U << 12))
#define AB_RIGHT_MANAGE_THREAD  ((ab_rights_t)(1U << 13))
#define AB_RIGHT_SAME_RIGHTS    ((ab_rights_t)(1U << 31))

#define AB_RIGHTS_BASIC            (AB_RIGHT_TRANSFER | AB_RIGHT_DUPLICATE | AB_RIGHT_WAIT | AB_RIGHT_INSPECT)
#define AB_RIGHTS_IO               (AB_RIGHT_READ | AB_RIGHT_WRITE)
#define AB_RIGHTS_DEFAULT_LOG      (AB_RIGHTS_BASIC | AB_RIGHT_WRITE)
#define AB_RIGHTS_DEFAULT_EVENT    (AB_RIGHTS_BASIC | AB_RIGHT_SIGNAL)
#define AB_RIGHTS_DEFAULT_EVENTPAIR (AB_RIGHTS_BASIC | AB_RIGHT_SIGNAL | AB_RIGHT_SIGNAL_PEER)

/* Object Signals Bitmask (zircon_org/system/public/zircon/types.h) */
#define AB_SIGNAL_NONE          ((ab_signals_t)0U)
#define AB_SIGNAL_READABLE      ((ab_signals_t)(1U << 0))
#define AB_SIGNAL_WRITABLE      ((ab_signals_t)(1U << 1))
#define AB_SIGNAL_PEER_CLOSED   ((ab_signals_t)(1U << 2))
#define AB_SIGNAL_SIGNALED      ((ab_signals_t)(1U << 3))
#define AB_USER_SIGNAL_0        ((ab_signals_t)(1U << 24))
#define AB_USER_SIGNAL_1        ((ab_signals_t)(1U << 25))
#define AB_USER_SIGNAL_2        ((ab_signals_t)(1U << 26))
#define AB_USER_SIGNAL_3        ((ab_signals_t)(1U << 27))
#define AB_USER_SIGNAL_ALL \
    (AB_USER_SIGNAL_0 | AB_USER_SIGNAL_1 | AB_USER_SIGNAL_2 | AB_USER_SIGNAL_3)

/* WaitStateObserver node registered on an object's StateTracker */
struct ab_object;
typedef struct ab_wait_observer {
    struct ab_wait_observer *prev;
    struct ab_wait_observer *next;
    ab_signals_t             watched_signals;
    ab_signals_t             wakeup_reasons;
    event_t                  event;
    int                      is_async_port;
    struct ab_object        *port_obj;
    uint64_t                 key;
} ab_wait_observer_t;

/* StateTracker embedded in every Dispatcher */
typedef struct ab_state_tracker {
    spin_lock_t         lock;
    ab_signals_t        signals;
    ab_wait_observer_t *head;
    ab_wait_observer_t *tail;
} ab_state_tracker_t;

/* Object info returned by ab_object_get_info */
typedef struct {
    ab_koid_t      koid;
    ab_koid_t      related_koid;
    ab_obj_type_t  type;
    ab_rights_t    rights;
    uint32_t       handle_count;
} ab_handle_basic_info_t;

struct ab_object;
typedef void (*ab_obj_hook_t)(struct ab_object *obj);

/* Base Kernel Object (models Zircon's Dispatcher class) */
typedef struct ab_object {
    ab_koid_t          koid;
    ab_koid_t          related_koid;
    ab_obj_type_t      type;
    volatile int32_t   ref_count;
    volatile int32_t   handle_count;
    ab_state_tracker_t state_tracker;
    ab_obj_hook_t      on_zero_handles;
    ab_obj_hook_t      destroy;
} ab_object_t;

#define MAX_HANDLES_PER_PROCESS 64

typedef struct {
    ab_object_t *obj;
    ab_rights_t  rights;
    uint32_t     generation;
} ab_handle_entry_t;

typedef struct ab_process {
    ab_object_t        base;
    char               name[32];
    vm_aspace_t       *aspace;
    spin_lock_t        handle_lock;
    ab_handle_entry_t  handles[MAX_HANDLES_PER_PROCESS];
    int                exit_code;
} ab_process_t;

typedef struct {
    ab_object_t base;
    uint32_t    flags;
} ab_log_dispatcher_t;

typedef struct {
    ab_object_t base;
    uint32_t    options;
} ab_event_dispatcher_t;

typedef struct ab_eventpair_dispatcher {
    ab_object_t                     base;
    spin_lock_t                     peer_lock;
    struct ab_eventpair_dispatcher *peer;
} ab_eventpair_dispatcher_t;

ab_koid_t    ab_koid_alloc(void);
ab_object_t *ab_object_alloc(size_t struct_size, ab_obj_type_t type,
                             ab_signals_t initial_signals,
                             ab_obj_hook_t on_zero_handles, ab_obj_hook_t destroy);
void         ab_object_add_ref(ab_object_t *obj);
void         ab_object_release(ab_object_t *obj);
const char  *ab_obj_type_name(ab_obj_type_t type);

/* StateTracker API */
void         state_tracker_update_signals(ab_object_t *obj,
                                          ab_signals_t clear_mask,
                                          ab_signals_t set_mask);
ab_signals_t state_tracker_get_signals(ab_object_t *obj);
ab_status_t  state_tracker_wait_one(ab_object_t *obj,
                                    ab_signals_t watched_signals,
                                    ab_time_t deadline_ns,
                                    ab_signals_t *observed_out);

/* Process & Handle Table API */
ab_process_t *ab_process_create_kernel(const char *name, uint16_t asid);
ab_process_t *get_current_process(void);

ab_status_t handle_alloc(ab_process_t *proc, ab_object_t *obj, ab_rights_t rights,
                         ab_handle_t *out_handle);
ab_status_t handle_lookup(ab_process_t *proc, ab_handle_t h, ab_obj_type_t expected_type,
                          ab_rights_t required_rights, ab_object_t **out_obj,
                          ab_rights_t *out_rights);
ab_status_t handle_close(ab_process_t *proc, ab_handle_t h);
ab_status_t handle_duplicate(ab_process_t *proc, ab_handle_t h, ab_rights_t new_rights,
                             ab_handle_t *out_new_handle);
ab_status_t handle_replace(ab_process_t *proc, ab_handle_t h, ab_rights_t new_rights,
                           ab_handle_t *out_new_handle);

ab_status_t ab_log_dispatcher_create(uint32_t flags, ab_object_t **out_obj);
ab_status_t ab_event_dispatcher_create(uint32_t options, ab_object_t **out_obj);
ab_status_t ab_eventpair_dispatcher_create(ab_object_t **out_obj0,
                                           ab_object_t **out_obj1);
ab_status_t ab_eventpair_signal_peer(ab_object_t *obj,
                                     ab_signals_t clear_mask,
                                     ab_signals_t set_mask);


/* =========================================================================
 * Lesson 09: PortDispatcher & Asynchronous Wait (zx_port_packet_t)
 * References:
 *   - zircon_org/kernel/object/include/object/port_dispatcher.h
 *   - zircon_org/system/public/zircon/syscalls/port.h
 * ========================================================================= */
#define AB_RIGHTS_DEFAULT_PORT \
    (AB_RIGHT_TRANSFER | AB_RIGHT_DUPLICATE | AB_RIGHT_READ | AB_RIGHT_WRITE | AB_RIGHT_INSPECT)

#define AB_PKT_TYPE_USER        0U
#define AB_PKT_TYPE_SIGNAL_ONE  1U

typedef struct {
    uint64_t     key;
    uint32_t     type;
    ab_status_t  status;
    ab_signals_t trigger;
    ab_signals_t observed;
    uint64_t     timestamp;
    uint64_t     user_val;
} ab_port_packet_t;

#define PORT_QUEUE_CAPACITY 32

typedef struct ab_port_dispatcher {
    ab_object_t      base;
    uint32_t         options;
    spin_lock_t      lock;
    ab_port_packet_t packets[PORT_QUEUE_CAPACITY];
    uint32_t         head;
    uint32_t         tail;
    uint32_t         count;
    event_t           readable_event;
} ab_port_dispatcher_t;

ab_status_t ab_port_dispatcher_create(uint32_t options, ab_object_t **out_obj);
ab_status_t ab_port_queue(ab_object_t *port_obj, const ab_port_packet_t *pkt);
ab_status_t ab_port_wait(ab_object_t *port_obj, ab_time_t deadline_ns,
                         ab_port_packet_t *out_pkt);
ab_status_t state_tracker_wait_async(ab_object_t *obj, ab_object_t *port_obj,
                                     uint64_t key, ab_signals_t watched_signals);
#endif /* ABOS_OBJECT_H */
