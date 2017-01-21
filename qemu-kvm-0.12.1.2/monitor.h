#ifndef MONITOR_H
#define MONITOR_H

#include "qemu-common.h"
#include "qemu-char.h"
#include "qerror.h"
#include "qdict.h"
#include "block.h"

extern Monitor *cur_mon;
extern Monitor *default_mon;

/* flags for monitor_init */
#define MONITOR_IS_DEFAULT    0x01
#define MONITOR_USE_READLINE  0x02
#define MONITOR_USE_CONTROL   0x04

/* Red Hat Monitor's prefix (reversed fully qualified domain) */
#define RFQDN_REDHAT "__com.redhat_"

/* flags for monitor commands */
#define MONITOR_CMD_ASYNC       0x0001
#define MONITOR_CMD_USER_ONLY   0x0002
#define MONITOR_CMD_QMP_ONLY    0x0004

/* QMP events */
typedef enum MonitorEvent {
    QEVENT_SHUTDOWN,
    QEVENT_RESET,
    QEVENT_POWERDOWN,
    QEVENT_STOP,
    QEVENT_RESUME,
    QEVENT_VNC_CONNECTED,
    QEVENT_VNC_INITIALIZED,
    QEVENT_VNC_DISCONNECTED,
    QEVENT_BLOCK_IO_ERROR,
    QEVENT_RTC_CHANGE,
    QEVENT_WATCHDOG,
    QEVENT_SPICE_CONNECTED,
    QEVENT_SPICE_INITIALIZED,
    QEVENT_SPICE_DISCONNECTED,
    QEVENT_DEVICE_DELETED,
    QEVENT_DEVICE_TRAY_MOVED,
    QEVENT_BLOCK_JOB_COMPLETED,
    QEVENT_BLOCK_JOB_CANCELLED,
    QEVENT_RH_SPICE_INITIALIZED,
    QEVENT_RH_SPICE_DISCONNECTED,
    QEVENT_SUSPEND,
    QEVENT_SUSPEND_DISK,
    QEVENT_WAKEUP,
    QEVENT_BALLOON_CHANGE,
    QEVENT_SPICE_MIGRATE_COMPLETED,
    QEVENT_GUEST_PANICKED,
    QEVENT_BLOCK_IMAGE_CORRUPTED,

    /* Add to 'monitor_event_names' array in monitor.c when
     * defining new events here */

    QEVENT_MAX,
} MonitorEvent;

int monitor_cur_is_qmp(void);

void monitor_protocol_event(MonitorEvent event, QObject *data);
void monitor_init(CharDriverState *chr, int flags);

int monitor_suspend(Monitor *mon);
void monitor_resume(Monitor *mon);

int monitor_read_bdrv_key_start(Monitor *mon, BlockDriverState *bs,
                                BlockDriverCompletionFunc *completion_cb,
                                void *opaque);

int monitor_get_fd(Monitor *mon, const char *fdname);

void monitor_vprintf(Monitor *mon, const char *fmt, va_list ap);
void monitor_printf(Monitor *mon, const char *fmt, ...)
    __attribute__ ((__format__ (__printf__, 2, 3)));
void monitor_print_filename(Monitor *mon, const char *filename);
void monitor_flush(Monitor *mon);
int monitor_set_cpu(int cpu_index);

typedef void (MonitorCompletion)(void *opaque, QObject *ret_data);

void monitor_set_error(Monitor *mon, QError *qerror);

#endif /* !MONITOR_H */
