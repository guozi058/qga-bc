/*
 * Compatibility for qemu-img/qemu-nbd
 *
 * Copyright IBM, Corp. 2008
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include "qemu-common.h"
#include "monitor.h"
#include "qemu-timer.h"
#include "qemu-log.h"
#include "sysemu.h"
#include "migration.h"

#include <sys/time.h>

QEMUClock *rt_clock;
QEMUClock *vm_clock;

FILE *logfile;

struct QEMUBH
{
    QEMUBHFunc *cb;
    void *opaque;
};

void qemu_service_io(void)
{
}

Monitor *cur_mon;

int monitor_get_fd(Monitor *mon, const char *name)
{
    return -1;
}

int monitor_cur_is_qmp(void)
{
    return 0;
}

void monitor_set_error(Monitor *mon, QError *qerror)
{
}

void monitor_vprintf(Monitor *mon, const char *fmt, va_list ap)
{
}

void monitor_printf(Monitor *mon, const char *fmt, ...)
{
}

void monitor_print_filename(Monitor *mon, const char *filename)
{
}

void monitor_protocol_event(MonitorEvent event, QObject *data)
{
}

QEMUTimer *qemu_new_timer(QEMUClock *clock, QEMUTimerCB *cb, void *opaque)
{
    return qemu_malloc(1);
}

void qemu_free_timer(QEMUTimer *ts)
{
    qemu_free(ts);
}

void qemu_del_timer(QEMUTimer *ts)
{
}

void qemu_mod_timer(QEMUTimer *ts, int64_t expire_time)
{
}

int qemu_timer_pending(QEMUTimer *ts)
{
    return 0;
}

int qemu_set_fd_handler2(int fd,
                         IOCanReadHandler *fd_read_poll,
                         IOHandler *fd_read,
                         IOHandler *fd_write,
                         void *opaque)
{
    return 0;
}

int64_t qemu_get_clock(QEMUClock *clock)
{
    qemu_timeval tv;
    qemu_gettimeofday(&tv);
    return (tv.tv_sec * 1000000000LL + (tv.tv_usec * 1000)) / 1000000;
}

/*
 * XXX: non-functional stub, but we do not need block latency accounting
 * in the tools anyway.
 */
int64_t get_clock(void)
{
	return 0;
}

void qemu_notify_event(void)
{
}

bool runstate_check(RunState state)
{
    return state == RUN_STATE_RUNNING;
}

void qemu_mutex_lock_iothread(void)
{
}

void qemu_mutex_unlock_iothread(void)
{
}

void migrate_add_blocker(Error *reason)
{
}

void migrate_del_blocker(Error *reason)
{
}
