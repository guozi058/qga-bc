/*
 * QEMU PCI bus manager
 *
 * Copyright (c) 2004 Fabrice Bellard
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
#include "hw.h"
#include "pci.h"
#include "monitor.h"
#include "net.h"
#include "sysemu.h"
#include "loader.h"
#include "qmp-commands.h"
#include "qemu-kvm.h"
#include "hw/pc.h"
#include "device-assignment.h"
#include "range.h"

//#define DEBUG_PCI
#ifdef DEBUG_PCI
# define PCI_DPRINTF(format, ...)       printf(format, ## __VA_ARGS__)
#else
# define PCI_DPRINTF(format, ...)       do { } while (0)
#endif

struct PCIBus {
    BusState qbus;
    int devfn_min;
    pci_set_irq_fn set_irq;
    pci_map_irq_fn map_irq;
    pci_hotplug_fn hotplug;
    uint32_t config_reg; /* XXX: suppress */
    void *irq_opaque;
    PCIDevice *devices[256];
    PCIDevice *parent_dev;

    QLIST_HEAD(, PCIBus) child; /* this will be replaced by qdev later */
    QLIST_ENTRY(PCIBus) sibling;/* this will be replaced by qdev later */

    /* The bus IRQ state is the logical OR of the connected devices.
       Keep a count of the number of devices with raised IRQs.  */
    int nirq;
    int *irq_count;
};

static void pcibus_dev_print(Monitor *mon, DeviceState *dev, int indent);
static char *pcibus_get_dev_path(DeviceState *dev);
static char *pcibus_get_fw_dev_path(DeviceState *dev);

static struct BusInfo pci_bus_info = {
    .name       = "PCI",
    .size       = sizeof(PCIBus),
    .print_dev  = pcibus_dev_print,
    .get_dev_path = pcibus_get_dev_path,
    .get_fw_dev_path = pcibus_get_fw_dev_path,
    .props      = (Property[]) {
        DEFINE_PROP_PCI_DEVFN("addr", PCIDevice, devfn, -1),
        DEFINE_PROP_STRING("romfile", PCIDevice, romfile),
        DEFINE_PROP_UINT32("rombar",  PCIDevice, rom_bar, 1),
        DEFINE_PROP_BIT("multifunction", PCIDevice, cap_present,
                        QEMU_PCI_CAP_MULTIFUNCTION_BITNR, false),
        DEFINE_PROP_END_OF_LIST()
    }
};

static void pci_update_mappings(PCIDevice *d);
static void pci_set_irq(void *opaque, int irq_num, int level);
static int pci_add_option_rom(PCIDevice *pdev);
static void pci_del_option_rom(PCIDevice *pdev);

target_phys_addr_t pci_mem_base;
static uint16_t pci_default_sub_vendor_id = PCI_SUBVENDOR_ID_REDHAT_QUMRANET;
static uint16_t pci_default_sub_device_id = PCI_SUBDEVICE_ID_QEMU;

struct PCIHostBus {
    int domain;
    struct PCIBus *bus;
    QLIST_ENTRY(PCIHostBus) next;
};
static QLIST_HEAD(, PCIHostBus) host_buses;

static const VMStateDescription vmstate_pcibus = {
    .name = "PCIBUS",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_INT32_EQUAL(nirq, PCIBus),
        VMSTATE_VARRAY_INT32(irq_count, PCIBus, nirq, 0, vmstate_info_int32, int32_t),
        VMSTATE_END_OF_LIST()
    }
};

static int pci_bar(PCIDevice *d, int reg)
{
    uint8_t type;

    if (reg != PCI_ROM_SLOT)
        return PCI_BASE_ADDRESS_0 + reg * 4;

    type = d->config[PCI_HEADER_TYPE] & ~PCI_HEADER_TYPE_MULTI_FUNCTION;
    return type == PCI_HEADER_TYPE_BRIDGE ? PCI_ROM_ADDRESS1 : PCI_ROM_ADDRESS;
}

static inline int pci_irq_state(PCIDevice *d, int irq_num)
{
	return (d->irq_state >> irq_num) & 0x1;
}

static inline void pci_set_irq_state(PCIDevice *d, int irq_num, int level)
{
	d->irq_state &= ~(0x1 << irq_num);
	d->irq_state |= level << irq_num;
}

static void pci_change_irq_level(PCIDevice *pci_dev, int irq_num, int change)
{
    PCIBus *bus;
    for (;;) {
        bus = pci_dev->bus;
        irq_num = bus->map_irq(pci_dev, irq_num);
        if (bus->set_irq)
            break;
        pci_dev = bus->parent_dev;
    }
    bus->irq_count[irq_num] += change;
    bus->set_irq(bus->irq_opaque, irq_num, bus->irq_count[irq_num] != 0);
}

/* Update interrupt status bit in config space on interrupt
 * state change. */
static void pci_update_irq_status(PCIDevice *dev)
{
    if (dev->irq_state) {
        dev->config[PCI_STATUS] |= PCI_STATUS_INTERRUPT;
    } else {
        dev->config[PCI_STATUS] &= ~PCI_STATUS_INTERRUPT;
    }
}

static void pci_device_reset(PCIDevice *dev)
{
    int r;

    dev->irq_state = 0;
    pci_update_irq_status(dev);
    dev->config[PCI_COMMAND] &= ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
                                  PCI_COMMAND_MASTER);
    dev->config[PCI_CACHE_LINE_SIZE] = 0x0;
    dev->config[PCI_INTERRUPT_LINE] = 0x0;
    for (r = 0; r < PCI_NUM_REGIONS; ++r) {
        if (!dev->io_regions[r].size) {
            continue;
        }
        pci_set_long(dev->config + pci_bar(dev, r), dev->io_regions[r].type);
    }
    pci_update_mappings(dev);
}

static void pci_bus_reset(void *opaque)
{
    PCIBus *bus = opaque;
    int i;

    for (i = 0; i < bus->nirq; i++) {
        bus->irq_count[i] = 0;
    }
    for (i = 0; i < ARRAY_SIZE(bus->devices); ++i) {
        if (bus->devices[i]) {
            pci_device_reset(bus->devices[i]);
        }
    }
}

static void pci_host_bus_register(int domain, PCIBus *bus)
{
    struct PCIHostBus *host;
    host = qemu_mallocz(sizeof(*host));
    host->domain = domain;
    host->bus = bus;
    QLIST_INSERT_HEAD(&host_buses, host, next);
}

PCIBus *pci_find_root_bus(int domain)
{
    struct PCIHostBus *host;

    QLIST_FOREACH(host, &host_buses, next) {
        if (host->domain == domain) {
            return host->bus;
        }
    }

    return NULL;
}

void pci_bus_new_inplace(PCIBus *bus, DeviceState *parent,
                         const char *name, int devfn_min)
{
    qbus_create_inplace(&bus->qbus, &pci_bus_info, parent, name);
    assert(PCI_FUNC(devfn_min) == 0);
    bus->devfn_min = devfn_min;

    /* host bridge */
    QLIST_INIT(&bus->child);
    pci_host_bus_register(0, bus); /* for now only pci domain 0 is supported */

    vmstate_register(NULL, -1, &vmstate_pcibus, bus);
    qemu_register_reset(pci_bus_reset, bus);
}

PCIBus *pci_bus_new(DeviceState *parent, const char *name, int devfn_min)
{
    PCIBus *bus;

    bus = qemu_mallocz(sizeof(*bus));
    bus->qbus.qdev_allocated = 1;
    pci_bus_new_inplace(bus, parent, name, devfn_min);
    return bus;
}

void pci_bus_irqs(PCIBus *bus, pci_set_irq_fn set_irq, pci_map_irq_fn map_irq,
                  void *irq_opaque, int nirq)
{
    bus->set_irq = set_irq;
    bus->map_irq = map_irq;
    bus->irq_opaque = irq_opaque;
    bus->nirq = nirq;
    bus->irq_count = qemu_mallocz(nirq * sizeof(bus->irq_count[0]));
}

void pci_bus_hotplug(PCIBus *bus, pci_hotplug_fn hotplug)
{
    bus->qbus.allow_hotplug = 1;
    bus->hotplug = hotplug;
}

PCIBus *pci_register_bus(DeviceState *parent, const char *name,
                         pci_set_irq_fn set_irq, pci_map_irq_fn map_irq,
                         void *irq_opaque, int devfn_min, int nirq)
{
    PCIBus *bus;

    bus = pci_bus_new(parent, name, devfn_min);
    pci_bus_irqs(bus, set_irq, map_irq, irq_opaque, nirq);
    return bus;
}

