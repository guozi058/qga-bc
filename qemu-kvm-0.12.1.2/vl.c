/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>
#include <grp.h>
#include <zlib.h>
#include "bitmap.h"

/* Needed early for CONFIG_BSD etc. */
#include "config-host.h"

#ifndef _WIN32
#include <libgen.h>
#include <pwd.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <netdb.h>
#include <sys/select.h>
#ifdef CONFIG_BSD
#include <sys/stat.h>
#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__)
#include <libutil.h>
#else
#include <util.h>
#endif
#else
#ifdef __linux__
#include <pty.h>
#include <malloc.h>
#include <linux/rtc.h>
#include <sys/prctl.h>

/* For the benefit of older linux systems which don't supply it,
   we use a local copy of hpet.h. */
/* #include <linux/hpet.h> */
#include "hpet.h"

#include <linux/ppdev.h>
#include <linux/parport.h>
#endif
#ifdef __sun__
#include <sys/stat.h>
#include <sys/ethernet.h>
#include <sys/sockio.h>
#include <netinet/arp.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h> // must come after ip.h
#include <netinet/udp.h>
#include <netinet/tcp.h>
#include <net/if.h>
#include <syslog.h>
#include <stropts.h>
/* See MySQL bug #7156 (http://bugs.mysql.com/bug.php?id=7156) for
   discussion about Solaris header problems */
extern int madvise(caddr_t, size_t, int);
#endif
#endif
#endif

#if defined(__OpenBSD__)
#include <util.h>
#endif

#if defined(CONFIG_VDE)
#include <libvdeplug.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

#ifdef CONFIG_SDL
#if defined(__APPLE__) || defined(main)
#include <SDL.h>
int qemu_main(int argc, char **argv, char **envp);
int main(int argc, char **argv)
{
    return qemu_main(argc, argv, NULL);
}
#undef main
#define main qemu_main
#endif
#endif /* CONFIG_SDL */

#ifdef CONFIG_COCOA
#undef main
#define main qemu_main
#endif /* CONFIG_COCOA */

#include <glib.h>

#include "hw/hw.h"
#include "hw/boards.h"
#include "hw/usb.h"
#include "hw/pcmcia.h"
#include "hw/pc.h"
#include "hw/audiodev.h"
#include "hw/isa.h"
#include "hw/bt.h"
#include "hw/watchdog.h"
#include "hw/smbios.h"
#include "hw/xen.h"
#include "hw/qdev.h"
#include "hw/loader.h"
#include "bt-host.h"
#include "net.h"
#include "net/slirp.h"
#include "monitor.h"
#include "console.h"
#include "sysemu.h"
#include "gdbstub.h"
#include "qemu-timer.h"
#include "qemu-char.h"
#include "cache-utils.h"
#include "block.h"
#include "blockdev.h"
#include "block-migration.h"
#include "dma.h"
#include "audio/audio.h"
#include "migration.h"
#include "kvm.h"
#include "qemu-option.h"
#include "qemu-config.h"
#include "qemu-objects.h"
#include "qemu-kvm.h"
#include "hw/device-assignment.h"
#include "buffered_file.h"

#include "disas.h"

#include "exec-all.h"

#include "qemu_socket.h"

#include "slirp/libslirp.h"

#include "trace.h"
#include "qemu-queue.h"

#include "trace.h"

#include "ui/qemu-spice.h"
#include "qapi/string-input-visitor.h"
#include "qom/object.h"

//#define DEBUG_NET
//#define DEBUG_SLIRP

#define DEFAULT_RAM_SIZE 128

#define MAX_VIRTIO_CONSOLES 1

static const char *data_dir;
const char *bios_name = NULL;
enum vga_retrace_method vga_retrace_method = VGA_RETRACE_DUMB;
static DisplayState *display_state;
DisplayType display_type = DT_DEFAULT;
int display_remote = 0;
const char* keyboard_layout = NULL;
ram_addr_t ram_size;
int nb_nics;
NICInfo nd_table[MAX_NICS];
int autostart;
static int rtc_utc = 1;
static int rtc_date_offset = -1; /* -1 means no change */
QEMUClock *rtc_clock;
int vga_interface_type = VGA_NONE;
#ifdef TARGET_SPARC
int graphic_width = 1024;
int graphic_height = 768;
int graphic_depth = 8;
#else
int graphic_width = 800;
int graphic_height = 600;
int graphic_depth = 15;
#endif
static int full_screen = 0;
#ifdef CONFIG_SDL
static int no_frame = 0;
#endif
int no_quit = 0;
CharDriverState *serial_hds[MAX_SERIAL_PORTS];
CharDriverState *parallel_hds[MAX_PARALLEL_PORTS];
CharDriverState *virtcon_hds[MAX_VIRTIO_CONSOLES];
#ifdef TARGET_I386
int win2k_install_hack = 0;
int rtc_td_hack = 0;
#endif
int usb_enabled = 0;
int singlestep = 0;
const char *assigned_devices[MAX_DEV_ASSIGN_CMDLINE];
int assigned_devices_index;
int smp_cpus = 1;
int max_cpus = 0;
int smp_cores = 1;
int smp_threads = 1;
const char *vnc_display;
int acpi_enabled = 1;
#ifdef TARGET_I386
int no_hpet = 1; /* Always disabled in Red Hat Enterprise Linux */
#endif
int fd_bootchk = 1;
int no_reboot = 0;
int no_shutdown = 0;
int cursor_hide = 1;
int graphic_rotate = 0;
uint8_t irq0override = 1;
#ifndef _WIN32
int daemonize = 0;
#endif
const char *watchdog;
QEMUOptionRom option_rom[MAX_OPTION_ROMS];
int nb_option_roms;
int semihosting_enabled = 0;
int time_drift_fix = 0;
unsigned int kvm_shadow_memory = 0;
const char *mem_path = NULL;
int disable_KSM;
#ifdef MAP_POPULATE
int mem_prealloc = 0; /* force preallocation of physical target memory */
#endif
#ifdef TARGET_ARM
int old_param = 0;
#endif
const char *qemu_name;
int alt_grab = 0;
int ctrl_grab = 0;
#if defined(TARGET_SPARC) || defined(TARGET_PPC)
unsigned int nb_prom_envs = 0;
const char *prom_envs[MAX_PROM_ENVS];
#endif
const char *nvram = NULL;
int boot_menu;
#ifdef CONFIG_FAKE_MACHINE
int fake_machine = 0;
#endif
bool boot_strict;

typedef struct FWBootEntry FWBootEntry;

struct FWBootEntry {
    QTAILQ_ENTRY(FWBootEntry) link;
    int32_t bootindex;
    DeviceState *dev;
    char *suffix;
};

QTAILQ_HEAD(, FWBootEntry) fw_boot_order = QTAILQ_HEAD_INITIALIZER(fw_boot_order);

int nb_numa_nodes;
uint64_t node_mem[MAX_NODES];
unsigned long *node_cpumask[MAX_NODES];

static CPUState *cur_cpu;
static CPUState *next_cpu;
static int timer_alarm_pending = 1;
/* Conversion factor from emulated instructions to virtual clock ticks.  */
static int icount_time_shift;
/* Arbitrarily pick 1MIPS as the minimum allowable speed.  */
#define MAX_ICOUNT_SHIFT 10
/* Compensate for varying guest execution speed.  */
static int64_t qemu_icount_bias;
static QEMUTimer *icount_rt_timer;
static QEMUTimer *icount_vm_timer;

uint8_t qemu_uuid[16];

static QEMUBootSetHandler *boot_set_handler;
static void *boot_set_opaque;

static NotifierList exit_notifiers =
    NOTIFIER_LIST_INITIALIZER(exit_notifiers);

static int tcg_allowed = 1;
int kvm_allowed = 0;
uint32_t xen_domid;
enum xen_mode xen_mode = XEN_EMULATE;

static NotifierList machine_init_done_notifiers =
    NOTIFIER_LIST_INITIALIZER(machine_init_done_notifiers);

static int default_serial = 1;
static int default_parallel = 1;
static int default_virtcon = 1;
static int default_monitor = 1;
static int default_vga = 1;
static int default_floppy = 1;
static int default_cdrom = 1;
static int default_sdcard = 1;

static struct {
    const char *driver;
    int *flag;
} default_list[] = {
    { .driver = "isa-serial",           .flag = &default_serial    },
    { .driver = "isa-parallel",         .flag = &default_parallel  },
    { .driver = "isa-fdc",              .flag = &default_floppy    },
    { .driver = "ide-drive",            .flag = &default_cdrom     },
    { .driver = "virtio-serial-pci",    .flag = &default_virtcon   },
    { .driver = "virtio-serial-s390",   .flag = &default_virtcon   },
    { .driver = "virtio-serial",        .flag = &default_virtcon   },
    { .driver = "VGA",                  .flag = &default_vga       },
    { .driver = "cirrus-vga",           .flag = &default_vga       },
    { .driver = "vmware-svga",          .flag = &default_vga       },
};

static int default_driver_check(QemuOpts *opts, void *opaque)
{
    const char *driver = qemu_opt_get(opts, "driver");
    int i;

    if (!driver)
        return 0;
    for (i = 0; i < ARRAY_SIZE(default_list); i++) {
        if (strcmp(default_list[i].driver, driver) != 0)
            continue;
        *(default_list[i].flag) = 0;
    }
    return 0;
}

/***********************************************************/
/* x86 ISA bus support */

target_phys_addr_t isa_mem_base = 0;
PicState2 *isa_pic;

/* QEMU state */

static RunState current_run_state = RUN_STATE_NO_STATE;

typedef struct {
    RunState from;
    RunState to;
} RunStateTransition;

static const RunStateTransition runstate_transitions_def[] = {
    /*     from      ->     to      */
    { RUN_STATE_NO_STATE, RUN_STATE_RUNNING },
    { RUN_STATE_NO_STATE, RUN_STATE_INMIGRATE },
    { RUN_STATE_NO_STATE, RUN_STATE_PRELAUNCH },

    { RUN_STATE_DEBUG, RUN_STATE_RUNNING },

    { RUN_STATE_INMIGRATE, RUN_STATE_RUNNING },
    { RUN_STATE_INMIGRATE, RUN_STATE_PRELAUNCH },

    { RUN_STATE_INTERNAL_ERROR, RUN_STATE_PAUSED },
    { RUN_STATE_INTERNAL_ERROR, RUN_STATE_FINISH_MIGRATE },

    { RUN_STATE_IO_ERROR, RUN_STATE_RUNNING },
    { RUN_STATE_IO_ERROR, RUN_STATE_FINISH_MIGRATE },

    { RUN_STATE_PAUSED, RUN_STATE_RUNNING },
    { RUN_STATE_PAUSED, RUN_STATE_FINISH_MIGRATE },

    { RUN_STATE_POSTMIGRATE, RUN_STATE_RUNNING },
    { RUN_STATE_POSTMIGRATE, RUN_STATE_FINISH_MIGRATE },

    { RUN_STATE_PRELAUNCH, RUN_STATE_RUNNING },
    { RUN_STATE_PRELAUNCH, RUN_STATE_FINISH_MIGRATE },
    { RUN_STATE_PRELAUNCH, RUN_STATE_INMIGRATE },

    { RUN_STATE_FINISH_MIGRATE, RUN_STATE_RUNNING },
    { RUN_STATE_FINISH_MIGRATE, RUN_STATE_POSTMIGRATE },

    { RUN_STATE_RESTORE_VM, RUN_STATE_RUNNING },

    { RUN_STATE_RUNNING, RUN_STATE_DEBUG },
    { RUN_STATE_RUNNING, RUN_STATE_INTERNAL_ERROR },
    { RUN_STATE_RUNNING, RUN_STATE_IO_ERROR },
    { RUN_STATE_RUNNING, RUN_STATE_PAUSED },
    { RUN_STATE_RUNNING, RUN_STATE_FINISH_MIGRATE },
    { RUN_STATE_RUNNING, RUN_STATE_RESTORE_VM },
    { RUN_STATE_RUNNING, RUN_STATE_SAVE_VM },
    { RUN_STATE_RUNNING, RUN_STATE_SHUTDOWN },
    { RUN_STATE_RUNNING, RUN_STATE_WATCHDOG },
    { RUN_STATE_RUNNING, RUN_STATE_GUEST_PANICKED },

    { RUN_STATE_SAVE_VM, RUN_STATE_RUNNING },

    { RUN_STATE_SHUTDOWN, RUN_STATE_PAUSED },
    { RUN_STATE_SHUTDOWN, RUN_STATE_FINISH_MIGRATE },

    { RUN_STATE_WATCHDOG, RUN_STATE_RUNNING },
    { RUN_STATE_WATCHDOG, RUN_STATE_FINISH_MIGRATE },

    { RUN_STATE_GUEST_PANICKED, RUN_STATE_PAUSED },
    { RUN_STATE_GUEST_PANICKED, RUN_STATE_FINISH_MIGRATE },

    { RUN_STATE_MAX, RUN_STATE_MAX },
};

static bool runstate_valid_transitions[RUN_STATE_MAX][RUN_STATE_MAX];

static const char *const runstate_name_tbl[RUN_STATE_MAX] = {
    [RUN_STATE_DEBUG] = "debug",
    [RUN_STATE_INMIGRATE] = "incoming-migration",
    [RUN_STATE_INTERNAL_ERROR] = "internal-error",
    [RUN_STATE_IO_ERROR] = "io-error",
    [RUN_STATE_PAUSED] = "paused",
    [RUN_STATE_POSTMIGRATE] = "post-migrate",
    [RUN_STATE_PRELAUNCH] = "prelaunch",
    [RUN_STATE_FINISH_MIGRATE] = "finish-migrate",
    [RUN_STATE_RESTORE_VM] = "restore-vm",
    [RUN_STATE_RUNNING] = "running",
    [RUN_STATE_SAVE_VM] = "save-vm",
    [RUN_STATE_SHUTDOWN] = "shutdown",
    [RUN_STATE_GUEST_PANICKED] = "guest-panicked",
    [RUN_STATE_WATCHDOG] = "watchdog",
};

bool runstate_check(RunState state)
{
    return current_run_state == state;
}

void runstate_init(void)
{
    const RunStateTransition *p;

    memset(&runstate_valid_transitions, 0, sizeof(runstate_valid_transitions));

    for (p = &runstate_transitions_def[0]; p->from != RUN_STATE_MAX; p++) {
        runstate_valid_transitions[p->from][p->to] = true;
    }
}

/* This function will abort() on invalid state transitions */
void runstate_set(RunState new_state)
{
    assert(new_state < RUN_STATE_MAX);

    if (!runstate_valid_transitions[current_run_state][new_state]) {
        fprintf(stderr, "invalid runstate transition: %s -> %s\n",
                runstate_name_tbl[current_run_state],
                runstate_name_tbl[new_state]);
        /* We've decided to disable this check in RHEL6 because we're not
           confident enough that we've captured all valid transitions */
        // abort();
    }
    trace_runstate_set(new_state);
    current_run_state = new_state;
}

const char *runstate_as_string(void)
{
    assert(current_run_state > RUN_STATE_NO_STATE &&
           current_run_state < RUN_STATE_MAX);
    return runstate_name_tbl[current_run_state];
}

int runstate_is_running(void)
{
    return runstate_check(RUN_STATE_RUNNING);
}

bool runstate_needs_reset(void)
{
    return runstate_check(RUN_STATE_INTERNAL_ERROR) ||
        runstate_check(RUN_STATE_SHUTDOWN) ||
        runstate_check(RUN_STATE_GUEST_PANICKED);
}

/***********************************************************/
void hw_error(const char *fmt, ...)
{
    va_list ap;
    CPUState *env;

    va_start(ap, fmt);
    fprintf(stderr, "qemu: hardware error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    for(env = first_cpu; env != NULL; env = env->next_cpu) {
        fprintf(stderr, "CPU #%d:\n", env->cpu_index);
#ifdef TARGET_I386
        cpu_dump_state(env, stderr, fprintf, X86_DUMP_FPU);
#else
        cpu_dump_state(env, stderr, fprintf, 0);
#endif
    }
    va_end(ap);
    abort();
}

static void set_proc_name(const char *s)
{
#if defined(__linux__) && defined(PR_SET_NAME)
    char name[16];
    if (!s)
        return;
    name[sizeof(name) - 1] = 0;
    strncpy(name, s, sizeof(name));
    /* Could rewrite argv[0] too, but that's a bit more complicated.
       This simple way is enough for `top'. */
    prctl(PR_SET_NAME, name);
#endif    	
}
 
/***********************************************************/
/* keyboard/mouse */

static QEMUPutKBDEvent *qemu_put_kbd_event;
static void *qemu_put_kbd_event_opaque;
static QTAILQ_HEAD(, QEMUPutLEDEntry) led_handlers = QTAILQ_HEAD_INITIALIZER(led_handlers);
static int ledstate;
static QTAILQ_HEAD(, QEMUPutMouseEntry) mouse_handlers =
    QTAILQ_HEAD_INITIALIZER(mouse_handlers);
static NotifierList mouse_mode_notifiers = 
    NOTIFIER_LIST_INITIALIZER(mouse_mode_notifiers);

void qemu_add_kbd_event_handler(QEMUPutKBDEvent *func, void *opaque)
{
    qemu_put_kbd_event_opaque = opaque;
    qemu_put_kbd_event = func;
}

void qemu_remove_kbd_event_handler(void)
{
    qemu_put_kbd_event_opaque = NULL;
    qemu_put_kbd_event = NULL;
}

static void check_mode_change(void)
{
    static int current_is_absolute, current_has_absolute;
    int is_absolute;
    int has_absolute;

    is_absolute = kbd_mouse_is_absolute();
    has_absolute = kbd_mouse_has_absolute();

    if (is_absolute != current_is_absolute ||
        has_absolute != current_has_absolute) {
        notifier_list_notify(&mouse_mode_notifiers, NULL);
    }

    current_is_absolute = is_absolute;
    current_has_absolute = has_absolute;
}

QEMUPutMouseEntry *qemu_add_mouse_event_handler(QEMUPutMouseEvent *func,
                                                void *opaque, int absolute,
                                                const char *name)
{
    QEMUPutMouseEntry *s;
    static int mouse_index = 0;

    s = qemu_mallocz(sizeof(QEMUPutMouseEntry));

    s->qemu_put_mouse_event = func;
    s->qemu_put_mouse_event_opaque = opaque;
    s->qemu_put_mouse_event_absolute = absolute;
    s->qemu_put_mouse_event_name = qemu_strdup(name);
    s->index = mouse_index++;

    QTAILQ_INSERT_TAIL(&mouse_handlers, s, node);

    check_mode_change();

    return s;
}

void qemu_activate_mouse_event_handler(QEMUPutMouseEntry *entry)
{
    QTAILQ_REMOVE(&mouse_handlers, entry, node);
    QTAILQ_INSERT_HEAD(&mouse_handlers, entry, node);

    check_mode_change();
}

void qemu_remove_mouse_event_handler(QEMUPutMouseEntry *entry)
{
    QTAILQ_REMOVE(&mouse_handlers, entry, node);

    qemu_free(entry->qemu_put_mouse_event_name);
    qemu_free(entry);

    check_mode_change();
}

QEMUPutLEDEntry *qemu_add_led_event_handler(QEMUPutLEDEvent *func,
                                            void *opaque)
{
    QEMUPutLEDEntry *s;

    s = qemu_mallocz(sizeof(QEMUPutLEDEntry));

    s->put_led = func;
    s->opaque = opaque;
    QTAILQ_INSERT_TAIL(&led_handlers, s, next);
    return s;
}

void qemu_remove_led_event_handler(QEMUPutLEDEntry *entry)
{
    if (entry == NULL)
        return;
    QTAILQ_REMOVE(&led_handlers, entry, next);
    qemu_free(entry);
}

void kbd_put_keycode(int keycode)
{
    if (!runstate_is_running()) {
        return;
    }
    if (qemu_put_kbd_event) {
        qemu_put_kbd_event(qemu_put_kbd_event_opaque, keycode);
    }
}

void kbd_put_ledstate(int l)
{
    QEMUPutLEDEntry *cursor;

    ledstate = l;
    QTAILQ_FOREACH(cursor, &led_handlers, next) {
        cursor->put_led(cursor->opaque, ledstate);
    }
}

int kbd_get_ledstate(void)
{
    return ledstate;
}

void kbd_mouse_event(int dx, int dy, int dz, int buttons_state)
{
    QEMUPutMouseEntry *entry;
    QEMUPutMouseEvent *mouse_event;
    void *mouse_event_opaque;
    int width;

    if (!runstate_is_running()) {
        return;
    }
    if (QTAILQ_EMPTY(&mouse_handlers)) {
        return;
    }

    entry = QTAILQ_FIRST(&mouse_handlers);

    mouse_event = entry->qemu_put_mouse_event;
    mouse_event_opaque = entry->qemu_put_mouse_event_opaque;

    if (mouse_event) {
        if (graphic_rotate) {
            if (entry->qemu_put_mouse_event_absolute)
                width = 0x7fff;
            else
                width = graphic_width - 1;
            mouse_event(mouse_event_opaque,
                        width - dy, dx, dz, buttons_state);
        } else
            mouse_event(mouse_event_opaque,
                        dx, dy, dz, buttons_state);
    }
}

int kbd_mouse_is_absolute(void)
{
    if (QTAILQ_EMPTY(&mouse_handlers)) {
        return 0;
    }

    return QTAILQ_FIRST(&mouse_handlers)->qemu_put_mouse_event_absolute;
}

int kbd_mouse_has_absolute(void)
{
    QEMUPutMouseEntry *entry;

    QTAILQ_FOREACH(entry, &mouse_handlers, node) {
        if (entry->qemu_put_mouse_event_absolute) {
            return 1;
        }
    }

    return 0;
}

static void info_mice_iter(QObject *data, void *opaque)
{
    QDict *mouse;
    Monitor *mon = opaque;

    mouse = qobject_to_qdict(data);
    monitor_printf(mon, "%c Mouse #%" PRId64 ": %s\n",
                  (qdict_get_bool(mouse, "current") ? '*' : ' '),
                  qdict_get_int(mouse, "index"), qdict_get_str(mouse, "name"));
}

void do_info_mice_print(Monitor *mon, const QObject *data)
{
    QList *mice_list;

    mice_list = qobject_to_qlist(data);
    if (qlist_empty(mice_list)) {
        monitor_printf(mon, "No mouse devices connected\n");
        return;
    }

    qlist_iter(mice_list, info_mice_iter, mon);
}

void do_info_mice(Monitor *mon, QObject **ret_data)
{
    QEMUPutMouseEntry *cursor;
    QList *mice_list;
    int current;

    mice_list = qlist_new();

    if (QTAILQ_EMPTY(&mouse_handlers)) {
        goto out;
    }

    current = QTAILQ_FIRST(&mouse_handlers)->index;

    QTAILQ_FOREACH(cursor, &mouse_handlers, node) {
        QObject *obj;
        obj = qobject_from_jsonf("{ 'name': %s, 'index': %d, 'current': %i }",
                                 cursor->qemu_put_mouse_event_name,
                                 cursor->index,
                                 cursor->index == current);
        qlist_append_obj(mice_list, obj);
    }

out:
    *ret_data = QOBJECT(mice_list);
}

void do_mouse_set(Monitor *mon, const QDict *qdict)
{
    QEMUPutMouseEntry *cursor;
    int index = qdict_get_int(qdict, "index");
    int found = 0;

    if (QTAILQ_EMPTY(&mouse_handlers)) {
        monitor_printf(mon, "No mouse devices connected\n");
        return;
    }

    QTAILQ_FOREACH(cursor, &mouse_handlers, node) {
        if (cursor->index == index) {
            found = 1;
            qemu_activate_mouse_event_handler(cursor);
            break;
        }
    }

    if (!found) {
        monitor_printf(mon, "Mouse at given index not found\n");
    }

    check_mode_change();
}

