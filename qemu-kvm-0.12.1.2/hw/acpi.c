/*
 * ACPI implementation
 *
 * Copyright (c) 2006 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>
 */
#include "hw.h"
#include "pc.h"
#include "pci.h"
#include "qemu-timer.h"
#include "sysemu.h"
#include "i2c.h"
#include "smbus.h"
#include "kvm.h"
#include "qemu-kvm.h"
#include "string.h"
#include "ioport.h"
#include "fw_cfg.h"
#include "monitor.h"

//#define DEBUG

/* i82731AB (PIIX4) compatible power management function */
#define PM_FREQ 3579545

#define GPE_BASE 0xafe0
#define PROC_BASE 0xaf00
#define PCI_UP_BASE 0xae00
#define PCI_DOWN_BASE 0xae04
#define PCI_EJ_BASE 0xae08
#define PCI_RMV_BASE 0xae0c

#define PIIX4_PCI_HOTPLUG_STATUS 2
#define PIIX4_CPU_HOTPLUG_STATUS 4

struct gpe_regs {
    uint16_t sts; /* status */
    uint16_t en;  /* enabled */
    uint8_t cpus_sts[32];
};

struct pci_status {
    uint32_t up; /* deprecated, maintained for migration compatibility */
    uint32_t down;
};

typedef struct PIIX4PMState {
    PCIDevice dev;
    uint16_t pmsts;
    uint16_t pmen;
    uint16_t pmcntrl;
    uint8_t apmc;
    uint8_t apms;
    QEMUTimer *tmr_timer;
    int64_t tmr_overflow_time;
    i2c_bus *smbus;
    uint8_t smb_stat;
    uint8_t smb_ctl;
    uint8_t smb_cmd;
    uint8_t smb_addr;
    uint8_t smb_data0;
    uint8_t smb_data1;
    uint8_t smb_data[32];
    uint8_t smb_index;
    qemu_irq irq;

    uint32_t smb_io_base;
    Notifier machine_ready;
    Notifier wakeup;

    /* for pci hotplug */
    struct gpe_regs gpe;
    struct pci_status pci0_status;
    uint32_t pci0_hotplug_enable;
    uint32_t pci0_slot_device_present;

    uint8_t disable_s3;
    uint8_t disable_s4;
    uint8_t s4_val;
} PIIX4PMState;

#define RSM_STS (1 << 15)
#define RTC_STS (1 << 10)
#define PWRBTN_STS (1 << 8)
#define TMROF_STS (1 << 0)
#define RTC_EN (1 << 10)
#define PWRBTN_EN (1 << 8)
#define GBL_EN (1 << 5)
#define TMROF_EN (1 << 0)

#define SCI_EN (1 << 0)

#define SUS_EN (1 << 13)

#define ACPI_ENABLE 0xf1
#define ACPI_DISABLE 0xf0

#define SMBHSTSTS 0x00
#define SMBHSTCNT 0x02
#define SMBHSTCMD 0x03
#define SMBHSTADD 0x04
#define SMBHSTDAT0 0x05
#define SMBHSTDAT1 0x06
#define SMBBLKDAT 0x07

static PIIX4PMState *pm_state;

static uint32_t get_pmtmr(PIIX4PMState *s)
{
    uint32_t d;
    d = muldiv64(qemu_get_clock(vm_clock), PM_FREQ, get_ticks_per_sec());
    return d & 0xffffff;
}

static int get_pmsts(PIIX4PMState *s)
{
    int64_t d;

    d = muldiv64(qemu_get_clock(vm_clock), PM_FREQ, get_ticks_per_sec());
    if (d >= s->tmr_overflow_time)
        s->pmsts |= TMROF_EN;
    return s->pmsts;
}

static void pm_update_sci(PIIX4PMState *s)
{
    int sci_level, pmsts;
    int64_t expire_time;

    pmsts = get_pmsts(s);
    sci_level = (((pmsts & s->pmen) &
                  (RTC_EN | PWRBTN_EN | GBL_EN | TMROF_EN)) != 0) ||
        (((s->gpe.sts & s->gpe.en) &
          (PIIX4_CPU_HOTPLUG_STATUS | PIIX4_PCI_HOTPLUG_STATUS)) != 0);

    qemu_set_irq(s->irq, sci_level);
    /* schedule a timer interruption if needed */
    if ((s->pmen & TMROF_EN) && !(pmsts & TMROF_EN)) {
        expire_time = muldiv64(s->tmr_overflow_time, get_ticks_per_sec(), PM_FREQ);
        qemu_mod_timer(s->tmr_timer, expire_time);
    } else {
        qemu_del_timer(s->tmr_timer);
    }
}

static void pm_tmr_timer(void *opaque)
{
    PIIX4PMState *s = opaque;
    qemu_system_wakeup_request(QEMU_WAKEUP_REASON_PMTIMER);
    pm_update_sci(s);
}