static void pci_register_secondary_bus(PCIBus *parent,
                                       PCIBus *bus,
                                       PCIDevice *dev,
                                       pci_map_irq_fn map_irq,
                                       const char *name)
{
    qbus_create_inplace(&bus->qbus, &pci_bus_info, &dev->qdev, name);
    bus->map_irq = map_irq;
    bus->parent_dev = dev;

    QLIST_INIT(&bus->child);
    QLIST_INSERT_HEAD(&parent->child, bus, sibling);
}

static void pci_unregister_secondary_bus(PCIBus *bus)
{
    assert(QLIST_EMPTY(&bus->child));
    QLIST_REMOVE(bus, sibling);
}

int pci_bus_num(PCIBus *s)
{
    if (!s->parent_dev)
        return 0;       /* pci host bridge */
    return s->parent_dev->config[PCI_SECONDARY_BUS];
}

static int get_pci_config_device(QEMUFile *f, void *pv, size_t size)
{
    PCIDevice *s = container_of(pv, PCIDevice, config);
    uint8_t *config;
    int i;

    assert(size == pci_config_size(s));
    config = qemu_malloc(size);

    qemu_get_buffer(f, config, size);
    for (i = 0; i < size; ++i) {
        if ((config[i] ^ s->config[i]) & s->cmask[i] & ~s->wmask[i]) {
            qemu_free(config);
            return -EINVAL;
        }
    }
    memcpy(s->config, config, size);

    pci_update_mappings(s);

    qemu_free(config);
    return 0;
}

/* just put buffer */
static void put_pci_config_device(QEMUFile *f, void *pv, size_t size)
{
    const uint8_t **v = pv;
    assert(size == pci_config_size(container_of(pv, PCIDevice, config)));
    qemu_put_buffer(f, *v, size);
}

static VMStateInfo vmstate_info_pci_config = {
    .name = "pci config",
    .get  = get_pci_config_device,
    .put  = put_pci_config_device,
};

static int get_pci_irq_state(QEMUFile *f, void *pv, size_t size)
{
    PCIDevice *s = container_of(pv, PCIDevice, irq_state);
    uint32_t irq_state[PCI_NUM_PINS];
    int i;
    for (i = 0; i < PCI_NUM_PINS; ++i) {
        irq_state[i] = qemu_get_be32(f);
        if (irq_state[i] != 0x1 && irq_state[i] != 0) {
            fprintf(stderr, "irq state %d: must be 0 or 1.\n",
                    irq_state[i]);
            return -EINVAL;
        }
    }

    for (i = 0; i < PCI_NUM_PINS; ++i) {
        pci_set_irq_state(s, i, irq_state[i]);
    }

    return 0;
}

static void put_pci_irq_state(QEMUFile *f, void *pv, size_t size)
{
    int i;
    PCIDevice *s = container_of(pv, PCIDevice, irq_state);

    for (i = 0; i < PCI_NUM_PINS; ++i) {
        qemu_put_be32(f, pci_irq_state(s, i));
    }
}

static VMStateInfo vmstate_info_pci_irq_state = {
    .name = "pci irq state",
    .get  = get_pci_irq_state,
    .put  = put_pci_irq_state,
};

const VMStateDescription vmstate_pci_device = {
    .name = "PCIDevice",
    .version_id = 2,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_INT32_LE(version_id, PCIDevice),
        VMSTATE_BUFFER_UNSAFE_INFO(config, PCIDevice, 0,
                                   vmstate_info_pci_config,
                                   PCI_CONFIG_SPACE_SIZE),
        VMSTATE_BUFFER_UNSAFE_INFO(irq_state, PCIDevice, 2,
				   vmstate_info_pci_irq_state,
				   PCI_NUM_PINS * sizeof(int32_t)),
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_pcie_device = {
    .name = "PCIDevice",
    .version_id = 2,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_INT32_LE(version_id, PCIDevice),
        VMSTATE_BUFFER_UNSAFE_INFO(config, PCIDevice, 0,
                                   vmstate_info_pci_config,
                                   PCIE_CONFIG_SPACE_SIZE),
        VMSTATE_BUFFER_UNSAFE_INFO(irq_state, PCIDevice, 2,
				   vmstate_info_pci_irq_state,
				   PCI_NUM_PINS * sizeof(int32_t)),
        VMSTATE_END_OF_LIST()
    }
};

static inline const VMStateDescription *pci_get_vmstate(PCIDevice *s)
{
    return pci_is_express(s) ? &vmstate_pcie_device : &vmstate_pci_device;
}

void pci_device_save(PCIDevice *s, QEMUFile *f)
{
    /* Clear interrupt status bit: it is implicit
     * in irq_state which we are saving.
     * This makes us compatible with old devices
     * which never set or clear this bit. */
    s->config[PCI_STATUS] &= ~PCI_STATUS_INTERRUPT;
    vmstate_save_state(f, pci_get_vmstate(s), s);
    /* Restore the interrupt status bit. */
    pci_update_irq_status(s);
}

int pci_device_load(PCIDevice *s, QEMUFile *f)
{
    int ret;
    ret = vmstate_load_state(f, pci_get_vmstate(s), s, s->version_id);
    /* Restore the interrupt status bit. */
    pci_update_irq_status(s);
    return ret;
}

static int pci_set_default_subsystem_id(PCIDevice *pci_dev)
{
    uint16_t *id;

    id = (void*)(&pci_dev->config[PCI_SUBSYSTEM_VENDOR_ID]);
    id[0] = cpu_to_le16(pci_default_sub_vendor_id);
    id[1] = cpu_to_le16(pci_default_sub_device_id);
    return 0;
}

/*
 * Parse pci address in qemu command
 * Parse [[<domain>:]<bus>:]<slot>, return -1 on error
 */
static int pci_parse_devaddr(const char *addr, int *domp, int *busp, unsigned *slotp)
{
    const char *p;
    char *e;
    unsigned long val;
    unsigned long dom = 0, bus = 0;
    unsigned slot = 0;

    p = addr;
    val = strtoul(p, &e, 16);
    if (e == p)
	return -1;
    if (*e == ':') {
	bus = val;
	p = e + 1;
	val = strtoul(p, &e, 16);
	if (e == p)
	    return -1;
	if (*e == ':') {
	    dom = bus;
	    bus = val;
	    p = e + 1;
	    val = strtoul(p, &e, 16);
	    if (e == p)
		return -1;
	}
    }

    if (dom > 0xffff || bus > 0xff || val > 0x1f)
	return -1;

    slot = val;

    if (*e)
	return -1;

    /* Note: QEMU doesn't implement domains other than 0 */
    if (!pci_find_bus(pci_find_root_bus(dom), bus))
	return -1;

    *domp = dom;
    *busp = bus;
    *slotp = slot;
    return 0;
}

/*
 * Parse device seg and bdf in device assignment command:
 *
 * -pcidevice host=[seg:]bus:dev.func
 *
 * Parse [seg:]<bus>:<slot>.<func> return -1 on error
 */
int pci_parse_host_devaddr(const char *addr, int *segp, int *busp,
                           int *slotp, int *funcp)
{
    const char *p;
    char *e;
    int val;
    int seg = 0, bus = 0, slot = 0, func = 0;

    /* parse optional seg */
    p = addr;
    val = 0;
    while (1) {
        p = strchr(p, ':');
        if (p) {
            val++;
            p++;
        } else
            break;
    }
    if (val <= 0 || val > 2)
        return -1;

    p = addr;
    if (val == 2) {
        val = strtoul(p, &e, 16);
        if (e == p)
            return -1;
        if (*e == ':') {
            seg = val;
            p = e + 1;
        }
    } else
        seg = 0;


    /* parse bdf */
    val = strtoul(p, &e, 16);
    if (e == p)
	return -1;
    if (*e == ':') {
	bus = val;
	p = e + 1;
	val = strtoul(p, &e, 16);
	if (e == p)
	    return -1;
	if (*e == '.') {
	    slot = val;
	    p = e + 1;
	    val = strtoul(p, &e, 16);
	    if (e == p)
		return -1;
	    func = val;
	} else
	    return -1;
    } else
	return -1;

    if (seg > 0xffff || bus > 0xff || slot > 0x1f || func > 0x7)
	return -1;

    if (*e)
	return -1;

    *segp = seg;
    *busp = bus;
    *slotp = slot;
    *funcp = func;
    return 0;
}

int pci_read_devaddr(Monitor *mon, const char *addr, int *domp, int *busp,
                     unsigned *slotp)
{
    /* strip legacy tag */
    if (!strncmp(addr, "pci_addr=", 9)) {
        addr += 9;
    }
    if (pci_parse_devaddr(addr, domp, busp, slotp)) {
        monitor_printf(mon, "Invalid pci address\n");
        return -1;
    }
    return 0;
}