void qemu_add_mouse_mode_change_notifier(Notifier *notify)
{
    notifier_list_add(&mouse_mode_notifiers, notify);
}

void qemu_remove_mouse_mode_change_notifier(Notifier *notify)
{
    notifier_list_remove(&mouse_mode_notifiers, notify);
}

/* compute with 96 bit intermediate result: (a*b)/c */
uint64_t muldiv64(uint64_t a, uint32_t b, uint32_t c)
{
    union {
        uint64_t ll;
        struct {
#ifdef HOST_WORDS_BIGENDIAN
            uint32_t high, low;
#else
            uint32_t low, high;
#endif
        } l;
    } u, res;
    uint64_t rl, rh;

    u.ll = a;
    rl = (uint64_t)u.l.low * (uint64_t)b;
    rh = (uint64_t)u.l.high * (uint64_t)b;
    rh += (rl >> 32);
    res.l.high = rh / c;
    res.l.low = (((rh % c) << 32) + (rl & 0xffffffff)) / c;
    return res.ll;
}

/***********************************************************/
/* real time host monotonic timer */

static int64_t get_clock_realtime(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000000LL + (tv.tv_usec * 1000);
}

#ifdef WIN32

static int64_t clock_freq;

static void init_get_clock(void)
{
    LARGE_INTEGER freq;
    int ret;
    ret = QueryPerformanceFrequency(&freq);
    if (ret == 0) {
        fprintf(stderr, "Could not calibrate ticks\n");
        exit(1);
    }
    clock_freq = freq.QuadPart;
}

int64_t get_clock(void)
{
    LARGE_INTEGER ti;
    QueryPerformanceCounter(&ti);
    return muldiv64(ti.QuadPart, get_ticks_per_sec(), clock_freq);
}

#else

static int use_rt_clock;

static void init_get_clock(void)
{
    use_rt_clock = 0;
#if defined(__linux__) || (defined(__FreeBSD__) && __FreeBSD_version >= 500000) \
    || defined(__DragonFly__) || defined(__FreeBSD_kernel__)
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            use_rt_clock = 1;
        }
    }
#endif
}

int64_t get_clock(void)
{
#if defined(__linux__) || (defined(__FreeBSD__) && __FreeBSD_version >= 500000) \
	|| defined(__DragonFly__) || defined(__FreeBSD_kernel__)
    if (use_rt_clock) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec * 1000000000LL + ts.tv_nsec;
    } else
#endif
    {
        /* XXX: using gettimeofday leads to problems if the date
           changes, so it should be avoided. */
        return get_clock_realtime();
    }
}
#endif

/* Return the virtual CPU time, based on the instruction counter.  */
static int64_t cpu_get_icount(void)
{
    int64_t icount;
    CPUState *env = cpu_single_env;;
    icount = qemu_icount;
    if (env) {
        if (!can_do_io(env))
            fprintf(stderr, "Bad clock read\n");
        icount -= (env->icount_decr.u16.low + env->icount_extra);
    }
    return qemu_icount_bias + (icount << icount_time_shift);
}

/***********************************************************/
/* guest cycle counter */

typedef struct TimersState {
    int64_t cpu_ticks_prev;
    int64_t cpu_ticks_offset;
    int64_t cpu_clock_offset;
    int32_t cpu_ticks_enabled;
    int64_t dummy;
} TimersState;

TimersState timers_state;

/* return the host CPU cycle counter and handle stop/restart */
int64_t cpu_get_ticks(void)
{
    if (use_icount) {
        return cpu_get_icount();
    }
    if (!timers_state.cpu_ticks_enabled) {
        return timers_state.cpu_ticks_offset;
    } else {
        int64_t ticks;
        ticks = cpu_get_real_ticks();
        if (timers_state.cpu_ticks_prev > ticks) {
            /* Note: non increasing ticks may happen if the host uses
               software suspend */
            timers_state.cpu_ticks_offset += timers_state.cpu_ticks_prev - ticks;
        }
        timers_state.cpu_ticks_prev = ticks;
        return ticks + timers_state.cpu_ticks_offset;
    }
}

/* return the host CPU monotonic timer and handle stop/restart */
static int64_t cpu_get_clock(void)
{
    int64_t ti;
    if (!timers_state.cpu_ticks_enabled) {
        return timers_state.cpu_clock_offset;
    } else {
        ti = get_clock();
        return ti + timers_state.cpu_clock_offset;
    }
}

/* enable cpu_get_ticks() */
void cpu_enable_ticks(void)
{
    if (!timers_state.cpu_ticks_enabled) {
        timers_state.cpu_ticks_offset -= cpu_get_real_ticks();
        timers_state.cpu_clock_offset -= get_clock();
        timers_state.cpu_ticks_enabled = 1;
    }
}

/* disable cpu_get_ticks() : the clock is stopped. You must not call
   cpu_get_ticks() after that.  */
void cpu_disable_ticks(void)
{
    if (timers_state.cpu_ticks_enabled) {
        timers_state.cpu_ticks_offset = cpu_get_ticks();
        timers_state.cpu_clock_offset = cpu_get_clock();
        timers_state.cpu_ticks_enabled = 0;
    }
}

/***********************************************************/
/* timers */

#define QEMU_CLOCK_REALTIME 0
#define QEMU_CLOCK_VIRTUAL  1
#define QEMU_CLOCK_HOST     2

struct QEMUClock {
    int type;
    /* XXX: add frequency */

    NotifierList reset_notifiers;
    int64_t last;
};

struct QEMUTimer {
    QEMUClock *clock;
    int64_t expire_time;
    QEMUTimerCB *cb;
    void *opaque;
    struct QEMUTimer *next;
};

struct qemu_alarm_timer {
    char const *name;
    unsigned int flags;

    int (*start)(struct qemu_alarm_timer *t);
    void (*stop)(struct qemu_alarm_timer *t);
    void (*rearm)(struct qemu_alarm_timer *t);
    void *priv;
};

#define ALARM_FLAG_DYNTICKS  0x1
#define ALARM_FLAG_EXPIRED   0x2

static inline int alarm_has_dynticks(struct qemu_alarm_timer *t)
{
    return t && (t->flags & ALARM_FLAG_DYNTICKS);
}

static void qemu_rearm_alarm_timer(struct qemu_alarm_timer *t)
{
    if (!alarm_has_dynticks(t))
        return;

    t->rearm(t);
}

/* TODO: MIN_TIMER_REARM_US should be optimized */
#define MIN_TIMER_REARM_US 250

static struct qemu_alarm_timer *alarm_timer;

#ifdef _WIN32

struct qemu_alarm_win32 {
    MMRESULT timerId;
    unsigned int period;
} alarm_win32_data = {0, -1};

static int win32_start_timer(struct qemu_alarm_timer *t);
static void win32_stop_timer(struct qemu_alarm_timer *t);
static void win32_rearm_timer(struct qemu_alarm_timer *t);

#else

static int unix_start_timer(struct qemu_alarm_timer *t);
static void unix_stop_timer(struct qemu_alarm_timer *t);

#ifdef __linux__

static int dynticks_start_timer(struct qemu_alarm_timer *t);
static void dynticks_stop_timer(struct qemu_alarm_timer *t);
static void dynticks_rearm_timer(struct qemu_alarm_timer *t);

static int hpet_start_timer(struct qemu_alarm_timer *t);
static void hpet_stop_timer(struct qemu_alarm_timer *t);

static int rtc_start_timer(struct qemu_alarm_timer *t);
static void rtc_stop_timer(struct qemu_alarm_timer *t);

#endif /* __linux__ */

#endif /* _WIN32 */

/* Correlation between real and virtual time is always going to be
   fairly approximate, so ignore small variation.
   When the guest is idle real and virtual time will be aligned in
   the IO wait loop.  */
#define ICOUNT_WOBBLE (get_ticks_per_sec() / 10)

static void icount_adjust(void)
{
    int64_t cur_time;
    int64_t cur_icount;
    int64_t delta;
    static int64_t last_delta;
    /* If the VM is not running, then do nothing.  */
    if (!runstate_is_running())
        return;

    cur_time = cpu_get_clock();
    cur_icount = qemu_get_clock(vm_clock);
    delta = cur_icount - cur_time;
    /* FIXME: This is a very crude algorithm, somewhat prone to oscillation.  */
    if (delta > 0
        && last_delta + ICOUNT_WOBBLE < delta * 2
        && icount_time_shift > 0) {
        /* The guest is getting too far ahead.  Slow time down.  */
        icount_time_shift--;
    }
    if (delta < 0
        && last_delta - ICOUNT_WOBBLE > delta * 2
        && icount_time_shift < MAX_ICOUNT_SHIFT) {
        /* The guest is getting too far behind.  Speed time up.  */
        icount_time_shift++;
    }
    last_delta = delta;
    qemu_icount_bias = cur_icount - (qemu_icount << icount_time_shift);
}

static void icount_adjust_rt(void * opaque)
{
    qemu_mod_timer(icount_rt_timer,
                   qemu_get_clock(rt_clock) + 1000);
    icount_adjust();
}

static void icount_adjust_vm(void * opaque)
{
    qemu_mod_timer(icount_vm_timer,
                   qemu_get_clock(vm_clock) + get_ticks_per_sec() / 10);
    icount_adjust();
}

static void init_icount_adjust(void)
{
    /* Have both realtime and virtual time triggers for speed adjustment.
       The realtime trigger catches emulated time passing too slowly,
       the virtual time trigger catches emulated time passing too fast.
       Realtime triggers occur even when idle, so use them less frequently
       than VM triggers.  */
    icount_rt_timer = qemu_new_timer(rt_clock, icount_adjust_rt, NULL);
    qemu_mod_timer(icount_rt_timer,
                   qemu_get_clock(rt_clock) + 1000);
    icount_vm_timer = qemu_new_timer(vm_clock, icount_adjust_vm, NULL);
    qemu_mod_timer(icount_vm_timer,
                   qemu_get_clock(vm_clock) + get_ticks_per_sec() / 10);
}

static struct qemu_alarm_timer alarm_timers[] = {
#ifndef _WIN32
#ifdef __linux__
    {"dynticks", ALARM_FLAG_DYNTICKS, dynticks_start_timer,
     dynticks_stop_timer, dynticks_rearm_timer, NULL},
    /* HPET - if available - is preferred */
    {"hpet", 0, hpet_start_timer, hpet_stop_timer, NULL, NULL},
    /* ...otherwise try RTC */
    {"rtc", 0, rtc_start_timer, rtc_stop_timer, NULL, NULL},
#endif
    {"unix", 0, unix_start_timer, unix_stop_timer, NULL, NULL},
#else
    {"dynticks", ALARM_FLAG_DYNTICKS, win32_start_timer,
     win32_stop_timer, win32_rearm_timer, &alarm_win32_data},
    {"win32", 0, win32_start_timer,
     win32_stop_timer, NULL, &alarm_win32_data},
#endif
    {NULL, }
};

static void show_available_alarms(void)
{
    int i;

    printf("Available alarm timers, in order of precedence:\n");
    for (i = 0; alarm_timers[i].name; i++)
        printf("%s\n", alarm_timers[i].name);
}

static void configure_alarms(char const *opt)
{
    int i;
    int cur = 0;
    int count = ARRAY_SIZE(alarm_timers) - 1;
    char *arg;
    char *name;
    struct qemu_alarm_timer tmp;

    if (!strcmp(opt, "?")) {
        show_available_alarms();
        exit(0);
    }

    arg = qemu_strdup(opt);

    /* Reorder the array */
    name = strtok(arg, ",");
    while (name) {
        for (i = 0; i < count && alarm_timers[i].name; i++) {
            if (!strcmp(alarm_timers[i].name, name))
                break;
        }

        if (i == count) {
            fprintf(stderr, "Unknown clock %s\n", name);
            goto next;
        }

        if (i < cur)
            /* Ignore */
            goto next;

	/* Swap */
        tmp = alarm_timers[i];
        alarm_timers[i] = alarm_timers[cur];
        alarm_timers[cur] = tmp;

        cur++;
next:
        name = strtok(NULL, ",");
    }

    qemu_free(arg);

    if (cur) {
        /* Disable remaining timers */
        for (i = cur; i < count; i++)
            alarm_timers[i].name = NULL;
    } else {
        show_available_alarms();
        exit(1);
    }
}

#define QEMU_NUM_CLOCKS 3

QEMUClock *rt_clock;
QEMUClock *vm_clock;
QEMUClock *host_clock;

static QEMUTimer *active_timers[QEMU_NUM_CLOCKS];

static QEMUClock *qemu_new_clock(int type)
{
    QEMUClock *clock;

    clock = qemu_mallocz(sizeof(QEMUClock));
    clock->type = type;
    notifier_list_init(&clock->reset_notifiers);
    /* required to detect & report backward jumps */
    if (type == QEMU_CLOCK_HOST) {
        clock->last = get_clock_realtime();
    }
    return clock;
}

QEMUTimer *qemu_new_timer(QEMUClock *clock, QEMUTimerCB *cb, void *opaque)
{
    QEMUTimer *ts;

    ts = qemu_mallocz(sizeof(QEMUTimer));
    ts->clock = clock;
    ts->cb = cb;
    ts->opaque = opaque;
    return ts;
}

void qemu_free_timer(QEMUTimer *ts)
{
    qemu_free(ts);
}

/* stop a timer, but do not dealloc it */
void qemu_del_timer(QEMUTimer *ts)
{
    QEMUTimer **pt, *t;

    /* NOTE: this code must be signal safe because
       qemu_timer_expired() can be called from a signal. */
    pt = &active_timers[ts->clock->type];
    for(;;) {
        t = *pt;
        if (!t)
            break;
        if (t == ts) {
            *pt = t->next;
            break;
        }
        pt = &t->next;
    }
}

/* modify the current timer so that it will be fired when current_time
   >= expire_time. The corresponding callback will be called. */
void qemu_mod_timer(QEMUTimer *ts, int64_t expire_time)
{
    QEMUTimer **pt, *t;

    qemu_del_timer(ts);

    /* add the timer in the sorted list */
    /* NOTE: this code must be signal safe because
       qemu_timer_expired() can be called from a signal. */
    pt = &active_timers[ts->clock->type];
    for(;;) {
        t = *pt;
        if (!t)
            break;
        if (t->expire_time > expire_time)
            break;
        pt = &t->next;
    }
    ts->expire_time = expire_time;
    ts->next = *pt;
    *pt = ts;

    /* Rearm if necessary  */
    if (pt == &active_timers[ts->clock->type]) {
        if ((alarm_timer->flags & ALARM_FLAG_EXPIRED) == 0) {
            qemu_rearm_alarm_timer(alarm_timer);
        }
        /* Interrupt execution to force deadline recalculation.  */
        if (use_icount)
            qemu_notify_event();
    }
}

int qemu_timer_pending(QEMUTimer *ts)
{
    QEMUTimer *t;
    for(t = active_timers[ts->clock->type]; t != NULL; t = t->next) {
        if (t == ts)
            return 1;
    }
    return 0;
}

int qemu_timer_expired(QEMUTimer *timer_head, int64_t current_time)
{
    if (!timer_head)
        return 0;
    return (timer_head->expire_time <= current_time);
}

static void qemu_run_timers(QEMUTimer **ptimer_head, int64_t current_time)
{
    QEMUTimer *ts;

    for(;;) {
        ts = *ptimer_head;
        if (!ts || ts->expire_time > current_time)
            break;
        /* remove timer from the list before calling the callback */
        *ptimer_head = ts->next;
        ts->next = NULL;

        /* run the callback (the timer list can be modified) */
        ts->cb(ts->opaque);
    }
}

int64_t qemu_get_clock(QEMUClock *clock)
{
    int64_t now, last;

    switch(clock->type) {
    case QEMU_CLOCK_REALTIME:
        return get_clock() / 1000000;
    default:
    case QEMU_CLOCK_VIRTUAL:
        if (use_icount) {
            return cpu_get_icount();
        } else {
            return cpu_get_clock();
        }
    case QEMU_CLOCK_HOST:
        now = get_clock_realtime();
        last = clock->last;
        clock->last = now;
        if (now < last) {
            notifier_list_notify(&clock->reset_notifiers, &now);
        }
        return now;
    }
}

void qemu_register_clock_reset_notifier(QEMUClock *clock, Notifier *notifier)
{
    notifier_list_add(&clock->reset_notifiers, notifier);
}

void qemu_unregister_clock_reset_notifier(QEMUClock *clock, Notifier *notifier)
{
    notifier_list_remove(&clock->reset_notifiers, notifier);
}

static void init_clocks(void)
{
    init_get_clock();
    rt_clock = qemu_new_clock(QEMU_CLOCK_REALTIME);
    vm_clock = qemu_new_clock(QEMU_CLOCK_VIRTUAL);
    host_clock = qemu_new_clock(QEMU_CLOCK_HOST);

    rtc_clock = host_clock;
}

/* save a timer */
void qemu_put_timer(QEMUFile *f, QEMUTimer *ts)
{
    uint64_t expire_time;

    if (qemu_timer_pending(ts)) {
        expire_time = ts->expire_time;
    } else {
        expire_time = -1;
    }
    qemu_put_be64(f, expire_time);
}

void qemu_get_timer(QEMUFile *f, QEMUTimer *ts)
{
    uint64_t expire_time;

    expire_time = qemu_get_be64(f);
    if (expire_time != -1) {
        qemu_mod_timer(ts, expire_time);
    } else {
        qemu_del_timer(ts);
    }
}