static void acpi_notify_wakeup(Notifier *notifier, void *data)
{
    PIIX4PMState *s = container_of(notifier, PIIX4PMState, wakeup);
    WakeupReason *reason = data;

    switch (*reason) {
    case QEMU_WAKEUP_REASON_RTC:
        s->pmsts |= (RSM_STS | RTC_STS);
        break;
    case QEMU_WAKEUP_REASON_PMTIMER:
        s->pmsts |= (RSM_STS | TMROF_STS);
        break;
    case QEMU_WAKEUP_REASON_OTHER:
    default:
        /* RSM_STS should be set on resume. Pretend that resume
           was caused by power button */
        s->pmsts |= (RSM_STS | PWRBTN_STS);
        break;
    }
}

static void pm_ioport_writew(void *opaque, uint32_t addr, uint32_t val)
{
    PIIX4PMState *s = opaque;
    addr &= 0x3f;
    switch(addr) {
    case 0x00:
        {
            int64_t d;
            int pmsts;
            pmsts = get_pmsts(s);
            if (pmsts & val & TMROF_EN) {
                /* if TMRSTS is reset, then compute the new overflow time */
                d = muldiv64(qemu_get_clock(vm_clock), PM_FREQ,
                             get_ticks_per_sec());
                s->tmr_overflow_time = (d + 0x800000LL) & ~0x7fffffLL;
            }
            s->pmsts &= ~val;
            pm_update_sci(s);
        }
        break;
    case 0x02:
        s->pmen = val;
        qemu_system_wakeup_enable(QEMU_WAKEUP_REASON_RTC, val & RTC_EN);
        qemu_system_wakeup_enable(QEMU_WAKEUP_REASON_PMTIMER, val & TMROF_EN);
        pm_update_sci(s);
        break;
    case 0x04:
        {
            int sus_typ;
            s->pmcntrl = val & ~(SUS_EN);
            if (val & SUS_EN) {
                /* change suspend type */
                sus_typ = (val >> 10) & 7;
                switch(sus_typ) {
                case 0: /* soft power off */
                    qemu_system_shutdown_request();
                    break;
                case 1:
                    qemu_system_suspend_request();
                    break;
                default:
                    if (sus_typ == s->s4_val) { /* S4 request */
                        monitor_protocol_event(QEVENT_SUSPEND_DISK, NULL);
                        qemu_system_shutdown_request();
                    }
                    break;
                }
            }
        }
        break;
    default:
        break;
    }
#ifdef DEBUG
    printf("PM writew port=0x%04x val=0x%04x\n", addr, val);
#endif
}

static uint32_t pm_ioport_readw(void *opaque, uint32_t addr)
{
    PIIX4PMState *s = opaque;
    uint32_t val;

    addr &= 0x3f;
    switch(addr) {
    case 0x00:
        val = get_pmsts(s);
        break;
    case 0x02:
        val = s->pmen;
        break;
    case 0x04:
        val = s->pmcntrl;
        break;
    default:
        val = 0;
        break;
    }
#ifdef DEBUG
    printf("PM readw port=0x%04x val=0x%04x\n", addr, val);
#endif
    return val;
}

static void pm_ioport_writel(void *opaque, uint32_t addr, uint32_t val)
{
    //    PIIX4PMState *s = opaque;
#ifdef DEBUG
    addr &= 0x3f;
    printf("PM writel port=0x%04x val=0x%08x\n", addr, val);
#endif
}

static uint32_t pm_ioport_readl(void *opaque, uint32_t addr)
{
    PIIX4PMState *s = opaque;
    uint32_t val;

    addr &= 0x3f;
    switch(addr) {
    case 0x08:
        val = get_pmtmr(s);
        break;
    default:
        val = 0;
        break;
    }
#ifdef DEBUG
    printf("PM readl port=0x%04x val=0x%08x\n", addr, val);
#endif
    return val;
}

static void pm_smi_writeb(void *opaque, uint32_t addr, uint32_t val)
{
    PIIX4PMState *s = opaque;
    addr &= 1;
#ifdef DEBUG
    printf("pm_smi_writeb addr=0x%x val=0x%02x\n", addr, val);
#endif
    if (addr == 0) {
        s->apmc = val;

        /* ACPI specs 3.0, 4.7.2.5 */
        if (val == ACPI_ENABLE) {
            s->pmcntrl |= SCI_EN;
        } else if (val == ACPI_DISABLE) {
            s->pmcntrl &= ~SCI_EN;
        }

        if (s->dev.config[0x5b] & (1 << 1)) {
            cpu_interrupt(first_cpu, CPU_INTERRUPT_SMI);
        }
    } else {
        s->apms = val;
    }
}

static uint32_t pm_smi_readb(void *opaque, uint32_t addr)
{
    PIIX4PMState *s = opaque;
    uint32_t val;

    addr &= 1;
    if (addr == 0) {
        val = s->apmc;
    } else {
        val = s->apms;
    }
#ifdef DEBUG
    printf("pm_smi_readb addr=0x%x val=0x%02x\n", addr, val);
#endif
    return val;
}