PCIBus *pci_get_bus_devfn(int *devfnp, const char *devaddr)
{
    int dom, bus;
    unsigned slot;

    if (!devaddr) {
        *devfnp = -1;
        return pci_find_bus(pci_find_root_bus(0), 0);
    }

    if (pci_parse_devaddr(devaddr, &dom, &bus, &slot) < 0) {
        return NULL;
    }

    *devfnp = slot << 3;
    return pci_find_bus(pci_find_root_bus(0), bus);
}

static void pci_init_cmask(PCIDevice *dev)
{
    pci_set_word(dev->cmask + PCI_VENDOR_ID, 0xffff);
    pci_set_word(dev->cmask + PCI_DEVICE_ID, 0xffff);
    dev->cmask[PCI_STATUS] = PCI_STATUS_CAP_LIST;
    dev->cmask[PCI_REVISION_ID] = 0xff;
    dev->cmask[PCI_CLASS_PROG] = 0xff;
    pci_set_word(dev->cmask + PCI_CLASS_DEVICE, 0xffff);
    dev->cmask[PCI_HEADER_TYPE] = 0xff;
    dev->cmask[PCI_CAPABILITY_LIST] = 0xff;
}

static void pci_init_wmask(PCIDevice *dev)
{
    int config_size = pci_config_size(dev);

    dev->wmask[PCI_CACHE_LINE_SIZE] = 0xff;
    dev->wmask[PCI_INTERRUPT_LINE] = 0xff;
    pci_set_word(dev->wmask + PCI_COMMAND,
                 PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);

    memset(dev->wmask + PCI_CONFIG_HEADER_SIZE, 0xff,
           config_size - PCI_CONFIG_HEADER_SIZE);
}

static void pci_init_wmask_bridge(PCIDevice *d)
{
    /* PCI_PRIMARY_BUS, PCI_SECONDARY_BUS, PCI_SUBORDINATE_BUS and
       PCI_SEC_LETENCY_TIMER */
    memset(d->wmask + PCI_PRIMARY_BUS, 0xff, 4);

    /* base and limit */
    d->wmask[PCI_IO_BASE] = PCI_IO_RANGE_MASK & 0xff;
    d->wmask[PCI_IO_LIMIT] = PCI_IO_RANGE_MASK & 0xff;
    pci_set_word(d->wmask + PCI_MEMORY_BASE,
                 PCI_MEMORY_RANGE_MASK & 0xffff);
    pci_set_word(d->wmask + PCI_MEMORY_LIMIT,
                 PCI_MEMORY_RANGE_MASK & 0xffff);
    pci_set_word(d->wmask + PCI_PREF_MEMORY_BASE,
                 PCI_PREF_RANGE_MASK & 0xffff);
    pci_set_word(d->wmask + PCI_PREF_MEMORY_LIMIT,
                 PCI_PREF_RANGE_MASK & 0xffff);

    /* PCI_PREF_BASE_UPPER32 and PCI_PREF_LIMIT_UPPER32 */
    memset(d->wmask + PCI_PREF_BASE_UPPER32, 0xff, 8);

    pci_set_word(d->wmask + PCI_BRIDGE_CONTROL, 0xffff);
}

static int pci_init_multifunction(PCIBus *bus, PCIDevice *dev)
{
    uint8_t slot = PCI_SLOT(dev->devfn);
    uint8_t func;

    if (dev->cap_present & QEMU_PCI_CAP_MULTIFUNCTION) {
        dev->config[PCI_HEADER_TYPE] |= PCI_HEADER_TYPE_MULTI_FUNCTION;
    }

    /*
     * multifuction bit is interpreted in two ways as follows.
     *   - all functions must set the bit to 1.
     *     Example: Intel X53
     *   - function 0 must set the bit, but the rest function (> 0)
     *     is allowed to leave the bit to 0.
     *     Example: PIIX3(also in qemu), PIIX4(also in qemu), ICH10,
     *
     * So OS (at least Linux) checks the bit of only function 0,
     * and doesn't see the bit of function > 0.
     *
     * The below check allows both interpretation.
     */
    if (PCI_FUNC(dev->devfn)) {
        PCIDevice *f0 = bus->devices[PCI_DEVFN(slot, 0)];
        if (f0 && !(f0->cap_present & QEMU_PCI_CAP_MULTIFUNCTION)) {
            /* function 0 should set multifunction bit */
            error_report("PCI: single function device can't be populated "
                         "in function %x.%x", slot, PCI_FUNC(dev->devfn));
            return -1;
        }
        return 0;
    }

    if (dev->cap_present & QEMU_PCI_CAP_MULTIFUNCTION) {
        return 0;
    }
    /* function 0 indicates single function, so function > 0 must be NULL */
    for (func = 1; func < PCI_FUNC_MAX; ++func) {
        if (bus->devices[PCI_DEVFN(slot, func)]) {
            error_report("PCI: %x.0 indicates single function, "
                         "but %x.%x is already populated.",
                         slot, slot, func);
            return -1;
        }
    }
    return 0;
}

static void pci_config_alloc(PCIDevice *pci_dev)
{
    int config_size = pci_config_size(pci_dev);

    pci_dev->config = qemu_mallocz(config_size);
    pci_dev->cmask = qemu_mallocz(config_size);
    pci_dev->wmask = qemu_mallocz(config_size);
    pci_dev->config_map = qemu_mallocz(config_size);
}

static void pci_config_free(PCIDevice *pci_dev)
{
    qemu_free(pci_dev->config);
    qemu_free(pci_dev->cmask);
    qemu_free(pci_dev->wmask);
    qemu_free(pci_dev->config_map);
}

/* -1 for devfn means auto assign */
static PCIDevice *do_pci_register_device(PCIDevice *pci_dev, PCIBus *bus,
                                         const char *name, int devfn,
                                         PCIConfigReadFunc *config_read,
                                         PCIConfigWriteFunc *config_write,
                                         uint8_t header_type)
{
    if (devfn < 0) {
        for(devfn = bus->devfn_min ; devfn < ARRAY_SIZE(bus->devices);
            devfn += PCI_FUNC_MAX) {
            if (!bus->devices[devfn])
                goto found;
        }
        error_report("PCI: no devfn available for %s, all in use", name);
        return NULL;
    found: ;
    } else if (bus->devices[devfn]) {
        error_report("PCI: devfn %d not available for %s, in use by %s",
                     devfn, name, bus->devices[devfn]->name);
        return NULL;
    }
    pci_dev->bus = bus;
    pci_dev->devfn = devfn;
    pstrcpy(pci_dev->name, sizeof(pci_dev->name), name);
    pci_dev->irq_state = 0;
    pci_config_alloc(pci_dev);

    header_type &= ~PCI_HEADER_TYPE_MULTI_FUNCTION;

    memset(pci_dev->config_map, 0xff, PCI_CONFIG_HEADER_SIZE);

    if (header_type == PCI_HEADER_TYPE_NORMAL) {
        pci_set_default_subsystem_id(pci_dev);
    }
    pci_init_cmask(pci_dev);
    pci_init_wmask(pci_dev);
    if (header_type == PCI_HEADER_TYPE_BRIDGE) {
        pci_init_wmask_bridge(pci_dev);
    }
    if (pci_init_multifunction(bus, pci_dev)) {
        pci_config_free(pci_dev);
        return NULL;
    }

    if (!config_read)
        config_read = pci_default_read_config;
    if (!config_write)
        config_write = pci_default_write_config;
    pci_dev->config_read = config_read;
    pci_dev->config_write = config_write;
    bus->devices[devfn] = pci_dev;
    pci_dev->irq = qemu_allocate_irqs(pci_set_irq, pci_dev, PCI_NUM_PINS);
    pci_dev->version_id = 2; /* Current pci device vmstate version */
    return pci_dev;
}

static void do_pci_unregister_device(PCIDevice *pci_dev)
{
    qemu_free_irqs(pci_dev->irq);
    pci_dev->bus->devices[pci_dev->devfn] = NULL;
    pci_config_free(pci_dev);
}

PCIDevice *pci_register_device(PCIBus *bus, const char *name,
                               int instance_size, int devfn,
                               PCIConfigReadFunc *config_read,
                               PCIConfigWriteFunc *config_write)
{
    PCIDevice *pci_dev;

    pci_dev = qemu_mallocz(instance_size);
    pci_dev = do_pci_register_device(pci_dev, bus, name, devfn,
                                     config_read, config_write,
                                     PCI_HEADER_TYPE_NORMAL);
    if (pci_dev == NULL) {
        hw_error("PCI: can't register device\n");
    }
    return pci_dev;
}
static target_phys_addr_t pci_to_cpu_addr(target_phys_addr_t addr)
{
    return addr + pci_mem_base;
}