static const VMStateDescription vmstate_timers = {
    .name = "timer",
    .version_id = 2,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_INT64(cpu_ticks_offset, TimersState),
        VMSTATE_INT64(dummy, TimersState),
        VMSTATE_INT64_V(cpu_clock_offset, TimersState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void qemu_event_increment(void);

#ifdef _WIN32
static void CALLBACK host_alarm_handler(UINT uTimerID, UINT uMsg,
                                        DWORD_PTR dwUser, DWORD_PTR dw1,
                                        DWORD_PTR dw2)
#else
static void host_alarm_handler(int host_signum)
#endif
{
#if 0
#define DISP_FREQ 1000
    {
        static int64_t delta_min = INT64_MAX;
        static int64_t delta_max, delta_cum, last_clock, delta, ti;
        static int count;
        ti = qemu_get_clock(vm_clock);
        if (last_clock != 0) {
            delta = ti - last_clock;
            if (delta < delta_min)
                delta_min = delta;
            if (delta > delta_max)
                delta_max = delta;
            delta_cum += delta;
            if (++count == DISP_FREQ) {
                printf("timer: min=%" PRId64 " us max=%" PRId64 " us avg=%" PRId64 " us avg_freq=%0.3f Hz\n",
                       muldiv64(delta_min, 1000000, get_ticks_per_sec()),
                       muldiv64(delta_max, 1000000, get_ticks_per_sec()),
                       muldiv64(delta_cum, 1000000 / DISP_FREQ, get_ticks_per_sec()),
                       (double)get_ticks_per_sec() / ((double)delta_cum / DISP_FREQ));
                count = 0;
                delta_min = INT64_MAX;
                delta_max = 0;
                delta_cum = 0;
            }
        }
        last_clock = ti;
    }
#endif
    if (alarm_has_dynticks(alarm_timer) ||
        (!use_icount &&
            qemu_timer_expired(active_timers[QEMU_CLOCK_VIRTUAL],
                               qemu_get_clock(vm_clock))) ||
        qemu_timer_expired(active_timers[QEMU_CLOCK_REALTIME],
                           qemu_get_clock(rt_clock)) ||
        qemu_timer_expired(active_timers[QEMU_CLOCK_HOST],
                           qemu_get_clock(host_clock))) {
        qemu_event_increment();
        if (alarm_timer) alarm_timer->flags |= ALARM_FLAG_EXPIRED;

#ifndef CONFIG_IOTHREAD
        if (next_cpu) {
            /* stop the currently executing cpu because a timer occured */
            cpu_exit(next_cpu);
        }
#endif
        timer_alarm_pending = 1;
        qemu_notify_event();
    }
}

static int64_t qemu_next_deadline(void)
{
    /* To avoid problems with overflow limit this to 2^32.  */
    int64_t delta = INT32_MAX;

    if (active_timers[QEMU_CLOCK_VIRTUAL]) {
        delta = active_timers[QEMU_CLOCK_VIRTUAL]->expire_time -
                     qemu_get_clock(vm_clock);
    }
    if (active_timers[QEMU_CLOCK_HOST]) {
        int64_t hdelta = active_timers[QEMU_CLOCK_HOST]->expire_time -
                 qemu_get_clock(host_clock);
        if (hdelta < delta)
            delta = hdelta;
    }

    if (delta < 0)
        delta = 0;

    return delta;
}

#if defined(__linux__)
static uint64_t qemu_next_deadline_dyntick(void)
{
    int64_t delta;
    int64_t rtdelta;

    if (use_icount)
        delta = INT32_MAX;
    else
        delta = (qemu_next_deadline() + 999) / 1000;

    if (active_timers[QEMU_CLOCK_REALTIME]) {
        rtdelta = (active_timers[QEMU_CLOCK_REALTIME]->expire_time -
                 qemu_get_clock(rt_clock))*1000;
        if (rtdelta < delta)
            delta = rtdelta;
    }

    if (delta < MIN_TIMER_REARM_US)
        delta = MIN_TIMER_REARM_US;

    return delta;
}
#endif

#ifndef _WIN32

#if defined(__linux__)

#define RTC_FREQ 1024

static void enable_sigio_timer(int fd)
{
    struct sigaction act;

    /* timer signal */
    sigfillset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = host_alarm_handler;

    sigaction(SIGIO, &act, NULL);
    fcntl_setfl(fd, O_ASYNC);
    fcntl(fd, F_SETOWN, getpid());
}

static int hpet_start_timer(struct qemu_alarm_timer *t)
{
    struct hpet_info info;
    int r, fd;

    fd = qemu_open("/dev/hpet", O_RDONLY);
    if (fd < 0)
        return -1;

    /* Set frequency */
    r = ioctl(fd, HPET_IRQFREQ, RTC_FREQ);
    if (r < 0) {
        fprintf(stderr, "Could not configure '/dev/hpet' to have a 1024Hz timer. This is not a fatal\n"
                "error, but for better emulation accuracy type:\n"
                "'echo 1024 > /proc/sys/dev/hpet/max-user-freq' as root.\n");
        goto fail;
    }

    /* Check capabilities */
    r = ioctl(fd, HPET_INFO, &info);
    if (r < 0)
        goto fail;

    /* Enable periodic mode */
    r = ioctl(fd, HPET_EPI, 0);
    if (info.hi_flags && (r < 0))
        goto fail;

    /* Enable interrupt */
    r = ioctl(fd, HPET_IE_ON, 0);
    if (r < 0)
        goto fail;

    enable_sigio_timer(fd);
    t->priv = (void *)(long)fd;

    return 0;
fail:
    close(fd);
    return -1;
}

static void hpet_stop_timer(struct qemu_alarm_timer *t)
{
    int fd = (long)t->priv;

    close(fd);
}

static int rtc_start_timer(struct qemu_alarm_timer *t)
{
    int rtc_fd;
    unsigned long current_rtc_freq = 0;

    TFR(rtc_fd = qemu_open("/dev/rtc", O_RDONLY));
    if (rtc_fd < 0)
        return -1;
    ioctl(rtc_fd, RTC_IRQP_READ, &current_rtc_freq);
    if (current_rtc_freq != RTC_FREQ &&
        ioctl(rtc_fd, RTC_IRQP_SET, RTC_FREQ) < 0) {
        fprintf(stderr, "Could not configure '/dev/rtc' to have a 1024 Hz timer. This is not a fatal\n"
                "error, but for better emulation accuracy either use a 2.6 host Linux kernel or\n"
                "type 'echo 1024 > /proc/sys/dev/rtc/max-user-freq' as root.\n");
        goto fail;
    }
    if (ioctl(rtc_fd, RTC_PIE_ON, 0) < 0) {
    fail:
        close(rtc_fd);
        return -1;
    }

    enable_sigio_timer(rtc_fd);

    t->priv = (void *)(long)rtc_fd;

    return 0;
}

static void rtc_stop_timer(struct qemu_alarm_timer *t)
{
    int rtc_fd = (long)t->priv;

    close(rtc_fd);
}

static int dynticks_start_timer(struct qemu_alarm_timer *t)
{
    struct sigevent ev;
    timer_t host_timer;
    struct sigaction act;

    sigfillset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = host_alarm_handler;

    sigaction(SIGALRM, &act, NULL);

    /* 
     * Initialize ev struct to 0 to avoid valgrind complaining
     * about uninitialized data in timer_create call
     */
    memset(&ev, 0, sizeof(ev));
    ev.sigev_value.sival_int = 0;
    ev.sigev_notify = SIGEV_SIGNAL;
    ev.sigev_signo = SIGALRM;

    if (timer_create(CLOCK_REALTIME, &ev, &host_timer)) {
        perror("timer_create");

        /* disable dynticks */
        fprintf(stderr, "Dynamic Ticks disabled\n");

        return -1;
    }

    t->priv = (void *)(long)host_timer;

    return 0;
}

static void dynticks_stop_timer(struct qemu_alarm_timer *t)
{
    timer_t host_timer = (timer_t)(long)t->priv;

    timer_delete(host_timer);
}

static void dynticks_rearm_timer(struct qemu_alarm_timer *t)
{
    timer_t host_timer = (timer_t)(long)t->priv;
    struct itimerspec timeout;
    int64_t nearest_delta_us = INT64_MAX;
    int64_t current_us;

    if (!active_timers[QEMU_CLOCK_REALTIME] &&
        !active_timers[QEMU_CLOCK_VIRTUAL] &&
        !active_timers[QEMU_CLOCK_HOST])
        return;

    nearest_delta_us = qemu_next_deadline_dyntick();

    /* check whether a timer is already running */
    if (timer_gettime(host_timer, &timeout)) {
        perror("gettime");
        fprintf(stderr, "Internal timer error: aborting\n");
        exit(1);
    }
    current_us = timeout.it_value.tv_sec * 1000000 + timeout.it_value.tv_nsec/1000;
    if (current_us && current_us <= nearest_delta_us)
        return;

    timeout.it_interval.tv_sec = 0;
    timeout.it_interval.tv_nsec = 0; /* 0 for one-shot timer */
    timeout.it_value.tv_sec =  nearest_delta_us / 1000000;
    timeout.it_value.tv_nsec = (nearest_delta_us % 1000000) * 1000;
    if (timer_settime(host_timer, 0 /* RELATIVE */, &timeout, NULL)) {
        perror("settime");
        fprintf(stderr, "Internal timer error: aborting\n");
        exit(1);
    }
}

#endif /* defined(__linux__) */

static int unix_start_timer(struct qemu_alarm_timer *t)
{
    struct sigaction act;
    struct itimerval itv;
    int err;

    /* timer signal */
    sigfillset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = host_alarm_handler;

    sigaction(SIGALRM, &act, NULL);

    itv.it_interval.tv_sec = 0;
    /* for i386 kernel 2.6 to get 1 ms */
    itv.it_interval.tv_usec = 999;
    itv.it_value.tv_sec = 0;
    itv.it_value.tv_usec = 10 * 1000;

    err = setitimer(ITIMER_REAL, &itv, NULL);
    if (err)
        return -1;

    return 0;
}

static void unix_stop_timer(struct qemu_alarm_timer *t)
{
    struct itimerval itv;

    memset(&itv, 0, sizeof(itv));
    setitimer(ITIMER_REAL, &itv, NULL);
}

#endif /* !defined(_WIN32) */


#ifdef _WIN32

static int win32_start_timer(struct qemu_alarm_timer *t)
{
    TIMECAPS tc;
    struct qemu_alarm_win32 *data = t->priv;
    UINT flags;

    memset(&tc, 0, sizeof(tc));
    timeGetDevCaps(&tc, sizeof(tc));

    if (data->period < tc.wPeriodMin)
        data->period = tc.wPeriodMin;

    timeBeginPeriod(data->period);

    flags = TIME_CALLBACK_FUNCTION;
    if (alarm_has_dynticks(t))
        flags |= TIME_ONESHOT;
    else
        flags |= TIME_PERIODIC;

    data->timerId = timeSetEvent(1,         // interval (ms)
                        data->period,       // resolution
                        host_alarm_handler, // function
                        (DWORD)t,           // parameter
                        flags);

    if (!data->timerId) {
        fprintf(stderr, "Failed to initialize win32 alarm timer: %ld\n",
                GetLastError());
        timeEndPeriod(data->period);
        return -1;
    }

    return 0;
}

static void win32_stop_timer(struct qemu_alarm_timer *t)
{
    struct qemu_alarm_win32 *data = t->priv;

    timeKillEvent(data->timerId);
    timeEndPeriod(data->period);
}

static void win32_rearm_timer(struct qemu_alarm_timer *t)
{
    struct qemu_alarm_win32 *data = t->priv;

    if (!active_timers[QEMU_CLOCK_REALTIME] &&
        !active_timers[QEMU_CLOCK_VIRTUAL] &&
        !active_timers[QEMU_CLOCK_HOST])
        return;

    timeKillEvent(data->timerId);

    data->timerId = timeSetEvent(1,
                        data->period,
                        host_alarm_handler,
                        (DWORD)t,
                        TIME_ONESHOT | TIME_PERIODIC);

    if (!data->timerId) {
        fprintf(stderr, "Failed to re-arm win32 alarm timer %ld\n",
                GetLastError());

        timeEndPeriod(data->period);
        exit(1);
    }
}

#endif /* _WIN32 */

static void quit_timers(void)
{
    alarm_timer->stop(alarm_timer);
    alarm_timer = NULL;
}

static int init_timer_alarm(void)
{
    struct qemu_alarm_timer *t = NULL;
    int i, err = -1;

    for (i = 0; alarm_timers[i].name; i++) {
        t = &alarm_timers[i];

        err = t->start(t);
        if (!err)
            break;
    }

    if (err) {
        err = -ENOENT;
        goto fail;
    }

    alarm_timer = t;
    atexit(&quit_timers);

    return 0;

fail:
    return err;
}

/***********************************************************/
/* host time/date access */
void qemu_get_timedate(struct tm *tm, int offset)
{
    time_t ti;
    struct tm *ret;

    time(&ti);
    ti += offset;
    if (rtc_date_offset == -1) {
        if (rtc_utc)
            ret = gmtime(&ti);
        else
            ret = localtime(&ti);
    } else {
        ti -= rtc_date_offset;
        ret = gmtime(&ti);
    }

    memcpy(tm, ret, sizeof(struct tm));
}

int qemu_timedate_diff(struct tm *tm)
{
    time_t seconds;

    if (rtc_date_offset == -1)
        if (rtc_utc)
            seconds = mktimegm(tm);
        else
            seconds = mktime(tm);
    else
        seconds = mktimegm(tm) + rtc_date_offset;

    return seconds - time(NULL);
}

void rtc_change_mon_event(struct tm *tm)
{
    QObject *data;

    data = qobject_from_jsonf("{ 'offset': %d }", qemu_timedate_diff(tm));
    monitor_protocol_event(QEVENT_RTC_CHANGE, data);
    qobject_decref(data);
}

static void configure_rtc_date_offset(const char *startdate, int legacy)
{
    time_t rtc_start_date;
    struct tm tm;

    if (!strcmp(startdate, "now") && legacy) {
        rtc_date_offset = -1;
    } else {
        if (sscanf(startdate, "%d-%d-%dT%d:%d:%d",
                   &tm.tm_year,
                   &tm.tm_mon,
                   &tm.tm_mday,
                   &tm.tm_hour,
                   &tm.tm_min,
                   &tm.tm_sec) == 6) {
            /* OK */
        } else if (sscanf(startdate, "%d-%d-%d",
                          &tm.tm_year,
                          &tm.tm_mon,
                          &tm.tm_mday) == 3) {
            tm.tm_hour = 0;
            tm.tm_min = 0;
            tm.tm_sec = 0;
        } else {
            goto date_fail;
        }
        tm.tm_year -= 1900;
        tm.tm_mon--;
        rtc_start_date = mktimegm(&tm);
        if (rtc_start_date == -1) {
        date_fail:
            fprintf(stderr, "Invalid date format. Valid formats are:\n"
                            "'2006-06-17T16:01:21' or '2006-06-17'\n");
            exit(1);
        }
        rtc_date_offset = time(NULL) - rtc_start_date;
    }
}

static void configure_rtc(QemuOpts *opts)
{
    const char *value;

    value = qemu_opt_get(opts, "base");
    if (value) {
        if (!strcmp(value, "utc")) {
            rtc_utc = 1;
        } else if (!strcmp(value, "localtime")) {
            rtc_utc = 0;
        } else {
            configure_rtc_date_offset(value, 0);
        }
    }
    value = qemu_opt_get(opts, "clock");
    if (value) {
        if (!strcmp(value, "host")) {
            rtc_clock = host_clock;
        } else if (!strcmp(value, "vm")) {
            rtc_clock = vm_clock;
        } else {
            fprintf(stderr, "qemu: invalid option value '%s'\n", value);
            exit(1);
        }
    }
#ifdef TARGET_I386
    value = qemu_opt_get(opts, "driftfix");
    if (value) {
        if (!strcmp(value, "slew")) {
            rtc_td_hack = 1;
        } else if (!strcmp(value, "none")) {
            rtc_td_hack = 0;
        } else {
            fprintf(stderr, "qemu: invalid option value '%s'\n", value);
            exit(1);
        }
    }
#endif
}

#ifdef _WIN32
static void socket_cleanup(void)
{
    WSACleanup();
}

static int socket_init(void)
{
    WSADATA Data;
    int ret, err;

    ret = WSAStartup(MAKEWORD(2,2), &Data);
    if (ret != 0) {
        err = WSAGetLastError();
        fprintf(stderr, "WSAStartup: %d\n", err);
        return -1;
    }
    atexit(socket_cleanup);
    return 0;
}
#endif

#if 0 /* Disabled for Red Hat Enterprise Linux */

/***********************************************************/
/* Bluetooth support */
static int nb_hcis;
static int cur_hci;
static struct HCIInfo *hci_table[MAX_NICS];

static struct bt_vlan_s {
    struct bt_scatternet_s net;
    int id;
    struct bt_vlan_s *next;
} *first_bt_vlan;

/* find or alloc a new bluetooth "VLAN" */
static struct bt_scatternet_s *qemu_find_bt_vlan(int id)
{
    struct bt_vlan_s **pvlan, *vlan;
    for (vlan = first_bt_vlan; vlan != NULL; vlan = vlan->next) {
        if (vlan->id == id)
            return &vlan->net;
    }
    vlan = qemu_mallocz(sizeof(struct bt_vlan_s));
    vlan->id = id;
    pvlan = &first_bt_vlan;
    while (*pvlan != NULL)
        pvlan = &(*pvlan)->next;
    *pvlan = vlan;
    return &vlan->net;
}

static void null_hci_send(struct HCIInfo *hci, const uint8_t *data, int len)
{
}

static int null_hci_addr_set(struct HCIInfo *hci, const uint8_t *bd_addr)
{
    return -ENOTSUP;
}

static struct HCIInfo null_hci = {
    .cmd_send = null_hci_send,
    .sco_send = null_hci_send,
    .acl_send = null_hci_send,
    .bdaddr_set = null_hci_addr_set,
};

struct HCIInfo *qemu_next_hci(void)
{
    if (cur_hci == nb_hcis)
        return &null_hci;

    return hci_table[cur_hci++];
}

static struct HCIInfo *hci_init(const char *str)
{
    char *endp;
    struct bt_scatternet_s *vlan = 0;

    if (!strcmp(str, "null"))
        /* null */
        return &null_hci;
    else if (!strncmp(str, "host", 4) && (str[4] == '\0' || str[4] == ':'))
        /* host[:hciN] */
        return bt_host_hci(str[4] ? str + 5 : "hci0");
    else if (!strncmp(str, "hci", 3)) {
        /* hci[,vlan=n] */
        if (str[3]) {
            if (!strncmp(str + 3, ",vlan=", 6)) {
                vlan = qemu_find_bt_vlan(strtol(str + 9, &endp, 0));
                if (*endp)
                    vlan = 0;
            }
        } else
            vlan = qemu_find_bt_vlan(0);
        if (vlan)
           return bt_new_hci(vlan);
    }

    fprintf(stderr, "qemu: Unknown bluetooth HCI `%s'.\n", str);

    return 0;
}

static int bt_hci_parse(const char *str)
{
    struct HCIInfo *hci;
    bdaddr_t bdaddr;

    if (nb_hcis >= MAX_NICS) {
        fprintf(stderr, "qemu: Too many bluetooth HCIs (max %i).\n", MAX_NICS);
        return -1;
    }

    hci = hci_init(str);
    if (!hci)
        return -1;

    bdaddr.b[0] = 0x52;
    bdaddr.b[1] = 0x54;
    bdaddr.b[2] = 0x00;
    bdaddr.b[3] = 0x12;
    bdaddr.b[4] = 0x34;
    bdaddr.b[5] = 0x56 + nb_hcis;
    hci->bdaddr_set(hci, bdaddr.b);

    hci_table[nb_hcis++] = hci;

    return 0;
}

static void bt_vhci_add(int vlan_id)
{
    struct bt_scatternet_s *vlan = qemu_find_bt_vlan(vlan_id);

    if (!vlan->slave)
        fprintf(stderr, "qemu: warning: adding a VHCI to "
                        "an empty scatternet %i\n", vlan_id);

    bt_vhci_init(bt_new_hci(vlan));
}

static struct bt_device_s *bt_device_add(const char *opt)
{
    struct bt_scatternet_s *vlan;
    int vlan_id = 0;
    char *endp = strstr(opt, ",vlan=");
    int len = (endp ? endp - opt : strlen(opt)) + 1;
    char devname[10];

    pstrcpy(devname, MIN(sizeof(devname), len), opt);

    if (endp) {
        vlan_id = strtol(endp + 6, &endp, 0);
        if (*endp) {
            fprintf(stderr, "qemu: unrecognised bluetooth vlan Id\n");
            return 0;
        }
    }

    vlan = qemu_find_bt_vlan(vlan_id);

    if (!vlan->slave)
        fprintf(stderr, "qemu: warning: adding a slave device to "
                        "an empty scatternet %i\n", vlan_id);

    if (!strcmp(devname, "keyboard"))
        return bt_keyboard_init(vlan);

    fprintf(stderr, "qemu: unsupported bluetooth device `%s'\n", devname);
    return 0;
}

static int bt_parse(const char *opt)
{
    const char *endp, *p;
    int vlan;

    if (strstart(opt, "hci", &endp)) {
        if (!*endp || *endp == ',') {
            if (*endp)
                if (!strstart(endp, ",vlan=", 0))
                    opt = endp + 1;

            return bt_hci_parse(opt);
       }
    } else if (strstart(opt, "vhci", &endp)) {
        if (!*endp || *endp == ',') {
            if (*endp) {
                if (strstart(endp, ",vlan=", &p)) {
                    vlan = strtol(p, (char **) &endp, 0);
                    if (*endp) {
                        fprintf(stderr, "qemu: bad scatternet '%s'\n", p);
                        return 1;
                    }
                } else {
                    fprintf(stderr, "qemu: bad parameter '%s'\n", endp + 1);
                    return 1;
                }
            } else
                vlan = 0;

            bt_vhci_add(vlan);
            return 0;
        }
    } else if (strstart(opt, "device:", &endp))
        return !bt_device_add(endp);

    fprintf(stderr, "qemu: bad bluetooth parameter '%s'\n", opt);
    return 1;
}

#endif

/***********************************************************/
/* QEMU Block devices */

#define HD_OPTS "media=disk"
#define CDROM_OPTS "media=cdrom"
#define FD_OPTS ""
#define PFLASH_OPTS ""
#define MTD_OPTS ""
#define SD_OPTS ""

static int drive_init_func(QemuOpts *opts, void *opaque)
{
    int *use_scsi = opaque;

    return drive_init(opts, *use_scsi) == NULL;
}

static int drive_enable_snapshot(QemuOpts *opts, void *opaque)
{
    if (NULL == qemu_opt_get(opts, "snapshot")) {
        qemu_opt_set(opts, "snapshot", "on");
    }
    return 0;
}

static void default_drive(int enable, int snapshot, int use_scsi,
                          BlockInterfaceType type, int index,
                          const char *optstr)
{
    QemuOpts *opts;

    if (type == IF_DEFAULT) {
        type = use_scsi ? IF_SCSI : IF_IDE;
    }

    if (!enable || drive_get_by_index(type, index)) {
        return;
    }

    opts = drive_add(type, index, NULL, optstr);
    if (snapshot) {
        drive_enable_snapshot(opts, NULL);
    }
    if (!drive_init(opts, use_scsi)) {
        exit(1);
    }
}

void qemu_register_boot_set(QEMUBootSetHandler *func, void *opaque)
{
    boot_set_handler = func;
    boot_set_opaque = opaque;
}

int qemu_boot_set(const char *boot_devices)
{
    if (!boot_set_handler) {
        return -EINVAL;
    }
    return boot_set_handler(boot_set_opaque, boot_devices);
}

static void validate_bootdevices(char *devices)
{
    /* We just do some generic consistency checks */
    const char *p;
    int bitmap = 0;

    for (p = devices; *p != '\0'; p++) {
        /* Allowed boot devices are:
         * a-b: floppy disk drives
         * c-f: IDE disk drives
         * g-m: machine implementation dependant drives
         * n-p: network devices
         * It's up to each machine implementation to check if the given boot
         * devices match the actual hardware implementation and firmware
         * features.
         */
        if (*p < 'a' || *p > 'p') {
            fprintf(stderr, "Invalid boot device '%c'\n", *p);
            exit(1);
        }
        if (bitmap & (1 << (*p - 'a'))) {
            fprintf(stderr, "Boot device '%c' was given twice\n", *p);
            exit(1);
        }
        bitmap |= 1 << (*p - 'a');
    }
}

static void restore_boot_devices(void *opaque)
{
    char *standard_boot_devices = opaque;
    static int first = 1;

    /* Restore boot order and remove ourselves after the first boot */
    if (first) {
        first = 0;
        return;
    }

    qemu_boot_set(standard_boot_devices);

    qemu_unregister_reset(restore_boot_devices, standard_boot_devices);
    qemu_free(standard_boot_devices);
}

void add_boot_device_path(int32_t bootindex, DeviceState *dev,
                          const char *suffix)
{
    FWBootEntry *node, *i;

    if (bootindex < 0) {
        return;
    }

    assert(dev != NULL || suffix != NULL);

    node = qemu_mallocz(sizeof(FWBootEntry));
    node->bootindex = bootindex;
    node->suffix = suffix ? qemu_strdup(suffix) : NULL;
    node->dev = dev;

    QTAILQ_FOREACH(i, &fw_boot_order, link) {
        if (i->bootindex == bootindex) {
            fprintf(stderr, "Two devices with same boot index %d\n", bootindex);
            exit(1);
        } else if (i->bootindex < bootindex) {
            continue;
        }
        QTAILQ_INSERT_BEFORE(i, node, link);
        return;
    }
    QTAILQ_INSERT_TAIL(&fw_boot_order, node, link);
}

/*
 * This function returns null terminated string that consist of new line
 * separated device pathes.
 *
 * memory pointed by "size" is assigned total length of the array in bytes
 *
 */
char *get_boot_devices_list(uint32_t *size)
{
    FWBootEntry *i;
    uint32_t total = 0;
    char *list = NULL;

    QTAILQ_FOREACH(i, &fw_boot_order, link) {
        char *devpath = NULL, *bootpath;
        int len;

        if (i->dev) {
            devpath = qdev_get_fw_dev_path(i->dev);
            assert(devpath);
        }

        if (i->suffix && devpath) {
            bootpath = qemu_malloc(strlen(devpath) + strlen(i->suffix) + 1);
            sprintf(bootpath, "%s%s", devpath, i->suffix);
            qemu_free(devpath);
        } else if (devpath) {
            bootpath = devpath;
        } else {
            assert(i->suffix);
            bootpath = qemu_strdup(i->suffix);
        }

        if (total) {
            list[total-1] = '\n';
        }
        len = strlen(bootpath) + 1;
        list = qemu_realloc(list, total + len);
        memcpy(&list[total], bootpath, len);
        total += len;
        qemu_free(bootpath);
    }

    *size = total;

    if (boot_strict && *size > 0) {
        list[total-1] = '\n';
        list = g_realloc(list, total + 5);
        memcpy(&list[total], "HALT", 5);
        *size = total + 5;
    }
    return list;
}

static void numa_add(const char *optarg)
{
    char option[128];
    char *endptr;
    unsigned long long value, endvalue;
    int nodenr;

    optarg = get_opt_name(option, 128, optarg, ',') + 1;
    if (!strcmp(option, "node")) {
        if (get_param_value(option, 128, "nodeid", optarg) == 0) {
            nodenr = nb_numa_nodes;
        } else {
            nodenr = strtoull(option, NULL, 10);
        }

        if (get_param_value(option, 128, "mem", optarg) == 0) {
            node_mem[nodenr] = 0;
        } else {
            int64_t sval;
            sval = strtosz(option, NULL);
            if (sval < 0) {
                fprintf(stderr, "qemu: invalid numa mem size: %s\n", optarg);
                exit(1);
            }
            node_mem[nodenr] = sval;
        }
        if (get_param_value(option, 128, "cpus", optarg) != 0) {
            value = strtoull(option, &endptr, 10);
            if (*endptr == '-') {
                endvalue = strtoull(endptr+1, &endptr, 10);
            } else {
                endvalue = value;
            }
            if (!(endvalue < MAX_CPUMASK_BITS)) {
                endvalue = MAX_CPUMASK_BITS - 1;
                fprintf(stderr,
                    "A max of %d CPUs are supported in a guest\n",
                     MAX_CPUMASK_BITS);
            }

            bitmap_set(node_cpumask[nodenr], value, endvalue-value+1);
        }
        nb_numa_nodes++;
    }
    return;
}

static void smp_parse(const char *optarg)
{
    int smp, sockets = 0, threads = 0, cores = 0;
    char *endptr;
    char option[128];

    smp = strtoul(optarg, &endptr, 10);
    if (endptr != optarg) {
        if (*endptr == ',') {
            endptr++;
        }
    }
    if (get_param_value(option, 128, "sockets", endptr) != 0)
        sockets = strtoull(option, NULL, 10);
    if (get_param_value(option, 128, "cores", endptr) != 0)
        cores = strtoull(option, NULL, 10);
    if (get_param_value(option, 128, "threads", endptr) != 0)
        threads = strtoull(option, NULL, 10);
    if (get_param_value(option, 128, "maxcpus", endptr) != 0)
        max_cpus = strtoull(option, NULL, 10);

    /* compute missing values, prefer sockets over cores over threads */
    if (smp == 0 || sockets == 0) {
        sockets = sockets > 0 ? sockets : 1;
        cores = cores > 0 ? cores : 1;
        threads = threads > 0 ? threads : 1;
        if (smp == 0) {
            smp = cores * threads * sockets;
        }
    } else {
        if (cores == 0) {
            threads = threads > 0 ? threads : 1;
            cores = smp / (sockets * threads);
        } else {
            threads = smp / (cores * sockets);
        }
    }
    smp_cpus = smp;
    smp_cores = cores > 0 ? cores : 1;
    smp_threads = threads > 0 ? threads : 1;
    if (max_cpus == 0)
        max_cpus = smp_cpus;
}

static void configure_realtime(QemuOpts *opts)
{
    bool enable_mlock;

    enable_mlock = qemu_opt_get_bool(opts, "mlock", true);

    if (enable_mlock) {
        if (os_mlock() < 0) {
            fprintf(stderr, "qemu: locking memory failed\n");
            exit(1);
        }
    }
}


static void configure_msg(QemuOpts *opts)
{
    enable_timestamp_msg = qemu_opt_get_bool(opts, "timestamp", true);
}

/***********************************************************/
/* USB devices */

static int usb_device_add(const char *devname, int is_hotplug)
{
    const char *p;
    USBDevice *dev = NULL;

    if (!usb_enabled)
        return -1;

    /* drivers with .usbdevice_name entry in USBDeviceInfo */
    dev = usbdevice_create(devname);
    if (dev)
        goto done;

    /* the other ones */
    if (strstart(devname, "host:", &p)) {
        dev = usb_host_device_open(p);
#if 0 /* Disabled for Red Hat Enterprise Linux */
    } else if (!strcmp(devname, "bt") || strstart(devname, "bt:", &p)) {
        dev = usb_bt_init(devname[2] ? hci_init(p) :
                        bt_new_hci(qemu_find_bt_vlan(0)));
#endif
    } else {
        return -1;
    }
    if (!dev)
        return -1;

done:
    return 0;
}

static int usb_device_del(const char *devname)
{
    int bus_num, addr;
    const char *p;

    if (strstart(devname, "host:", &p))
        return usb_host_device_close(p);

    if (!usb_enabled)
        return -1;

    p = strchr(devname, '.');
    if (!p)
        return -1;
    bus_num = strtoul(devname, NULL, 0);
    addr = strtoul(p + 1, NULL, 0);

    return usb_device_delete_addr(bus_num, addr);
}

static int usb_parse(const char *cmdline)
{
    int r;
    r = usb_device_add(cmdline, 0);
    if (r < 0) {
        fprintf(stderr, "qemu: could not add USB device '%s'\n", cmdline);
    }
    return r;
}

void do_usb_add(Monitor *mon, const QDict *qdict)
{
    const char *devname = qdict_get_str(qdict, "devname");
    if (usb_device_add(devname, 1) < 0) {
        error_report("could not add USB device '%s'", devname);
    }
}

void do_usb_del(Monitor *mon, const QDict *qdict)
{
    const char *devname = qdict_get_str(qdict, "devname");
    if (usb_device_del(devname) < 0) {
        error_report("could not delete USB device '%s'", devname);
    }
}

/***********************************************************/
/* PCMCIA/Cardbus */

static struct pcmcia_socket_entry_s {
    PCMCIASocket *socket;
    struct pcmcia_socket_entry_s *next;
} *pcmcia_sockets = 0;

void pcmcia_socket_register(PCMCIASocket *socket)
{
    struct pcmcia_socket_entry_s *entry;

    entry = qemu_malloc(sizeof(struct pcmcia_socket_entry_s));
    entry->socket = socket;
    entry->next = pcmcia_sockets;
    pcmcia_sockets = entry;
}

void pcmcia_socket_unregister(PCMCIASocket *socket)
{
    struct pcmcia_socket_entry_s *entry, **ptr;

    ptr = &pcmcia_sockets;
    for (entry = *ptr; entry; ptr = &entry->next, entry = *ptr)
        if (entry->socket == socket) {
            *ptr = entry->next;
            qemu_free(entry);
        }
}

void pcmcia_info(Monitor *mon)
{
    struct pcmcia_socket_entry_s *iter;

    if (!pcmcia_sockets)
        monitor_printf(mon, "No PCMCIA sockets\n");

    for (iter = pcmcia_sockets; iter; iter = iter->next)
        monitor_printf(mon, "%s: %s\n", iter->socket->slot_string,
                       iter->socket->attached ? iter->socket->card_string :
                       "Empty");
}

/***********************************************************/
/* register display */

struct DisplayAllocator default_allocator = {
    defaultallocator_create_displaysurface,
    defaultallocator_resize_displaysurface,
    defaultallocator_free_displaysurface
};

void register_displaystate(DisplayState *ds)
{
    DisplayState **s;
    s = &display_state;
    while (*s != NULL)
        s = &(*s)->next;
    ds->next = NULL;
    *s = ds;
}

DisplayState *get_displaystate(void)
{
    return display_state;
}

DisplayAllocator *register_displayallocator(DisplayState *ds, DisplayAllocator *da)
{
    if(ds->allocator ==  &default_allocator) ds->allocator = da;
    return ds->allocator;
}

/* dumb display */

static void dumb_display_init(void)
{
    DisplayState *ds = qemu_mallocz(sizeof(DisplayState));
    ds->allocator = &default_allocator;
    ds->surface = qemu_create_displaysurface(ds, 640, 480);
    register_displaystate(ds);
}

/***********************************************************/
/* I/O handling */

typedef struct IOHandlerRecord {
    int fd;
    int pollfds_idx;
    IOCanReadHandler *fd_read_poll;
    IOHandler *fd_read;
    IOHandler *fd_write;
    int deleted;
    void *opaque;
    /* temporary data */
    struct pollfd *ufd;
    QLIST_ENTRY(IOHandlerRecord) next;
} IOHandlerRecord;

static QLIST_HEAD(, IOHandlerRecord) io_handlers =
    QLIST_HEAD_INITIALIZER(io_handlers);


/* XXX: fd_read_poll should be suppressed, but an API change is
   necessary in the character devices to suppress fd_can_read(). */
int qemu_set_fd_handler2(int fd,
                         IOCanReadHandler *fd_read_poll,
                         IOHandler *fd_read,
                         IOHandler *fd_write,
                         void *opaque)
{
    IOHandlerRecord *ioh;

    if (!fd_read && !fd_write) {
        QLIST_FOREACH(ioh, &io_handlers, next) {
            if (ioh->fd == fd) {
                ioh->deleted = 1;
                break;
            }
        }
    } else {
        QLIST_FOREACH(ioh, &io_handlers, next) {
            if (ioh->fd == fd)
                goto found;
        }
        ioh = qemu_mallocz(sizeof(IOHandlerRecord));
        QLIST_INSERT_HEAD(&io_handlers, ioh, next);
    found:
        ioh->fd = fd;
        ioh->fd_read_poll = fd_read_poll;
        ioh->fd_read = fd_read;
        ioh->fd_write = fd_write;
        ioh->opaque = opaque;
        ioh->pollfds_idx = -1;
        ioh->deleted = 0;
    }
    qemu_notify_event();
    return 0;
}

int qemu_set_fd_handler(int fd,
                        IOHandler *fd_read,
                        IOHandler *fd_write,
                        void *opaque)
{
    return qemu_set_fd_handler2(fd, NULL, fd_read, fd_write, opaque);
}

static void qemu_iohandler_fill(GArray *pollfds)
{
    IOHandlerRecord *ioh;

    QLIST_FOREACH(ioh, &io_handlers, next) {
        int events = 0;

        if (ioh->deleted)
            continue;
        if (ioh->fd_read &&
            (!ioh->fd_read_poll ||
             ioh->fd_read_poll(ioh->opaque) != 0)) {
            events |= G_IO_IN | G_IO_HUP | G_IO_ERR;
        }
        if (ioh->fd_write) {
            events |= G_IO_OUT | G_IO_ERR;
        }
        if (events) {
            GPollFD pfd = {
                .fd = ioh->fd,
                .events = events,
            };
            ioh->pollfds_idx = pollfds->len;
            g_array_append_val(pollfds, pfd);
        } else {
            ioh->pollfds_idx = -1;
        }
    }
}

static void qemu_iohandler_poll(GArray *pollfds, int ret)
{
    if (ret > 0) {
        IOHandlerRecord *pioh, *ioh;

        QLIST_FOREACH_SAFE(ioh, &io_handlers, next, pioh) {
            int revents = 0;

            if (!ioh->deleted && ioh->pollfds_idx != -1) {
                GPollFD *pfd = &g_array_index(pollfds, GPollFD,
                                              ioh->pollfds_idx);
                revents = pfd->revents;
            }

            if (!ioh->deleted && ioh->fd_read &&
                (revents & (G_IO_IN | G_IO_HUP | G_IO_ERR))) {
                ioh->fd_read(ioh->opaque);
            }
            if (!ioh->deleted && ioh->fd_write &&
                (revents & (G_IO_OUT | G_IO_ERR))) {
                ioh->fd_write(ioh->opaque);
            }

            /* Do this last in case read/write handlers marked it for deletion */
            if (ioh->deleted) {
                QLIST_REMOVE(ioh, next);
                g_free(ioh);
            }
        }
    }
}


/***********************************************************/
/* ram save/restore */

#define RAM_SAVE_FLAG_FULL	0x01 /* Obsolete, not used anymore */
#define RAM_SAVE_FLAG_COMPRESS	0x02
#define RAM_SAVE_FLAG_MEM_SIZE	0x04
#define RAM_SAVE_FLAG_PAGE	0x08
#define RAM_SAVE_FLAG_EOS	0x10
#define RAM_SAVE_FLAG_CONTINUE	0x20

static int is_dup_page(uint8_t *page, uint8_t ch)
{
    uint32_t val = ch << 24 | ch << 16 | ch << 8 | ch;
    uint32_t *array = (uint32_t *)page;
    int i;

    for (i = 0; i < (TARGET_PAGE_SIZE / 4); i++) {
        if (array[i] != val)
            return 0;
    }

    return 1;
}

static RAMBlock *last_block;
static ram_addr_t last_offset;

static int ram_save_block(QEMUFile *f)
{
    RAMBlock *block = last_block;
    ram_addr_t offset = last_offset;
    ram_addr_t current_addr;
    int bytes_sent = 0;

    if (!block)
        block = QLIST_FIRST(&ram_list.blocks);

    current_addr = block->offset + offset;

    do {
        if (kvm_enabled() && current_addr == 0) {
            int r;
            r = kvm_update_dirty_pages_log();
            if (r) {
                fprintf(stderr, "%s: update dirty pages log failed %d\n", __FUNCTION__, r);
                qemu_file_set_error(f, -EINVAL);
                return 0;
            }
        }
        if (cpu_physical_memory_get_dirty(current_addr, MIGRATION_DIRTY_FLAG)) {
            uint8_t *p;
            int cont = (block == last_block) ? RAM_SAVE_FLAG_CONTINUE : 0;

            cpu_physical_memory_reset_dirty(current_addr,
                                            current_addr + TARGET_PAGE_SIZE,
                                            MIGRATION_DIRTY_FLAG);

            p = block->host + offset;

            if (is_dup_page(p, *p)) {
                qemu_put_be64(f, offset | cont | RAM_SAVE_FLAG_COMPRESS);
                if (!cont) {
                    qemu_put_byte(f, strlen(block->idstr));
                    qemu_put_buffer(f, (uint8_t *)block->idstr,
                                    strlen(block->idstr));
                }
                qemu_put_byte(f, *p);
                bytes_sent = 1;
            } else {
                qemu_put_be64(f, offset | cont | RAM_SAVE_FLAG_PAGE);
                if (!cont) {
                    qemu_put_byte(f, strlen(block->idstr));
                    qemu_put_buffer(f, (uint8_t *)block->idstr,
                                    strlen(block->idstr));
                }
                qemu_put_buffer(f, p, TARGET_PAGE_SIZE);
                bytes_sent = TARGET_PAGE_SIZE;
            }

            break;
        }

        offset += TARGET_PAGE_SIZE;
        if (offset >= block->length) {
            offset = 0;
            block = QLIST_NEXT(block, next);
            if (!block)
                block = QLIST_FIRST(&ram_list.blocks);
        }

        current_addr = block->offset + offset;

    } while (current_addr != last_block->offset + last_offset);

    last_block = block;
    last_offset = offset;

    return bytes_sent;
}

static uint64_t bytes_transferred;

uint64_t ram_bytes_remaining(void)
{
    return ram_list.dirty_pages * TARGET_PAGE_SIZE;
}

uint64_t ram_bytes_transferred(void)
{
    return bytes_transferred;
}

uint64_t ram_bytes_total(void)
{
    RAMBlock *block;
    uint64_t total = 0;

    QLIST_FOREACH(block, &ram_list.blocks, next)
        total += block->length;

    return total;
}

static int ram_save_live(Monitor *mon, QEMUFile *f, int stage, void *opaque)
{
    ram_addr_t addr;
    uint64_t bytes_transferred_last;
    uint64_t t0;
    int i;
    int ret;

    if (stage < 0) {
        cpu_physical_memory_set_dirty_tracking(0);
        return 0;
    }

    if (cpu_physical_sync_dirty_bitmap(0, TARGET_PHYS_ADDR_MAX) != 0) {
        qemu_file_set_error(f, -EINVAL);
        return -EINVAL;
    }

    if (stage == 1) {
        RAMBlock *block;
        bytes_transferred = 0;
        last_block = NULL;
        last_offset = 0;

        /* Make sure all dirty bits are set */
        QLIST_FOREACH(block, &ram_list.blocks, next) {
            for (addr = block->offset; addr < block->offset + block->length;
                 addr += TARGET_PAGE_SIZE) {
                if (!cpu_physical_memory_get_dirty(addr,
                                                   MIGRATION_DIRTY_FLAG))
                    cpu_physical_memory_set_dirty(addr);
            }
        }

        /* Enable dirty memory tracking */
        cpu_physical_memory_set_dirty_tracking(1);

        qemu_put_be64(f, ram_bytes_total() | RAM_SAVE_FLAG_MEM_SIZE);

        QLIST_FOREACH(block, &ram_list.blocks, next) {
            qemu_put_byte(f, strlen(block->idstr));
            qemu_put_buffer(f, (uint8_t *)block->idstr, strlen(block->idstr));
            qemu_put_be64(f, block->length);
        }
    }

    bytes_transferred_last = bytes_transferred;
    t0 = get_clock();

    i = 0;
    while ((ret = qemu_file_rate_limit(f)) == 0) {
        int bytes_sent;

        bytes_sent = ram_save_block(f);
        bytes_transferred += bytes_sent;
        if (bytes_sent == 0) /* no more blocks */
            break;
       /* we want to check in the 1st loop, just in case it was the 1st time
           and we had to sync the dirty bitmap.
           get_clock() is a bit expensive, so we only check each some
           iterations
       */
        if ((i & 63) == 0) {
            uint64_t t1 = (get_clock() - t0) / 1000000;
            if (t1 > 50) { /* 50 ms */
               break;
            }
        }
        i++;
    }

    if (ret < 0) {
        return ret;
    }

    t0 = get_clock() - t0;

    /* try transferring iterative blocks of memory */
    if (stage == 3) {
        int bytes_sent;

        /* flush all remaining blocks regardless of rate limiting */
        while ((bytes_sent = ram_save_block(f)) != 0) {
            bytes_transferred += bytes_sent;
        }
        cpu_physical_memory_set_dirty_tracking(0);
    }

    qemu_put_be64(f, RAM_SAVE_FLAG_EOS);

    if (stage == 2) {
        uint64_t expected_time;
        double bwidth = 0;

        /*
         * If it is smaller than 10ms, we don't trust the measurement.
         *
         * Sometimes as the amount of time is very small and that
         * makes the bandwidth really, really high.  But if
         * networking is very flaky, we also want to finish migration
         * when the amount of memory pending is small enough. (10MB
         * on this case).
         */

        if ((ram_bytes_remaining() > 10 * 1024 * 1024) && (t0 < 10000000)) {
            return 0;
        }

        bwidth = ((double)bytes_transferred - bytes_transferred_last) / t0;

        /*
         * if we haven't transferred anything this round, force expected_time to a
         * a very high value, but without crashing
         */
        if (bwidth == 0)
            bwidth = 0.000001;

        expected_time = ram_bytes_remaining() / bwidth;
        return expected_time <= migrate_max_downtime();
    }
    return 0;
}

static inline void *host_from_stream_offset(QEMUFile *f,
                                            ram_addr_t offset,
                                            int flags)
{
    static RAMBlock *block = NULL;
    char id[256];
    uint8_t len;

    if (flags & RAM_SAVE_FLAG_CONTINUE) {
        if (!block || block->length <= offset) {
            fprintf(stderr, "Ack, bad migration stream!\n");
            return NULL;
        }

        return block->host + offset;
    }

    len = qemu_get_byte(f);
    qemu_get_buffer(f, (uint8_t *)id, len);
    id[len] = 0;

    QLIST_FOREACH(block, &ram_list.blocks, next) {
        if (!strncmp(id, block->idstr, sizeof(id)) && block->length > offset) {
            return block->host + offset;
        }
    }

    fprintf(stderr, "Can't find block %s!\n", id);
    return NULL;
}

static int ram_load(QEMUFile *f, void *opaque, int version_id)
{
    ram_addr_t addr;
    int flags;
    int error;

    if (version_id < 3 || version_id > 4)
        return -EINVAL;

    do {
        addr = qemu_get_be64(f);

        flags = addr & ~TARGET_PAGE_MASK;
        addr &= TARGET_PAGE_MASK;

        if (flags & RAM_SAVE_FLAG_MEM_SIZE) {
            if (version_id == 3) {
                if (addr != ram_bytes_total()) {
                    return -EINVAL;
                }
            } else {
                /* Synchronize RAM block list */
                char id[256];
                ram_addr_t length;
                ram_addr_t total_ram_bytes = addr;

                while (total_ram_bytes) {
                    RAMBlock *block;
                    uint8_t len;

                    len = qemu_get_byte(f);
                    qemu_get_buffer(f, (uint8_t *)id, len);
                    id[len] = 0;
                    length = qemu_get_be64(f);

                    QLIST_FOREACH(block, &ram_list.blocks, next) {
                        if (!strncmp(id, block->idstr, sizeof(id))) {
                            if (block->length != length)
                                return -EINVAL;
                            break;
                        }
                    }

                    if (!block) {
                        fprintf(stderr, "Unknown ramblock \"%s\", cannot "
                                "accept migration\n", id);
                        return -EINVAL;
                    }

                    total_ram_bytes -= length;
                }
            }
        }

        if (flags & RAM_SAVE_FLAG_COMPRESS) {
            void *host;
            uint8_t ch;

            if (version_id == 3)
                host = qemu_get_ram_ptr(addr);
            else
                host = host_from_stream_offset(f, addr, flags);

            ch = qemu_get_byte(f);
            memset(host, ch, TARGET_PAGE_SIZE);
#ifndef _WIN32
            if (ch == 0 &&
                (!kvm_enabled() || kvm_has_sync_mmu())) {
                madvise(host, TARGET_PAGE_SIZE, MADV_DONTNEED);
            }
#endif
        } else if (flags & RAM_SAVE_FLAG_PAGE) {
            void *host;

            if (version_id == 3)
                host = qemu_get_ram_ptr(addr);
            else
                host = host_from_stream_offset(f, addr, flags);

            qemu_get_buffer(f, host, TARGET_PAGE_SIZE);
        }
        error = qemu_file_get_error(f);
        if (error) {
            return error;
        }
    } while (!(flags & RAM_SAVE_FLAG_EOS));

    return 0;
}

void qemu_service_io(void)
{
    qemu_notify_event();
}

/***********************************************************/
/* machine registration */

static QEMUMachine *first_machine = NULL;
QEMUMachine *current_machine = NULL;

int qemu_register_machine(QEMUMachine *m)
{
    QEMUMachine **pm;
    pm = &first_machine;
    while (*pm != NULL)
        pm = &(*pm)->next;
    m->next = NULL;
    *pm = m;
    return 0;
}

static QEMUMachine *find_machine(const char *name)
{
    QEMUMachine *m;

    for(m = first_machine; m != NULL; m = m->next) {
        if (!strcmp(m->name, name))
            return m;
        if (m->alias && !strcmp(m->alias, name))
            return m;
    }
    return NULL;
}

static QEMUMachine *find_default_machine(void)
{
    QEMUMachine *m;

    for(m = first_machine; m != NULL; m = m->next) {
        if (m->is_default) {
            return m;
        }
    }
    return NULL;
}

/***********************************************************/
/* main execution loop */

static void gui_update(void *opaque)
{
    uint64_t interval = GUI_REFRESH_INTERVAL;
    DisplayState *ds = opaque;
    DisplayChangeListener *dcl = ds->listeners;

    dpy_refresh(ds);

    while (dcl != NULL) {
        if (dcl->gui_timer_interval &&
            dcl->gui_timer_interval < interval)
            interval = dcl->gui_timer_interval;
        dcl = dcl->next;
    }
    qemu_mod_timer(ds->gui_timer, interval + qemu_get_clock(rt_clock));
}

struct vm_change_state_entry {
    VMChangeStateHandler *cb;
    void *opaque;
    QLIST_ENTRY (vm_change_state_entry) entries;
};

static QLIST_HEAD(vm_change_state_head, vm_change_state_entry) vm_change_state_head;

VMChangeStateEntry *qemu_add_vm_change_state_handler(VMChangeStateHandler *cb,
                                                     void *opaque)
{
    VMChangeStateEntry *e;

    e = qemu_mallocz(sizeof (*e));

    e->cb = cb;
    e->opaque = opaque;
    QLIST_INSERT_HEAD(&vm_change_state_head, e, entries);
    return e;
}

void qemu_del_vm_change_state_handler(VMChangeStateEntry *e)
{
    QLIST_REMOVE (e, entries);
    qemu_free (e);
}

static void vm_state_notify(int running, RunState state)
{
    VMChangeStateEntry *e;

    trace_vm_state_notify(running, state);

    for (e = vm_change_state_head.lh_first; e; e = e->entries.le_next) {
        e->cb(e->opaque, running, state);
    }
}

void vm_start(void)
{
    if (!runstate_is_running()) {
        cpu_enable_ticks();
        runstate_set(RUN_STATE_RUNNING);
        vm_state_notify(1, RUN_STATE_RUNNING);
        qemu_rearm_alarm_timer(alarm_timer);
        resume_all_vcpus();
        monitor_protocol_event(QEVENT_RESUME, NULL);
    }
}

/* reset/shutdown handler */

typedef struct QEMUResetEntry {
    QTAILQ_ENTRY(QEMUResetEntry) entry;
    QEMUResetHandler *func;
    void *opaque;
} QEMUResetEntry;

static QTAILQ_HEAD(reset_handlers, QEMUResetEntry) reset_handlers =
    QTAILQ_HEAD_INITIALIZER(reset_handlers);
static int reset_requested;
static int shutdown_requested, shutdown_signal = -1;
static pid_t shutdown_pid;
static int powerdown_requested;
static int debug_requested;
static int suspend_requested;
static int wakeup_requested;
static bool is_suspended;
static NotifierList suspend_notifiers =
    NOTIFIER_LIST_INITIALIZER(suspend_notifiers);
static NotifierList wakeup_notifiers =
    NOTIFIER_LIST_INITIALIZER(wakeup_notifiers);
static uint32_t wakeup_reason_mask = ~0;
static int vmstop_requested = RUN_STATE_NO_STATE;

int qemu_no_shutdown(void)
{
    int r = no_shutdown;
    return r;
}

int qemu_shutdown_requested(void)
{
    int r = shutdown_requested;
    shutdown_requested = 0;
    return r;
}

void qemu_kill_report(void)
{
    if (shutdown_signal != -1) {
        fprintf(stderr, "qemu: terminating on signal %d", shutdown_signal);
        if (shutdown_pid == 0) {
            /* This happens for eg ^C at the terminal, so it's worth
             * avoiding printing an odd message in that case.
             */
            fputc('\n', stderr);
        } else {
            fprintf(stderr, " from pid %d\n", shutdown_pid);
        }
        shutdown_signal = -1;
    }
}

int qemu_reset_requested(void)
{
    int r = reset_requested;
    reset_requested = 0;
    return r;
}

int qemu_suspend_requested(void)
{
    int r = suspend_requested;
    suspend_requested = 0;
    return r;
}

int qemu_wakeup_requested(void)
{
    int r = wakeup_requested;
    wakeup_requested = 0;
    return r;
}

int qemu_powerdown_requested(void)
{
    int r = powerdown_requested;
    powerdown_requested = 0;
    return r;
}

static int qemu_debug_requested(void)
{
    int r = debug_requested;
    debug_requested = 0;
    return r;
}

static RunState qemu_vmstop_requested(void)
{
    int r = vmstop_requested;
    vmstop_requested = RUN_STATE_NO_STATE;
    return r;
}

static void do_vm_stop(RunState state)
{
    if (runstate_is_running()) {
        cpu_disable_ticks();
        pause_all_vcpus();
        runstate_set(state);
        vm_state_notify(0, state);
        monitor_protocol_event(QEVENT_STOP, NULL);
    }
}

void qemu_register_reset(QEMUResetHandler *func, void *opaque)
{
    QEMUResetEntry *re = qemu_mallocz(sizeof(QEMUResetEntry));

    re->func = func;
    re->opaque = opaque;
    QTAILQ_INSERT_TAIL(&reset_handlers, re, entry);
}

void qemu_unregister_reset(QEMUResetHandler *func, void *opaque)
{
    QEMUResetEntry *re;

    QTAILQ_FOREACH(re, &reset_handlers, entry) {
        if (re->func == func && re->opaque == opaque) {
            QTAILQ_REMOVE(&reset_handlers, re, entry);
            qemu_free(re);
            return;
        }
    }
}

void qemu_system_reset(bool report)
{
    QEMUResetEntry *re, *nre;

    /* reset all devices */
    QTAILQ_FOREACH_SAFE(re, &reset_handlers, entry, nre) {
        re->func(re->opaque);
    }
    if (report) {
        monitor_protocol_event(QEVENT_RESET, NULL);
    }
}

void qemu_system_reset_request(void)
{
    if (no_reboot) {
        shutdown_requested = 1;
    } else {
        reset_requested = 1;
    }
    if (cpu_single_env) {
        cpu_single_env->stopped = 1;
        cpu_exit(cpu_single_env);
    }
    qemu_notify_event();
}

void qemu_system_suspend(void)
{
    pause_all_vcpus();
    notifier_list_notify(&suspend_notifiers, NULL);
    monitor_protocol_event(QEVENT_SUSPEND, NULL);
    is_suspended = true;
}

void qemu_system_suspend_request(void)
{
    if (is_suspended) {
        return;
    }
    suspend_requested = 1;
    if (cpu_single_env) {
        cpu_single_env->stopped = 1;
        cpu_exit(cpu_single_env);
    }
    qemu_notify_event();
}

void qemu_register_suspend_notifier(Notifier *notifier)
{
    notifier_list_add(&suspend_notifiers, notifier);
}

void qemu_unregister_suspend_notifier(Notifier *notifier)
{
    notifier_list_remove(&suspend_notifiers, notifier);
}

void qemu_system_wakeup_request(WakeupReason reason)
{
    if (!is_suspended) {
        return;
    }
    if (!(wakeup_reason_mask & (1 << reason))) {
        return;
    }
    notifier_list_notify(&wakeup_notifiers, &reason);
    wakeup_requested = 1;
    qemu_notify_event();
    is_suspended = false;
}

void qemu_system_wakeup_enable(WakeupReason reason, bool enabled)
{
    if (enabled) {
        wakeup_reason_mask |= (1 << reason);
    } else {
        wakeup_reason_mask &= ~(1 << reason);
    }
}

void qemu_register_wakeup_notifier(Notifier *notifier)
{
    notifier_list_add(&wakeup_notifiers, notifier);
}

void qemu_system_killed(int signal, pid_t pid)
{
    shutdown_signal = signal;
    shutdown_pid = pid;
    qemu_system_shutdown_request();
}

void qemu_system_shutdown_request(void)
{
    trace_qemu_system_shutdown_request();
    shutdown_requested = 1;
    qemu_notify_event();
}

void qemu_system_powerdown_request(void)
{
    trace_qemu_system_powerdown_request();
    powerdown_requested = 1;
    qemu_notify_event();
}

#ifdef CONFIG_IOTHREAD
static void qemu_system_vmstop_request(int reason)
{
    vmstop_requested = reason;
    qemu_notify_event();
}
#endif

#ifndef _WIN32
static int io_thread_fd = -1;

static void qemu_event_increment(void)
{
    static const char byte = 0;
    ssize_t ret;

    if (io_thread_fd == -1)
        return;

    ret = write(io_thread_fd, &byte, sizeof(byte));
    if (ret < 0 && (errno != EINTR && errno != EAGAIN)) {
        fprintf(stderr, "qemu_event_increment: write() filed: %s\n",
                strerror(errno));
        exit (1);
    }
}

static void qemu_event_read(void *opaque)
{
    int fd = (unsigned long)opaque;
    ssize_t len;

    /* Drain the notify pipe */
    do {
        char buffer[512];
        len = read(fd, buffer, sizeof(buffer));
    } while ((len == -1 && errno == EINTR) || len > 0);
}

static int qemu_event_init(void)
{
    int err;
    int fds[2];

    err = qemu_pipe(fds);
    if (err == -1)
        return -errno;

    err = fcntl_setfl(fds[0], O_NONBLOCK);
    if (err < 0)
        goto fail;

    err = fcntl_setfl(fds[1], O_NONBLOCK);
    if (err < 0)
        goto fail;

    qemu_set_fd_handler2(fds[0], NULL, qemu_event_read, NULL,
                         (void *)(unsigned long)fds[0]);

    io_thread_fd = fds[1];
    return 0;

fail:
    close(fds[0]);
    close(fds[1]);
    return err;
}
#else
HANDLE qemu_event_handle;

static void dummy_event_handler(void *opaque)
{
}

static int qemu_event_init(void)
{
    qemu_event_handle = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!qemu_event_handle) {
        fprintf(stderr, "Failed CreateEvent: %ld\n", GetLastError());
        return -1;
    }
    qemu_add_wait_object(qemu_event_handle, dummy_event_handler, NULL);
    return 0;
}

static void qemu_event_increment(void)
{
    if (!SetEvent(qemu_event_handle)) {
        fprintf(stderr, "qemu_event_increment: SetEvent failed: %ld\n",
                GetLastError());
        exit (1);
    }
}
#endif

static int cpu_can_run(CPUState *env)
{
    if (env->stop)
        return 0;
    if (env->stopped)
        return 0;
    return 1;
}

static GArray *gpollfds;

#ifndef CONFIG_IOTHREAD
static int qemu_init_main_loop(void)
{
    gpollfds = g_array_new(FALSE, FALSE, sizeof(GPollFD));
    return qemu_event_init();
}

void qemu_init_vcpu(void *_env)
{
    CPUState *env = _env;

    env->nr_cores = smp_cores;
    env->nr_threads = smp_threads;
    if (kvm_enabled())
        kvm_init_vcpu(env);
    return;
}

int qemu_cpu_self(void *env)
{
    return 1;
}

void resume_all_vcpus(void)
{
    if (kvm_enabled()) {
        kvm_resume_all_threads();
    }
}

void pause_all_vcpus(void)
{
    if (kvm_enabled()) {
        kvm_pause_all_threads();
    }
}

void qemu_cpu_kick(void *env)
{
    return;
}

void qemu_notify_event(void)
{
    CPUState *env = cpu_single_env;

    if (kvm_enabled()) {
        qemu_kvm_notify_work();
        return;
    }
    if (env) {
        cpu_exit(env);
    }
}

void vm_stop(RunState state)
{
    do_vm_stop(state);
}

/* does a state transition even if the VM is already stopped,
   current state is forgotten forever */
void vm_stop_force_state(RunState state)
{
    if (runstate_is_running()) {
         vm_stop(state);
    } else {
        runstate_set(state);
    }
}

#else /* CONFIG_IOTHREAD */

#include "qemu-thread.h"

QemuMutex qemu_global_mutex;
static QemuMutex qemu_fair_mutex;

static QemuThread io_thread;

static QemuThread *tcg_cpu_thread;
static QemuCond *tcg_halt_cond;

static int qemu_system_ready;
/* cpu creation */
static QemuCond qemu_cpu_cond;
/* system init */
static QemuCond qemu_system_cond;
static QemuCond qemu_pause_cond;

static void block_io_signals(void);
static void unblock_io_signals(void);
static int tcg_has_work(void);

static int qemu_init_main_loop(void)
{
    int ret;

    ret = qemu_event_init();
    if (ret)
        return ret;

    gpollfds = g_array_new(FALSE, FALSE, sizeof(GPollFD));
    qemu_cond_init(&qemu_pause_cond);
    qemu_mutex_init(&qemu_fair_mutex);
    qemu_mutex_init(&qemu_global_mutex);
    qemu_mutex_lock(&qemu_global_mutex);

    unblock_io_signals();
    qemu_thread_self(&io_thread);

    return 0;
}

static void qemu_wait_io_event(CPUState *env)
{
    while (!tcg_has_work())
        qemu_cond_timedwait(env->halt_cond, &qemu_global_mutex, 1000);

    qemu_mutex_unlock(&qemu_global_mutex);

    /*
     * Users of qemu_global_mutex can be starved, having no chance
     * to acquire it since this path will get to it first.
     * So use another lock to provide fairness.
     */
    qemu_mutex_lock(&qemu_fair_mutex);
    qemu_mutex_unlock(&qemu_fair_mutex);

    qemu_mutex_lock(&qemu_global_mutex);
    if (env->stop) {
        env->stop = 0;
        env->stopped = 1;
        qemu_cond_signal(&qemu_pause_cond);
    }
}

static int qemu_cpu_exec(CPUState *env);

static void *kvm_cpu_thread_fn(void *arg)
{
    CPUState *env = arg;

    block_io_signals();
    qemu_thread_self(env->thread);
    if (kvm_enabled())
        kvm_init_vcpu(env);

    /* signal CPU creation */
    qemu_mutex_lock(&qemu_global_mutex);
    env->created = 1;
    qemu_cond_signal(&qemu_cpu_cond);

    /* and wait for machine initialization */
    while (!qemu_system_ready)
        qemu_cond_timedwait(&qemu_system_cond, &qemu_global_mutex, 100);

    while (1) {
        if (cpu_can_run(env))
            qemu_cpu_exec(env);
        qemu_wait_io_event(env);
    }

    return NULL;
}

static void tcg_cpu_exec(void);

static void *tcg_cpu_thread_fn(void *arg)
{
    CPUState *env = arg;

    block_io_signals();
    qemu_thread_self(env->thread);

    /* signal CPU creation */
    qemu_mutex_lock(&qemu_global_mutex);
    for (env = first_cpu; env != NULL; env = env->next_cpu)
        env->created = 1;
    qemu_cond_signal(&qemu_cpu_cond);

    /* and wait for machine initialization */
    while (!qemu_system_ready)
        qemu_cond_timedwait(&qemu_system_cond, &qemu_global_mutex, 100);

    while (1) {
        tcg_cpu_exec();
        qemu_wait_io_event(cur_cpu);
    }

    return NULL;
}

void qemu_cpu_kick(void *_env)
{
    CPUState *env = _env;
    qemu_cond_broadcast(env->halt_cond);
    if (kvm_enabled())
        qemu_thread_signal(env->thread, SIGUSR1);
}

int qemu_cpu_self(void *_env)
{
    CPUState *env = _env;
    QemuThread this;
 
    qemu_thread_self(&this);
 
    return qemu_thread_equal(&this, env->thread);
}

static void cpu_signal(int sig)
{
    if (cpu_single_env)
        cpu_exit(cpu_single_env);
}

static void block_io_signals(void)
{
    sigset_t set;
    struct sigaction sigact;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    sigaddset(&set, SIGIO);
    sigaddset(&set, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);

    memset(&sigact, 0, sizeof(sigact));
    sigact.sa_handler = cpu_signal;
    sigaction(SIGUSR1, &sigact, NULL);
}

static void unblock_io_signals(void)
{
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    sigaddset(&set, SIGIO);
    sigaddset(&set, SIGALRM);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

static void qemu_signal_lock(unsigned int msecs)
{
    qemu_mutex_lock(&qemu_fair_mutex);

    while (qemu_mutex_trylock(&qemu_global_mutex)) {
        qemu_thread_signal(tcg_cpu_thread, SIGUSR1);
        if (!qemu_mutex_timedlock(&qemu_global_mutex, msecs))
            break;
    }
    qemu_mutex_unlock(&qemu_fair_mutex);
}

void qemu_mutex_lock_iothread(void)
{
    if (kvm_enabled()) {
        qemu_mutex_lock(&qemu_fair_mutex);
        qemu_mutex_lock(&qemu_global_mutex);
        qemu_mutex_unlock(&qemu_fair_mutex);
    } else
        qemu_signal_lock(100);
}

void qemu_mutex_unlock_iothread(void)
{
    qemu_mutex_unlock(&qemu_global_mutex);
}

static int all_vcpus_paused(void)
{
    CPUState *penv = first_cpu;

    while (penv) {
        if (!penv->stopped)
            return 0;
        penv = (CPUState *)penv->next_cpu;
    }

    return 1;
}

void pause_all_vcpus(void)
{
    CPUState *penv = first_cpu;

    while (penv) {
        penv->stop = 1;
        qemu_thread_signal(penv->thread, SIGUSR1);
        qemu_cpu_kick(penv);
        penv = (CPUState *)penv->next_cpu;
    }

    while (!all_vcpus_paused()) {
        qemu_cond_timedwait(&qemu_pause_cond, &qemu_global_mutex, 100);
        penv = first_cpu;
        while (penv) {
            qemu_thread_signal(penv->thread, SIGUSR1);
            penv = (CPUState *)penv->next_cpu;
        }
    }
}

void resume_all_vcpus(void)
{
    CPUState *penv = first_cpu;

    while (penv) {
        penv->stop = 0;
        penv->stopped = 0;
        qemu_thread_signal(penv->thread, SIGUSR1);
        qemu_cpu_kick(penv);
        penv = (CPUState *)penv->next_cpu;
    }
}

static void tcg_init_vcpu(void *_env)
{
    CPUState *env = _env;
    /* share a single thread for all cpus with TCG */
    if (!tcg_cpu_thread) {
        env->thread = qemu_mallocz(sizeof(QemuThread));
        env->halt_cond = qemu_mallocz(sizeof(QemuCond));
        qemu_cond_init(env->halt_cond);
        qemu_thread_create(env->thread, tcg_cpu_thread_fn, env);
        while (env->created == 0)
            qemu_cond_timedwait(&qemu_cpu_cond, &qemu_global_mutex, 100);
        tcg_cpu_thread = env->thread;
        tcg_halt_cond = env->halt_cond;
    } else {
        env->thread = tcg_cpu_thread;
        env->halt_cond = tcg_halt_cond;
    }
}

static void kvm_start_vcpu(CPUState *env)
{
    env->thread = qemu_mallocz(sizeof(QemuThread));
    env->halt_cond = qemu_mallocz(sizeof(QemuCond));
    qemu_cond_init(env->halt_cond);
    qemu_thread_create(env->thread, kvm_cpu_thread_fn, env);
    while (env->created == 0)
        qemu_cond_timedwait(&qemu_cpu_cond, &qemu_global_mutex, 100);
}

void qemu_init_vcpu(void *_env)
{
    CPUState *env = _env;

    env->nr_cores = smp_cores;
    env->nr_threads = smp_threads;
    if (kvm_enabled())
        kvm_start_vcpu(env);
    else
        tcg_init_vcpu(env);
}

void qemu_notify_event(void)
{
    qemu_event_increment();
}

void vm_stop(RunState reason)
{
    QemuThread me;
    qemu_thread_self(&me);

    if (!qemu_thread_equal(&me, &io_thread)) {
        qemu_system_vmstop_request(reason);
        /*
         * FIXME: should not return to device code in case
         * vm_stop() has been requested.
         */
        if (cpu_single_env) {
            cpu_exit(cpu_single_env);
            cpu_single_env->stop = 1;
        }
        return;
    }
    do_vm_stop(reason);
}

#endif


static int glib_pollfds_idx;
static int glib_n_poll_fds;
static int max_priority;

static void glib_pollfds_fill(int *cur_timeout)
{
    GMainContext *context = g_main_context_default();
    int timeout = 0;
    int n;

    g_main_context_prepare(context, &max_priority);

    glib_pollfds_idx = gpollfds->len;
    n = glib_n_poll_fds;
    do {
        GPollFD *pfds;
        glib_n_poll_fds = n;
        g_array_set_size(gpollfds, glib_pollfds_idx + glib_n_poll_fds);
        pfds = &g_array_index(gpollfds, GPollFD, glib_pollfds_idx);
        n = g_main_context_query(context, max_priority, &timeout, pfds,
                                 glib_n_poll_fds);
    } while (n != glib_n_poll_fds);

    if (timeout >= 0 && timeout < *cur_timeout) {
        *cur_timeout = timeout;
    }
}

static void glib_pollfds_poll(void)
{
    GMainContext *context = g_main_context_default();
    GPollFD *pfds = &g_array_index(gpollfds, GPollFD, glib_pollfds_idx);

    if (g_main_context_check(context, max_priority, pfds, glib_n_poll_fds)) {
        g_main_context_dispatch(context);
    }
}

void main_loop_wait(int timeout)
{
    int ret;

    qemu_bh_update_timeout((uint32_t *)&timeout);

    /* poll any events */
    g_array_set_size(gpollfds, 0); /* reset for new iteration */
    /* XXX: separate device handlers from system ones */

    qemu_iohandler_fill(gpollfds);
    slirp_pollfds_fill(gpollfds);
    glib_pollfds_fill(&timeout);

    os_host_main_loop_wait(&timeout);

    qemu_mutex_unlock_iothread();
    ret = g_poll((GPollFD *)gpollfds->data, gpollfds->len, timeout);
    qemu_mutex_lock_iothread();

    qemu_iohandler_poll(gpollfds, ret);
    slirp_pollfds_poll(gpollfds, (ret < 0));
    glib_pollfds_poll();

    /* rearm timer, if not periodic */
    if (alarm_timer->flags & ALARM_FLAG_EXPIRED) {
        alarm_timer->flags &= ~ALARM_FLAG_EXPIRED;
        qemu_rearm_alarm_timer(alarm_timer);
    }

    /* vm time timers */
    if (runstate_is_running()) {
        if (!cur_cpu || likely(!(cur_cpu->singlestep_enabled & SSTEP_NOTIMER)))
            qemu_run_timers(&active_timers[QEMU_CLOCK_VIRTUAL],
                            qemu_get_clock(vm_clock));
    }

    /* real time timers */
    qemu_run_timers(&active_timers[QEMU_CLOCK_REALTIME],
                    qemu_get_clock(rt_clock));

    qemu_run_timers(&active_timers[QEMU_CLOCK_HOST],
                    qemu_get_clock(host_clock));

    /* Check bottom-halves last in case any of the earlier events triggered
       them.  */
    qemu_bh_poll();

}

static int qemu_cpu_exec(CPUState *env)
{
    int ret;
#ifdef CONFIG_PROFILER
    int64_t ti;
#endif

#ifdef CONFIG_PROFILER
    ti = profile_getclock();
#endif
    if (use_icount) {
        int64_t count;
        int decr;
        qemu_icount -= (env->icount_decr.u16.low + env->icount_extra);
        env->icount_decr.u16.low = 0;
        env->icount_extra = 0;
        count = qemu_next_deadline();
        count = (count + (1 << icount_time_shift) - 1)
                >> icount_time_shift;
        qemu_icount += count;
        decr = (count > 0xffff) ? 0xffff : count;
        count -= decr;
        env->icount_decr.u16.low = decr;
        env->icount_extra = count;
    }
    ret = cpu_exec(env);
#ifdef CONFIG_PROFILER
    qemu_time += profile_getclock() - ti;
#endif
    if (use_icount) {
        /* Fold pending instructions back into the
           instruction counter, and clear the interrupt flag.  */
        qemu_icount -= (env->icount_decr.u16.low
                        + env->icount_extra);
        env->icount_decr.u32 = 0;
        env->icount_extra = 0;
    }
    return ret;
}

static void tcg_cpu_exec(void)
{
    int ret = 0;

    if (next_cpu == NULL)
        next_cpu = first_cpu;
    for (; next_cpu != NULL; next_cpu = next_cpu->next_cpu) {
        CPUState *env = cur_cpu = next_cpu;

        if (!runstate_is_running())
            break;
        if (timer_alarm_pending) {
            timer_alarm_pending = 0;
            break;
        }
        if (cpu_can_run(env))
            ret = qemu_cpu_exec(env);
        if (ret == EXCP_DEBUG) {
            gdb_set_stop_cpu(env);
            debug_requested = 1;
            break;
        }
    }
}

static int cpu_has_work(CPUState *env)
{
    if (fake_machine) {
        return 0;
    }
    if (env->stop)
        return 1;
    if (env->stopped)
        return 0;
    if (!env->halted)
        return 1;
    if (qemu_cpu_has_work(env))
        return 1;
    return 0;
}

static int tcg_has_work(void)
{
    CPUState *env;

    for (env = first_cpu; env != NULL; env = env->next_cpu)
        if (cpu_has_work(env))
            return 1;
    return 0;
}

static int qemu_calculate_timeout(void)
{
#ifndef CONFIG_IOTHREAD
    int timeout;

    if (!runstate_is_running())
        timeout = 5000;
    else if (tcg_has_work())
        timeout = 0;
    else if (!use_icount)
        timeout = 5000;
    else {
     /* XXX: use timeout computed from timers */
        int64_t add;
        int64_t delta;
        /* Advance virtual time to the next event.  */
        if (use_icount == 1) {
            /* When not using an adaptive execution frequency
               we tend to get badly out of sync with real time,
               so just delay for a reasonable amount of time.  */
            delta = 0;
        } else {
            delta = cpu_get_icount() - cpu_get_clock();
        }
        if (delta > 0) {
            /* If virtual time is ahead of real time then just
               wait for IO.  */
            timeout = (delta / 1000000) + 1;
        } else {
            /* Wait for either IO to occur or the next
               timer event.  */
            add = qemu_next_deadline();
            /* We advance the timer before checking for IO.
               Limit the amount we advance so that early IO
               activity won't get the guest too far ahead.  */
            if (add > 10000000)
                add = 10000000;
            delta += add;
            add = (add + (1 << icount_time_shift) - 1)
                  >> icount_time_shift;
            qemu_icount += add;
            timeout = delta / 1000000;
            if (timeout < 0)
                timeout = 0;
        }
    }

    return timeout;
#else /* CONFIG_IOTHREAD */
    return 1000;
#endif
}

static int vm_can_run(void)
{
    if (powerdown_requested)
        return 0;
    if (reset_requested)
        return 0;
    if (shutdown_requested)
        return 0;
    if (debug_requested)
        return 0;
    if (suspend_requested)
        return 0;
    return 1;
}

qemu_irq qemu_system_powerdown;

static void main_loop(void)
{
    RunState r;

    if (kvm_enabled()) {
        kvm_main_loop();
        cpu_disable_ticks();
        return;
    }

#ifdef CONFIG_IOTHREAD
    qemu_system_ready = 1;
    qemu_cond_broadcast(&qemu_system_cond);
#endif

    for (;;) {
        do {
#ifdef CONFIG_PROFILER
            int64_t ti;
#endif
#ifndef CONFIG_IOTHREAD
            tcg_cpu_exec();
#endif
#ifdef CONFIG_PROFILER
            ti = profile_getclock();
#endif
            main_loop_wait(qemu_calculate_timeout());
#ifdef CONFIG_PROFILER
            dev_time += profile_getclock() - ti;
#endif
        } while (vm_can_run());

        if (qemu_debug_requested()) {
            vm_stop(RUN_STATE_DEBUG);
        }
        if (qemu_suspend_requested()) {
            qemu_system_suspend();
        }
        if (qemu_shutdown_requested()) {
            monitor_protocol_event(QEVENT_SHUTDOWN, NULL);
            if (no_shutdown) {
                vm_stop(RUN_STATE_SHUTDOWN);
            } else
                break;
        }
        if (qemu_reset_requested()) {
            pause_all_vcpus();
            qemu_system_reset(VMRESET_REPORT);
            resume_all_vcpus();
            if (runstate_needs_reset()) {
                runstate_set(RUN_STATE_PAUSED);
            }
        }
        if (qemu_wakeup_requested()) {
            pause_all_vcpus();
            qemu_system_reset(VMRESET_SILENT);
            resume_all_vcpus();
            monitor_protocol_event(QEVENT_WAKEUP, NULL);
        }
        if (qemu_powerdown_requested()) {
            monitor_protocol_event(QEVENT_POWERDOWN, NULL);
            qemu_irq_raise(qemu_system_powerdown);
        }
        if ((r = qemu_vmstop_requested())) {
            vm_stop(r);
        }
    }
    bdrv_close_all();
    pause_all_vcpus();
}

static void version(void)
{
    printf("QEMU PC emulator version " QEMU_VERSION QEMU_PKGVERSION ", Copyright (c) 2003-2008 Fabrice Bellard\n");
}

static void print_rh_warning(void)
{
    printf("\nWARNING: Direct use of qemu-kvm from the command line is not recommended by Red Hat.\n"
             "WARNING: Some command line options listed here may not be available in future releases.\n"
             "WARNING: Red Hat recommends the use of libvirt as the stable management interface.\n\n");
}

static void help(int exitcode)
{
    version();
    print_rh_warning();
    printf("usage: %s [options] [disk_image]\n"
           "\n"
           "'disk_image' is a raw hard image image for IDE hard disk 0\n"
           "\n"
#define DEF(option, opt_arg, opt_enum, opt_help)        \
           opt_help
#define DEFHEADING(text) stringify(text) "\n"
#include "qemu-options.def"
#undef DEF
#undef DEFHEADING
#undef GEN_DOCS
           "\n"
           "During emulation, the following keys are useful:\n"
           "ctrl-alt-f      toggle full screen\n"
           "ctrl-alt-n      switch to virtual console 'n'\n"
           "ctrl-alt        toggle mouse and keyboard grab\n"
           "\n"
           "When using -nographic, press 'ctrl-a h' to get some help.\n"
           ,
           "qemu",
           DEFAULT_RAM_SIZE,
#ifndef _WIN32
           DEFAULT_NETWORK_SCRIPT,
           DEFAULT_NETWORK_DOWN_SCRIPT,
#endif
           DEFAULT_GDBSTUB_PORT,
           "/tmp/qemu.log");
    print_rh_warning();
    exit(exitcode);
}

#define HAS_ARG 0x0001

enum {
#define DEF(option, opt_arg, opt_enum, opt_help)        \
    opt_enum,
#define DEFHEADING(text)
#include "qemu-options.def"
#undef DEF
#undef DEFHEADING
#undef GEN_DOCS
};

typedef struct QEMUOption {
    const char *name;
    int flags;
    int index;
} QEMUOption;

static const QEMUOption qemu_options[] = {
    { "h", 0, QEMU_OPTION_h },
#define DEF(option, opt_arg, opt_enum, opt_help)        \
    { option, opt_arg, opt_enum },
#define DEFHEADING(text)
#include "qemu-options.def"
#undef DEF
#undef DEFHEADING
#undef GEN_DOCS
    { NULL },
};

#ifdef HAS_AUDIO
struct soundhw soundhw[] = {
#ifdef HAS_AUDIO_CHOICE
#if defined(TARGET_I386) || defined(TARGET_MIPS)
    {
        "pcspk",
        "PC speaker",
        0,
        1,
        { .init_isa = pcspk_audio_init }
    },
#endif

#ifdef CONFIG_SB16
    {
        "sb16",
        "Creative Sound Blaster 16",
        0,
        1,
        { .init_isa = SB16_init }
    },
#endif

#ifdef CONFIG_CS4231A
    {
        "cs4231a",
        "CS4231A",
        0,
        1,
        { .init_isa = cs4231a_init }
    },
#endif

#ifdef CONFIG_ADLIB
    {
        "adlib",
#ifdef HAS_YMF262
        "Yamaha YMF262 (OPL3)",
#else
        "Yamaha YM3812 (OPL2)",
#endif
        0,
        1,
        { .init_isa = Adlib_init }
    },
#endif

#ifdef CONFIG_GUS
    {
        "gus",
        "Gravis Ultrasound GF1",
        0,
        1,
        { .init_isa = GUS_init }
    },
#endif

#ifdef CONFIG_AC97
    {
        "ac97",
        "Intel 82801AA AC97 Audio",
        0,
        0,
        { .init_pci = ac97_init }
    },
#endif

#ifdef CONFIG_ES1370
    {
        "es1370",
        "ENSONIQ AudioPCI ES1370",
        0,
        0,
        { .init_pci = es1370_init }
    },
#endif

#endif /* HAS_AUDIO_CHOICE */

    { NULL, NULL, 0, 0, { NULL } }
};

static void select_soundhw (const char *optarg)
{
    struct soundhw *c;

    if (*optarg == '?') {
    show_valid_cards:

        printf ("Valid sound card names (comma separated):\n");
        for (c = soundhw; c->name; ++c) {
            printf ("%-11s %s\n", c->name, c->descr);
        }
        printf ("\n-soundhw all will enable all of the above\n");
        exit (*optarg != '?');
    }
    else {
        size_t l;
        const char *p;
        char *e;
        int bad_card = 0;

        if (!strcmp (optarg, "all")) {
            for (c = soundhw; c->name; ++c) {
                c->enabled = 1;
            }
            return;
        }

        p = optarg;
        while (*p) {
            e = strchr (p, ',');
            l = !e ? strlen (p) : (size_t) (e - p);

            for (c = soundhw; c->name; ++c) {
                if (!strncmp (c->name, p, l) && !c->name[l]) {
                    c->enabled = 1;
                    break;
                }
            }

            if (!c->name) {
                if (l > 80) {
                    fprintf (stderr,
                             "Unknown sound card name (too big to show)\n");
                }
                else {
                    fprintf (stderr, "Unknown sound card name `%.*s'\n",
                             (int) l, p);
                }
                bad_card = 1;
            }
            p += l + (e != NULL);
        }

        if (bad_card)
            goto show_valid_cards;
    }
}
#endif

static void select_vgahw (const char *p)
{
    const char *opts;

    default_vga = 0;
    vga_interface_type = VGA_NONE;
    if (strstart(p, "std", &opts)) {
        vga_interface_type = VGA_STD;
    } else if (strstart(p, "cirrus", &opts)) {
        vga_interface_type = VGA_CIRRUS;
    } else if (strstart(p, "vmware", &opts)) {
        vga_interface_type = VGA_VMWARE;
    } else if (strstart(p, "xenfb", &opts)) {
        vga_interface_type = VGA_XENFB;
    } else if (strstart(p, "qxl", &opts)) {
        vga_interface_type = VGA_QXL;
    } else if (!strstart(p, "none", &opts)) {
    invalid_vga:
        fprintf(stderr, "Unknown vga type: %s\n", p);
        exit(1);
    }
    while (*opts) {
        const char *nextopt;

        if (strstart(opts, ",retrace=", &nextopt)) {
            opts = nextopt;
            if (strstart(opts, "dumb", &nextopt))
                vga_retrace_method = VGA_RETRACE_DUMB;
            else if (strstart(opts, "precise", &nextopt))
                vga_retrace_method = VGA_RETRACE_PRECISE;
            else goto invalid_vga;
        } else goto invalid_vga;
        opts = nextopt;
    }
}

#ifdef TARGET_I386
static int balloon_parse(const char *arg)
{
    QemuOpts *opts;

    if (strcmp(arg, "none") == 0) {
        return 0;
    }

    if (!strncmp(arg, "virtio", 6)) {
        if (arg[6] == ',') {
            /* have params -> parse them */
            opts = qemu_opts_parse(&qemu_device_opts, arg+7, 0);
            if (!opts)
                return  -1;
        } else {
            /* create empty opts */
            opts = qemu_opts_create(&qemu_device_opts, NULL, 0);
        }
        qemu_opt_set(opts, "driver", "virtio-balloon-pci");
        return 0;
    }

    return -1;
}
#endif


int qemu_uuid_parse(const char *str, uint8_t *uuid)
{
    int ret;

    if(strlen(str) != 36)
        return -1;

    ret = sscanf(str, UUID_FMT, &uuid[0], &uuid[1], &uuid[2], &uuid[3],
            &uuid[4], &uuid[5], &uuid[6], &uuid[7], &uuid[8], &uuid[9],
            &uuid[10], &uuid[11], &uuid[12], &uuid[13], &uuid[14], &uuid[15]);

    if(ret != 16)
        return -1;

#ifdef TARGET_I386
    smbios_add_field(1, offsetof(struct smbios_type_1, uuid), 16, uuid);
#endif

    return 0;
}

#ifndef _WIN32

static void termsig_handler(int signal, siginfo_t *info, void *c)
{
    no_shutdown = 0;
    qemu_system_killed(info->si_signo, info->si_pid);
}

static void sighandler_setup(void)
{
    struct sigaction act;

    memset(&act, 0, sizeof(act));
    act.sa_sigaction = termsig_handler;
    act.sa_flags = SA_SIGINFO;

    sigaction(SIGINT,  &act, NULL);
    sigaction(SIGHUP,  &act, NULL);
    sigaction(SIGTERM, &act, NULL);
}

#endif

#ifdef _WIN32
/* Look for support files in the same directory as the executable.  */
static char *find_datadir(const char *argv0)
{
    char *p;
    char buf[MAX_PATH];
    DWORD len;

    len = GetModuleFileName(NULL, buf, sizeof(buf) - 1);
    if (len == 0) {
        return NULL;
    }

    buf[len] = 0;
    p = buf + len - 1;
    while (p != buf && *p != '\\')
        p--;
    *p = 0;
    if (access(buf, R_OK) == 0) {
        return qemu_strdup(buf);
    }
    return NULL;
}
#else /* !_WIN32 */

/* Find a likely location for support files using the location of the binary.
   For installed binaries this will be "$bindir/../share/qemu".  When
   running from the build tree this will be "$bindir/../pc-bios".  */
#define SHARE_SUFFIX "/share/qemu-kvm"
#define BUILD_SUFFIX "/pc-bios"
static char *find_datadir(const char *argv0)
{
    char *dir;
    char *p = NULL;
    char *res;
    char buf[PATH_MAX];
    size_t max_len;

#if defined(__linux__)
    {
        int len;
        len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = 0;
            p = buf;
        }
    }
#elif defined(__FreeBSD__)
    {
        int len;
        len = readlink("/proc/curproc/file", buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = 0;
            p = buf;
        }
    }
#endif
    /* If we don't have any way of figuring out the actual executable
       location then try argv[0].  */
    if (!p) {
        p = realpath(argv0, buf);
        if (!p) {
            return NULL;
        }
    }
    dir = dirname(p);
    dir = dirname(dir);

    max_len = strlen(dir) +
        MAX(strlen(SHARE_SUFFIX), strlen(BUILD_SUFFIX)) + 1;
    res = qemu_mallocz(max_len);
    snprintf(res, max_len, "%s%s", dir, SHARE_SUFFIX);
    if (access(res, R_OK)) {
        snprintf(res, max_len, "%s%s", dir, BUILD_SUFFIX);
        if (access(res, R_OK)) {
            qemu_free(res);
            res = NULL;
        }
    }

    return res;
}
#undef SHARE_SUFFIX
#undef BUILD_SUFFIX
#endif

char *qemu_find_file(int type, const char *name)
{
    int len;
    const char *subdir;
    char *buf;

    /* If name contains path separators then try it as a straight path.  */
    if ((strchr(name, '/') || strchr(name, '\\'))
        && access(name, R_OK) == 0) {
        return qemu_strdup(name);
    }
    switch (type) {
    case QEMU_FILE_TYPE_BIOS:
        subdir = "";
        break;
    case QEMU_FILE_TYPE_KEYMAP:
        subdir = "keymaps/";
        break;
    default:
        abort();
    }
    len = strlen(data_dir) + strlen(name) + strlen(subdir) + 2;
    buf = qemu_mallocz(len);
    snprintf(buf, len, "%s/%s%s", data_dir, subdir, name);
    if (access(buf, R_OK)) {
        qemu_free(buf);
        return NULL;
    }
    return buf;
}

static int device_help_func(QemuOpts *opts, void *opaque)
{
    return qdev_device_help(opts);
}

static int device_init_func(QemuOpts *opts, void *opaque)
{
    DeviceState *dev;

    dev = qdev_device_add(opts);
    if (!dev)
        return -1;
    return 0;
}

static int chardev_init_func(QemuOpts *opts, void *opaque)
{
    Error *local_err = NULL;

    qemu_chr_new_from_opts(opts, NULL, &local_err);
    if (error_is_set(&local_err)) {
        error_report("%s", error_get_pretty(local_err));
        error_free(local_err);
        return -1;
    }
    return 0;
}

static int mon_init_func(QemuOpts *opts, void *opaque)
{
    CharDriverState *chr;
    const char *chardev;
    const char *mode;
    int flags;

    mode = qemu_opt_get(opts, "mode");
    if (mode == NULL) {
        mode = "readline";
    }
    if (strcmp(mode, "readline") == 0) {
        flags = MONITOR_USE_READLINE;
    } else if (strcmp(mode, "control") == 0) {
        flags = MONITOR_USE_CONTROL;
    } else {
        fprintf(stderr, "unknown monitor mode \"%s\"\n", mode);
        exit(1);
    }

    if (qemu_opt_get_bool(opts, "default", 0))
        flags |= MONITOR_IS_DEFAULT;

    chardev = qemu_opt_get(opts, "chardev");
    chr = qemu_chr_find(chardev);
    if (chr == NULL) {
        fprintf(stderr, "chardev \"%s\" not found\n", chardev);
        exit(1);
    }

    monitor_init(chr, flags);
    return 0;
}

static void monitor_parse(const char *optarg, const char *mode)
{
    static int monitor_device_index = 0;
    QemuOpts *opts;
    const char *p;
    char label[32];
    int def = 0;

    if (strstart(optarg, "chardev:", &p)) {
        snprintf(label, sizeof(label), "%s", p);
    } else {
        if (monitor_device_index) {
            snprintf(label, sizeof(label), "monitor%d",
                     monitor_device_index);
        } else {
            snprintf(label, sizeof(label), "monitor");
            def = 1;
        }
        opts = qemu_chr_parse_compat(label, optarg);
        if (!opts) {
            fprintf(stderr, "parse error: %s\n", optarg);
            exit(1);
        }
    }

    opts = qemu_opts_create(&qemu_mon_opts, label, 1);
    if (!opts) {
        fprintf(stderr, "duplicate chardev: %s\n", label);
        exit(1);
    }
    qemu_opt_set(opts, "mode", mode);
    qemu_opt_set(opts, "chardev", label);
    if (def)
        qemu_opt_set(opts, "default", "on");
    monitor_device_index++;
}

struct device_config {
    enum {
        DEV_USB,       /* -usbdevice     */
        DEV_BT,        /* -bt            */
        DEV_SERIAL,    /* -serial        */
        DEV_PARALLEL,  /* -parallel      */
        DEV_VIRTCON,   /* -virtioconsole */
        DEV_DEBUGCON,  /* -debugcon */
    } type;
    const char *cmdline;
    QTAILQ_ENTRY(device_config) next;
};
QTAILQ_HEAD(, device_config) device_configs = QTAILQ_HEAD_INITIALIZER(device_configs);

static void add_device_config(int type, const char *cmdline)
{
    struct device_config *conf;

    conf = qemu_mallocz(sizeof(*conf));
    conf->type = type;
    conf->cmdline = cmdline;
    QTAILQ_INSERT_TAIL(&device_configs, conf, next);
}

static int foreach_device_config(int type, int (*func)(const char *cmdline))
{
    struct device_config *conf;
    int rc;

    QTAILQ_FOREACH(conf, &device_configs, next) {
        if (conf->type != type)
            continue;
        rc = func(conf->cmdline);
        if (0 != rc)
            return rc;
    }
    return 0;
}

static int serial_parse(const char *devname)
{
    static int index = 0;
    char label[32];

    if (strcmp(devname, "none") == 0)
        return 0;
    if (index == MAX_SERIAL_PORTS) {
        fprintf(stderr, "qemu: too many serial ports\n");
        exit(1);
    }
    snprintf(label, sizeof(label), "serial%d", index);
    serial_hds[index] = qemu_chr_new(label, devname, NULL);
    if (!serial_hds[index]) {
        fprintf(stderr, "qemu: could not open serial device '%s': %s\n",
                devname, strerror(errno));
        return -1;
    }
    index++;
    return 0;
}

static int parallel_parse(const char *devname)
{
    static int index = 0;
    char label[32];

    if (strcmp(devname, "none") == 0)
        return 0;
    if (index == MAX_PARALLEL_PORTS) {
        fprintf(stderr, "qemu: too many parallel ports\n");
        exit(1);
    }
    snprintf(label, sizeof(label), "parallel%d", index);
    parallel_hds[index] = qemu_chr_new(label, devname, NULL);
    if (!parallel_hds[index]) {
        fprintf(stderr, "qemu: could not open parallel device '%s': %s\n",
                devname, strerror(errno));
        return -1;
    }
    index++;
    return 0;
}

static int virtcon_parse(const char *devname)
{
    static int index = 0;
    char label[32];
    QemuOpts *bus_opts, *dev_opts;

    if (strcmp(devname, "none") == 0)
        return 0;
    if (index == MAX_VIRTIO_CONSOLES) {
        fprintf(stderr, "qemu: too many virtio consoles\n");
        exit(1);
    }

    bus_opts = qemu_opts_create(&qemu_device_opts, NULL, 0);
    qemu_opt_set(bus_opts, "driver", "virtio-serial");

    dev_opts = qemu_opts_create(&qemu_device_opts, NULL, 0);
    qemu_opt_set(dev_opts, "driver", "virtconsole");

    snprintf(label, sizeof(label), "virtcon%d", index);
    virtcon_hds[index] = qemu_chr_new(label, devname, NULL);
    if (!virtcon_hds[index]) {
        fprintf(stderr, "qemu: could not open virtio console '%s': %s\n",
                devname, strerror(errno));
        return -1;
    }
    qemu_opt_set(dev_opts, "chardev", label);

    index++;
    return 0;
}

static int debugcon_parse(const char *devname)
{   
    QemuOpts *opts;

    if (!qemu_chr_new("debugcon", devname, NULL)) {
        exit(1);
    }
    opts = qemu_opts_create(&qemu_device_opts, "debugcon", 1);
    if (!opts) {
        fprintf(stderr, "qemu: already have a debugcon device\n");
        exit(1);
    }
    qemu_opt_set(opts, "driver", "isa-debugcon");
    qemu_opt_set(opts, "chardev", "debugcon");
    return 0;
}

static int kvm_available(void)
{
#ifdef CONFIG_KVM
    return 1;
#else
    return 0;
#endif
}

static int tcg_available(void)
{
    return 1;
}

static QEMUMachine *machine_parse(const char *name)
{
    QEMUMachine *m, *machine = NULL;

    if (name) {
        machine = find_machine(name);
    }
    if (machine) {
        return machine;
    }
    if (name && *name != '?') {
        error_report("Unsupported machine type");
        error_printf("Use -M ? to list supported machines!\n");
    } else {   
        printf("Supported machines are:\n");
        for (m = first_machine; m != NULL; m = m->next) {
            if (m->alias) {
                printf("%-10s %s (alias of %s)\n", m->alias, m->desc, m->name);
            }
            printf("%-10s %s%s\n", m->name, m->desc,
                   m->is_default ? " (default)" : "");
        }
    }
    exit(!name || *name != '?');
}

static int tcg_init(int smp_cpus)
{
    return 0;
}

static struct {
    const char *opt_name;
    const char *name;
    int (*available)(void);
    int (*init)(int);
    int *allowed;
} accel_list[] = {
    { "tcg", "tcg", tcg_available, tcg_init, &tcg_allowed },
    { "kvm", "KVM", kvm_available, kvm_init, &kvm_allowed },
};

static int configure_accelerator(void)
{
    const char *p = NULL;
    char buf[10];
    int i, ret;
    bool accel_initalised = 0;
    bool init_failed = 0;

    QemuOptsList *list = qemu_find_opts("machine");
    if (!QTAILQ_EMPTY(&list->head)) {
        p = qemu_opt_get(QTAILQ_FIRST(&list->head), "accel");
    }

    if (p == NULL) {
        /* RHEL6 default "accelerator", kvm:tcg */
        p = "kvm:tcg";
    }

    while (!accel_initalised && *p != '\0') {
        if (*p == ':') {
            p++;
        }
        p = get_opt_name(buf, sizeof (buf), p, ':');
        for (i = 0; i < ARRAY_SIZE(accel_list); i++) {
            if (strcmp(accel_list[i].opt_name, buf) == 0) {
                *(accel_list[i].allowed) = 1;
                ret = accel_list[i].init(smp_cpus);
                if (ret < 0) {
                    init_failed = 1;
                    if (!accel_list[i].available()) {
                        printf("%s not supported for this target\n",
                               accel_list[i].name);
                    } else {
                        fprintf(stderr, "failed to initialize %s: %s\n",
                                accel_list[i].name,
                                strerror(-ret));
                    }
                    *(accel_list[i].allowed) = 0;
                } else {
                    accel_initalised = 1;
                }
                break;
            }
        }
        if (i == ARRAY_SIZE(accel_list)) {
            fprintf(stderr, "\"%s\" accelerator does not exist.\n", buf);
        }
    }

    if (!accel_initalised) {
        fprintf(stderr, "No accelerator found!\n");
        exit(1);
    }

    if (init_failed) {
        fprintf(stderr, "Back to %s accelerator.\n", accel_list[i].name);
    }

    return !accel_initalised;
}

void qemu_add_machine_init_done_notifier(Notifier *notify)
{
    notifier_list_add(&machine_init_done_notifiers, notify);
}

static void qemu_run_machine_init_done_notifiers(void)
{
    notifier_list_notify(&machine_init_done_notifiers, NULL);
}

void qemu_add_exit_notifier(Notifier *notify)
{
    notifier_list_add(&exit_notifiers, notify);
}

void qemu_remove_exit_notifier(Notifier *notify)
{
    notifier_list_remove(&exit_notifiers, notify);
}

static void qemu_run_exit_notifiers(void)
{
    notifier_list_notify(&exit_notifiers, NULL);
}

static const QEMUOption *lookup_opt(int argc, char **argv,
                                    const char **poptarg, int *poptind)
{
    const QEMUOption *popt;
    int optind = *poptind;
    char *r = argv[optind];
    const char *optarg;

    loc_set_cmdline(argv, optind, 1);
    optind++;
    /* Treat --foo the same as -foo.  */
    if (r[1] == '-')
        r++;
    popt = qemu_options;
    for(;;) {
        if (!popt->name) {
            error_report("invalid option");
            exit(1);
        }
        if (!strcmp(popt->name, r + 1))
            break;
        popt++;
    }
    if (popt->flags & HAS_ARG) {
        if (optind >= argc) {
            error_report("requires an argument");
            exit(1);
        }
        optarg = argv[optind++];
        loc_set_cmdline(argv, optind - 2, 2);
    } else {
        optarg = NULL;
    }

    *poptarg = optarg;
    *poptind = optind;

    return popt;
}

static int object_set_property(const char *name, const char *value, void *opaque)
{
    Object *obj = OBJECT(opaque);
    StringInputVisitor *siv;
    Error *local_err = NULL;

    if (strcmp(name, "qom-type") == 0 || strcmp(name, "id") == 0) {
        return 0;
    }

    siv = string_input_visitor_new(value);
    object_property_set(obj, string_input_get_visitor(siv), name, &local_err);
    string_input_visitor_cleanup(siv);

    if (local_err) {
        qerror_report_err(local_err);
        error_free(local_err);
        return -1;
    }

    return 0;
}

static int object_create(QemuOpts *opts, void *opaque)
{
    const char *type = qemu_opt_get(opts, "qom-type");
    const char *id = qemu_opts_id(opts);
    Object *obj;

    g_assert(type != NULL);

    if (id == NULL) {
        qerror_report(QERR_MISSING_PARAMETER, "id");
        return -1;
    }

    obj = object_new(type);
    if (qemu_opt_foreach(opts, object_set_property, obj, 1) < 0) {
        object_unref(obj);
        return -1;
    }

    object_property_add_child(container_get(object_get_root(), "/objects"),
                              id, obj, NULL);
    object_unref(obj);
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    const char *gdbstub_dev = NULL;
    int i;
    int snapshot, linux_boot;
    const char *initrd_filename;
    const char *kernel_filename, *kernel_cmdline;
    char boot_devices[33] = "cad"; /* default to HD->floppy->CD-ROM */
    DisplayState *ds;
    DisplayChangeListener *dcl;
    int cyls, heads, secs, translation;
    QemuOpts *hda_opts = NULL, *opts;
    QemuOptsList *olist;
    int optind;
    const char *optarg;
    const char *loadvm = NULL;
    QEMUMachine *machine;
    const char *cpu_model;
#ifndef _WIN32
    int fds[2];
#endif
    int tb_size;
    const char *pid_file = NULL;
    const char *incoming = NULL;
#ifndef _WIN32
    int fd = 0;
    struct passwd *pwd = NULL;
    const char *chroot_dir = NULL;
    const char *run_as = NULL;
#endif
    CPUState *env;
    int show_vnc_port = 0;
    int defconfig = 1;
    int defconfig_verbose = 0;
    FILE *vmstate_dump_file = NULL;

    atexit(qemu_run_exit_notifiers);
    error_set_progname(argv[0]);

    runstate_init();

    init_clocks();

    qemu_cache_utils_init(envp);

    QLIST_INIT (&vm_change_state_head);
    os_setup_early_signal_handling();

    module_call_init(MODULE_INIT_QOM);

    module_call_init(MODULE_INIT_MACHINE);
    machine = find_default_machine();
    cpu_model = NULL;
    initrd_filename = NULL;
    ram_size = 0;
    snapshot = 0;
    kernel_filename = NULL;
    kernel_cmdline = "";
    cyls = heads = secs = 0;
    translation = BIOS_ATA_TRANSLATION_AUTO;

    for (i = 0; i < MAX_NODES; i++) {
        node_mem[i] = 0;
        node_cpumask[i] = bitmap_new(MAX_CPUMASK_BITS);
    }

    assigned_devices_index = 0;

    nb_numa_nodes = 0;
    nb_nics = 0;

    tb_size = 0;
    autostart= 1;

    /* first pass of CLI options -- interpret config file parse modifiers
     */
    optind = 1;
    while (optind < argc) {
        if (argv[optind][0] != '-') {
            /* disk image */
            optind++;
            continue;
        } else {
            const QEMUOption *popt;

            popt = lookup_opt(argc, argv, &optarg, &optind);
            switch (popt->index) {
            case QEMU_OPTION_nodefconfig:
                defconfig=0;
                break;
            case QEMU_OPTION_readconfig:
                /* pseudo filename "?" enables verbose config file handling */
                if (!strcmp(optarg, "?"))
                    defconfig_verbose = 1;
                break;
            }
        }
    }

    /* pull in default configs unless explicitly disabled
     */
    if (defconfig) {
        int ret;

        ret = qemu_read_config_file(CONFIG_QEMU_CONFDIR "/qemu.conf",
                                    defconfig_verbose);
        if (ret == -EINVAL) {
            exit(1);
        }

        ret = qemu_read_config_file(
            CONFIG_QEMU_CONFDIR "/target-" TARGET_ARCH ".conf",
            defconfig_verbose);
        if (ret == -EINVAL) {
            exit(1);
        }
    }

    /* second pass of CLI options -- bring in any -readconfig flags
     */
    optind = 1;
    while (optind < argc) {
        if (argv[optind][0] != '-') {
            /* disk image */
            optind++;
            continue;
        } else {
            const QEMUOption *popt;

            popt = lookup_opt(argc, argv, &optarg, &optind);
            switch (popt->index) {
            case QEMU_OPTION_readconfig:
                /* pull in cli specified config files in advance of general
                 * flag parsing which may reference their content
                 */
                if (strcmp(optarg, "?") &&
                    qemu_read_config_file(optarg, defconfig_verbose) < 0)
                        exit(1);
                break;
            }
        }
    }
#if defined(cpudef_setup)
    cpudef_setup(); /* parse cpu definitions in target config file */
#endif

    /* third pass of CLI options -- general flag interpretation
     */
    optind = 1;
    for(;;) {
        if (optind >= argc)
            break;
        if (argv[optind][0] != '-') {
	    hda_opts = drive_add(IF_DEFAULT, 0, argv[optind++], HD_OPTS);
        } else {
            const QEMUOption *popt;

            popt = lookup_opt(argc, argv, &optarg, &optind);
            switch(popt->index) {
            case QEMU_OPTION_M:
                machine = machine_parse(optarg);
                break;
            case QEMU_OPTION_cpu:
                /* hw initialization will check this */
                if (*optarg == '?') {
/* XXX: implement xxx_cpu_list for targets that still miss it */
#if defined(cpu_list_id)
                    cpu_list_id(stdout, &fprintf, optarg);
#elif defined(cpu_list)
                    cpu_list(stdout, &fprintf);	        /* deprecated */
#endif
                    exit(0);
                } else {
                    cpu_model = optarg;
                }
                break;
            case QEMU_OPTION_initrd:
                initrd_filename = optarg;
                break;
            case QEMU_OPTION_hda:
                {
                    char buf[256];
                    if (cyls == 0)
                        snprintf(buf, sizeof(buf), "%s", HD_OPTS);
                    else
                        snprintf(buf, sizeof(buf),
                                 "%s,cyls=%d,heads=%d,secs=%d%s",
                                 HD_OPTS , cyls, heads, secs,
                                 translation == BIOS_ATA_TRANSLATION_LBA ?
                                 ",trans=lba" :
                                 translation == BIOS_ATA_TRANSLATION_NONE ?
                                 ",trans=none" : "");
                    drive_add(IF_DEFAULT, 0, optarg, buf);
                    break;
                }
            case QEMU_OPTION_hdb:
            case QEMU_OPTION_hdc:
            case QEMU_OPTION_hdd:
                drive_add(IF_DEFAULT, popt->index - QEMU_OPTION_hda, optarg,
                          HD_OPTS);
                break;
            case QEMU_OPTION_drive:
                if (drive_def(optarg) == NULL) {
                    exit(1);
                }
	        break;
            case QEMU_OPTION_set:
                if (qemu_set_option(optarg) != 0)
                    exit(1);
	        break;
            case QEMU_OPTION_global:
                if (qemu_global_option(optarg) != 0)
                    exit(1);
	        break;
            case QEMU_OPTION_mtdblock:
                drive_add(IF_MTD, -1, optarg, MTD_OPTS);
                break;
            case QEMU_OPTION_sd:
                drive_add(IF_SD, 0, optarg, SD_OPTS);
                break;
            case QEMU_OPTION_pflash:
                drive_add(IF_PFLASH, -1, optarg, PFLASH_OPTS);
                break;
            case QEMU_OPTION_snapshot:
                snapshot = 1;
                break;
            case QEMU_OPTION_hdachs:
                {
                    const char *p;
                    p = optarg;
                    cyls = strtol(p, (char **)&p, 0);
                    if (cyls < 1 || cyls > 16383)
                        goto chs_fail;
                    if (*p != ',')
                        goto chs_fail;
                    p++;
                    heads = strtol(p, (char **)&p, 0);
                    if (heads < 1 || heads > 16)
                        goto chs_fail;
                    if (*p != ',')
                        goto chs_fail;
                    p++;
                    secs = strtol(p, (char **)&p, 0);
                    if (secs < 1 || secs > 63)
                        goto chs_fail;
                    if (*p == ',') {
                        p++;
                        if (!strcmp(p, "none"))
                            translation = BIOS_ATA_TRANSLATION_NONE;
                        else if (!strcmp(p, "lba"))
                            translation = BIOS_ATA_TRANSLATION_LBA;
                        else if (!strcmp(p, "auto"))
                            translation = BIOS_ATA_TRANSLATION_AUTO;
                        else
                            goto chs_fail;
                    } else if (*p != '\0') {
                    chs_fail:
                        fprintf(stderr, "qemu: invalid physical CHS format\n");
                        exit(1);
                    }
		    if (hda_opts != NULL) {
                        char num[16];
                        snprintf(num, sizeof(num), "%d", cyls);
                        qemu_opt_set(hda_opts, "cyls", num);
                        snprintf(num, sizeof(num), "%d", heads);
                        qemu_opt_set(hda_opts, "heads", num);
                        snprintf(num, sizeof(num), "%d", secs);
                        qemu_opt_set(hda_opts, "secs", num);
                        if (translation == BIOS_ATA_TRANSLATION_LBA)
                            qemu_opt_set(hda_opts, "trans", "lba");
                        if (translation == BIOS_ATA_TRANSLATION_NONE)
                            qemu_opt_set(hda_opts, "trans", "none");
                    }
                }
                break;
            case QEMU_OPTION_numa:
                if (nb_numa_nodes >= MAX_NODES) {
                    fprintf(stderr, "qemu: too many NUMA nodes\n");
                    exit(1);
                }
                numa_add(optarg);
                break;
            case QEMU_OPTION_nographic:
                display_type = DT_NOGRAPHIC;
                break;
#ifdef CONFIG_CURSES
            case QEMU_OPTION_curses:
                display_type = DT_CURSES;
                break;
#endif
            case QEMU_OPTION_portrait:
                graphic_rotate = 1;
                break;
            case QEMU_OPTION_kernel:
                kernel_filename = optarg;
                break;
            case QEMU_OPTION_append:
                kernel_cmdline = optarg;
                break;
            case QEMU_OPTION_cdrom:
                drive_add(IF_DEFAULT, 2, optarg, CDROM_OPTS);
                break;
            case QEMU_OPTION_boot:
                {
                    static const char * const params[] = {
                        "order", "once", "menu",
                        "reboot-timeout", "strict", NULL
                    };
                    char buf[sizeof(boot_devices)];
                    char *standard_boot_devices;
                    int legacy = 0;

                    if (!strchr(optarg, '=')) {
                        legacy = 1;
                        pstrcpy(buf, sizeof(buf), optarg);
                    } else if (check_params(buf, sizeof(buf), params, optarg) < 0) {
                        fprintf(stderr,
                                "qemu: unknown boot parameter '%s' in '%s'\n",
                                buf, optarg);
                        exit(1);
                    }

                    if (legacy ||
                        get_param_value(buf, sizeof(buf), "order", optarg)) {
                        validate_bootdevices(buf);
                        pstrcpy(boot_devices, sizeof(boot_devices), buf);
                    }
                    if (!legacy) {
                        if (get_param_value(buf, sizeof(buf),
                                            "once", optarg)) {
                            validate_bootdevices(buf);
                            standard_boot_devices = qemu_strdup(boot_devices);
                            pstrcpy(boot_devices, sizeof(boot_devices), buf);
                            qemu_register_reset(restore_boot_devices,
                                                standard_boot_devices);
                        }
                        if (get_param_value(buf, sizeof(buf),
                                            "menu", optarg)) {
                            if (!strcmp(buf, "on")) {
                                boot_menu = 1;
                            } else if (!strcmp(buf, "off")) {
                                boot_menu = 0;
                            } else {
                                fprintf(stderr,
                                        "qemu: invalid option value '%s'\n",
                                        buf);
                                exit(1);
                            }
                        }
                        if (get_param_value(buf, sizeof(buf),
                                            "strict", optarg)) {
                            if (!strcmp(buf, "on")) {
                                boot_strict = true;
                            } else if (!strcmp(buf, "off")) {
                                boot_strict = false;
                            } else {
                                fprintf(stderr,
                                        "qemu: invalid option value '%s'\n",
                                        buf);
                                exit(1);
                            }
                        }
                        qemu_opts_parse(qemu_find_opts("boot-opts"),
                                        optarg, 0);
                    }
                }
                break;
            case QEMU_OPTION_fda:
            case QEMU_OPTION_fdb:
                drive_add(IF_FLOPPY, popt->index - QEMU_OPTION_fda,
                          optarg, FD_OPTS);
                break;
#ifdef TARGET_I386
            case QEMU_OPTION_no_fd_bootchk:
                fd_bootchk = 0;
                break;
#endif
            case QEMU_OPTION_netdev:
                if (net_client_parse(&qemu_netdev_opts, optarg) == -1) {
                    exit(1);
                }
                break;
            case QEMU_OPTION_net:
                if (net_client_parse(&qemu_net_opts, optarg) == -1) {
                    exit(1);
                }
                break;
#ifdef CONFIG_SLIRP
            case QEMU_OPTION_tftp:
                legacy_tftp_prefix = optarg;
                break;
            case QEMU_OPTION_bootp:
                legacy_bootp_filename = optarg;
                break;
#ifndef _WIN32
            case QEMU_OPTION_smb:
                if (net_slirp_smb(optarg) < 0)
                    exit(1);
                break;
#endif
            case QEMU_OPTION_redir:
                if (net_slirp_redir(optarg) < 0)
                    exit(1);
                break;
#endif
            case QEMU_OPTION_bt:
                add_device_config(DEV_BT, optarg);
                break;
#ifdef HAS_AUDIO
            case QEMU_OPTION_audio_help:
                AUD_help ();
                exit (0);
                break;
            case QEMU_OPTION_soundhw:
                select_soundhw (optarg);
                break;
#endif
            case QEMU_OPTION_h:
                help(0);
                break;
            case QEMU_OPTION_version:
                version();
                exit(0);
                break;
            case QEMU_OPTION_m: {
                int64_t value;
                uint64_t sz;
                char *end;

                value = strtosz(optarg, &end);
                if (value < 0 || *end) {
                    fprintf(stderr, "qemu: invalid ram size: %s\n", optarg);
                    exit(1);
                }

                /* On 32-bit hosts, QEMU is limited by virtual address space */
                if (value > (2047 << 20) && HOST_LONG_BITS == 32) {
                    fprintf(stderr, "qemu: at most 2047 MB RAM can be simulated\n");
                    exit(1);
                }
                sz = QEMU_ALIGN_UP((uint64_t)value, 4096);
                sz = MAX(sz, 2 * 1024 * 1024);
                ram_size = sz;
                if (ram_size != sz) {
                    fprintf(stderr, "qemu: ram size too large\n");
                    exit(1);
                }
                break;
            }
            case QEMU_OPTION_d:
                {
                    int mask;
                    const CPULogItem *item;

                    mask = cpu_str_to_log_mask(optarg);
                    if (!mask) {
                        printf("Log items (comma separated):\n");
                    for(item = cpu_log_items; item->mask != 0; item++) {
                        printf("%-10s %s\n", item->name, item->help);
                    }
                    exit(1);
                    }
                    cpu_set_log(mask);
                }
                break;
            case QEMU_OPTION_s:
                gdbstub_dev = "tcp::" DEFAULT_GDBSTUB_PORT;
                break;
            case QEMU_OPTION_gdb:
                gdbstub_dev = optarg;
                break;
            case QEMU_OPTION_L:
                data_dir = optarg;
                break;
            case QEMU_OPTION_bios:
                bios_name = optarg;
                break;
            case QEMU_OPTION_singlestep:
                singlestep = 1;
                break;
            case QEMU_OPTION_S:
                autostart = 0;
                break;
	    case QEMU_OPTION_k:
		keyboard_layout = optarg;
		break;
            case QEMU_OPTION_localtime:
                rtc_utc = 0;
                break;
            case QEMU_OPTION_vga:
                select_vgahw (optarg);
                break;
#if defined(TARGET_PPC) || defined(TARGET_SPARC)
            case QEMU_OPTION_g:
                {
                    const char *p;
                    int w, h, depth;
                    p = optarg;
                    w = strtol(p, (char **)&p, 10);
                    if (w <= 0) {
                    graphic_error:
                        fprintf(stderr, "qemu: invalid resolution or depth\n");
                        exit(1);
                    }
                    if (*p != 'x')
                        goto graphic_error;
                    p++;
                    h = strtol(p, (char **)&p, 10);
                    if (h <= 0)
                        goto graphic_error;
                    if (*p == 'x') {
                        p++;
                        depth = strtol(p, (char **)&p, 10);
                        if (depth != 8 && depth != 15 && depth != 16 &&
                            depth != 24 && depth != 32)
                            goto graphic_error;
                    } else if (*p == '\0') {
                        depth = graphic_depth;
                    } else {
                        goto graphic_error;
                    }

                    graphic_width = w;
                    graphic_height = h;
                    graphic_depth = depth;
                }
                break;
#endif
            case QEMU_OPTION_echr:
                {
                    char *r;
                    term_escape_char = strtol(optarg, &r, 0);
                    if (r == optarg)
                        printf("Bad argument to echr\n");
                    break;
                }
            case QEMU_OPTION_monitor:
                monitor_parse(optarg, "readline");
                default_monitor = 0;
                break;
            case QEMU_OPTION_qmp:
                monitor_parse(optarg, "control");
                default_monitor = 0;
                break;
            case QEMU_OPTION_mon:
                opts = qemu_opts_parse(&qemu_mon_opts, optarg, 1);
                if (!opts) {
                    exit(1);
                }
                default_monitor = 0;
                break;
            case QEMU_OPTION_chardev:
                opts = qemu_opts_parse(&qemu_chardev_opts, optarg, 1);
                if (!opts) {
                    exit(1);
                }
                break;
            case QEMU_OPTION_serial:
                add_device_config(DEV_SERIAL, optarg);
                default_serial = 0;
                if (strncmp(optarg, "mon:", 4) == 0) {
                    default_monitor = 0;
                }
                break;
            case QEMU_OPTION_watchdog:
                if (watchdog) {
                    fprintf(stderr,
                            "qemu: only one watchdog option may be given\n");
                    return 1;
                }
                watchdog = optarg;
                break;
            case QEMU_OPTION_watchdog_action:
                if (select_watchdog_action(optarg) == -1) {
                    fprintf(stderr, "Unknown -watchdog-action parameter\n");
                    exit(1);
                }
                break;
            case QEMU_OPTION_virtiocon:
                add_device_config(DEV_VIRTCON, optarg);
                default_virtcon = 0;
                if (strncmp(optarg, "mon:", 4) == 0) {
                    default_monitor = 0;
                }
                break;
            case QEMU_OPTION_parallel:
                add_device_config(DEV_PARALLEL, optarg);
                default_parallel = 0;
                if (strncmp(optarg, "mon:", 4) == 0) {
                    default_monitor = 0;
                }
                break;
            case QEMU_OPTION_debugcon:
                add_device_config(DEV_DEBUGCON, optarg);
                break;
	    case QEMU_OPTION_loadvm:
		loadvm = optarg;
		break;
            case QEMU_OPTION_full_screen:
                full_screen = 1;
                break;
#ifdef CONFIG_SDL
            case QEMU_OPTION_no_frame:
                no_frame = 1;
                break;
            case QEMU_OPTION_alt_grab:
                alt_grab = 1;
                break;
            case QEMU_OPTION_ctrl_grab:
                ctrl_grab = 1;
                break;
            case QEMU_OPTION_no_quit:
                no_quit = 1;
                break;
            case QEMU_OPTION_sdl:
                display_type = DT_SDL;
                break;
#endif
            case QEMU_OPTION_pidfile:
                pid_file = optarg;
                break;
#ifdef TARGET_I386
            case QEMU_OPTION_win2k_hack:
                win2k_install_hack = 1;
                break;
            case QEMU_OPTION_rtc_td_hack:
                rtc_td_hack = 1;
                break;
            case QEMU_OPTION_acpitable:
                if(acpi_table_add(optarg) < 0) {
                    fprintf(stderr, "Wrong acpi table provided\n");
                    exit(1);
                }
                break;
            case QEMU_OPTION_smbios:
                if(smbios_entry_add(optarg) < 0) {
                    fprintf(stderr, "Wrong smbios provided\n");
                    exit(1);
                }
                break;
#endif
#ifdef CONFIG_KVM
            case QEMU_OPTION_enable_kvm:
                olist = qemu_find_opts("machine");
                qemu_opts_parse(olist, "accel=kvm", 0);
                break;
            case QEMU_OPTION_no_kvm:
                olist = qemu_find_opts("machine");
                qemu_opts_parse(olist, "accel=tcg", 0);
                break;
	    case QEMU_OPTION_no_kvm_irqchip: {
		kvm_irqchip = 0;
		kvm_pit = 0;
		break;
	    }
	    case QEMU_OPTION_no_kvm_pit: {
		kvm_pit = 0;
		break;
	    }
            case QEMU_OPTION_no_kvm_pit_reinjection: {
                kvm_pit_reinject = 0;
                break;
            }
	    case QEMU_OPTION_enable_nesting: {
		kvm_nested = 1;
		break;
	    }
#if defined(TARGET_I386) || defined(TARGET_X86_64) || defined(TARGET_IA64) || defined(__linux__)
            case QEMU_OPTION_pcidevice:
		if (assigned_devices_index >= MAX_DEV_ASSIGN_CMDLINE) {
                    fprintf(stderr, "Too many assigned devices\n");
                    exit(1);
		}
		assigned_devices[assigned_devices_index] = optarg;
		assigned_devices_index++;
                break;
#endif
#endif
            case QEMU_OPTION_machine:
                olist = qemu_find_opts("machine");
                opts = qemu_opts_parse(olist, optarg, 1);
                if (!opts) {
                    fprintf(stderr, "parse error: %s\n", optarg);
                    exit(1);
                }
                optarg = qemu_opt_get(opts, "type");
                if (optarg) {
                    machine = machine_parse(optarg);
                }
                break;
            case QEMU_OPTION_usb:
                usb_enabled = 1;
                break;
            case QEMU_OPTION_usbdevice:
                usb_enabled = 1;
                add_device_config(DEV_USB, optarg);
                break;
            case QEMU_OPTION_device:
                if (!qemu_opts_parse(&qemu_device_opts, optarg, 1)) {
                    exit(1);
                }
                break;
            case QEMU_OPTION_smp:
                smp_parse(optarg);
                if (smp_cpus < 1) {
                    fprintf(stderr, "Invalid number of CPUs\n");
                    exit(1);
                }
                if (max_cpus < smp_cpus) {
                    fprintf(stderr, "maxcpus must be equal to or greater than "
                            "smp\n");
                    exit(1);
                }
                if (max_cpus > 255) {
                    fprintf(stderr, "Unsupported number of maxcpus\n");
                    exit(1);
                }
                break;
	    case QEMU_OPTION_vnc:
                display_remote++;
		vnc_display = optarg;
		break;
#ifdef TARGET_I386
            case QEMU_OPTION_no_acpi:
                acpi_enabled = 0;
                break;
#if 0 /* Disabled for Red Hat Enterprise Linux */
            case QEMU_OPTION_no_hpet:
                no_hpet = 1;
                break;
#endif
            case QEMU_OPTION_balloon:
                if (balloon_parse(optarg) < 0) {
                    fprintf(stderr, "Unknown -balloon argument %s\n", optarg);
                    exit(1);
                }
                break;
#endif
            case QEMU_OPTION_no_reboot:
                no_reboot = 1;
                break;
            case QEMU_OPTION_no_shutdown:
                no_shutdown = 1;
                break;
            case QEMU_OPTION_show_cursor:
                cursor_hide = 0;
                break;
            case QEMU_OPTION_uuid:
                if(qemu_uuid_parse(optarg, qemu_uuid) < 0) {
                    fprintf(stderr, "Fail to parse UUID string."
                            " Wrong format.\n");
                    exit(1);
                }
                break;
#ifndef _WIN32
	    case QEMU_OPTION_daemonize:
		daemonize = 1;
		break;
#endif
	    case QEMU_OPTION_option_rom:
		if (nb_option_roms >= MAX_OPTION_ROMS) {
		    fprintf(stderr, "Too many option ROMs\n");
		    exit(1);
		}
                opts = qemu_opts_parse(qemu_find_opts("option-rom"), optarg, 1);
                option_rom[nb_option_roms].name = qemu_opt_get(opts, "romfile");
                option_rom[nb_option_roms].bootindex =
                    qemu_opt_get_number(opts, "bootindex", -1);
                if (!option_rom[nb_option_roms].name) {
                    fprintf(stderr, "Option ROM file is not specified\n");
                    exit(1);
                }
		nb_option_roms++;
		break;
#if defined(TARGET_ARM) || defined(TARGET_M68K)
            case QEMU_OPTION_semihosting:
                semihosting_enabled = 1;
                break;
#endif
            case QEMU_OPTION_tdf:
                time_drift_fix = 1;
		break;
            case QEMU_OPTION_kvm_shadow_memory:
                kvm_shadow_memory = (int64_t)atoi(optarg) * 1024 * 1024 / 4096;
                break;
            case QEMU_OPTION_mempath:
		mem_path = optarg;
		break;
            case QEMU_OPTION_disable_KSM:
                disable_KSM = 1;
                break;
#ifdef MAP_POPULATE
            case QEMU_OPTION_mem_prealloc:
                mem_prealloc = 1;
                break;
#endif
            case QEMU_OPTION_name:
                qemu_name = qemu_strdup(optarg);
		 {
		     char *p = strchr(qemu_name, ',');
		     if (p != NULL) {
		        *p++ = 0;
			if (strncmp(p, "process=", 8)) {
			    fprintf(stderr, "Unknown subargument %s to -name", p);
			    exit(1);
			}
			p += 8;
			set_proc_name(p);
		     }	
		 }	
                break;
#if defined(TARGET_SPARC) || defined(TARGET_PPC)
            case QEMU_OPTION_prom_env:
                if (nb_prom_envs >= MAX_PROM_ENVS) {
                    fprintf(stderr, "Too many prom variables\n");
                    exit(1);
                }
                prom_envs[nb_prom_envs] = optarg;
                nb_prom_envs++;
                break;
#endif
#ifdef TARGET_ARM
            case QEMU_OPTION_old_param:
                old_param = 1;
                break;
#endif
            case QEMU_OPTION_clock:
                configure_alarms(optarg);
                break;
            case QEMU_OPTION_startdate:
                configure_rtc_date_offset(optarg, 1);
                break;
            case QEMU_OPTION_rtc:
                opts = qemu_opts_parse(&qemu_rtc_opts, optarg, 0);
                if (!opts) {
                    exit(1);
                }
                configure_rtc(opts);
                break;
            case QEMU_OPTION_tb_size:
                tb_size = strtol(optarg, NULL, 0);
                if (tb_size < 0)
                    tb_size = 0;
                break;
            case QEMU_OPTION_icount:
                use_icount = 1;
                if (strcmp(optarg, "auto") == 0) {
                    icount_time_shift = -1;
                } else {
                    icount_time_shift = strtol(optarg, NULL, 0);
                }
                break;
            case QEMU_OPTION_incoming:
                incoming = optarg;
                break;
            case QEMU_OPTION_nodefaults:
                default_serial = 0;
                default_parallel = 0;
                default_virtcon = 0;
                default_monitor = 0;
                default_vga = 0;
                default_net = 0;
                default_floppy = 0;
                default_cdrom = 0;
                default_sdcard = 0;
                break;
#ifndef _WIN32
            case QEMU_OPTION_chroot:
                chroot_dir = optarg;
                break;
            case QEMU_OPTION_runas:
                run_as = optarg;
                break;
            case QEMU_OPTION_nvram:
                nvram = optarg;
                break;
#endif
#ifdef CONFIG_XEN
            case QEMU_OPTION_xen_domid:
                xen_domid = atoi(optarg);
                break;
            case QEMU_OPTION_xen_create:
                xen_mode = XEN_CREATE;
                break;
            case QEMU_OPTION_xen_attach:
                xen_mode = XEN_ATTACH;
                break;
#endif
            case QEMU_OPTION_spice:
                olist = qemu_find_opts("spice");
                if (!olist) {
                    fprintf(stderr, "spice is not supported by this qemu build.\n");
                    exit(1);
                }
                opts = qemu_opts_parse(olist, optarg, 0);
                if (!opts) {
                    fprintf(stderr, "parse error: %s\n", optarg);
                    exit(1);
                }
                break;
            case QEMU_OPTION_writeconfig:
                {
                    FILE *fp;
                    if (strcmp(optarg, "-") == 0) {
                        fp = stdout;
                    } else {
                        fp = fopen(optarg, "w");
                        if (fp == NULL) {
                            fprintf(stderr, "open %s: %s\n", optarg, strerror(errno));
                            exit(1);
                        }
                    }
                    qemu_config_write(fp);
                    fclose(fp);
                    break;
                }
#ifdef CONFIG_FAKE_MACHINE
            case QEMU_OPTION_fake_machine:
                fake_machine = 1;
                break;
#endif
            case QEMU_OPTION_realtime:
                opts = qemu_opts_parse(qemu_find_opts("realtime"), optarg, 0);
                if (!opts) {
                    exit(1);
                }
                configure_realtime(opts);
                break;
            case QEMU_OPTION_msg:
                opts = qemu_opts_parse(qemu_find_opts("msg"), optarg, 0);
                if (!opts) {
                    exit(1);
                }
                configure_msg(opts);
                break;
            case QEMU_OPTION_object:
                opts = qemu_opts_parse(qemu_find_opts("object"), optarg, 1);
                if (!opts) {
                    exit(1);
                }
                break;
            case QEMU_OPTION_dump_vmstate:
                vmstate_dump_file = fopen(optarg, "w");
                if (vmstate_dump_file == NULL) {
                    fprintf(stderr, "open %s: %s\n", optarg, strerror(errno));
                    exit(1);
                }
                break;
            }
        }
    }
    fips_set_state(true);
    loc_set_none();

    if (qemu_opts_foreach(qemu_find_opts("object"),
                          object_create, NULL, 0) != 0) {
        exit(1);
    }

    /* If no data_dir is specified then try to find it relative to the
       executable path.  */
    if (!data_dir) {
        data_dir = find_datadir(argv[0]);
    }
    /* If all else fails use the install patch specified when building.  */
    if (!data_dir) {
        data_dir = CONFIG_QEMU_SHAREDIR;
    }

    /*
     * Default to max_cpus = smp_cpus, in case the user doesn't
     * specify a max_cpus value.
     */
    if (!max_cpus)
        max_cpus = smp_cpus;

    machine->max_cpus = machine->max_cpus ?: 1; /* Default to UP */
    if (smp_cpus > machine->max_cpus) {
        fprintf(stderr, "Number of SMP cpus requested (%d), exceeds max cpus "
                "supported by machine `%s' (%d)\n", smp_cpus,  machine->name,
                machine->max_cpus);
        exit(1);
    }

    qemu_opts_foreach(&qemu_device_opts, default_driver_check, NULL, 0);
    qemu_opts_foreach(&qemu_global_opts, default_driver_check, NULL, 0);

    if (machine->no_serial) {
        default_serial = 0;
    }
    if (machine->no_parallel) {
        default_parallel = 0;
    }
    if (!machine->use_virtcon) {
        default_virtcon = 0;
    }
    if (machine->no_vga) {
        default_vga = 0;
    }
    if (machine->no_floppy) {
        default_floppy = 0;
    }
    if (machine->no_cdrom) {
        default_cdrom = 0;
    }
    if (machine->no_sdcard) {
        default_sdcard = 0;
    }
    if (machine->no_mixemu) {
        disable_mixemu();
    }

    if (display_type == DT_NOGRAPHIC) {
        if (default_parallel)
            add_device_config(DEV_PARALLEL, "null");
        if (default_serial && default_monitor) {
            add_device_config(DEV_SERIAL, "mon:stdio");
        } else if (default_virtcon && default_monitor) {
            add_device_config(DEV_VIRTCON, "mon:stdio");
        } else {
            if (default_serial)
                add_device_config(DEV_SERIAL, "stdio");
            if (default_virtcon)
                add_device_config(DEV_VIRTCON, "stdio");
            if (default_monitor)
                monitor_parse("stdio", "readline");
        }
    } else {
        if (default_serial)
            add_device_config(DEV_SERIAL, "vc:80Cx24C");
        if (default_parallel)
            add_device_config(DEV_PARALLEL, "vc:80Cx24C");
        if (default_monitor)
            monitor_parse("vc:80Cx24C", "readline");
        if (default_virtcon)
            add_device_config(DEV_VIRTCON, "vc:80Cx24C");
    }
    if (default_vga)
        vga_interface_type = VGA_CIRRUS;
    if (fake_machine) {
        /* HACK: Ideally we'd configure VGA as usual, but this causes
         * several MB of VGA RAM to be allocated, and we can't do the
         * tricks we use elsewhere to just return a single 4k page,
         * because the VGA driver immediately memsets() the entire
         * allocation it requested
         * TODO try to use -vga none instead
         */
        vga_interface_type = VGA_NONE;
    }

    if (qemu_opts_foreach(&qemu_chardev_opts, chardev_init_func, NULL, 1) != 0)
        exit(1);

#ifndef _WIN32
    if (daemonize) {
	pid_t pid;

	if (pipe(fds) == -1)
	    exit(1);

	pid = fork();
	if (pid > 0) {
	    uint8_t status;
	    ssize_t len;

	    close(fds[1]);

	again:
            len = read(fds[0], &status, 1);
            if (len == -1 && (errno == EINTR))
                goto again;

            if (len != 1)
                exit(1);
            else if (status == 1) {
                fprintf(stderr, "Could not acquire pidfile: %s\n", strerror(errno));
                exit(1);
            } else
                exit(0);
	} else if (pid < 0)
            exit(1);

	close(fds[0]);
	qemu_set_cloexec(fds[1]);

	setsid();

	pid = fork();
	if (pid > 0)
	    exit(0);
	else if (pid < 0)
	    exit(1);

	umask(027);

        signal(SIGTSTP, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
    }

    if (pid_file && qemu_create_pidfile(pid_file) != 0) {
        if (daemonize) {
            uint8_t status = 1;
            if (write(fds[1], &status, 1) != 1) {
                perror("daemonize. Writing to pipe\n");
            }
        } else
            fprintf(stderr, "Could not acquire pid file: %s\n", strerror(errno));
        exit(1);
    }
#endif

    if (fake_machine) {
        olist = qemu_find_opts("machine");
        qemu_opts_parse(olist, "accel=tcg", 0);
    }
    configure_accelerator();

    if (qemu_init_main_loop()) {
        fprintf(stderr, "qemu_init_main_loop failed\n");
        exit(1);
    }
    linux_boot = (kernel_filename != NULL);

    if (!linux_boot && *kernel_cmdline != '\0') {
        fprintf(stderr, "-append only allowed with -kernel option\n");
        exit(1);
    }

    if (!linux_boot && initrd_filename != NULL) {
        fprintf(stderr, "-initrd only allowed with -kernel option\n");
        exit(1);
    }

#ifndef _WIN32
    /* Win32 doesn't support line-buffering and requires size >= 2 */
    setvbuf(stdout, NULL, _IOLBF, 0);
#endif

    if (init_timer_alarm() < 0) {
        fprintf(stderr, "could not initialize alarm timer\n");
        exit(1);
    }
    if (use_icount && icount_time_shift < 0) {
        use_icount = 2;
        /* 125MIPS seems a reasonable initial guess at the guest speed.
           It will be corrected fairly quickly anyway.  */
        icount_time_shift = 3;
        init_icount_adjust();
    }

#ifdef _WIN32
    socket_init();
#endif

    /* clean up network at qemu process termination */
    atexit(&net_cleanup);

    if (net_init_clients() < 0) {
        exit(1);
    }

#if 0 /* Disabled for Red Hat Enterprise Linux */
    /* init the bluetooth world */
    if (foreach_device_config(DEV_BT, bt_parse))
        exit(1);
#endif

    /* init the memory */
    if (ram_size == 0)
        ram_size = DEFAULT_RAM_SIZE * 1024 * 1024;

    /* init the dynamic translator */
    cpu_exec_init_all(tb_size * 1024 * 1024);

    bdrv_init_with_whitelist();

    blk_mig_init();

    /* open the virtual block devices */
    if (snapshot)
        qemu_opts_foreach(&qemu_drive_opts, drive_enable_snapshot, NULL, 0);
    if (qemu_opts_foreach(&qemu_drive_opts, drive_init_func, &machine->use_scsi, 1) != 0)
        exit(1);

    default_drive(default_cdrom, snapshot, machine->use_scsi,
                  IF_DEFAULT, 2, CDROM_OPTS);
    default_drive(default_floppy, snapshot, machine->use_scsi,
                  IF_FLOPPY, 0, FD_OPTS);
    default_drive(default_sdcard, snapshot, machine->use_scsi,
                  IF_SD, 0, SD_OPTS);

    vmstate_register(NULL, 0, &vmstate_timers ,&timers_state);
    register_savevm_live(NULL, "ram", 0, 4, NULL, ram_save_live, NULL,
                         ram_load, NULL);

    if (nb_numa_nodes > 0) {
        int i;

        if (nb_numa_nodes > smp_cpus) {
            nb_numa_nodes = smp_cpus;
        }

        /* If no memory size if given for any node, assume the default case
         * and distribute the available memory equally across all nodes
         */
        for (i = 0; i < nb_numa_nodes; i++) {
            if (node_mem[i] != 0)
                break;
        }
        if (i == nb_numa_nodes) {
            uint64_t usedmem = 0;

            /* On Linux, the each node's border has to be 8MB aligned,
             * the final node gets the rest.
             */
            for (i = 0; i < nb_numa_nodes - 1; i++) {
                node_mem[i] = (ram_size / nb_numa_nodes) & ~((1 << 23UL) - 1);
                usedmem += node_mem[i];
            }
            node_mem[i] = ram_size - usedmem;
        }

        for (i = 0; i < nb_numa_nodes; i++) {
            if (!bitmap_empty(node_cpumask[i], MAX_CPUMASK_BITS)) {
                break;
            }
        }
        /* assigning the VCPUs round-robin is easier to implement, guest OSes
         * must cope with this anyway, because there are BIOSes out there in
         * real machines which also use this scheme.
         */
        if (i == nb_numa_nodes) {
            for (i = 0; i < smp_cpus; i++) {
                set_bit(i, node_cpumask[i % nb_numa_nodes]);
            }
        }
    }

    if (foreach_device_config(DEV_SERIAL, serial_parse) < 0)
        exit(1);
    if (foreach_device_config(DEV_PARALLEL, parallel_parse) < 0)
        exit(1);
    if (foreach_device_config(DEV_VIRTCON, virtcon_parse) < 0)
        exit(1);
    if (foreach_device_config(DEV_DEBUGCON, debugcon_parse) < 0)
        exit(1);

    module_call_init(MODULE_INIT_DEVICE);

    if (qemu_opts_foreach(&qemu_device_opts, device_help_func, NULL, 0) != 0)
        exit(0);

    if (watchdog) {
        i = select_watchdog(watchdog);
        if (i > 0)
            exit (i == 1 ? 1 : 0);
    }

    if (machine->compat_props) {
        qdev_prop_register_global_list(machine->compat_props);
    }
    qemu_add_globals();

    machine->init(ram_size, boot_devices,
                  kernel_filename, kernel_cmdline, initrd_filename, cpu_model);


    for (env = first_cpu; env != NULL; env = env->next_cpu) {
        for (i = 0; i < nb_numa_nodes; i++) {
            if (test_bit(env->cpu_index, node_cpumask[i])) {
                env->numa_node = i;
            }
        }
    }

    current_machine = machine;

    /* init USB devices */
    if (usb_enabled) {
        if (foreach_device_config(DEV_USB, usb_parse) < 0)
            exit(1);
    }

    /* init generic devices */
    if (qemu_opts_foreach(&qemu_device_opts, device_init_func, NULL, 1) != 0)
        exit(1);

    net_check_clients();

    if (!display_state)
        dumb_display_init();
    /* just use the first displaystate for the moment */
    ds = display_state;

    if (using_spice)
        display_remote++;
    if (display_type == DT_DEFAULT && !display_remote) {
#if defined(CONFIG_SDL) || defined(CONFIG_COCOA)
        display_type = DT_SDL;
#else
        vnc_display = "localhost:0,to=99";
        show_vnc_port = 1;
#endif
    }
        

    /* init local displays */
    switch (display_type) {
    case DT_NOGRAPHIC:
        break;
#if defined(CONFIG_CURSES)
    case DT_CURSES:
        curses_display_init(ds, full_screen);
        break;
#endif
#if defined(CONFIG_SDL)
    case DT_SDL:
        sdl_display_init(ds, full_screen, no_frame);
        break;
#elif defined(CONFIG_COCOA)
    case DT_SDL:
        cocoa_display_init(ds, full_screen);
        break;
#endif
    default:
        break;
    }

#ifndef _WIN32
    /* must be after terminal init, SDL library changes signal handlers */
    sighandler_setup();
#endif

    /* init remote displays */
    if (vnc_display) {
        Error *local_err = NULL;
        vnc_display_init(ds);
        vnc_display_open(ds, vnc_display, &local_err);
        if (local_err != NULL) {
            error_report("Failed to start VNC server on `%s': %s",
                         vnc_display, error_get_pretty(local_err));
            error_free(local_err);
            exit(1);
        }

        if (show_vnc_port) {
            printf("VNC server running on `%s'\n", vnc_display_local_addr(ds));
        }
    }
#ifdef CONFIG_SPICE
    if (using_spice && !qxl_enabled) {
        qemu_spice_display_init(ds);
    }
#endif

    /* display setup */
    dpy_resize(ds);
    dcl = ds->listeners;
    while (dcl != NULL) {
        if (dcl->dpy_refresh != NULL) {
            ds->gui_timer = qemu_new_timer(rt_clock, gui_update, ds);
            qemu_mod_timer(ds->gui_timer, qemu_get_clock(rt_clock));
        }
        dcl = dcl->next;
    }
    text_consoles_set_display(ds);

    if (qemu_opts_foreach(&qemu_mon_opts, mon_init_func, NULL, 1) != 0)
        exit(1);

    if (gdbstub_dev && gdbserver_start(gdbstub_dev) < 0) {
        fprintf(stderr, "qemu: could not open gdbserver on device '%s'\n",
                gdbstub_dev);
        exit(1);
    }

    qdev_machine_creation_done();

    if (rom_load_all() != 0) {
        fprintf(stderr, "rom loading failed\n");
        exit(1);
    }

    qemu_run_machine_init_done_notifiers();

    qemu_system_reset(VMRESET_SILENT);
    if (loadvm) {
        if (load_vmstate(loadvm) < 0) {
            autostart = 0;
        }
    }

    if (vmstate_dump_file) {
        /* dump and exit */
        dump_vmstate_json_to_file(vmstate_dump_file, current_machine->name);
        return 0;
    }

    if (incoming) {
        runstate_set(RUN_STATE_INMIGRATE);
        Error *errp = NULL;
        int ret = qemu_start_incoming_migration(incoming, &errp);
        if (ret < 0) {
            if (error_is_set(&errp)) {
                error_report("Migrate: %s", error_get_pretty(errp));
                error_free(errp);
            }
            fprintf(stderr, "Migration failed. Exit code %s(%d), exiting.\n",
                    incoming, ret);
            exit(ret);
        }
    } else if (autostart) {
        vm_start();
    } else {
        runstate_set(RUN_STATE_PRELAUNCH);
    }

#ifndef _WIN32
    if (daemonize) {
	uint8_t status = 0;
	ssize_t len;

    again1:
	len = write(fds[1], &status, 1);
	if (len == -1 && (errno == EINTR))
	    goto again1;

	if (len != 1)
	    exit(1);

        if (chdir("/")) {
            perror("not able to chdir to /");
            exit(1);
        }
	TFR(fd = qemu_open("/dev/null", O_RDWR));
	if (fd == -1)
	    exit(1);
    }

    if (run_as) {
        pwd = getpwnam(run_as);
        if (!pwd) {
            fprintf(stderr, "User \"%s\" doesn't exist\n", run_as);
            exit(1);
        }
    }

    if (chroot_dir) {
        if (chroot(chroot_dir) < 0) {
            fprintf(stderr, "chroot failed\n");
            exit(1);
        }
        if (chdir("/")) {
            perror("not able to chdir to /");
            exit(1);
        }
    }

    if (run_as) {
        if (setgid(pwd->pw_gid) < 0) {
            fprintf(stderr, "Failed to setgid(%d)\n", pwd->pw_gid);
            exit(1);
        }
        if (initgroups(pwd->pw_name, pwd->pw_gid) < 0) {
            fprintf(stderr, "Failed to initgroups(\"%s\", %d)\n",
                    pwd->pw_name, pwd->pw_gid);
            exit(1);
        }
        if (setuid(pwd->pw_uid) < 0) {
            fprintf(stderr, "Failed to setuid(%d)\n", pwd->pw_uid);
            exit(1);
        }
        if (setuid(0) != -1) {
            fprintf(stderr, "Dropping privileges failed\n");
            exit(1);
        }
    }

    if (daemonize) {
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);

        close(fd);
    }
#endif

    main_loop();

    return 0;
}