static void smb_transaction(PIIX4PMState *s)
{
    uint8_t prot = (s->smb_ctl >> 2) & 0x07;
    uint8_t read = s->smb_addr & 0x01;
    uint8_t cmd = s->smb_cmd;
    uint8_t addr = s->smb_addr >> 1;
    i2c_bus *bus = s->smbus;

#ifdef DEBUG
    printf("SMBus trans addr=0x%02x prot=0x%02x\n", addr, prot);
#endif
    switch(prot) {
    case 0x0:
        smbus_quick_command(bus, addr, read);
        break;
    case 0x1:
        if (read) {
            s->smb_data0 = smbus_receive_byte(bus, addr);
        } else {
            smbus_send_byte(bus, addr, cmd);
        }
        break;
    case 0x2:
        if (read) {
            s->smb_data0 = smbus_read_byte(bus, addr, cmd);
        } else {
            smbus_write_byte(bus, addr, cmd, s->smb_data0);
        }
        break;
    case 0x3:
        if (read) {
            uint16_t val;
            val = smbus_read_word(bus, addr, cmd);
            s->smb_data0 = val;
            s->smb_data1 = val >> 8;
        } else {
            smbus_write_word(bus, addr, cmd, (s->smb_data1 << 8) | s->smb_data0);
        }
        break;
    case 0x5:
        if (read) {
            s->smb_data0 = smbus_read_block(bus, addr, cmd, s->smb_data);
        } else {
            smbus_write_block(bus, addr, cmd, s->smb_data, s->smb_data0);
        }
        break;
    default:
        goto error;
    }
    return;

  error:
    s->smb_stat |= 0x04;
}

static void smb_ioport_writeb(void *opaque, uint32_t addr, uint32_t val)
{
    PIIX4PMState *s = opaque;
    addr &= 0x3f;
#ifdef DEBUG
    printf("SMB writeb port=0x%04x val=0x%02x\n", addr, val);
#endif
    switch(addr) {
    case SMBHSTSTS:
        s->smb_stat = 0;
        s->smb_index = 0;
        break;
    case SMBHSTCNT:
        s->smb_ctl = val;
        if (val & 0x40)
            smb_transaction(s);
        break;
    case SMBHSTCMD:
        s->smb_cmd = val;
        break;
    case SMBHSTADD:
        s->smb_addr = val;
        break;
    case SMBHSTDAT0:
        s->smb_data0 = val;
        break;
    case SMBHSTDAT1:
        s->smb_data1 = val;
        break;
    case SMBBLKDAT:
        s->smb_data[s->smb_index++] = val;
        if (s->smb_index > 31)
            s->smb_index = 0;
        break;
    default:
        break;
    }
}

static uint32_t smb_ioport_readb(void *opaque, uint32_t addr)
{
    PIIX4PMState *s = opaque;
    uint32_t val;

    addr &= 0x3f;
    switch(addr) {
    case SMBHSTSTS:
        val = s->smb_stat;
        break;
    case SMBHSTCNT:
        s->smb_index = 0;
        val = s->smb_ctl & 0x1f;
        break;
    case SMBHSTCMD:
        val = s->smb_cmd;
        break;
    case SMBHSTADD:
        val = s->smb_addr;
        break;
    case SMBHSTDAT0:
        val = s->smb_data0;
        break;
    case SMBHSTDAT1:
        val = s->smb_data1;
        break;
    case SMBBLKDAT:
        val = s->smb_data[s->smb_index++];
        if (s->smb_index > 31)
            s->smb_index = 0;
        break;
    default:
        val = 0;
        break;
    }
#ifdef DEBUG
    printf("SMB readb port=0x%04x val=0x%02x\n", addr, val);
#endif
    return val;
}

static void pm_io_space_update(PIIX4PMState *s)
{
    uint32_t pm_io_base;

    if (s->dev.config[0x80] & 1) {
        pm_io_base = le32_to_cpu(*(uint32_t *)(s->dev.config + 0x40));
        pm_io_base &= 0xffc0;

        /* XXX: need to improve memory and ioport allocation */
#if defined(DEBUG)
        printf("PM: mapping to 0x%x\n", pm_io_base);
#endif
        register_ioport_write(pm_io_base, 64, 2, pm_ioport_writew, s);
        register_ioport_read(pm_io_base, 64, 2, pm_ioport_readw, s);
        register_ioport_write(pm_io_base, 64, 4, pm_ioport_writel, s);
        register_ioport_read(pm_io_base, 64, 4, pm_ioport_readl, s);
    }
}

static void pm_write_config(PCIDevice *d,
                            uint32_t address, uint32_t val, int len)
{
    pci_default_write_config(d, address, val, len);
    if (address == 0x80)
        pm_io_space_update((PIIX4PMState *)d);
}