static void pci_unregister_io_regions(PCIDevice *pci_dev)
{
    PCIIORegion *r;
    int i;

    for(i = 0; i < PCI_NUM_REGIONS; i++) {
        r = &pci_dev->io_regions[i];
        if (!r->size || r->addr == PCI_BAR_UNMAPPED)
            continue;
        if (r->type == PCI_BASE_ADDRESS_SPACE_IO) {
            isa_unassign_ioport(r->addr, r->filtered_size);
        } else {
            cpu_register_physical_memory(pci_to_cpu_addr(r->addr),
                                                     r->filtered_size,
                                                     IO_MEM_UNASSIGNED);
        }
    }
}

static int pci_unregister_device(DeviceState *dev)
{
    PCIDevice *pci_dev = DO_UPCAST(PCIDevice, qdev, dev);
    PCIDeviceInfo *info = DO_UPCAST(PCIDeviceInfo, qdev, dev->info);
    int ret = 0;

    if (info->exit)
        ret = info->exit(pci_dev);
    if (ret)
        return ret;

    pci_unregister_io_regions(pci_dev);
    pci_del_option_rom(pci_dev);
    qemu_free(pci_dev->romfile);
    do_pci_unregister_device(pci_dev);
    return 0;
}

void pci_register_bar(PCIDevice *pci_dev, int region_num,
                            pcibus_t size, int type,
                            PCIMapIORegionFunc *map_func)
{
    PCIIORegion *r;
    uint32_t addr;
    pcibus_t wmask;

    if ((unsigned int)region_num >= PCI_NUM_REGIONS)
        return;

    if (size & (size-1)) {
        fprintf(stderr, "ERROR: PCI region size must be pow2 "
                    "type=0x%x, size=0x%"FMT_PCIBUS"\n", type, size);
        exit(1);
    }

    r = &pci_dev->io_regions[region_num];
    r->addr = PCI_BAR_UNMAPPED;
    r->size = size;
    r->filtered_size = size;
    r->type = type;
    r->map_func = map_func;

    wmask = ~(size - 1);
    addr = pci_bar(pci_dev, region_num);
    if (region_num == PCI_ROM_SLOT) {
        /* ROM enable bit is writeable */
        wmask |= PCI_ROM_ADDRESS_ENABLE;
    }
    pci_set_long(pci_dev->config + addr, type);
    if (!(r->type & PCI_BASE_ADDRESS_SPACE_IO) &&
        r->type & PCI_BASE_ADDRESS_MEM_TYPE_64) {
        pci_set_quad(pci_dev->wmask + addr, wmask);
        pci_set_quad(pci_dev->cmask + addr, ~0ULL);
    } else {
        pci_set_long(pci_dev->wmask + addr, wmask & 0xffffffff);
        pci_set_long(pci_dev->cmask + addr, 0xffffffff);
    }
}

static uint32_t pci_config_get_io_base(PCIDevice *d,
                                       uint32_t base, uint32_t base_upper16)
{
    uint32_t val;

    val = ((uint32_t)d->config[base] & PCI_IO_RANGE_MASK) << 8;
    if (d->config[base] & PCI_IO_RANGE_TYPE_32) {
        val |= (uint32_t)pci_get_word(d->config + base_upper16) << 16;
    }
    return val;
}

static pcibus_t pci_config_get_memory_base(PCIDevice *d, uint32_t base)
{
    return ((pcibus_t)pci_get_word(d->config + base) & PCI_MEMORY_RANGE_MASK)
        << 16;
}

static pcibus_t pci_config_get_pref_base(PCIDevice *d,
                                         uint32_t base, uint32_t upper)
{
    pcibus_t tmp;
    pcibus_t val;

    tmp = (pcibus_t)pci_get_word(d->config + base);
    val = (tmp & PCI_PREF_RANGE_MASK) << 16;
    if (tmp & PCI_PREF_RANGE_TYPE_64) {
        val |= (pcibus_t)pci_get_long(d->config + upper) << 32;
    }
    return val;
}

static pcibus_t pci_bridge_get_base(PCIDevice *bridge, uint8_t type)
{
    pcibus_t base;
    if (type & PCI_BASE_ADDRESS_SPACE_IO) {
        base = pci_config_get_io_base(bridge,
                                      PCI_IO_BASE, PCI_IO_BASE_UPPER16);
    } else {
        if (type & PCI_BASE_ADDRESS_MEM_PREFETCH) {
            base = pci_config_get_pref_base(
                bridge, PCI_PREF_MEMORY_BASE, PCI_PREF_BASE_UPPER32);
        } else {
            base = pci_config_get_memory_base(bridge, PCI_MEMORY_BASE);
        }
    }

    return base;
}

static pcibus_t pci_bridge_get_limit(PCIDevice *bridge, uint8_t type)
{
    pcibus_t limit;
    if (type & PCI_BASE_ADDRESS_SPACE_IO) {
        limit = pci_config_get_io_base(bridge,
                                      PCI_IO_LIMIT, PCI_IO_LIMIT_UPPER16);
        limit |= 0xfff;         /* PCI bridge spec 3.2.5.6. */
    } else {
        if (type & PCI_BASE_ADDRESS_MEM_PREFETCH) {
            limit = pci_config_get_pref_base(
                bridge, PCI_PREF_MEMORY_LIMIT, PCI_PREF_LIMIT_UPPER32);
        } else {
            limit = pci_config_get_memory_base(bridge, PCI_MEMORY_LIMIT);
        }
        limit |= 0xfffff;       /* PCI bridge spec 3.2.5.{1, 8}. */
    }
    return limit;
}

static void pci_bridge_filter(PCIDevice *d, pcibus_t *addr, pcibus_t *size,
                              uint8_t type)
{
    pcibus_t base = *addr;
    pcibus_t limit = *addr + *size - 1;
    PCIDevice *br;

    for (br = d->bus->parent_dev; br; br = br->bus->parent_dev) {
        uint16_t cmd = pci_get_word(d->config + PCI_COMMAND);

        if (type & PCI_BASE_ADDRESS_SPACE_IO) {
            if (!(cmd & PCI_COMMAND_IO)) {
                goto no_map;
            }
        } else {
            if (!(cmd & PCI_COMMAND_MEMORY)) {
                goto no_map;
            }
        }

        base = MAX(base, pci_bridge_get_base(br, type));
        limit = MIN(limit, pci_bridge_get_limit(br, type));
    }

    if (base > limit) {
        goto no_map;
    }
    *addr = base;
    *size = limit - base + 1;
    return;
no_map:
    *addr = PCI_BAR_UNMAPPED;
    *size = 0;
}

static pcibus_t pci_bar_address(PCIDevice *d,
				int reg, uint8_t type, pcibus_t size)
{
    pcibus_t new_addr, last_addr;
    int bar = pci_bar(d, reg);
    uint16_t cmd = pci_get_word(d->config + PCI_COMMAND);

    if (type & PCI_BASE_ADDRESS_SPACE_IO) {
        if (!(cmd & PCI_COMMAND_IO)) {
            return PCI_BAR_UNMAPPED;
        }
        new_addr = pci_get_long(d->config + bar) & ~(size - 1);
        last_addr = new_addr + size - 1;
        /* NOTE: we have only 64K ioports on PC */
        if (last_addr <= new_addr || new_addr == 0 || last_addr > UINT16_MAX) {
            return PCI_BAR_UNMAPPED;
        }
        return new_addr;
    }

    if (!(cmd & PCI_COMMAND_MEMORY)) {
        return PCI_BAR_UNMAPPED;
    }
    if (type & PCI_BASE_ADDRESS_MEM_TYPE_64) {
        new_addr = pci_get_quad(d->config + bar);
    } else {
        new_addr = pci_get_long(d->config + bar);
    }
    /* the ROM slot has a specific enable bit */
    if (reg == PCI_ROM_SLOT && !(new_addr & PCI_ROM_ADDRESS_ENABLE)) {
        return PCI_BAR_UNMAPPED;
    }
    new_addr &= ~(size - 1);
    last_addr = new_addr + size - 1;
    /* NOTE: we do not support wrapping */
    /* XXX: as we cannot support really dynamic
       mappings, we handle specific values as invalid
       mappings. */
    if (last_addr <= new_addr || new_addr == 0 ||
        last_addr == PCI_BAR_UNMAPPED) {
        return PCI_BAR_UNMAPPED;
    }

    /* Now pcibus_t is 64bit.
     * Check if 32 bit BAR wraps around explicitly.
     * Without this, PC ide doesn't work well.
     * TODO: remove this work around.
     */
    if  (!(type & PCI_BASE_ADDRESS_MEM_TYPE_64) && last_addr >= UINT32_MAX) {
        return PCI_BAR_UNMAPPED;
    }

    /*
     * OS is allowed to set BAR beyond its addressable
     * bits. For example, 32 bit OS can set 64bit bar
     * to >4G. Check it. TODO: we might need to support
     * it in the future for e.g. PAE.
     */
    if (last_addr >= TARGET_PHYS_ADDR_MAX) {
        return PCI_BAR_UNMAPPED;
    }

    return new_addr;
}

static void pci_update_mappings(PCIDevice *d)
{
    PCIIORegion *r;
    int i;
    pcibus_t new_addr, filtered_size;

    for(i = 0; i < PCI_NUM_REGIONS; i++) {
        r = &d->io_regions[i];

        /* this region isn't registered */
        if (!r->size)
            continue;

        new_addr = pci_bar_address(d, i, r->type, r->size);

        /* bridge filtering */
        filtered_size = r->size;
        if (new_addr != PCI_BAR_UNMAPPED) {
            pci_bridge_filter(d, &new_addr, &filtered_size, r->type);
        }

        /* This bar isn't changed */
        if (new_addr == r->addr && filtered_size == r->filtered_size)
            continue;

        /* now do the real mapping */
        if (r->addr != PCI_BAR_UNMAPPED) {
            if (r->type & PCI_BASE_ADDRESS_SPACE_IO) {
                int class;
                /* NOTE: specific hack for IDE in PC case:
                   only one byte must be mapped. */
                class = pci_get_word(d->config + PCI_CLASS_DEVICE);
                if (class == 0x0101 && r->size == 4) {
                    isa_unassign_ioport(r->addr + 2, 1);
                } else {
                    isa_unassign_ioport(r->addr, r->filtered_size);
                }
            } else {
                cpu_register_physical_memory(pci_to_cpu_addr(r->addr),
                                             r->filtered_size,
                                             IO_MEM_UNASSIGNED);
                qemu_unregister_coalesced_mmio(r->addr, r->filtered_size);
            }
        }
        r->addr = new_addr;
        r->filtered_size = filtered_size;
        if (r->addr != PCI_BAR_UNMAPPED) {
            /*
             * TODO: currently almost all the map funcions assumes
             * filtered_size == size and addr & ~(size - 1) == addr.
             * However with bridge filtering, they aren't always true.
             * Teach them such cases, such that filtered_size < size and
             * addr & (size - 1) != 0.
             */
            r->map_func(d, i, r->addr, r->filtered_size, r->type);
        }
    }
}

uint32_t pci_default_read_config(PCIDevice *d,
                                 uint32_t address, int len)
{
    uint32_t val = 0;
    assert(len == 1 || len == 2 || len == 4);
    len = MIN(len, pci_config_size(d) - address);
    memcpy(&val, d->config + address, len);
    return le32_to_cpu(val);
}

void pci_default_write_config(PCIDevice *d, uint32_t addr, uint32_t val, int l)
{
    int i;
    uint32_t config_size = pci_config_size(d);

    for (i = 0; i < l && addr + i < config_size; val >>= 8, ++i) {
        uint8_t wmask = d->wmask[addr + i];
        d->config[addr + i] = (d->config[addr + i] & ~wmask) | (val & wmask);
    }

#ifdef CONFIG_KVM_DEVICE_ASSIGNMENT
    if (kvm_enabled() && kvm_irqchip_in_kernel() &&
        addr >= PIIX_CONFIG_IRQ_ROUTE &&
	addr < PIIX_CONFIG_IRQ_ROUTE + 4)
        assigned_dev_update_irqs();
#endif /* CONFIG_KVM_DEVICE_ASSIGNMENT */

    if (ranges_overlap(addr, l, PCI_BASE_ADDRESS_0, 24) ||
        ranges_overlap(addr, l, PCI_ROM_ADDRESS, 4) ||
        ranges_overlap(addr, l, PCI_ROM_ADDRESS1, 4) ||
        range_covers_byte(addr, l, PCI_COMMAND))
        pci_update_mappings(d);
}

/***********************************************************/
/* generic PCI irq support */

/* 0 <= irq_num <= 3. level must be 0 or 1 */
static void pci_set_irq(void *opaque, int irq_num, int level)
{
    PCIDevice *pci_dev = opaque;
    int change;

    change = level - pci_irq_state(pci_dev, irq_num);
    if (!change)
        return;

#if defined(TARGET_IA64)
    ioapic_set_irq(pci_dev, irq_num, level);
#endif

    pci_set_irq_state(pci_dev, irq_num, level);
    pci_update_irq_status(pci_dev);
    pci_change_irq_level(pci_dev, irq_num, change);
}

int pci_map_irq(PCIDevice *pci_dev, int pin)
{
    return pci_dev->bus->map_irq(pci_dev, pin);
}

/***********************************************************/
/* monitor info on PCI */

typedef struct {
    uint16_t class;
    const char *desc;
    const char *fw_name;
    uint16_t fw_ign_bits;
} pci_class_desc;

static const pci_class_desc pci_class_descriptions[] =
{
    { 0x0001, "VGA controller", "display"},
    { 0x0100, "SCSI controller", "scsi"},
    { 0x0101, "IDE controller", "ide"},
    { 0x0102, "Floppy controller", "fdc"},
    { 0x0103, "IPI controller", "ipi"},
    { 0x0104, "RAID controller", "raid"},
    { 0x0106, "SATA controller"},
    { 0x0107, "SAS controller"},
    { 0x0180, "Storage controller"},
    { 0x0200, "Ethernet controller", "ethernet"},
    { 0x0201, "Token Ring controller", "token-ring"},
    { 0x0202, "FDDI controller", "fddi"},
    { 0x0203, "ATM controller", "atm"},
    { 0x0280, "Network controller"},
    { 0x0300, "VGA controller", "display", 0x00ff},
    { 0x0301, "XGA controller"},
    { 0x0302, "3D controller"},
    { 0x0380, "Display controller"},
    { 0x0400, "Video controller", "video"},
    { 0x0401, "Audio controller", "sound"},
    { 0x0402, "Phone"},
    { 0x0480, "Multimedia controller"},
    { 0x0500, "RAM controller", "memory"},
    { 0x0501, "Flash controller", "flash"},
    { 0x0580, "Memory controller"},
    { 0x0600, "Host bridge", "host"},
    { 0x0601, "ISA bridge", "isa"},
    { 0x0602, "EISA bridge", "eisa"},
    { 0x0603, "MC bridge", "mca"},
    { 0x0604, "PCI bridge", "pci"},
    { 0x0605, "PCMCIA bridge", "pcmcia"},
    { 0x0606, "NUBUS bridge", "nubus"},
    { 0x0607, "CARDBUS bridge", "cardbus"},
    { 0x0608, "RACEWAY bridge"},
    { 0x0680, "Bridge"},
    { 0x0700, "Serial port", "serial"},
    { 0x0701, "Parallel port", "parallel"},
    { 0x0800, "Interrupt controller", "interrupt-controller"},
    { 0x0801, "DMA controller", "dma-controller"},
    { 0x0802, "Timer", "timer"},
    { 0x0803, "RTC", "rtc"},
    { 0x0900, "Keyboard", "keyboard"},
    { 0x0901, "Pen", "pen"},
    { 0x0902, "Mouse", "mouse"},
    { 0x0A00, "Dock station", "dock", 0x00ff},
    { 0x0B00, "i386 cpu", "cpu", 0x00ff},
    { 0x0c00, "Fireware contorller", "fireware"},
    { 0x0c01, "Access bus controller", "access-bus"},
    { 0x0c02, "SSA controller", "ssa"},
    { 0x0c03, "USB controller", "usb"},
    { 0x0c04, "Fibre channel controller", "fibre-channel"},
    { 0, NULL}
};

static void pci_for_each_device_under_bus(PCIBus *bus,
                                          void (*fn)(PCIBus *b, PCIDevice *d))
{
    PCIDevice *d;
    int devfn;

    for(devfn = 0; devfn < ARRAY_SIZE(bus->devices); devfn++) {
        d = bus->devices[devfn];
        if (d) {
            fn(bus, d);
        }
    }
}

void pci_for_each_device(PCIBus *bus, int bus_num,
                         void (*fn)(PCIBus *b, PCIDevice *d))
{
    bus = pci_find_bus(bus, bus_num);

    if (bus) {
        pci_for_each_device_under_bus(bus, fn);
    }
}