static void vmstate_pci_status_pre_save(void *opaque)
{
    struct pci_status *pci0_status = opaque;
    PIIX4PMState *s = container_of(pci0_status, PIIX4PMState, pci0_status);

    /* We no longer track up, so build a safe value for migrating
     * to a version that still does... of course these might get lost
     * by an old buggy implementation, but we try. */
    pci0_status->up = s->pci0_slot_device_present & s->pci0_hotplug_enable;
}

static int vmstate_acpi_post_load(void *opaque, int version_id)
{
    PIIX4PMState *s = opaque;

    pm_io_space_update(s);
    return 0;
}

static const VMStateDescription vmstate_gpe = {
    .name = "gpe",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT16(sts, struct gpe_regs),
        VMSTATE_UINT16(en, struct gpe_regs),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_pci_status = {
    .name = "pci_status",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .pre_save = vmstate_pci_status_pre_save,
    .fields      = (VMStateField []) {
        VMSTATE_UINT32(up, struct pci_status),
        VMSTATE_UINT32(down, struct pci_status),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_acpi = {
    .name = "piix4_pm",
    .version_id = 2,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .post_load = vmstate_acpi_post_load,
    .fields      = (VMStateField []) {
        VMSTATE_PCI_DEVICE(dev, PIIX4PMState),
        VMSTATE_UINT16(pmsts, PIIX4PMState),
        VMSTATE_UINT16(pmen, PIIX4PMState),
        VMSTATE_UINT16(pmcntrl, PIIX4PMState),
        VMSTATE_UINT8(apmc, PIIX4PMState),
        VMSTATE_UINT8(apms, PIIX4PMState),
        VMSTATE_TIMER(tmr_timer, PIIX4PMState),
        VMSTATE_INT64(tmr_overflow_time, PIIX4PMState),
        VMSTATE_STRUCT(gpe, PIIX4PMState, 2, vmstate_gpe, struct gpe_regs),
        VMSTATE_STRUCT(pci0_status, PIIX4PMState, 2, vmstate_pci_status,
                       struct pci_status),
        VMSTATE_END_OF_LIST()
    }
};

static void acpi_piix_eject_slot(PIIX4PMState *s, unsigned slots)
{
    DeviceState *qdev, *next;
    BusState *bus = qdev_get_parent_bus(&s->dev.qdev);
    int slot = ffs(slots) - 1;
    bool slot_free = true;

    /* Mark request as complete */
    s->pci0_status.down &= ~(1U << slot);

    QTAILQ_FOREACH_SAFE(qdev, &bus->children, sibling, next) {
        PCIDevice *dev = DO_UPCAST(PCIDevice, qdev, qdev);
        PCIDeviceInfo *info = container_of(qdev->info, PCIDeviceInfo, qdev);
        if (PCI_SLOT(dev->devfn) == slot) {
            if (info->no_hotplug) {
                slot_free = false;
            } else {
                qdev_free(qdev);
            }
        }
    }
    if (slot_free) {
        s->pci0_slot_device_present &= ~(1U << slot);
    }

}

static void piix4_update_hotplug(PIIX4PMState *s)
{
    PCIDevice *dev = &s->dev;
    BusState *bus = qdev_get_parent_bus(&dev->qdev);
    DeviceState *qdev, *next;

    /* Execute any pending removes during reset */
    while (s->pci0_status.down) {
        acpi_piix_eject_slot(s, s->pci0_status.down);
    }

    s->pci0_hotplug_enable = ~0;
    s->pci0_slot_device_present = 0;

    QTAILQ_FOREACH_SAFE(qdev, &bus->children, sibling, next) {
        PCIDeviceInfo *info = container_of(qdev->info, PCIDeviceInfo, qdev);
        PCIDevice *pdev = DO_UPCAST(PCIDevice, qdev, qdev);
        int slot = PCI_SLOT(pdev->devfn);

        if (info->no_hotplug) {
            s->pci0_hotplug_enable &= ~(1U << slot);
        }

        s->pci0_slot_device_present |= (1U << slot);
    }
}

static void piix4_reset(void *opaque)
{
    PIIX4PMState *s = opaque;
    uint8_t *pci_conf = s->dev.config;

    pci_conf[0x58] = 0;
    pci_conf[0x59] = 0;
    pci_conf[0x5a] = 0;
    pci_conf[0x5b] = 0;

    pci_conf[0x40] = 0x01; /* PM io base read only bit */
    pci_conf[0x80] = 0;

    if (kvm_enabled()) {
        /* Mark SMM as already inited (until KVM supports SMM). */
        pci_conf[0x5B] = 0x02;
    }
    piix4_update_hotplug(s);
}

static void piix4_powerdown(void *opaque, int irq, int power_failing)
{
#if defined(TARGET_I386)
    PIIX4PMState *s = opaque;

    if (!s) {
        qemu_system_shutdown_request();
    } else if (s->pmen & PWRBTN_EN) {
        s->pmsts |= PWRBTN_EN;
        pm_update_sci(s);
    }
#endif
}

static void piix4_pm_machine_ready(struct Notifier* n, void *data)
{
    PIIX4PMState *s = container_of(n, PIIX4PMState, machine_ready);
    uint8_t *pci_conf;

    pci_conf = s->dev.config;
    pci_conf[0x5f] = (isa_is_ioport_assigned(0x378) ? 0x80 : 0) | 0x10;
    pci_conf[0x63] = 0x60;
    pci_conf[0x67] = (isa_is_ioport_assigned(0x3f8) ? 0x08 : 0) |
	(isa_is_ioport_assigned(0x2f8) ? 0x90 : 0);

}

static int piix4_pm_initfn(PCIDevice *dev)
{
    PIIX4PMState *s = DO_UPCAST(PIIX4PMState, dev, dev);
    uint8_t *pci_conf;

    pm_state = s;
    pci_conf = s->dev.config;
    pci_config_set_vendor_id(pci_conf, PCI_VENDOR_ID_INTEL);
    pci_config_set_device_id(pci_conf, PCI_DEVICE_ID_INTEL_82371AB_3);
    pci_conf[0x06] = 0x80;
    pci_conf[0x07] = 0x02;
    pci_conf[0x08] = 0x03; // revision number
    pci_conf[0x09] = 0x00;
    pci_config_set_class(pci_conf, PCI_CLASS_BRIDGE_OTHER);
    pci_conf[PCI_HEADER_TYPE] = PCI_HEADER_TYPE_NORMAL; // header_type
    pci_conf[0x3d] = 0x01; // interrupt pin 1

#if defined(TARGET_IA64)
    pci_conf[0x40] = 0x41; /* PM io base read only bit */
    pci_conf[0x41] = 0x1f;
    pm_write_config(s, 0x80, 0x01, 1); /*Set default pm_io_base 0x1f40*/
    s->pmcntrl = SCI_EN;
#endif

    register_ioport_write(0xb2, 2, 1, pm_smi_writeb, s);
    register_ioport_read(0xb2, 2, 1, pm_smi_readb, s);

    if (kvm_enabled()) {
        /* Mark SMM as already inited to prevent SMM from running.  KVM does not
         * support SMM mode. */
        pci_conf[0x5B] = 0x02;
    }

    /* XXX: which specification is used ? The i82731AB has different
       mappings */
    pci_conf[0x90] = s->smb_io_base | 1;
    pci_conf[0x91] = s->smb_io_base >> 8;
    pci_conf[0xd2] = 0x09;
    register_ioport_write(s->smb_io_base, 64, 1, smb_ioport_writeb, s);
    register_ioport_read(s->smb_io_base, 64, 1, smb_ioport_readb, s);

    s->tmr_timer = qemu_new_timer(vm_clock, pm_tmr_timer, s);

    qemu_system_powerdown = *qemu_allocate_irqs(piix4_powerdown, s, 1);

    /* RHEL - To maintain migration compatibility, don't pass a device */
    vmstate_register(NULL, 0, &vmstate_acpi, s);

    s->smbus = i2c_init_bus(NULL, "i2c");
    s->machine_ready.notify = piix4_pm_machine_ready;
    qemu_add_machine_init_done_notifier(&s->machine_ready);
    s->wakeup.notify = acpi_notify_wakeup;
    qemu_register_wakeup_notifier(&s->wakeup);
    qemu_register_reset(piix4_reset, s);

    return 0;
}

i2c_bus *piix4_pm_init(PCIBus *bus, int devfn, uint32_t smb_io_base,
                       qemu_irq sci_irq, void *fw_cfg)
{
    PCIDevice *dev;
    PIIX4PMState *s;

    dev = pci_create(bus, devfn, "PIIX4_PM");
    qdev_prop_set_uint32(&dev->qdev, "smb_io_base", smb_io_base);

    s = DO_UPCAST(PIIX4PMState, dev, dev);
    s->irq = sci_irq;

    qdev_init_nofail(&dev->qdev);

    if (fw_cfg) {
        uint8_t suspend[6] = {128, 0, 0, 129, 128, 128};
        suspend[3] = 1 | ((!s->disable_s3) << 7);
        suspend[4] = s->s4_val | ((!s->disable_s4) << 7);

        fw_cfg_add_file(fw_cfg, "etc/system-states", g_memdup(suspend, 6), 6);
    }

    return s->smbus;
}

static PCIDeviceInfo piix4_pm_info = {
    .qdev.name          = "PIIX4_PM",
    .qdev.desc          = "PM",
    .qdev.size          = sizeof(PIIX4PMState),
    .qdev.no_user       = 1,
    .no_hotplug         = 1,
    .init               = piix4_pm_initfn,
    .config_write       = pm_write_config,
    .qdev.props         = (Property[]) {
        DEFINE_PROP_UINT32("smb_io_base", PIIX4PMState, smb_io_base, 0),
        DEFINE_PROP_UINT8("disable_s3", PIIX4PMState, disable_s3, 1),
        DEFINE_PROP_UINT8("disable_s4", PIIX4PMState, disable_s4, 1),
        DEFINE_PROP_UINT8("s4_val", PIIX4PMState, s4_val, 2),
        DEFINE_PROP_END_OF_LIST(),
    }
};

static void piix4_pm_register(void)
{
    pci_qdev_register(&piix4_pm_info);
}

device_init(piix4_pm_register);

static uint32_t gpe_read_val(uint16_t val, uint32_t addr)
{
    if (addr & 1)
        return (val >> 8) & 0xff;
    return val & 0xff;
}

static uint32_t gpe_readb(void *opaque, uint32_t addr)
{
    uint32_t val = 0;
    struct gpe_regs *g = opaque;
    switch (addr) {
        case PROC_BASE ... PROC_BASE+31:
            val = g->cpus_sts[addr - PROC_BASE];
            break;

        case GPE_BASE:
        case GPE_BASE + 1:
            val = gpe_read_val(g->sts, addr);
            break;
        case GPE_BASE + 2:
        case GPE_BASE + 3:
            val = gpe_read_val(g->en, addr);
            break;
        default:
            break;
    }

#if defined(DEBUG)
    printf("gpe read %x == %x\n", addr, val);
#endif
    return val;
}

static void gpe_write_val(uint16_t *cur, int addr, uint32_t val)
{
    if (addr & 1)
        *cur = (*cur & 0xff) | (val << 8);
    else
        *cur = (*cur & 0xff00) | (val & 0xff);
}

static void gpe_reset_val(uint16_t *cur, int addr, uint32_t val)
{
    uint16_t x1, x0 = val & 0xff;
    int shift = (addr & 1) ? 8 : 0;

    x1 = (*cur >> shift) & 0xff;

    x1 = x1 & ~x0;

    *cur = (*cur & (0xff << (8 - shift))) | (x1 << shift);
}

static void gpe_writeb(void *opaque, uint32_t addr, uint32_t val)
{
    struct gpe_regs *g = opaque;
    switch (addr) {
        case PROC_BASE ... PROC_BASE + 31:
            /* don't allow to change cpus_sts from inside a guest */
            break;

        case GPE_BASE:
        case GPE_BASE + 1:
            gpe_reset_val(&g->sts, addr, val);
            break;
        case GPE_BASE + 2:
        case GPE_BASE + 3:
            gpe_write_val(&g->en, addr, val);
            break;
        default:
            break;
    }

    pm_update_sci(pm_state);

#if defined(DEBUG)
    printf("gpe write %x <== %d\n", addr, val);
#endif
}

static uint32_t pci_up_read(void *opaque, uint32_t addr)
{
    PIIX4PMState *s = opaque;
    uint32_t val;

    /* Manufacture an "up" value to cause a device check on any hotplug
     * slot with a device.  Extra device checks are harmless. */
    val = s->pci0_slot_device_present & s->pci0_hotplug_enable;

#if defined(DEBUG)
    printf("pci_up_read %x\n", val);
#endif
    return val;
}

static uint32_t pci_down_read(void *opaque, uint32_t addr)
{
    PIIX4PMState *s = opaque;
    uint32_t val = s->pci0_status.down;

#if defined(DEBUG)
    printf("pci_down_read %x\n", val);
#endif
    return val;
}

static uint32_t pci_features_read(void *opaque, uint32_t addr)
{
    /* No feature defined yet */
#if defined(DEBUG)
    printf("pci_features_read %x\n", 0);
#endif
    return 0;
}

static void pciej_write(void *opaque, uint32_t addr, uint32_t val)
{
    acpi_piix_eject_slot(opaque, val);

#if defined(DEBUG)
    printf("pciej write %x <== %d\n", addr, val);
#endif
}

static uint32_t pcirmv_read(void *opaque, uint32_t addr)
{
    PIIX4PMState *s = opaque;

    return s->pci0_hotplug_enable;
}

static const char *model;

static int piix4_device_hotplug(PCIDevice *dev, int state);

void piix4_acpi_system_hot_add_init(PCIBus *bus, const char *cpu_model)
{
    int cpu_index;
    struct gpe_regs *gpe = &pm_state->gpe;

    memset(gpe->cpus_sts, 0, sizeof gpe->cpus_sts);
    for (cpu_index = 0; cpu_index < smp_cpus; ++cpu_index) {
        uint32_t apic_id;

        apic_id = apic_id_for_cpu(cpu_index);
        if (apic_id < sizeof gpe->cpus_sts * 8) {
            gpe->cpus_sts[apic_id / 8] |= (1 << (apic_id % 8));
        }
    }

    register_ioport_write(GPE_BASE, 4, 1, gpe_writeb, gpe);
    register_ioport_read(GPE_BASE, 4, 1,  gpe_readb, gpe);

    register_ioport_write(PROC_BASE, 32, 1, gpe_writeb, gpe);
    register_ioport_read(PROC_BASE, 32, 1,  gpe_readb, gpe);

    register_ioport_read(PCI_UP_BASE, 4, 4, pci_up_read, pm_state);
    register_ioport_read(PCI_DOWN_BASE, 4, 4, pci_down_read, pm_state);

    register_ioport_write(PCI_EJ_BASE, 4, 4, pciej_write, pm_state);
    register_ioport_read(PCI_EJ_BASE, 4, 4,  pci_features_read, pm_state);

    register_ioport_read(PCI_RMV_BASE, 4, 4,  pcirmv_read, pm_state);

    model = cpu_model;

    pci_bus_hotplug(bus, piix4_device_hotplug);
}

static int acpi_online_cpu_count(void)
{
    int i, j, online_cpu_count = 0;
    struct gpe_regs *gpe = &pm_state->gpe;

    /* count plugged in cpus in cpus_sts bitmap */
    for (i = 0; i < sizeof(gpe->cpus_sts); ++i) {
        for (j = 0; j < 8; ++j)
            if ((gpe->cpus_sts[i] >> j) & 1)
                ++online_cpu_count;
    }
    return online_cpu_count;
}

#if defined(TARGET_I386)
static void enable_processor(struct gpe_regs *g, int cpu)
{
    g->sts |= PIIX4_CPU_HOTPLUG_STATUS;
    g->cpus_sts[cpu / 8] |= (1 << (cpu % 8));
}

static void disable_processor(struct gpe_regs *g, int cpu)
{
    g->sts |= PIIX4_CPU_HOTPLUG_STATUS;
    g->cpus_sts[cpu / 8] &= ~(1 << (cpu % 8));
}

void qemu_system_cpu_hot_add(int cpu, int state, Monitor *mon)
{
    CPUState *env;
    uint32_t apic_id;

    if (!acpi_enabled) {
        monitor_printf(mon, "CPU hot add is disabled by -no-acpi option\n");
        return;
    }

    if ((cpu < 1) || (cpu > max_cpus - 1)) {
        monitor_printf(mon, "cpu id[%d] must be in range [1..%d]\n",
            cpu, max_cpus - 1);
        return;
    }

    apic_id = apic_id_for_cpu(cpu);
    if (state && !qemu_get_cpu(cpu)) {
        env = pc_new_cpu(model);
        if (!env) {
            monitor_printf(mon, "cpu %d creation failed\n", cpu);
            return;
        }
        env->cpuid_apic_id = apic_id;
    }

    if (state)
        enable_processor(&pm_state->gpe, apic_id);
    else
        disable_processor(&pm_state->gpe, apic_id);

    /* update number of cpus in cmos, to allow BIOS see it on reboot */
    rtc_set_memory(rtc_state, 0x5f, acpi_online_cpu_count() - 1);

    pm_update_sci(pm_state);
}
#endif

/* Query the ACPI enabled/disabled state of a VCPU, based on the VCPU's APIC
 * ID. If the information is unavailable, @enabled is indeterminate on output
 * and -1 is returned. Otherwise, @enabled is set to the VCPU's ACPI state, and
 * 0 is returned.
 */
int acpi_query_processor(bool *enabled, uint32_t apic_id)
{
    const struct gpe_regs *g;

    if (!acpi_enabled || apic_id >= sizeof g->cpus_sts * 8) {
        return -1;
    }

    g = &pm_state->gpe;
    *enabled = !!(g->cpus_sts[apic_id / 8] & (1 << (apic_id % 8)));
    return 0;
}

static void enable_device(PIIX4PMState *s, int slot)
{
    s->gpe.sts |= PIIX4_PCI_HOTPLUG_STATUS;
    s->pci0_slot_device_present |= (1U << slot);
}

static void disable_device(PIIX4PMState *s, int slot)
{
    s->gpe.sts |= PIIX4_PCI_HOTPLUG_STATUS;
    s->pci0_status.down |= (1U << slot);
}

static int piix4_device_hotplug(PCIDevice *dev, int state)
{
    int slot = PCI_SLOT(dev->devfn);

    if (state)
        enable_device(pm_state, slot);
    else
        disable_device(pm_state, slot);

    pm_update_sci(pm_state);

    return 0;
}

struct acpi_table_header
{
    char signature [4];    /* ACPI signature (4 ASCII characters) */
    uint32_t length;          /* Length of table, in bytes, including header */
    uint8_t revision;         /* ACPI Specification minor version # */
    uint8_t checksum;         /* To make sum of entire table == 0 */
    char oem_id [6];       /* OEM identification */
    char oem_table_id [8]; /* OEM table identification */
    uint32_t oem_revision;    /* OEM revision number */
    char asl_compiler_id [4]; /* ASL compiler vendor ID */
    uint32_t asl_compiler_revision; /* ASL compiler revision number */
} __attribute__((packed));

char *acpi_tables;
size_t acpi_tables_len;

static int acpi_checksum(const uint8_t *data, int len)
{
    int sum, i;
    sum = 0;
    for(i = 0; i < len; i++)
        sum += data[i];
    return (-sum) & 0xff;
}

int acpi_table_add(const char *t)
{
    static const char *dfl_id = "QEMUQEMU";
    char buf[1024], *p, *f;
    struct acpi_table_header acpi_hdr;
    unsigned long val;
    size_t off;

    memset(&acpi_hdr, 0, sizeof(acpi_hdr));
  
    if (get_param_value(buf, sizeof(buf), "sig", t)) {
        strncpy(acpi_hdr.signature, buf, 4);
    } else {
        strncpy(acpi_hdr.signature, dfl_id, 4);
    }
    if (get_param_value(buf, sizeof(buf), "rev", t)) {
        val = strtoul(buf, &p, 10);
        if (val > 255 || *p != '\0')
            goto out;
    } else {
        val = 1;
    }
    acpi_hdr.revision = (int8_t)val;

    if (get_param_value(buf, sizeof(buf), "oem_id", t)) {
        strncpy(acpi_hdr.oem_id, buf, 6);
    } else {
        strncpy(acpi_hdr.oem_id, dfl_id, 6);
    }

    if (get_param_value(buf, sizeof(buf), "oem_table_id", t)) {
        strncpy(acpi_hdr.oem_table_id, buf, 8);
    } else {
        strncpy(acpi_hdr.oem_table_id, dfl_id, 8);
    }

    if (get_param_value(buf, sizeof(buf), "oem_rev", t)) {
        val = strtol(buf, &p, 10);
        if(*p != '\0')
            goto out;
    } else {
        val = 1;
    }
    acpi_hdr.oem_revision = cpu_to_le32(val);

    if (get_param_value(buf, sizeof(buf), "asl_compiler_id", t)) {
        strncpy(acpi_hdr.asl_compiler_id, buf, 4);
    } else {
        strncpy(acpi_hdr.asl_compiler_id, dfl_id, 4);
    }

    if (get_param_value(buf, sizeof(buf), "asl_compiler_rev", t)) {
        val = strtol(buf, &p, 10);
        if(*p != '\0')
            goto out;
    } else {
        val = 1;
    }
    acpi_hdr.asl_compiler_revision = cpu_to_le32(val);
    
    if (!get_param_value(buf, sizeof(buf), "data", t)) {
         buf[0] = '\0';
    }

    acpi_hdr.length = sizeof(acpi_hdr);

    f = buf;
    while (buf[0]) {
        struct stat s;
        char *n = strchr(f, ':');
        if (n)
            *n = '\0';
        if(stat(f, &s) < 0) {
            fprintf(stderr, "Can't stat file '%s': %s\n", f, strerror(errno));
            goto out;
        }
        acpi_hdr.length += s.st_size;
        if (!n)
            break;
        *n = ':';
        f = n + 1;
    }

    if (!acpi_tables) {
        acpi_tables_len = sizeof(uint16_t);
        acpi_tables = qemu_mallocz(acpi_tables_len);
    }
    p = acpi_tables + acpi_tables_len;
    acpi_tables_len += sizeof(uint16_t) + acpi_hdr.length;
    acpi_tables = qemu_realloc(acpi_tables, acpi_tables_len);

    acpi_hdr.length = cpu_to_le32(acpi_hdr.length);
    *(uint16_t*)p = acpi_hdr.length;
    p += sizeof(uint16_t);
    memcpy(p, &acpi_hdr, sizeof(acpi_hdr));
    off = sizeof(acpi_hdr);

    f = buf;
    while (buf[0]) {
        struct stat s;
        int fd;
        char *n = strchr(f, ':');
        if (n)
            *n = '\0';
        fd = open(f, O_RDONLY);

        if(fd < 0)
            goto out;
        if(fstat(fd, &s) < 0) {
            close(fd);
            goto out;
        }

        do {
            int r;
            r = read(fd, p + off, s.st_size);
            if (r > 0) {
                off += r;
                s.st_size -= r;
            } else if ((r < 0 && errno != EINTR) || r == 0) {
                close(fd);
                goto out;
            }
        } while(s.st_size);

        close(fd);
        if (!n)
            break;
        f = n + 1;
    }

    ((struct acpi_table_header*)p)->checksum = acpi_checksum((uint8_t*)p, off);
    /* increase number of tables */
    (*(uint16_t*)acpi_tables) =
	    cpu_to_le32(le32_to_cpu(*(uint16_t*)acpi_tables) + 1);
    return 0;
out:
    if (acpi_tables) {
        qemu_free(acpi_tables);
        acpi_tables = NULL;
    }
    return -1;
}