static const pci_class_desc *get_class_desc(int class)
{
    const pci_class_desc *desc;

    desc = pci_class_descriptions;
    while (desc->desc && class != desc->class) {
        desc++;
    }

    return desc;
}

static PciDeviceInfoList *qmp_query_pci_devices(PCIBus *bus, int bus_num);

static PciMemoryRegionList *qmp_query_pci_regions(const PCIDevice *dev)
{
    PciMemoryRegionList *head = NULL, *cur_item = NULL;
    int i;

    for (i = 0; i < PCI_NUM_REGIONS; i++) {
        const PCIIORegion *r = &dev->io_regions[i];
        PciMemoryRegionList *region;

        if (!r->size) {
            continue;
        }

        region = g_malloc0(sizeof(*region));
        region->value = g_malloc0(sizeof(*region->value));

        if (r->type & PCI_BASE_ADDRESS_SPACE_IO) {
            region->value->type = g_strdup("io");
        } else {
            region->value->type = g_strdup("memory");
            region->value->has_prefetch = true;
            region->value->prefetch = !!(r->type & PCI_BASE_ADDRESS_MEM_PREFETCH);
            region->value->has_mem_type_64 = true;
            region->value->mem_type_64 = !!(r->type & PCI_BASE_ADDRESS_MEM_TYPE_64);
        }

        region->value->bar = i;
        region->value->address = r->addr;
        region->value->size = r->size;

        /* XXX: waiting for the qapi to support GSList */
        if (!cur_item) {
            head = cur_item = region;
        } else {
            cur_item->next = region;
            cur_item = region;
        }
    }

    return head;
}

static PciBridgeInfo *qmp_query_pci_bridge(PCIDevice *dev, PCIBus *bus,
                                           int bus_num)
{
    PciBridgeInfo *info;

    info = g_malloc0(sizeof(*info));

    info->bus.number = dev->config[PCI_PRIMARY_BUS];
    info->bus.secondary = dev->config[PCI_SECONDARY_BUS];
    info->bus.subordinate = dev->config[PCI_SUBORDINATE_BUS];

    info->bus.io_range = g_malloc0(sizeof(*info->bus.io_range));
    info->bus.io_range->base = pci_bridge_get_base(dev, PCI_BASE_ADDRESS_SPACE_IO);
    info->bus.io_range->limit = pci_bridge_get_limit(dev, PCI_BASE_ADDRESS_SPACE_IO);

    info->bus.memory_range = g_malloc0(sizeof(*info->bus.memory_range));
    info->bus.memory_range->base = pci_bridge_get_base(dev, PCI_BASE_ADDRESS_SPACE_MEMORY);
    info->bus.memory_range->limit = pci_bridge_get_limit(dev, PCI_BASE_ADDRESS_SPACE_MEMORY);

    info->bus.prefetchable_range = g_malloc0(sizeof(*info->bus.prefetchable_range));
    info->bus.prefetchable_range->base = pci_bridge_get_base(dev, PCI_BASE_ADDRESS_MEM_PREFETCH);
    info->bus.prefetchable_range->limit = pci_bridge_get_limit(dev, PCI_BASE_ADDRESS_MEM_PREFETCH);

    if (dev->config[PCI_SECONDARY_BUS] != 0) {
        PCIBus *child_bus = pci_find_bus(bus, dev->config[PCI_SECONDARY_BUS]);
        if (child_bus) {
            info->has_devices = true;
            info->devices = qmp_query_pci_devices(child_bus, dev->config[PCI_SECONDARY_BUS]);
        }
    }

    return info;
}

static PciDeviceInfo *qmp_query_pci_device(PCIDevice *dev, PCIBus *bus,
                                           int bus_num)
{
    const pci_class_desc *desc;
    PciDeviceInfo *info;
    uint8_t type;

    int class;

    info = g_malloc0(sizeof(*info));
    info->bus = bus_num;
    info->slot = PCI_SLOT(dev->devfn);
    info->function = PCI_FUNC(dev->devfn);

    class = pci_get_word(dev->config + PCI_CLASS_DEVICE);
    info->class_info.class = class;
    desc = get_class_desc(class);
    if (desc->desc) {
        info->class_info.has_desc = true;
        info->class_info.desc = g_strdup(desc->desc);
    }

    info->id.vendor = pci_get_word(dev->config + PCI_VENDOR_ID);
    info->id.device = pci_get_word(dev->config + PCI_DEVICE_ID);
    info->regions = qmp_query_pci_regions(dev);
    info->qdev_id = g_strdup(dev->qdev.id ? dev->qdev.id : "");

    if (dev->config[PCI_INTERRUPT_PIN] != 0) {
        info->has_irq = true;
        info->irq = dev->config[PCI_INTERRUPT_LINE];
    }

    type = dev->config[PCI_HEADER_TYPE] & ~PCI_HEADER_TYPE_MULTI_FUNCTION;
    if (type == PCI_HEADER_TYPE_BRIDGE) {
        info->has_pci_bridge = true;
        info->pci_bridge = qmp_query_pci_bridge(dev, bus, bus_num);
    }

    return info;
}

static PciDeviceInfoList *qmp_query_pci_devices(PCIBus *bus, int bus_num)
{
    PciDeviceInfoList *info, *head = NULL, *cur_item = NULL;
    PCIDevice *dev;
    int devfn;

    for (devfn = 0; devfn < ARRAY_SIZE(bus->devices); devfn++) {
        dev = bus->devices[devfn];
        if (dev) {
            info = g_malloc0(sizeof(*info));
            info->value = qmp_query_pci_device(dev, bus, bus_num);

            /* XXX: waiting for the qapi to support GSList */
            if (!cur_item) {
                head = cur_item = info;
            } else {
                cur_item->next = info;
                cur_item = info;
            }
        }
    }

    return head;
}

static PciInfo *qmp_query_pci_bus(PCIBus *bus, int bus_num)
{
    PciInfo *info = NULL;

    bus = pci_find_bus(bus, bus_num);
    if (bus) {
        info = g_malloc0(sizeof(*info));
        info->bus = bus_num;
        info->devices = qmp_query_pci_devices(bus, bus_num);
    }

    return info;
}

PciInfoList *qmp_query_pci(Error **errp)
{
    PciInfoList *info, *head = NULL, *cur_item = NULL;
    struct PCIHostBus *host;


    QLIST_FOREACH(host, &host_buses, next) {
        info = g_malloc0(sizeof(*info));
        info->value = qmp_query_pci_bus(host->bus, 0);

        /* XXX: waiting for the qapi to support GSList */
        if (!cur_item) {
            head = cur_item = info;
        } else {
            cur_item->next = info;
            cur_item = info;
        }
    }

    return head;
}

static const char * const pci_nic_models[] = {
    "ne2k_pci",
    "i82551",
    "i82557b",
    "i82559er",
    "rtl8139",
    "e1000",
    "pcnet",
    "virtio",
    NULL
};

static const char * const pci_nic_names[] = {
    "ne2k_pci",
    "i82551",
    "i82557b",
    "i82559er",
    "rtl8139",
    "e1000",
    "pcnet",
    "virtio-net-pci",
    NULL
};

/* Initialize a PCI NIC.  */
/* FIXME callers should check for failure, but don't */
PCIDevice *pci_nic_init(NICInfo *nd, const char *default_model,
                        const char *default_devaddr)
{
    const char *devaddr = nd->devaddr ? nd->devaddr : default_devaddr;
    PCIBus *bus;
    int devfn;
    PCIDevice *pci_dev;
    DeviceState *dev;
    int i;

    i = qemu_find_nic_model(nd, pci_nic_models, default_model);
    if (i < 0)
        return NULL;

    bus = pci_get_bus_devfn(&devfn, devaddr);
    if (!bus) {
        error_report("Invalid PCI device address %s for device %s",
                     devaddr, pci_nic_names[i]);
        return NULL;
    }

    pci_dev = pci_create(bus, devfn, pci_nic_names[i]);
    dev = &pci_dev->qdev;
    if (nd->name)
        dev->id = qemu_strdup(nd->name);
    qdev_set_nic_properties(dev, nd);
    if (qdev_init(dev) < 0)
        return NULL;
    return pci_dev;
}

PCIDevice *pci_nic_init_nofail(NICInfo *nd, const char *default_model,
                               const char *default_devaddr)
{
    PCIDevice *res;

    if (qemu_show_nic_models(nd->model, pci_nic_models))
        exit(0);

    res = pci_nic_init(nd, default_model, default_devaddr);
    if (!res)
        exit(1);
    return res;
}

typedef struct {
    PCIDevice dev;
    PCIBus bus;
    uint32_t vid;
    uint32_t did;
} PCIBridge;


static void pci_bridge_update_mappings_fn(PCIBus *b, PCIDevice *d)
{
    pci_update_mappings(d);
}

static void pci_bridge_update_mappings(PCIBus *b)
{
    PCIBus *child;

    pci_for_each_device_under_bus(b, pci_bridge_update_mappings_fn);

    QLIST_FOREACH(child, &b->child, sibling) {
        pci_bridge_update_mappings(child);
    }
}

static void pci_bridge_write_config(PCIDevice *d,
                             uint32_t address, uint32_t val, int len)
{
    pci_default_write_config(d, address, val, len);

    if (/* io base/limit */
        ranges_overlap(address, len, PCI_IO_BASE, 2) ||

        /* memory base/limit, prefetchable base/limit and
           io base/limit upper 16 */
        ranges_overlap(address, len, PCI_MEMORY_BASE, 20)) {
        pci_bridge_update_mappings(d->bus);
    }
}

PCIBus *pci_find_bus(PCIBus *bus, int bus_num)
{
    PCIBus *sec;

    if (!bus)
        return NULL;

    if (pci_bus_num(bus) == bus_num) {
        return bus;
    }

    /* try child bus */
    QLIST_FOREACH(sec, &bus->child, sibling) {

        if (!bus->parent_dev /* pci host bridge */
            || (pci_bus_num(sec) <= bus_num &&
                bus->parent_dev->config[PCI_SUBORDINATE_BUS])) {
            return pci_find_bus(sec, bus_num);
        }
    }

    return NULL;
}

PCIDevice *pci_find_device(PCIBus *bus, int bus_num, int slot, int function)
{
    bus = pci_find_bus(bus, bus_num);

    if (!bus)
        return NULL;

    return bus->devices[PCI_DEVFN(slot, function)];
}

static int pci_bridge_initfn(PCIDevice *dev)
{
    PCIBridge *s = DO_UPCAST(PCIBridge, dev, dev);

    pci_config_set_vendor_id(s->dev.config, s->vid);
    pci_config_set_device_id(s->dev.config, s->did);

    pci_set_word(dev->config + PCI_STATUS,
                 PCI_STATUS_66MHZ | PCI_STATUS_FAST_BACK);
    pci_config_set_class(dev->config, PCI_CLASS_BRIDGE_PCI);
    dev->config[PCI_HEADER_TYPE] = PCI_HEADER_TYPE_BRIDGE;
    pci_set_word(dev->config + PCI_SEC_STATUS,
                 PCI_STATUS_66MHZ | PCI_STATUS_FAST_BACK);
    return 0;
}

static int pci_bridge_exitfn(PCIDevice *pci_dev)
{
    PCIBridge *s = DO_UPCAST(PCIBridge, dev, pci_dev);
    PCIBus *bus = &s->bus;
    pci_unregister_secondary_bus(bus);
    return 0;
}

PCIBus *pci_bridge_init(PCIBus *bus, int devfn, bool multifunction,
                        uint16_t vid, uint16_t did,
                        pci_map_irq_fn map_irq, const char *name)
{
    PCIDevice *dev;
    PCIBridge *s;

    dev = pci_create_multifunction(bus, devfn, multifunction, "pci-bridge");
    qdev_prop_set_uint32(&dev->qdev, "vendorid", vid);
    qdev_prop_set_uint32(&dev->qdev, "deviceid", did);
    qdev_init_nofail(&dev->qdev);

    s = DO_UPCAST(PCIBridge, dev, dev);
    pci_register_secondary_bus(bus, &s->bus, &s->dev, map_irq, name);
    return &s->bus;
}

PCIDevice *pci_bridge_get_device(PCIBus *bus)
{
    return bus->parent_dev;
}

static int pci_qdev_init(DeviceState *qdev, DeviceInfo *base)
{
    PCIDevice *pci_dev = (PCIDevice *)qdev;
    PCIDeviceInfo *info = container_of(base, PCIDeviceInfo, qdev);
    PCIBus *bus;
    int devfn, rc;

    /* initialize cap_present for pci_is_express() and pci_config_size() */
    if (info->is_express) {
        pci_dev->cap_present |= QEMU_PCI_CAP_EXPRESS;
    }

    bus = FROM_QBUS(PCIBus, qdev_get_parent_bus(qdev));
    devfn = pci_dev->devfn;
    pci_dev = do_pci_register_device(pci_dev, bus, base->name, devfn,
                                     info->config_read, info->config_write,
                                     info->header_type);
    if (pci_dev == NULL)
        return -1;
    if (qdev->hotplugged && info->no_hotplug) {
        qerror_report(QERR_DEVICE_NO_HOTPLUG, info->qdev.name);
        do_pci_unregister_device(pci_dev);
        return -1;
    }
    rc = info->init(pci_dev);
    if (rc != 0) {
        do_pci_unregister_device(pci_dev);
        return rc;
    }

    /* rom loading */
    if (pci_dev->romfile == NULL && info->romfile != NULL)
        pci_dev->romfile = qemu_strdup(info->romfile);
    pci_add_option_rom(pci_dev);

    if (qdev->hotplugged)
        bus->hotplug(pci_dev, 1);
    return 0;
}

static int pci_unplug_device(DeviceState *qdev)
{
    PCIDevice *dev = DO_UPCAST(PCIDevice, qdev, qdev);
    PCIDeviceInfo *info = container_of(qdev->info, PCIDeviceInfo, qdev);

    if (info->no_hotplug) {
        qerror_report(QERR_DEVICE_NO_HOTPLUG, info->qdev.name);
        return -1;
    }

    dev->bus->hotplug(dev, 0);
    return 0;
}

void pci_qdev_register(PCIDeviceInfo *info)
{
    info->qdev.init = pci_qdev_init;
    info->qdev.unplug = pci_unplug_device;
    info->qdev.exit = pci_unregister_device;
    info->qdev.bus_info = &pci_bus_info;
    qdev_register(&info->qdev);
}

void pci_qdev_register_many(PCIDeviceInfo *info)
{
    while (info->qdev.name) {
        pci_qdev_register(info);
        info++;
    }
}

PCIDevice *pci_create_multifunction(PCIBus *bus, int devfn, bool multifunction,
                                    const char *name)
{
    DeviceState *dev;

    dev = qdev_create(&bus->qbus, name);
    qdev_prop_set_uint32(dev, "addr", devfn);
    qdev_prop_set_bit(dev, "multifunction", multifunction);
    return DO_UPCAST(PCIDevice, qdev, dev);
}

PCIDevice *pci_create_simple_multifunction(PCIBus *bus, int devfn,
                                           bool multifunction,
                                           const char *name)
{
    PCIDevice *dev = pci_create_multifunction(bus, devfn, multifunction, name);
    qdev_init_nofail(&dev->qdev);
    return dev;
}

PCIDevice *pci_create(PCIBus *bus, int devfn, const char *name)
{
    return pci_create_multifunction(bus, devfn, false, name);
}

PCIDevice *pci_create_simple(PCIBus *bus, int devfn, const char *name)
{
    return pci_create_simple_multifunction(bus, devfn, false, name);
}

static int pci_find_space(PCIDevice *pdev, uint8_t size)
{
    int config_size = pci_config_size(pdev);
    int offset = PCI_CONFIG_HEADER_SIZE;
    int i;
    for (i = PCI_CONFIG_HEADER_SIZE; i < config_size; ++i)
        if (pdev->config_map[i])
            offset = i + 1;
        else if (i - offset + 1 == size)
            return offset;
    return 0;
}

static uint8_t pci_find_capability_list(PCIDevice *pdev, uint8_t cap_id,
                                        uint8_t *prev_p)
{
    uint8_t next, prev;

    if (!(pdev->config[PCI_STATUS] & PCI_STATUS_CAP_LIST))
        return 0;

    for (prev = PCI_CAPABILITY_LIST; (next = pdev->config[prev]);
         prev = next + PCI_CAP_LIST_NEXT)
        if (pdev->config[next + PCI_CAP_LIST_ID] == cap_id)
            break;

    if (prev_p)
        *prev_p = prev;
    return next;
}

void pci_map_option_rom(PCIDevice *pdev, int region_num, pcibus_t addr, pcibus_t size, int type)
{
    cpu_register_physical_memory(addr, size, pdev->rom_offset);
}

/* Add an option rom for the device */
static int pci_add_option_rom(PCIDevice *pdev)
{
    int size;
    char *path;
    void *ptr;
    char name[32];

    if (!pdev->romfile)
        return 0;
    if (strlen(pdev->romfile) == 0)
        return 0;

    if (!pdev->rom_bar) {
        int class;
        /*
         * Load rom via fw_cfg instead of creating a rom bar,
         * for 0.11 compatibility. fw_cfg is initialized at boot, so
         * we cannot do hotplug load of option roms.
         */
        if (pdev->qdev.hotplugged)
            return 0;
        class = pci_get_word(pdev->config + PCI_CLASS_DEVICE);
        if (class == 0x0300) {
            rom_add_vga(pdev->romfile);
        } else {
            rom_add_option(pdev->romfile, -1);
        }
        return 0;
    }

    path = qemu_find_file(QEMU_FILE_TYPE_BIOS, pdev->romfile);
    if (path == NULL) {
        path = qemu_strdup(pdev->romfile);
    }

    size = get_image_size(path);
    if (size < 0) {
        error_report("%s: failed to find romfile \"%s\"",
                     __FUNCTION__, pdev->romfile);
        qemu_free(path);
        return -1;
    }
    if (size & (size - 1)) {
        size = 1 << qemu_fls(size);
    }

    if (pdev->qdev.info->vmsd)
        snprintf(name, sizeof(name), "%s.rom", pdev->qdev.info->vmsd->name);
    else
        snprintf(name, sizeof(name), "%s.rom", pdev->qdev.info->name);
    pdev->rom_offset = qemu_ram_alloc(&pdev->qdev, name, size);

    ptr = qemu_get_ram_ptr(pdev->rom_offset);
    load_image(path, ptr);
    qemu_free(path);

    pci_register_bar(pdev, PCI_ROM_SLOT, size,
                     0, pci_map_option_rom);

    return 0;
}

static void pci_del_option_rom(PCIDevice *pdev)
{
    if (!pdev->rom_offset)
        return;

    qemu_ram_free(pdev->rom_offset);
    pdev->rom_offset = 0;
}

/*
 * if !offset
 * Reserve space and add capability to the linked list in pci config space
 *
 * if offset = 0,
 * Find and reserve space and add capability to the linked list
 * in pci config space */
int pci_add_capability(PCIDevice *pdev, uint8_t cap_id,
                       uint8_t offset, uint8_t size)
{
    uint8_t *config;
    if (!offset) {
        offset = pci_find_space(pdev, size);
        if (!offset) {
            return -ENOSPC;
        }
    } else {
        int i;

        for (i = offset; i < offset + size; i++) {
            if (pdev->config_map[i]) {
                fprintf(stderr, "ERROR: %04x:%02x:%02x.%x "
                        "Attempt to add PCI capability %x at offset "
                        "%x overlaps existing capability %x at offset %x\n",
                        /* pci_find_domain(pdev->bus) */ 0, pci_bus_num(pdev->bus),
                        PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn),
                        cap_id, offset, pdev->config_map[i], i);
                return -EFAULT;
            }
        }
    }

    config = pdev->config + offset;
    config[PCI_CAP_LIST_ID] = cap_id;
    config[PCI_CAP_LIST_NEXT] = pdev->config[PCI_CAPABILITY_LIST];
    pdev->config[PCI_CAPABILITY_LIST] = offset;
    memset(pdev->config_map + offset, cap_id, size);
    /* Make capability read-only by default */
    memset(pdev->wmask + offset, 0, size);
    /* Check capability by default */
    memset(pdev->cmask + offset, 0xFF, size);

    pdev->config[PCI_STATUS] |= PCI_STATUS_CAP_LIST;

    return offset;
}

/* Unlink capability from the pci config space. */
void pci_del_capability(PCIDevice *pdev, uint8_t cap_id, uint8_t size)
{
    uint8_t prev, offset = pci_find_capability_list(pdev, cap_id, &prev);
    if (!offset)
        return;
    pdev->config[prev] = pdev->config[offset + PCI_CAP_LIST_NEXT];
    /* Make capability writeable again */
    memset(pdev->wmask + offset, 0xff, size);
    /* Clear cmask as device-specific registers can't be checked */
    memset(pdev->cmask + offset, 0, size);
    memset(pdev->config_map + offset, 0, size);

    if (!pdev->config[PCI_CAPABILITY_LIST]) {
        pdev->config[PCI_STATUS] &= ~PCI_STATUS_CAP_LIST;
    }
}

uint8_t pci_find_capability(PCIDevice *pdev, uint8_t cap_id)
{
    return pci_find_capability_list(pdev, cap_id, NULL);
}

static void pcibus_dev_print(Monitor *mon, DeviceState *dev, int indent)
{
    PCIDevice *d = (PCIDevice *)dev;
    const pci_class_desc *desc;
    char ctxt[64];
    PCIIORegion *r;
    int i, class;

    class = pci_get_word(d->config + PCI_CLASS_DEVICE);
    desc = pci_class_descriptions;
    while (desc->desc && class != desc->class)
        desc++;
    if (desc->desc) {
        snprintf(ctxt, sizeof(ctxt), "%s", desc->desc);
    } else {
        snprintf(ctxt, sizeof(ctxt), "Class %04x", class);
    }

    monitor_printf(mon, "%*sclass %s, addr %02x:%02x.%x, "
                   "pci id %04x:%04x (sub %04x:%04x)\n",
                   indent, "", ctxt, pci_bus_num(d->bus),
                   PCI_SLOT(d->devfn), PCI_FUNC(d->devfn),
                   pci_get_word(d->config + PCI_VENDOR_ID),
                   pci_get_word(d->config + PCI_DEVICE_ID),
                   pci_get_word(d->config + PCI_SUBSYSTEM_VENDOR_ID),
                   pci_get_word(d->config + PCI_SUBSYSTEM_ID));
    for (i = 0; i < PCI_NUM_REGIONS; i++) {
        r = &d->io_regions[i];
        if (!r->size)
            continue;
        monitor_printf(mon, "%*sbar %d: %s at 0x%"FMT_PCIBUS
                       " [0x%"FMT_PCIBUS"]\n",
                       indent, "",
                       i, r->type & PCI_BASE_ADDRESS_SPACE_IO ? "i/o" : "mem",
                       r->addr, r->addr + r->size - 1);
    }
}

static char *pci_dev_fw_name(DeviceState *dev, char *buf, int len)
{
    PCIDevice *d = (PCIDevice *)dev;
    const char *name = NULL;
    const pci_class_desc *desc =  pci_class_descriptions;
    int class = pci_get_word(d->config + PCI_CLASS_DEVICE);

    while (desc->desc &&
          (class & ~desc->fw_ign_bits) !=
          (desc->class & ~desc->fw_ign_bits)) {
        desc++;
    }

    if (desc->desc) {
        name = desc->fw_name;
    }

    if (name) {
        pstrcpy(buf, len, name);
    } else {
        snprintf(buf, len, "pci%04x,%04x",
                 pci_get_word(d->config + PCI_VENDOR_ID),
                 pci_get_word(d->config + PCI_DEVICE_ID));
    }

    return buf;
}

static char *pcibus_get_fw_dev_path(DeviceState *dev)
{
    PCIDevice *d = (PCIDevice *)dev;
    char path[50], name[33];
    int off;

    off = snprintf(path, sizeof(path), "%s@%x",
                   pci_dev_fw_name(dev, name, sizeof name),
                   PCI_SLOT(d->devfn));
    if (PCI_FUNC(d->devfn))
        snprintf(path + off, sizeof(path) + off, ",%x", PCI_FUNC(d->devfn));
    return strdup(path);
}

static char *pcibus_get_dev_path(DeviceState *dev)
{
    PCIDevice *d = (PCIDevice *)dev;
    char path[16];

    snprintf(path, sizeof(path), "%04x:%02x:%02x.%x",
             /* pci_find_domain(d->bus) */ 0, d->config[PCI_SECONDARY_BUS],
             PCI_SLOT(d->devfn), PCI_FUNC(d->devfn));

    return strdup(path);
}

static PCIDeviceInfo bridge_info = {
    .qdev.name    = "pci-bridge",
    .qdev.size    = sizeof(PCIBridge),
    .init         = pci_bridge_initfn,
    .exit         = pci_bridge_exitfn,
    .config_write = pci_bridge_write_config,
    .qdev.props   = (Property[]) {
        DEFINE_PROP_HEX32("vendorid", PCIBridge, vid, 0),
        DEFINE_PROP_HEX32("deviceid", PCIBridge, did, 0),
        DEFINE_PROP_END_OF_LIST(),
    }
};

static void pci_register_devices(void)
{
    pci_qdev_register(&bridge_info);
}

device_init(pci_register_devices)
