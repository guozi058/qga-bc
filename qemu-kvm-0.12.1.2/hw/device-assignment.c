/*
 * Copyright (c) 2007, Neocleus Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 *
 *
 *  Assign a PCI device from the host to a guest VM.
 *
 *  Adapted for KVM by Qumranet.
 *
 *  Copyright (c) 2007, Neocleus, Alex Novik (alex@neocleus.com)
 *  Copyright (c) 2007, Neocleus, Guy Zana (guy@neocleus.com)
 *  Copyright (C) 2008, Qumranet, Amit Shah (amit.shah@qumranet.com)
 *  Copyright (C) 2008, Red Hat, Amit Shah (amit.shah@redhat.com)
 *  Copyright (C) 2008, IBM, Muli Ben-Yehuda (muli@il.ibm.com)
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/io.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "qemu-kvm.h"
#include "qemu-error.h"
#include "hw.h"
#include "pc.h"
#include "console.h"
#include "device-assignment.h"
#include "loader.h"
#include "monitor.h"
#include "range.h"
#include <pci/header.h>
#include "sysemu.h"

/* From linux/ioport.h */
#define IORESOURCE_IO       0x00000100  /* Resource type */
#define IORESOURCE_MEM      0x00000200
#define IORESOURCE_IRQ      0x00000400
#define IORESOURCE_DMA      0x00000800
#define IORESOURCE_PREFETCH 0x00001000  /* No side effects */

/* #define DEVICE_ASSIGNMENT_DEBUG 1 */

#ifdef DEVICE_ASSIGNMENT_DEBUG
#define DEBUG(fmt, ...)                                       \
    do {                                                      \
      fprintf(stderr, "%s: " fmt, __func__ , __VA_ARGS__);    \
    } while (0)
#else
#define DEBUG(fmt, ...) do { } while(0)
#endif

static void assigned_dev_load_option_rom(AssignedDevice *dev);

static void assigned_dev_unregister_msix_mmio(AssignedDevice *dev);

static void assigned_device_pci_cap_write_config(PCIDevice *pci_dev,
                                                 uint32_t address,
                                                 uint32_t val, int len);

static uint32_t assigned_device_pci_cap_read_config(PCIDevice *pci_dev,
                                                    uint32_t address, int len);

static uint32_t assigned_dev_ioport_rw(AssignedDevRegion *dev_region,
                                       uint32_t addr, int len, uint32_t *val)
{
    uint32_t ret = 0;
    uint32_t offset = addr - dev_region->e_physbase;
    int fd = dev_region->region->resource_fd;

    if (fd >= 0) {
        if (val) {
            DEBUG("pwrite val=%x, len=%d, e_phys=%x, offset=%x\n",
                  *val, len, addr, offset);
            if (pwrite(fd, val, len, offset) != len) {
                fprintf(stderr, "%s - pwrite failed %s\n",
                        __func__, strerror(errno));
            }
        } else {
            if (pread(fd, &ret, len, offset) != len) {
                fprintf(stderr, "%s - pread failed %s\n",
                        __func__, strerror(errno));
                ret = (1UL << (len * 8)) - 1;
            }
            DEBUG("pread ret=%x, len=%d, e_phys=%x, offset=%x\n",
                  ret, len, addr, offset);
        }
    } else {
        uint32_t port = offset + dev_region->u.r_baseport;

        if (val) {
            DEBUG("out val=%x, len=%d, e_phys=%x, host=%x\n",
                  *val, len, addr, port);
            switch (len) {
                case 1:
                    outb(*val, port);
                    break;
                case 2:
                    outw(*val, port);
                    break;
                case 4:
                    outl(*val, port);
                    break;
            }
        } else {
            switch (len) {
                case 1:
                    ret = inb(port);
                    break;
                case 2:
                    ret = inw(port);
                    break;
                case 4:
                    ret = inl(port);
                    break;
            }
            DEBUG("in val=%x, len=%d, e_phys=%x, host=%x\n",
                  ret, len, addr, port);
        }
    }
    return ret;
}

static void assigned_dev_ioport_writeb(void *opaque, uint32_t addr,
                                       uint32_t value)
{
    assigned_dev_ioport_rw(opaque, addr, 1, &value);
    return;
}

static void assigned_dev_ioport_writew(void *opaque, uint32_t addr,
                                       uint32_t value)
{
    assigned_dev_ioport_rw(opaque, addr, 2, &value);
    return;
}

static void assigned_dev_ioport_writel(void *opaque, uint32_t addr,
                       uint32_t value)
{
    assigned_dev_ioport_rw(opaque, addr, 4, &value);
    return;
}

static uint32_t assigned_dev_ioport_readb(void *opaque, uint32_t addr)
{
    return assigned_dev_ioport_rw(opaque, addr, 1, NULL);
}

static uint32_t assigned_dev_ioport_readw(void *opaque, uint32_t addr)
{
    return assigned_dev_ioport_rw(opaque, addr, 2, NULL);
}

static uint32_t assigned_dev_ioport_readl(void *opaque, uint32_t addr)
{
    return assigned_dev_ioport_rw(opaque, addr, 4, NULL);
}

static uint32_t slow_bar_readb(void *opaque, target_phys_addr_t addr)
{
    AssignedDevRegion *d = opaque;
    uint8_t *in = d->u.r_virtbase + addr;
    uint32_t r;

    r = *in;
    DEBUG("slow_bar_readl addr=0x" TARGET_FMT_plx " val=0x%08x\n", addr, r);

    return r;
}

static uint32_t slow_bar_readw(void *opaque, target_phys_addr_t addr)
{
    AssignedDevRegion *d = opaque;
    uint16_t *in = d->u.r_virtbase + addr;
    uint32_t r;

    r = *in;
    DEBUG("slow_bar_readl addr=0x" TARGET_FMT_plx " val=0x%08x\n", addr, r);

    return r;
}

static uint32_t slow_bar_readl(void *opaque, target_phys_addr_t addr)
{
    AssignedDevRegion *d = opaque;
    uint32_t *in = d->u.r_virtbase + addr;
    uint32_t r;

    r = *in;
    DEBUG("slow_bar_readl addr=0x" TARGET_FMT_plx " val=0x%08x\n", addr, r);

    return r;
}

static void slow_bar_writeb(void *opaque, target_phys_addr_t addr, uint32_t val)
{
    AssignedDevRegion *d = opaque;
    uint8_t *out = d->u.r_virtbase + addr;

    DEBUG("slow_bar_writeb addr=0x" TARGET_FMT_plx " val=0x%02x\n", addr, val);
    *out = val;
}

static void slow_bar_writew(void *opaque, target_phys_addr_t addr, uint32_t val)
{
    AssignedDevRegion *d = opaque;
    uint16_t *out = d->u.r_virtbase + addr;

    DEBUG("slow_bar_writew addr=0x" TARGET_FMT_plx " val=0x%04x\n", addr, val);
    *out = val;
}

static void slow_bar_writel(void *opaque, target_phys_addr_t addr, uint32_t val)
{
    AssignedDevRegion *d = opaque;
    uint32_t *out = d->u.r_virtbase + addr;

    DEBUG("slow_bar_writel addr=0x" TARGET_FMT_plx " val=0x%08x\n", addr, val);
    *out = val;
}

static CPUWriteMemoryFunc * const slow_bar_write[] = {
    &slow_bar_writeb,
    &slow_bar_writew,
    &slow_bar_writel
};

static CPUReadMemoryFunc * const slow_bar_read[] = {
    &slow_bar_readb,
    &slow_bar_readw,
    &slow_bar_readl
};

static void assigned_dev_iomem_map_slow(PCIDevice *pci_dev, int region_num,
                                        pcibus_t e_phys, pcibus_t e_size,
                                        int type)
{
    AssignedDevice *r_dev = container_of(pci_dev, AssignedDevice, dev);
    AssignedDevRegion *region = &r_dev->v_addrs[region_num];
    PCIRegion *real_region = &r_dev->real_device.regions[region_num];

    DEBUG("%s", "slow map\n");
    cpu_register_physical_memory(e_phys, e_size, region->mmio_index);

    /* MSI-X MMIO page */
    if ((e_size > 0) &&
        real_region->base_addr <= r_dev->msix_table_addr &&
        real_region->base_addr + real_region->size >= r_dev->msix_table_addr) {
        int offset = r_dev->msix_table_addr - real_region->base_addr;

        cpu_register_physical_memory(e_phys + offset,
                TARGET_PAGE_SIZE, r_dev->mmio_index);
    }
}

static void assigned_dev_iomem_map(PCIDevice *pci_dev, int region_num,
                                   pcibus_t e_phys, pcibus_t e_size, int type)
{
    AssignedDevice *r_dev = container_of(pci_dev, AssignedDevice, dev);
    AssignedDevRegion *region = &r_dev->v_addrs[region_num];
    PCIRegion *real_region = &r_dev->real_device.regions[region_num];
    pcibus_t old_ephys = region->e_physbase;
    pcibus_t old_esize = region->e_size;
    int first_map = (region->e_size == 0);
    int ret = 0;

    DEBUG("e_phys=%08" FMT_PCIBUS " r_virt=%p type=%d len=%08" FMT_PCIBUS " region_num=%d \n",
          e_phys, region->u.r_virtbase, type, e_size, region_num);

    region->e_physbase = e_phys;
    region->e_size = e_size;

    if (e_size > 0) {
        /* deal with MSI-X MMIO page */
        if (real_region->base_addr <= r_dev->msix_table_addr &&
                real_region->base_addr + real_region->size >=
                r_dev->msix_table_addr) {

            int offset = r_dev->msix_table_addr - real_region->base_addr;

            cpu_register_physical_memory(e_phys + offset,
                                         TARGET_PAGE_SIZE, r_dev->mmio_index);

            if (offset > 0) {
                if (!first_map)
                    kvm_destroy_phys_mem(kvm_context, old_ephys,
                                         TARGET_PAGE_ALIGN(offset));

                ret = kvm_register_phys_mem(kvm_context, e_phys,
                                            region->u.r_virtbase,
                                            TARGET_PAGE_ALIGN(offset), 0);
                if (ret != 0)
                    goto out;
            }

            if (e_size >  offset + TARGET_PAGE_SIZE) {
                if (!first_map)
                    kvm_destroy_phys_mem(kvm_context,
                                         old_ephys + offset + TARGET_PAGE_SIZE,
                                         TARGET_PAGE_ALIGN(e_size - offset -
                                                           TARGET_PAGE_SIZE));

                ret = kvm_register_phys_mem(kvm_context,
                                            e_phys + offset + TARGET_PAGE_SIZE,
                                            region->u.r_virtbase + offset +
                                            TARGET_PAGE_SIZE,
                                            TARGET_PAGE_ALIGN(e_size - offset -
                                                              TARGET_PAGE_SIZE),
                                            0);
                if (ret != 0)
                    goto out;
            }

        } else {

            if (!first_map)
                kvm_destroy_phys_mem(kvm_context, old_ephys,
                                     TARGET_PAGE_ALIGN(old_esize));

            ret = kvm_register_phys_mem(kvm_context, e_phys,
                                        region->u.r_virtbase,
                                        TARGET_PAGE_ALIGN(e_size), 0);
        }
    }

out:
    if (ret != 0) {
	fprintf(stderr, "%s: Error: create new mapping failed\n", __func__);
	exit(1);
    }
}

static void assigned_dev_ioport_map(PCIDevice *pci_dev, int region_num,
                                    pcibus_t addr, pcibus_t size, int type)
{
    AssignedDevice *r_dev = container_of(pci_dev, AssignedDevice, dev);
    AssignedDevRegion *region = &r_dev->v_addrs[region_num];
    int first_map = (region->e_size == 0);
    CPUState *env;

    region->e_physbase = addr;
    region->e_size = size;

    DEBUG("e_phys=0x%" FMT_PCIBUS " r_baseport=%x type=0x%x len=%" FMT_PCIBUS " region_num=%d \n",
          addr, region->u.r_baseport, type, size, region_num);

    if (first_map && region->region->resource_fd < 0) {
	struct ioperm_data *data;

	data = qemu_mallocz(sizeof(struct ioperm_data));
	if (data == NULL) {
	    fprintf(stderr, "%s: Out of memory\n", __func__);
	    exit(1);
	}

	data->start_port = region->u.r_baseport;
	data->num = region->r_size;
	data->turn_on = 1;

	kvm_add_ioperm_data(data);

	for (env = first_cpu; env; env = env->next_cpu)
	    kvm_ioperm(env, data);
    }

    register_ioport_read(addr, size, 1, assigned_dev_ioport_readb,
                         (r_dev->v_addrs + region_num));
    register_ioport_read(addr, size, 2, assigned_dev_ioport_readw,
                         (r_dev->v_addrs + region_num));
    register_ioport_read(addr, size, 4, assigned_dev_ioport_readl,
                         (r_dev->v_addrs + region_num));
    register_ioport_write(addr, size, 1, assigned_dev_ioport_writeb,
                          (r_dev->v_addrs + region_num));
    register_ioport_write(addr, size, 2, assigned_dev_ioport_writew,
                          (r_dev->v_addrs + region_num));
    register_ioport_write(addr, size, 4, assigned_dev_ioport_writel,
                          (r_dev->v_addrs + region_num));
}

static uint32_t assigned_dev_pci_read(PCIDevice *d, int pos, int len)
{
    AssignedDevice *pci_dev = container_of(d, AssignedDevice, dev);
    uint32_t val;
    ssize_t ret;
    int fd = pci_dev->real_device.config_fd;

again:
    ret = pread(fd, &val, len, pos);
    if (ret != len) {
	if ((ret < 0) && (errno == EINTR || errno == EAGAIN))
	    goto again;

	fprintf(stderr, "%s: pread failed, ret = %zd errno = %d\n",
		__func__, ret, errno);

	exit(1);
    }

    return val;
}

static uint8_t assigned_dev_pci_read_byte(PCIDevice *d, int pos)
{
    return (uint8_t)assigned_dev_pci_read(d, pos, 1);
}

static void assigned_dev_pci_write(PCIDevice *d, int pos, uint32_t val, int len)
{
    AssignedDevice *pci_dev = container_of(d, AssignedDevice, dev);
    ssize_t ret;
    int fd = pci_dev->real_device.config_fd;

again:
    ret = pwrite(fd, &val, len, pos);
    if (ret != len) {
	if ((ret < 0) && (errno == EINTR || errno == EAGAIN))
	    goto again;

	fprintf(stderr, "%s: pwrite failed, ret = %zd errno = %d\n",
		__func__, ret, errno);

	exit(1);
    }

    return;
}

static uint8_t pci_find_cap_offset(PCIDevice *d, uint8_t cap, uint8_t start)
{
    int id;
    int max_cap = 48;
    int pos = start ? start : PCI_CAPABILITY_LIST;
    int status;

    status = assigned_dev_pci_read_byte(d, PCI_STATUS);
    if ((status & PCI_STATUS_CAP_LIST) == 0)
        return 0;

    while (max_cap--) {
        pos = assigned_dev_pci_read_byte(d, pos);
        if (pos < 0x40)
            break;

        pos &= ~3;
        id = assigned_dev_pci_read_byte(d, pos + PCI_CAP_LIST_ID);

        if (id == 0xff)
            break;
        if (id == cap)
            return pos;

        pos += PCI_CAP_LIST_NEXT;
    }
    return 0;
}

static void assigned_dev_pci_write_config(PCIDevice *d, uint32_t address,
                                          uint32_t val, int len)
{
    int fd;
    ssize_t ret;
    AssignedDevice *pci_dev = container_of(d, AssignedDevice, dev);

    DEBUG("(%x.%x): address=%04x val=0x%08x len=%d\n",
          ((d->devfn >> 3) & 0x1F), (d->devfn & 0x7),
          (uint16_t) address, val, len);

    if (address >= PCI_CONFIG_HEADER_SIZE && d->config_map[address]) {
        return assigned_device_pci_cap_write_config(d, address, val, len);
    }

    if (address == 0x4) {
        pci_default_write_config(d, address, val, len);
        /* Continue to program the card */
    }

    if ((address >= 0x10 && address <= 0x24) || address == 0x30 ||
        address == 0x34 || address == 0x3c || address == 0x3d) {
        /* used for update-mappings (BAR emulation) */
        pci_default_write_config(d, address, val, len);
        return;
    }

    DEBUG("NON BAR (%x.%x): address=%04x val=0x%08x len=%d\n",
          ((d->devfn >> 3) & 0x1F), (d->devfn & 0x7),
          (uint16_t) address, val, len);

    fd = pci_dev->real_device.config_fd;

again:
    ret = pwrite(fd, &val, len, address);
    if (ret != len) {
	if ((ret < 0) && (errno == EINTR || errno == EAGAIN))
	    goto again;

	fprintf(stderr, "%s: pwrite failed, ret = %zd errno = %d\n",
		__func__, ret, errno);

	exit(1);
    }
}

static uint32_t merge_bits(uint32_t val, uint32_t mval, uint8_t addr,
                           int len, uint8_t pos, uint32_t mask);

static uint32_t assigned_dev_pci_read_config(PCIDevice *d, uint32_t address,
                                             int len)
{
    uint32_t val = 0;
    int fd;
    ssize_t ret;
    AssignedDevice *pci_dev = container_of(d, AssignedDevice, dev);

    if (address >= PCI_CONFIG_HEADER_SIZE && d->config_map[address]) {
        val = assigned_device_pci_cap_read_config(d, address, len);
        DEBUG("(%x.%x): address=%04x val=0x%08x len=%d\n",
              (d->devfn >> 3) & 0x1F, (d->devfn & 0x7), address, val, len);
        return val;
    }

    if (address < 0x4 || (pci_dev->need_emulate_cmd && address == 0x4) ||
	(address >= 0x10 && address <= 0x24) || address == 0x30 ||
        address == 0x34 || address == 0x3c || address == 0x3d) {
        val = pci_default_read_config(d, address, len);
        DEBUG("(%x.%x): address=%04x val=0x%08x len=%d\n",
              (d->devfn >> 3) & 0x1F, (d->devfn & 0x7), address, val, len);
        return val;
    }

    /* vga specific, remove later */
    if (address == 0xFC)
        goto do_log;

    fd = pci_dev->real_device.config_fd;

again:
    ret = pread(fd, &val, len, address);
    if (ret != len) {
	if ((ret < 0) && (errno == EINTR || errno == EAGAIN))
	    goto again;

	fprintf(stderr, "%s: pread failed, ret = %zd errno = %d\n",
		__func__, ret, errno);

	exit(1);
    }

do_log:
    DEBUG("(%x.%x): address=%04x val=0x%08x len=%d\n",
          (d->devfn >> 3) & 0x1F, (d->devfn & 0x7), address, val, len);

    if (!pci_dev->cap.available) {
        /* kill the special capabilities */
        if (address == 4 && len == 4)
            val &= ~0x100000;
        else if (address == 6)
            val &= ~0x10;
    }

    /* Use emulated multifunction header type bit */
    val = merge_bits(val, pci_get_long(d->config + address), address,
                     len, PCI_HEADER_TYPE, PCI_HEADER_TYPE_MULTI_FUNCTION);

    return val;
}

static int assigned_dev_register_regions(PCIRegion *io_regions,
                                         unsigned long regions_num,
                                         AssignedDevice *pci_dev)
{
    uint32_t i;
    PCIRegion *cur_region = io_regions;

    for (i = 0; i < regions_num; i++, cur_region++) {
        if (!cur_region->valid)
            continue;
        pci_dev->v_addrs[i].num = i;

        /* handle memory io regions */
        if (cur_region->type & IORESOURCE_MEM) {
            int slow_map = 0;
            int t = cur_region->type & IORESOURCE_PREFETCH
                ? PCI_BASE_ADDRESS_MEM_PREFETCH
                : PCI_BASE_ADDRESS_SPACE_MEMORY;

            if (cur_region->size & 0xFFF) {
                fprintf(stderr, "PCI region %d at address 0x%llx "
                        "has size 0x%x, which is not a multiple of 4K. "
                        "You might experience some performance hit "
                        "due to that.\n",
                        i, (unsigned long long)cur_region->base_addr,
                        cur_region->size);
                slow_map = 1;

                pci_dev->v_addrs[i].mmio_index =
                                 cpu_register_io_memory(slow_bar_read,
                                                        slow_bar_write,
                                                        &pci_dev->v_addrs[i]);
                if (pci_dev->v_addrs[i].mmio_index < 0) {
                    fprintf(stderr, "%s: Error registering IO memory\n",
                            __func__);
                    return -1;
                }
            }

            /* map physical memory */
            pci_dev->v_addrs[i].e_physbase = cur_region->base_addr;
            pci_dev->v_addrs[i].u.r_virtbase = mmap(NULL, cur_region->size,
                                                    PROT_WRITE | PROT_READ,
                                                    MAP_SHARED,
                                                    cur_region->resource_fd,
                                                    (off_t)0);

            if (pci_dev->v_addrs[i].u.r_virtbase == MAP_FAILED) {
                pci_dev->v_addrs[i].u.r_virtbase = NULL;
                fprintf(stderr, "%s: Error: Couldn't mmap 0x%x!"
                        "\n", __func__,
                        (uint32_t) (cur_region->base_addr));

                if (slow_map) {
                    cpu_unregister_io_memory(pci_dev->v_addrs[i].mmio_index);
                    pci_dev->v_addrs[i].mmio_index = -1;
                }

                return -1;
            }

            pci_dev->v_addrs[i].r_size = cur_region->size;
            pci_dev->v_addrs[i].e_size = 0;

            /* add offset */
            pci_dev->v_addrs[i].u.r_virtbase +=
                (cur_region->base_addr & 0xFFF);

            pci_register_bar((PCIDevice *) pci_dev, i,
                             cur_region->size, t,
                             slow_map ? assigned_dev_iomem_map_slow
                                      : assigned_dev_iomem_map);
            continue;
        } else {
            /* handle port io regions */
            uint32_t val;
            int ret;

            /* Test kernel support for ioport resource read/write.  Old
             * kernels return EIO.  New kernels only allow 1/2/4 byte reads
             * so should return EINVAL for a 3 byte read */
            ret = pread(pci_dev->v_addrs[i].region->resource_fd, &val, 3, 0);
            if (ret >= 0) {
                fprintf(stderr, "Unexpected return from I/O port read: %d\n",
                        ret);
                abort();
            } else if (errno != EINVAL) {
                fprintf(stderr, "Using raw in/out ioport access (sysfs - %s)\n",
                        strerror(errno));
                close(pci_dev->v_addrs[i].region->resource_fd);
                pci_dev->v_addrs[i].region->resource_fd = -1;
            }

            pci_dev->v_addrs[i].e_physbase = cur_region->base_addr;
            pci_dev->v_addrs[i].u.r_baseport = cur_region->base_addr;
            pci_dev->v_addrs[i].r_size = cur_region->size;
            pci_dev->v_addrs[i].e_size = 0;

            pci_register_bar((PCIDevice *) pci_dev, i,
                             cur_region->size, PCI_BASE_ADDRESS_SPACE_IO,
                             assigned_dev_ioport_map);

            /* not relevant for port io */
            pci_dev->v_addrs[i].memory_index = 0;
        }
    }

    /* success */
    return 0;
}

static int get_real_device(AssignedDevice *pci_dev, uint16_t r_seg,
			   uint8_t r_bus, uint8_t r_dev, uint8_t r_func)
{
    char dir[128], name[128];
    int fd, r = 0;
    FILE *f;
    unsigned long long start, end, size, flags;
    unsigned long id;
    struct stat statbuf;
    PCIRegion *rp;
    PCIDevRegions *dev = &pci_dev->real_device;

    dev->region_number = 0;

    snprintf(dir, sizeof(dir), "/sys/bus/pci/devices/%04x:%02x:%02x.%x/",
	     r_seg, r_bus, r_dev, r_func);

    snprintf(name, sizeof(name), "%sconfig", dir);

    if (pci_dev->configfd_name && *pci_dev->configfd_name) {
        if (qemu_isdigit(pci_dev->configfd_name[0])) {
            dev->config_fd = strtol(pci_dev->configfd_name, NULL, 0);
        } else {
            dev->config_fd = monitor_get_fd(cur_mon, pci_dev->configfd_name);
            if (dev->config_fd < 0) {
                fprintf(stderr, "%s: (%s) unkown\n", __func__,
                        pci_dev->configfd_name);
                return 1;
            }
        }
    } else {
        dev->config_fd = open(name, O_RDWR);

        if (dev->config_fd == -1) {
            fprintf(stderr, "%s: %s: %m\n", __func__, name);
            return 1;
        }
    }
again:
    r = read(dev->config_fd, pci_dev->dev.config,
             pci_config_size(&pci_dev->dev));
    if (r < 0) {
        if (errno == EINTR || errno == EAGAIN)
            goto again;
        fprintf(stderr, "%s: read failed, errno = %d\n", __func__, errno);
    }

    /* Restore or clear multifunction, this is always controlled by qemu */
    if (pci_dev->dev.cap_present & QEMU_PCI_CAP_MULTIFUNCTION) {
        pci_dev->dev.config[PCI_HEADER_TYPE] |= PCI_HEADER_TYPE_MULTI_FUNCTION;
    } else {
        pci_dev->dev.config[PCI_HEADER_TYPE] &= ~PCI_HEADER_TYPE_MULTI_FUNCTION;
    }

    /* Clear host resource mapping info.  If we choose not to register a
     * BAR, such as might be the case with the option ROM, we can get
     * confusing, unwritable, residual addresses from the host here. */
    memset(&pci_dev->dev.config[PCI_BASE_ADDRESS_0], 0, 24);
    memset(&pci_dev->dev.config[PCI_ROM_ADDRESS], 0, 4);

    snprintf(name, sizeof(name), "%sresource", dir);

    f = fopen(name, "r");
    if (f == NULL) {
        fprintf(stderr, "%s: %s: %m\n", __func__, name);
        return 1;
    }

    for (r = 0; r < PCI_ROM_SLOT; r++) {
	if (fscanf(f, "%lli %lli %lli\n", &start, &end, &flags) != 3)
	    break;

        rp = dev->regions + r;
        rp->valid = 0;
        rp->resource_fd = -1;
        size = end - start + 1;
        flags &= IORESOURCE_IO | IORESOURCE_MEM | IORESOURCE_PREFETCH;
        if (size == 0 || (flags & ~IORESOURCE_PREFETCH) == 0)
            continue;
        if (flags & IORESOURCE_MEM) {
            flags &= ~IORESOURCE_IO;
        } else {
            flags &= ~IORESOURCE_PREFETCH;
        }
        snprintf(name, sizeof(name), "%sresource%d", dir, r);
        fd = open(name, O_RDWR);
        if (fd == -1)
            continue;
        rp->resource_fd = fd;

        rp->type = flags;
        rp->valid = 1;
        rp->base_addr = start;
        rp->size = size;
        pci_dev->v_addrs[r].region = rp;
        pci_dev->v_addrs[r].mmio_index = -1;
        DEBUG("region %d size %d start 0x%llx type %d resource_fd %d\n",
              r, rp->size, start, rp->type, rp->resource_fd);
    }

    fclose(f);

    /* read and fill device ID */
    snprintf(name, sizeof(name), "%svendor", dir);
    f = fopen(name, "r");
    if (f == NULL) {
        fprintf(stderr, "%s: %s: %m\n", __func__, name);
        return 1;
    }
    if (fscanf(f, "%li\n", &id) == 1) {
	pci_dev->dev.config[0] = id & 0xff;
	pci_dev->dev.config[1] = (id & 0xff00) >> 8;
    }
    fclose(f);

    /* read and fill vendor ID */
    snprintf(name, sizeof(name), "%sdevice", dir);
    f = fopen(name, "r");
    if (f == NULL) {
        fprintf(stderr, "%s: %s: %m\n", __func__, name);
        return 1;
    }
    if (fscanf(f, "%li\n", &id) == 1) {
	pci_dev->dev.config[2] = id & 0xff;
	pci_dev->dev.config[3] = (id & 0xff00) >> 8;
    }
    fclose(f);

    /* dealing with virtual function device */
    snprintf(name, sizeof(name), "%sphysfn/", dir);
    if (!stat(name, &statbuf))
	    pci_dev->need_emulate_cmd = 1;
    else
	    pci_dev->need_emulate_cmd = 0;

    dev->region_number = r;
    return 0;
}

static QLIST_HEAD(, AssignedDevice) devs = QLIST_HEAD_INITIALIZER(devs);
static Notifier suspend_notifier;
static QEMUBH *suspend_bh;

#ifdef KVM_CAP_IRQ_ROUTING
static void free_dev_irq_entries(AssignedDevice *dev)
{
    int i;

    for (i = 0; i < dev->irq_entries_nr; i++) {
        if (dev->entry[i].type) {
            kvm_del_routing_entry(kvm_context, &dev->entry[i]);
        }
    }
    free(dev->entry);
    dev->entry = NULL;
    dev->irq_entries_nr = 0;
}
#endif

static void free_assigned_device(AssignedDevice *dev)
{
    if (dev) {
        int i;

        for (i = 0; i < dev->real_device.region_number; i++) {
            PCIRegion *pci_region = &dev->real_device.regions[i];
            AssignedDevRegion *region = &dev->v_addrs[i];

            if (!pci_region->valid)
                continue;

            if (pci_region->type & IORESOURCE_IO) {
                if (pci_region->resource_fd < 0) {
                    kvm_remove_ioperm_data(region->u.r_baseport,
                                           region->r_size);
                }
            } else if (pci_region->type & IORESOURCE_MEM) {
                if (region->e_size) {
                    if (pci_region->base_addr <= dev->msix_table_addr &&
                        pci_region->base_addr + pci_region->size >=
                        dev->msix_table_addr) {

                        int offset = dev->msix_table_addr -
                                     pci_region->base_addr;

                        if (offset > 0) {
                            kvm_destroy_phys_mem(kvm_context,
                                                 region->e_physbase,
                                                 TARGET_PAGE_ALIGN(offset));
                        }
                        if (region->e_size > offset + TARGET_PAGE_SIZE) {
                            kvm_destroy_phys_mem(kvm_context,
                                 region->e_physbase + offset + TARGET_PAGE_SIZE,
                                 TARGET_PAGE_ALIGN(region->e_size - offset -
                                                   TARGET_PAGE_SIZE));
                        }
                    } else {
                        kvm_destroy_phys_mem(kvm_context, region->e_physbase,
                                             TARGET_PAGE_ALIGN(region->e_size));
                    }
                }

                if (region->mmio_index >= 0) {
                    cpu_unregister_io_memory(region->mmio_index);
                }

                if (region->u.r_virtbase) {
                    int ret = munmap(region->u.r_virtbase,
                                     (pci_region->size + 0xFFF) & 0xFFFFF000);
                    if (ret != 0)
                        fprintf(stderr,
				"Failed to unmap assigned device region: %s\n",
				strerror(errno));
                }
            }
            if (pci_region->resource_fd >= 0) {
                close(pci_region->resource_fd);
            }
        }

        if (dev->cap.available & ASSIGNED_DEVICE_CAP_MSIX)
            assigned_dev_unregister_msix_mmio(dev);

        if (dev->real_device.config_fd >= 0) {
            close(dev->real_device.config_fd);
        }

#ifdef KVM_CAP_IRQ_ROUTING
        free_dev_irq_entries(dev);
#endif
    }
}

static uint32_t calc_assigned_dev_id(uint16_t seg, uint8_t bus, uint8_t devfn)
{
    return (uint32_t)seg << 16 | (uint32_t)bus << 8 | (uint32_t)devfn;
}

static int assign_device(AssignedDevice *dev)
{
    struct kvm_assigned_pci_dev assigned_dev_data;
    int r;

#ifdef KVM_CAP_PCI_SEGMENT
    /* Only pass non-zero PCI segment to capable module */
    if (!kvm_check_extension(kvm_state, KVM_CAP_PCI_SEGMENT) &&
        dev->h_segnr) {
        fprintf(stderr, "Can't assign device inside non-zero PCI segment "
                "as this KVM module doesn't support it.\n");
        return -ENODEV;
    }
#endif

    memset(&assigned_dev_data, 0, sizeof(assigned_dev_data));
    assigned_dev_data.assigned_dev_id  =
	calc_assigned_dev_id(dev->h_segnr, dev->h_busnr, dev->h_devfn);
    assigned_dev_data.segnr = dev->h_segnr;
    assigned_dev_data.busnr = dev->h_busnr;
    assigned_dev_data.devfn = dev->h_devfn;

#ifdef KVM_CAP_IOMMU
    /* We always enable the IOMMU unless disabled on the command line */
    if (dev->features & ASSIGNED_DEVICE_USE_IOMMU_MASK) {
        if (!kvm_check_extension(kvm_state, KVM_CAP_IOMMU)) {
            fprintf(stderr, "No IOMMU found.  Unable to assign device \"%s\"\n",
                    dev->dev.qdev.id);
            return -ENODEV;
        }
        assigned_dev_data.flags |= KVM_DEV_ASSIGN_ENABLE_IOMMU;
    }
#else
    dev->features &= ~ASSIGNED_DEVICE_USE_IOMMU_MASK;
#endif

    r = kvm_assign_pci_device(kvm_context, &assigned_dev_data);
    if (r < 0)
	fprintf(stderr, "Failed to assign device \"%s\" : %s\n",
                dev->dev.qdev.id, strerror(-r));
    return r;
}

static int assign_irq(AssignedDevice *dev)
{
    struct kvm_assigned_irq assigned_irq_data;
    int irq, r = 0;

    /* Interrupt PIN 0 means don't use INTx */
    if (assigned_dev_pci_read_byte(&dev->dev, PCI_INTERRUPT_PIN) == 0)
        return 0;

    irq = pci_map_irq(&dev->dev, dev->intpin);
    irq = piix_get_irq(irq);

#ifdef TARGET_IA64
    irq = ipf_map_irq(&dev->dev, irq);
#endif

    if (dev->girq == irq)
        return r;

    memset(&assigned_irq_data, 0, sizeof(assigned_irq_data));
    assigned_irq_data.assigned_dev_id =
        calc_assigned_dev_id(dev->h_segnr, dev->h_busnr, dev->h_devfn);
    assigned_irq_data.guest_irq = irq;
    assigned_irq_data.host_irq = dev->real_device.irq;
#ifdef KVM_CAP_ASSIGN_DEV_IRQ
    if (dev->irq_requested_type) {
        assigned_irq_data.flags = dev->irq_requested_type;
        r = kvm_deassign_irq(kvm_context, &assigned_irq_data);
        /* -ENXIO means no assigned irq */
        if (r && r != -ENXIO)
            perror("assign_irq: deassign");
    }

    assigned_irq_data.flags = KVM_DEV_IRQ_GUEST_INTX;
    if (dev->features & ASSIGNED_DEVICE_PREFER_MSI_MASK &&
        dev->cap.available & ASSIGNED_DEVICE_CAP_MSI)
        assigned_irq_data.flags |= KVM_DEV_IRQ_HOST_MSI;
    else
        assigned_irq_data.flags |= KVM_DEV_IRQ_HOST_INTX;
#endif

    r = kvm_assign_irq(kvm_context, &assigned_irq_data);
    if (r < 0) {
        qerror_report(QERR_DEVICE_INIT_FAILED_2, "pci-assign",
                      "Failed to assign irq", strerror(-r));
        fprintf(stderr, "Failed to assign irq for \"%s\": %s\n",
                dev->dev.qdev.id, strerror(-r));
        fprintf(stderr, "Perhaps you are assigning a device "
                "that shares an IRQ with another device?\n");
        return r;
    }

    dev->girq = irq;
    dev->irq_requested_type = assigned_irq_data.flags;
    return r;
}

static void deassign_device(AssignedDevice *dev)
{
#ifdef KVM_CAP_DEVICE_DEASSIGNMENT
    struct kvm_assigned_pci_dev assigned_dev_data;
    int r;

    memset(&assigned_dev_data, 0, sizeof(assigned_dev_data));
    assigned_dev_data.assigned_dev_id  =
	calc_assigned_dev_id(dev->h_segnr, dev->h_busnr, dev->h_devfn);

    r = kvm_deassign_pci_device(kvm_context, &assigned_dev_data);
    if (r < 0)
	fprintf(stderr, "Failed to deassign device \"%s\" : %s\n",
                dev->dev.qdev.id, strerror(-r));
#endif
}

#if 0
AssignedDevInfo *get_assigned_device(int pcibus, int slot)
{
    AssignedDevice *assigned_dev = NULL;
    AssignedDevInfo *adev = NULL;

    QLIST_FOREACH(adev, &adev_head, next) {
        assigned_dev = adev->assigned_dev;
        if (pci_bus_num(assigned_dev->dev.bus) == pcibus &&
            PCI_SLOT(assigned_dev->dev.devfn) == slot)
            return adev;
    }

    return NULL;
}
#endif

/* The pci config space got updated. Check if irq numbers have changed
 * for our devices
 */
void assigned_dev_update_irqs(void)
{
    AssignedDevice *dev, *next;
    int r;

    dev = QLIST_FIRST(&devs);
    while (dev) {
        next = QLIST_NEXT(dev, next);
        r = assign_irq(dev);
        if (r < 0)
            qdev_unplug(&dev->dev.qdev);
        dev = next;
    }
}

#ifdef KVM_CAP_IRQ_ROUTING

#ifdef KVM_CAP_DEVICE_MSI
static void assigned_dev_update_msi(PCIDevice *pci_dev, unsigned int ctrl_pos)
{
    struct kvm_assigned_irq assigned_irq_data;
    AssignedDevice *assigned_dev = container_of(pci_dev, AssignedDevice, dev);
    uint8_t ctrl_byte = pci_dev->config[ctrl_pos];
    int r;

    memset(&assigned_irq_data, 0, sizeof assigned_irq_data);
    assigned_irq_data.assigned_dev_id  =
        calc_assigned_dev_id(assigned_dev->h_segnr, assigned_dev->h_busnr,
                (uint8_t)assigned_dev->h_devfn);

    /* Some guests gratuitously disable MSI even if they're not using it,
     * try to catch this by only deassigning irqs if the guest is using
     * MSI or intends to start. */
    if ((assigned_dev->irq_requested_type & KVM_DEV_IRQ_GUEST_MSI) ||
        (ctrl_byte & PCI_MSI_FLAGS_ENABLE)) {

        assigned_irq_data.flags = assigned_dev->irq_requested_type;
        free_dev_irq_entries(assigned_dev);
        r = kvm_deassign_irq(kvm_context, &assigned_irq_data);
        /* -ENXIO means no assigned irq */
        if (r && r != -ENXIO)
            perror("assigned_dev_update_msi: deassign irq");

        assigned_dev->irq_requested_type = 0;
    }

    if (ctrl_byte & PCI_MSI_FLAGS_ENABLE) {
        int pos = ctrl_pos - PCI_MSI_FLAGS;
        assigned_dev->entry = calloc(1, sizeof(struct kvm_irq_routing_entry));
        if (!assigned_dev->entry) {
            perror("assigned_dev_update_msi: ");
            return;
        }
        assigned_dev->entry->u.msi.address_lo =
            pci_get_long(pci_dev->config + pos + PCI_MSI_ADDRESS_LO);
        assigned_dev->entry->u.msi.address_hi = 0;
        assigned_dev->entry->u.msi.data =
            pci_get_word(pci_dev->config + pos + PCI_MSI_DATA_32);
        assigned_dev->entry->type = KVM_IRQ_ROUTING_MSI;
        r = kvm_get_irq_route_gsi(kvm_context);
        if (r < 0) {
            perror("assigned_dev_update_msi: kvm_get_irq_route_gsi");
            return;
        }
        assigned_dev->entry->gsi = r;

        kvm_add_routing_entry(kvm_context, assigned_dev->entry);
        if (kvm_commit_irq_routes(kvm_context) < 0) {
            perror("assigned_dev_update_msi: kvm_commit_irq_routes");
            assigned_dev->cap.state &= ~ASSIGNED_DEVICE_MSI_ENABLED;
            return;
        }
	assigned_dev->irq_entries_nr = 1;

        assigned_irq_data.guest_irq = assigned_dev->entry->gsi;
	assigned_irq_data.flags = KVM_DEV_IRQ_HOST_MSI | KVM_DEV_IRQ_GUEST_MSI;
        if (kvm_assign_irq(kvm_context, &assigned_irq_data) < 0)
            perror("assigned_dev_enable_msi: assign irq");

        assigned_dev->irq_requested_type = assigned_irq_data.flags;
    }
}

static void assigned_dev_update_msi_msg(PCIDevice *pci_dev,
                                        unsigned int ctrl_pos)
{
    AssignedDevice *assigned_dev = container_of(pci_dev, AssignedDevice, dev);
    uint8_t ctrl_byte = pci_dev->config[ctrl_pos];
    struct kvm_irq_routing_entry *orig;
    int pos, ret;

    if (!(assigned_dev->irq_requested_type & KVM_DEV_IRQ_GUEST_MSI) ||
        !(ctrl_byte & PCI_MSI_FLAGS_ENABLE)) {
        return;
    }

    orig = assigned_dev->entry;
    pos = ctrl_pos - PCI_MSI_FLAGS;

    assigned_dev->entry = calloc(1, sizeof(struct kvm_irq_routing_entry));
    if (!assigned_dev->entry) {
        assigned_dev->entry = orig;
        perror("assigned_dev_update_msi_msg: ");
        return;
    }

    assigned_dev->entry->u.msi.address_lo =
                    pci_get_long(pci_dev->config + pos + PCI_MSI_ADDRESS_LO);
    assigned_dev->entry->u.msi.address_hi = 0;
    assigned_dev->entry->u.msi.data =
                    pci_get_word(pci_dev->config + pos + PCI_MSI_DATA_32);
    assigned_dev->entry->type = KVM_IRQ_ROUTING_MSI;
    assigned_dev->entry->gsi = orig->gsi;

    ret = kvm_update_routing_entry(kvm_context, orig, assigned_dev->entry);
    if (ret) {
        fprintf(stderr, "Error updating MSI irq routing entry (%d)\n", ret);
        free(assigned_dev->entry);
        assigned_dev->entry = orig;
        return;
    }

    free(orig);

    ret = kvm_commit_irq_routes(kvm_context);
    if (ret) {
        fprintf(stderr, "Error committing MSI irq route (%d)\n", ret);
    }
}
#endif

#ifdef KVM_CAP_DEVICE_MSIX
static bool msix_masked(MSIXTableEntry *entry)
{
    return (entry->ctrl & cpu_to_le32(0x1)) != 0;
}

/*
 * When MSI-X is first enabled the vector table typically has all the
 * vectors masked, so we can't use that as the obvious test to figure out
 * how many vectors to initially enable.  Instead we look at the data field
 * because this is what worked for pci-assign for a long time.  This makes
 * sure the physical MSI-X state tracks the guest's view, which is important
 * for some VF/PF and PF/fw communication channels.
 */
static bool msix_skipped(MSIXTableEntry *entry)
{
    return !entry->data;
}

static int assigned_dev_update_msix_mmio(PCIDevice *pci_dev)
{
    AssignedDevice *adev = container_of(pci_dev, AssignedDevice, dev);
    uint16_t entries_nr = 0;
    int i, r = 0;
    struct kvm_assigned_msix_nr msix_nr;
    struct kvm_assigned_msix_entry msix_entry;
    MSIXTableEntry *entry = adev->msix_table;

    /* Get the usable entry number for allocating */
    for (i = 0; i < adev->msix_max; i++, entry++) {
        if (msix_skipped(entry)) {
            continue;
        }
        entries_nr++;
    }

    DEBUG("MSI-X entries: %d\n", entries_nr);

    /* It's valid to enable MSI-X with all entries masked */
    if (!entries_nr) {
        return 0;
    }

    msix_nr.assigned_dev_id = calc_assigned_dev_id(adev->h_segnr, adev->h_busnr,
                                          (uint8_t)adev->h_devfn);
    msix_nr.entry_nr = entries_nr;
    r = kvm_assign_set_msix_nr(kvm_context, &msix_nr);
    if (r != 0) {
        fprintf(stderr, "fail to set MSI-X entry number for MSIX! %s\n",
			strerror(-r));
        return r;
    }

    free_dev_irq_entries(adev);

    adev->irq_entries_nr = adev->msix_max;
    adev->entry = qemu_mallocz(adev->msix_max * sizeof(*(adev->entry)));
    if (!adev->entry) {
        perror("assigned_dev_update_msix_mmio: ");
        return -errno;
    }

    msix_entry.assigned_dev_id = msix_nr.assigned_dev_id;
    entry = adev->msix_table;
    for (i = 0; i < adev->msix_max; i++, entry++) {
        if (msix_skipped(entry)) {
            continue;
        }

        r = kvm_get_irq_route_gsi(kvm_context);
        if (r < 0)
            return r;

        adev->entry[i].gsi = r;
        adev->entry[i].type = KVM_IRQ_ROUTING_MSI;
        adev->entry[i].flags = 0;
        adev->entry[i].u.msi.address_lo = entry->addr_lo;
        adev->entry[i].u.msi.address_hi = entry->addr_hi;
        adev->entry[i].u.msi.data = entry->data;

        DEBUG("MSI-X vector %d, gsi %d, addr %08x_%08x, data %08x\n", i,
              r, entry->addr_hi, entry->addr_lo, entry->data);

        kvm_add_routing_entry(kvm_context, &adev->entry[i]);

        msix_entry.gsi = adev->entry[i].gsi;
        msix_entry.entry = i;
        r = kvm_assign_set_msix_entry(kvm_context, &msix_entry);
        if (r) {
            fprintf(stderr, "fail to set MSI-X entry! %s\n", strerror(-r));
            break;
        }
    }

    if (r == 0 && kvm_commit_irq_routes(kvm_context) < 0) {
	    perror("assigned_dev_update_msix_mmio: kvm_commit_irq_routes");
	    return -EINVAL;
    }

    return r;
}

static void assigned_dev_update_msix(PCIDevice *pci_dev, unsigned int ctrl_pos)
{
    struct kvm_assigned_irq assigned_irq_data;
    AssignedDevice *assigned_dev = container_of(pci_dev, AssignedDevice, dev);
    uint16_t *ctrl_word = (uint16_t *)(pci_dev->config + ctrl_pos);
    int r;

    memset(&assigned_irq_data, 0, sizeof assigned_irq_data);
    assigned_irq_data.assigned_dev_id  =
            calc_assigned_dev_id(assigned_dev->h_segnr, assigned_dev->h_busnr,
                    (uint8_t)assigned_dev->h_devfn);

    /* Some guests gratuitously disable MSIX even if they're not using it,
     * try to catch this by only deassigning irqs if the guest is using
     * MSIX or intends to start. */
    if ((assigned_dev->irq_requested_type & KVM_DEV_IRQ_GUEST_MSIX) ||
        (*ctrl_word & PCI_MSIX_ENABLE)) {

        assigned_irq_data.flags = assigned_dev->irq_requested_type;
        free_dev_irq_entries(assigned_dev);
        r = kvm_deassign_irq(kvm_context, &assigned_irq_data);
        /* -ENXIO means no assigned irq */
        if (r && r != -ENXIO)
            perror("assigned_dev_update_msix: deassign irq");

        assigned_dev->irq_requested_type = 0;
    }

    if (*ctrl_word & PCI_MSIX_ENABLE) {
        assigned_irq_data.flags = KVM_DEV_IRQ_HOST_MSIX |
                                  KVM_DEV_IRQ_GUEST_MSIX;

        if (assigned_dev_update_msix_mmio(pci_dev) < 0) {
            perror("assigned_dev_update_msix_mmio");
            return;
        }

        if (assigned_dev->irq_entries_nr) {
            if (kvm_assign_irq(kvm_context, &assigned_irq_data) < 0) {
                perror("assigned_dev_enable_msix: assign irq");
                return;
            }
        }
        assigned_dev->irq_requested_type = assigned_irq_data.flags;
    }
}
#endif
#endif

/* There can be multiple VNDR capabilities per device, we need to find the
 * one that starts closet to the given address without going over. */
static uint8_t find_vndr_start(PCIDevice *pci_dev, uint32_t address)
{
    uint8_t cap, pos;

    for (cap = pos = 0;
         (pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_VNDR, pos));
         pos += PCI_CAP_LIST_NEXT) {
        if (pos <= address) {
            cap = MAX(pos, cap);
        }
    }
    return cap;
}

/* Merge the bits set in mask from mval into val.  Both val and mval are
 * at the same addr offset, pos is the starting offset of the mask. */
static uint32_t merge_bits(uint32_t val, uint32_t mval, uint8_t addr,
                           int len, uint8_t pos, uint32_t mask)
{
    if (!ranges_overlap(addr, len, pos, 4)) {
        return val;
    }

    if (addr >= pos) {
        mask >>= (addr - pos) * 8;
    } else {
        mask <<= (pos - addr) * 8;
    }
    mask &= 0xffffffffU >> (4 - len) * 8;

    val &= ~mask;
    val |= (mval & mask);

    return val;
}

static uint32_t assigned_device_pci_cap_read_config(PCIDevice *pci_dev,
                                                    uint32_t address, int len)
{
    uint8_t cap, cap_id = pci_dev->config_map[address];
    uint32_t val;

    switch (cap_id) {

    case PCI_CAP_ID_VPD:
        cap = pci_find_capability(pci_dev, cap_id);
        val = assigned_dev_pci_read(pci_dev, address, len);
        return merge_bits(val, pci_get_long(pci_dev->config + address),
                          address, len, cap + PCI_CAP_LIST_NEXT, 0xff);

    case PCI_CAP_ID_VNDR:
        cap = find_vndr_start(pci_dev, address);
        val = assigned_dev_pci_read(pci_dev, address, len);
        return merge_bits(val, pci_get_long(pci_dev->config + address),
                          address, len, cap + PCI_CAP_LIST_NEXT, 0xff);
    }

    return pci_default_read_config(pci_dev, address, len);
}

static void assigned_device_pci_cap_write_config(PCIDevice *pci_dev,
                                                 uint32_t address,
                                                 uint32_t val, int len)
{
    uint8_t cap_id = pci_dev->config_map[address];

    pci_default_write_config(pci_dev, address, val, len);
    switch (cap_id) {
#ifdef KVM_CAP_IRQ_ROUTING
    case PCI_CAP_ID_MSI:
#ifdef KVM_CAP_DEVICE_MSI
        {
            uint8_t cap = pci_find_capability(pci_dev, cap_id);
            if (ranges_overlap(address - cap, len, PCI_MSI_FLAGS, 1)) {
                assigned_dev_update_msi(pci_dev, cap + PCI_MSI_FLAGS);
            } else if (ranges_overlap(address - cap, len, /* 32bit MSI only */
                                      PCI_MSI_ADDRESS_LO, 6)) {
                assigned_dev_update_msi_msg(pci_dev, cap + PCI_MSI_FLAGS);
            }
        }
#endif
        break;

    case PCI_CAP_ID_MSIX:
#ifdef KVM_CAP_DEVICE_MSIX
        {
            uint8_t cap = pci_find_capability(pci_dev, cap_id);
            if (ranges_overlap(address - cap, len, PCI_MSIX_FLAGS + 1, 1)) {
                assigned_dev_update_msix(pci_dev, cap + PCI_MSIX_FLAGS);
            }
        }
#endif
        break;
#endif

    case PCI_CAP_ID_VPD:
    case PCI_CAP_ID_VNDR:
        assigned_dev_pci_write(pci_dev, address, val, len);
        break;
    }
}

static int assigned_device_pci_cap_init(PCIDevice *pci_dev)
{
    AssignedDevice *dev = container_of(pci_dev, AssignedDevice, dev);
    PCIRegion *pci_region = dev->real_device.regions;
    int ret, pos;

    /* Clear initial capabilities pointer and status copied from hw */
    pci_set_byte(pci_dev->config + PCI_CAPABILITY_LIST, 0);
    pci_set_word(pci_dev->config + PCI_STATUS,
                 pci_get_word(pci_dev->config + PCI_STATUS) &
                 ~PCI_STATUS_CAP_LIST);

#ifdef KVM_CAP_IRQ_ROUTING
#ifdef KVM_CAP_DEVICE_MSI
    /* Expose MSI capability
     * MSI capability is the 1st capability in capability config */
    if ((pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_MSI, 0))) {
        dev->cap.available |= ASSIGNED_DEVICE_CAP_MSI;
        /* Only 32-bit/no-mask currently supported */
        if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_MSI, pos, 10)) < 0) {
            return ret;
        }

        pci_set_word(pci_dev->config + pos + PCI_MSI_FLAGS,
                     pci_get_word(pci_dev->config + pos + PCI_MSI_FLAGS) &
                     PCI_MSI_FLAGS_QMASK);
        pci_set_long(pci_dev->config + pos + PCI_MSI_ADDRESS_LO, 0);
        pci_set_word(pci_dev->config + pos + PCI_MSI_DATA_32, 0);

        /* Set writable fields */
        pci_set_word(pci_dev->wmask + pos + PCI_MSI_FLAGS,
                     PCI_MSI_FLAGS_QSIZE | PCI_MSI_FLAGS_ENABLE);
        pci_set_long(pci_dev->wmask + pos + PCI_MSI_ADDRESS_LO, 0xfffffffc);
        pci_set_word(pci_dev->wmask + pos + PCI_MSI_DATA_32, 0xffff);
    }
#endif
#ifdef KVM_CAP_DEVICE_MSIX
    /* Expose MSI-X capability */
    if ((pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_MSIX, 0))) {
        int bar_nr;
        uint32_t msix_table_entry;

        dev->cap.available |= ASSIGNED_DEVICE_CAP_MSIX;
        if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_MSIX, pos, 12)) < 0) {
            return ret;
        }

        pci_set_word(pci_dev->config + pos + PCI_MSIX_FLAGS,
                     pci_get_word(pci_dev->config + pos + PCI_MSIX_FLAGS) &
                     PCI_MSIX_TABSIZE);

        /* Only enable and function mask bits are writable */
        pci_set_word(pci_dev->wmask + pos + PCI_MSIX_FLAGS,
                     PCI_MSIX_FLAGS_ENABLE | PCI_MSIX_FLAGS_MASKALL);

        msix_table_entry = pci_get_long(pci_dev->config + pos + PCI_MSIX_TABLE);
        bar_nr = msix_table_entry & PCI_MSIX_BIR;
        msix_table_entry &= ~PCI_MSIX_BIR;
        dev->msix_table_addr = pci_region[bar_nr].base_addr + msix_table_entry;
        dev->msix_max = pci_get_word(pci_dev->config + pos + PCI_MSIX_FLAGS);
        dev->msix_max &= PCI_MSIX_TABSIZE;
        dev->msix_max += 1;
    }
#endif
#endif

    /* Minimal PM support, nothing writable, device appears to NAK changes */
    if ((pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_PM, 0))) {
        uint16_t pmc;
        if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_PM, pos,
                                      PCI_PM_SIZEOF)) < 0) {
            return ret;
        }

        pmc = pci_get_word(pci_dev->config + pos + PCI_CAP_FLAGS);
        pmc &= (PCI_PM_CAP_VER_MASK | PCI_PM_CAP_DSI);
        pci_set_word(pci_dev->config + pos + PCI_CAP_FLAGS, pmc);

        /* assign_device will bring the device up to D0, so we don't need
         * to worry about doing that ourselves here. */
        pci_set_word(pci_dev->config + pos + PCI_PM_CTRL,
                     PCI_PM_CTRL_NO_SOFT_RST);

        pci_set_byte(pci_dev->config + pos + PCI_PM_PPB_EXTENSIONS, 0);
        pci_set_byte(pci_dev->config + pos + PCI_PM_DATA_REGISTER, 0);
    }

    if ((pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_EXP, 0))) {
        uint8_t version, size = 0;
        uint16_t type, devctl, lnksta;
        uint32_t devcap, lnkcap;

        version = pci_get_byte(pci_dev->config + pos + PCI_EXP_FLAGS);
        version &= PCI_EXP_FLAGS_VERS;
        if (version == 1) {
            size = 0x14;
        } else if (version == 2) {
            /*
             * Check for non-std size, accept reduced size to 0x34,
             * which is what bcm5761 implemented, violating the 
             * PCIe v3.0 spec that regs should exist and be read as 0,
             * not optionally provided and shorten the struct size.
             */
            size = MIN(0x3c, PCI_CONFIG_SPACE_SIZE - pos);
            if (size < 0x34) {
                fprintf(stderr,
                        "%s: Invalid size PCIe cap-id 0x%x \n",
                        __func__, PCI_CAP_ID_EXP);
                return -EINVAL;
            } else if (size != 0x3c) {
                fprintf(stderr,
                        "WARNING, %s: PCIe cap-id 0x%x has "
                        "non-standard size 0x%x; std size should be 0x3c \n",
                         __func__, PCI_CAP_ID_EXP, size);
            } 
        } else if (version == 0) {
            uint16_t vid, did;
            vid = pci_get_word(pci_dev->config + PCI_VENDOR_ID);
            did = pci_get_word(pci_dev->config + PCI_DEVICE_ID);
            if (vid == PCI_VENDOR_ID_INTEL && did == 0x10ed) {
                /*
                 * quirk for Intel 82599 VF with invalid PCIe capability
                 * version, should really be version 2 (same as PF)
                 */
                size = 0x3c;
            }
        }

        if (size == 0) {
            fprintf(stderr, 
                    "%s: Unsupported PCI express capability version %d\n",
                    __func__, version);
            return -EINVAL;
        }

        if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_EXP,
                                      pos, size)) < 0) {
            return ret;
        }

        type = pci_get_word(pci_dev->config + pos + PCI_EXP_FLAGS);
        type = (type & PCI_EXP_FLAGS_TYPE) >> 4;
        if (type != PCI_EXP_TYPE_ENDPOINT &&
            type != PCI_EXP_TYPE_LEG_END && type != PCI_EXP_TYPE_RC_END) {
            fprintf(stderr,
                    "Device assignment only supports endpoint assignment, "
                    "device type %d\n", type);
            return -EINVAL;
        }

        /* capabilities, pass existing read-only copy
         * PCI_EXP_FLAGS_IRQ: updated by hardware, should be direct read */

        /* device capabilities: hide FLR */
        devcap = pci_get_long(pci_dev->config + pos + PCI_EXP_DEVCAP);
        devcap &= ~PCI_EXP_DEVCAP_FLR;
        pci_set_long(pci_dev->config + pos + PCI_EXP_DEVCAP, devcap);

        /* device control: clear all error reporting enable bits, leaving
         *                 only a few host values.  Note, these are
         *                 all writable, but not passed to hw.
         */
        devctl = pci_get_word(pci_dev->config + pos + PCI_EXP_DEVCTL);
        devctl = (devctl & (PCI_EXP_DEVCTL_READRQ | PCI_EXP_DEVCTL_PAYLOAD)) |
                  PCI_EXP_DEVCTL_RELAX_EN | PCI_EXP_DEVCTL_NOSNOOP_EN;
        pci_set_word(pci_dev->config + pos + PCI_EXP_DEVCTL, devctl);
        devctl = PCI_EXP_DEVCTL_BCR_FLR | PCI_EXP_DEVCTL_AUX_PME;
        pci_set_word(pci_dev->wmask + pos + PCI_EXP_DEVCTL, ~devctl);

        /* Clear device status */
        pci_set_word(pci_dev->config + pos + PCI_EXP_DEVSTA, 0);

        /* Link capabilities, expose links and latencues, clear reporting */
        lnkcap = pci_get_long(pci_dev->config + pos + PCI_EXP_LNKCAP);
        lnkcap &= (PCI_EXP_LNKCAP_SLS | PCI_EXP_LNKCAP_MLW |
                   PCI_EXP_LNKCAP_ASPMS | PCI_EXP_LNKCAP_L0SEL |
                   PCI_EXP_LNKCAP_L1EL);
        pci_set_long(pci_dev->config + pos + PCI_EXP_LNKCAP, lnkcap);

        /* Link control, pass existing read-only copy.  Should be writable? */

        /* Link status, only expose current speed and width */
        lnksta = pci_get_word(pci_dev->config + pos + PCI_EXP_LNKSTA);
        lnksta &= (PCI_EXP_LNKSTA_CLS | PCI_EXP_LNKSTA_NLW);
        pci_set_word(pci_dev->config + pos + PCI_EXP_LNKSTA, lnksta);

        if (version >= 2) {
            /* Slot capabilities, control, status - not needed for endpoints */
            pci_set_long(pci_dev->config + pos + PCI_EXP_SLTCAP, 0);
            pci_set_word(pci_dev->config + pos + PCI_EXP_SLTCTL, 0);
            pci_set_word(pci_dev->config + pos + PCI_EXP_SLTSTA, 0);

            /* Root control, capabilities, status - not needed for endpoints */
            pci_set_word(pci_dev->config + pos + PCI_EXP_RTCTL, 0);
            pci_set_word(pci_dev->config + pos + PCI_EXP_RTCAP, 0);
            pci_set_long(pci_dev->config + pos + PCI_EXP_RTSTA, 0);

            /* Device capabilities/control 2, pass existing read-only copy */
            /* Link control 2, pass existing read-only copy */
        }
    }

    if ((pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_PCIX, 0))) {
        uint16_t cmd;
        uint32_t status;

        /* Only expose the minimum, 8 byte capability */
        if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_PCIX, pos, 8)) < 0) {
            return ret;
        }

        /* Command register, clear upper bits, including extended modes */
        cmd = pci_get_word(pci_dev->config + pos + PCI_X_CMD);
        cmd &= (PCI_X_CMD_DPERR_E | PCI_X_CMD_ERO | PCI_X_CMD_MAX_READ |
                PCI_X_CMD_MAX_SPLIT);
        pci_set_word(pci_dev->config + pos + PCI_X_CMD, cmd);

        /* Status register, update with emulated PCI bus location, clear
         * error bits, leave the rest. */
        status = pci_get_long(pci_dev->config + pos + PCI_X_STATUS);
        status &= ~(PCI_X_STATUS_BUS | PCI_X_STATUS_DEVFN);
        status |= (pci_bus_num(pci_dev->bus) << 8) | pci_dev->devfn;
        status &= ~(PCI_X_STATUS_SPL_DISC | PCI_X_STATUS_UNX_SPL |
                    PCI_X_STATUS_SPL_ERR);
        pci_set_long(pci_dev->config + pos + PCI_X_STATUS, status);
    }

    if ((pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_VPD, 0))) {
        /* Direct R/W passthrough */
        if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_VPD, pos, 8)) < 0) {
            return ret;
        }
    }

    /* Devices can have multiple vendor capabilities, get them all */
    for (pos = 0; (pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_VNDR, pos));
        pos += PCI_CAP_LIST_NEXT) {
        uint8_t len = pci_get_byte(pci_dev->config + pos + PCI_CAP_FLAGS);
        /* Direct R/W passthrough */
        if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_VNDR,
                                      pos, len)) < 0) {
            return ret;
        }
    }

    return 0;
}

static uint32_t msix_mmio_readl(void *opaque, target_phys_addr_t addr)
{
    AssignedDevice *adev = opaque;
    unsigned int offset = addr & 0xfff;
    uint32_t val = 0;

    memcpy(&val, (void *)((uint8_t *)adev->msix_table + offset), 4);

    return val;
}

static uint32_t msix_mmio_readb(void *opaque, target_phys_addr_t addr)
{
    return ((msix_mmio_readl(opaque, addr & ~3)) >>
            (8 * (addr & 3))) & 0xff;
}

static uint32_t msix_mmio_readw(void *opaque, target_phys_addr_t addr)
{
    return ((msix_mmio_readl(opaque, addr & ~3)) >>
            (8 * (addr & 3))) & 0xffff;
}

static void msix_mmio_writel(void *opaque,
                             target_phys_addr_t addr, uint32_t val)
{
    AssignedDevice *adev = opaque;
    unsigned int offset = addr & 0xfff;
    PCIDevice *pdev = &adev->dev;
    uint16_t ctrl;
    MSIXTableEntry orig;
    int i = offset >> 4;
    uint8_t msix_cap;

    if (i >= adev->msix_max) {
        return; /* Drop write */
    }

    msix_cap = pci_find_capability(pdev, PCI_CAP_ID_MSIX);
    ctrl = pci_get_word(pdev->config + msix_cap + PCI_MSIX_FLAGS);

    DEBUG("write to MSI-X table offset 0x%lx, val 0x%lx\n", addr, val);

    if (ctrl & PCI_MSIX_FLAGS_ENABLE) {
        orig = adev->msix_table[i];
    }

    memcpy((void *)((uint8_t *)adev->msix_table + offset), &val, 4);

    if (ctrl & PCI_MSIX_FLAGS_ENABLE) {
        MSIXTableEntry *entry = &adev->msix_table[i];

        if (!msix_masked(&orig) && msix_masked(entry)) {
            /*
             * Vector masked, disable it
             *
             * XXX It's not clear if we can or should actually attempt
             * to mask or disable the interrupt.  KVM doesn't have
             * support for pending bits and kvm_assign_set_msix_entry
             * doesn't modify the device hardware mask.  Interrupts
             * while masked are simply not injected to the guest, so
             * are lost.  Can we get away with always injecting an
             * interrupt on unmask?
             */
        } else if (msix_masked(&orig) && !msix_masked(entry)) {
            /* Vector unmasked */
            if (i >= adev->irq_entries_nr || !adev->entry[i].type) {
                /* Previously unassigned vector, start from scratch */
                assigned_dev_update_msix(pdev, msix_cap + PCI_MSIX_FLAGS);
                return;
            } else {
                /* Update an existing, previously masked vector */
                struct kvm_irq_routing_entry orig = adev->entry[i];
                int ret;

                adev->entry[i].u.msi.address_lo = entry->addr_lo;
                adev->entry[i].u.msi.address_hi = entry->addr_hi;
                adev->entry[i].u.msi.data = entry->data;
                if (!memcmp(&adev->entry[i].u.msi, &orig.u.msi, sizeof orig.u.msi)) {
                    return;
                }

                ret = kvm_update_routing_entry(kvm_context, &orig,
                                               &adev->entry[i]);
                if (ret) {
                    fprintf(stderr,
                            "Error updating irq routing entry (%d)\n", ret);
                    return;
                }

                ret = kvm_commit_irq_routes(kvm_context);
                if (ret) {
                    fprintf(stderr,
                            "Error committing irq routes (%d)\n", ret);
                    return;
                }
            }
        }
    }
}

static void msix_mmio_writew(void *opaque,
                             target_phys_addr_t addr, uint32_t val)
{
    msix_mmio_writel(opaque, addr & ~3,
                     (val & 0xffff) << (8*(addr & 3)));
}

static void msix_mmio_writeb(void *opaque,
                             target_phys_addr_t addr, uint32_t val)
{
    msix_mmio_writel(opaque, addr & ~3,
                     (val & 0xff) << (8*(addr & 3)));
}

static CPUWriteMemoryFunc *msix_mmio_write[] = {
    msix_mmio_writeb,	msix_mmio_writew,	msix_mmio_writel
};

static CPUReadMemoryFunc *msix_mmio_read[] = {
    msix_mmio_readb,	msix_mmio_readw,	msix_mmio_readl
};

static void msix_reset(AssignedDevice *dev)
{
    MSIXTableEntry *entry;
    int i;

    if (!dev->msix_table) {
        return;
    }

    memset(dev->msix_table, 0, 0x1000);

    for (i = 0, entry = dev->msix_table; i < dev->msix_max; i++, entry++) {
        entry->ctrl = cpu_to_le32(0x1); /* Masked */
    }
}

static int assigned_dev_register_msix_mmio(AssignedDevice *dev)
{
    dev->msix_table = mmap(NULL, 0x1000, PROT_READ|PROT_WRITE,
                           MAP_ANONYMOUS|MAP_PRIVATE, 0, 0);
    if (dev->msix_table == MAP_FAILED) {
        fprintf(stderr, "fail allocate msix_table! %s\n", strerror(errno));
        return -EFAULT;
    }

    msix_reset(dev);

    dev->mmio_index = cpu_register_io_memory(
                        msix_mmio_read, msix_mmio_write, dev);
    return 0;
}

static void assigned_dev_unregister_msix_mmio(AssignedDevice *dev)
{
    if (!dev->msix_table) {
        return;
    }

    cpu_unregister_io_memory(dev->mmio_index);
    dev->mmio_index = 0;

    if (munmap(dev->msix_table, 0x1000) == -1) {
        fprintf(stderr, "error unmapping msix_table! %s\n",
                strerror(errno));
    }
    dev->msix_table = NULL;
}

static const VMStateDescription vmstate_assigned_device = {
    .name = "pci-assign",
    .fields = (VMStateField []) {
        VMSTATE_END_OF_LIST()
    }
};

static void reset_assigned_device(DeviceState *dev)
{
    PCIDevice *pci_dev = DO_UPCAST(PCIDevice, qdev, dev);
    AssignedDevice *adev = DO_UPCAST(AssignedDevice, dev, pci_dev);
    char reset_file[64];
    const char reset[] = "1";
    int fd, ret, pos;

    /*
     * If a guest is reset without being shutdown, MSI/MSI-X can still
     * be running.  We want to return the device to a known state on
     * reset, so disable those here.  We especially do not want MSI-X
     * enabled since it lives in MMIO space, which is about to get
     * disabled.
     */
    if (adev->irq_requested_type & KVM_DEV_IRQ_GUEST_MSIX) {
        if ((pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_MSIX, 0))) {
            uint16_t ctrl = pci_get_word(pci_dev->config + pos +
                                         PCI_MSIX_FLAGS);
            pci_set_word(pci_dev->config + pos + PCI_MSIX_FLAGS,
                         ctrl & ~PCI_MSIX_FLAGS_ENABLE);
            assigned_dev_update_msix(pci_dev, pos + PCI_MSIX_FLAGS);
        }
    } else if (adev->irq_requested_type & KVM_DEV_IRQ_GUEST_MSI) {
        if ((pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_MSI, 0))) {
            uint8_t ctrl = pci_get_byte(pci_dev->config + pos + PCI_MSI_FLAGS);
            pci_set_byte(pci_dev->config + pos + PCI_MSI_FLAGS,
                         ctrl & ~PCI_MSI_FLAGS_ENABLE);
            assigned_dev_update_msi(pci_dev, pos + PCI_MSI_FLAGS);
        }
    }

    snprintf(reset_file, sizeof(reset_file),
             "/sys/bus/pci/devices/%04x:%02x:%02x.%01x/reset",
             adev->host.seg, adev->host.bus, adev->host.dev, adev->host.func);

    /*
     * Issue a device reset via pci-sysfs.  Note that we use write(2) here
     * and ignore the return value because some kernels have a bug that
     * returns 0 rather than bytes written on success, sending us into an
     * infinite retry loop using other write mechanisms.
     */
    fd = open(reset_file, O_WRONLY);
    if (fd != -1) {
        ret = write(fd, reset, strlen(reset));
        close(fd);
    }

    /*
     * When a 0 is written to the command register, the device is logically
     * disconnected from the PCI bus. This avoids further DMA transfers.
     */
    assigned_dev_pci_write_config(pci_dev, PCI_COMMAND, 0, 2);
}

static void assigned_suspend_notify(Notifier *notifier, void *data)
{
    qemu_bh_schedule(suspend_bh);
}

static void assigned_suspend_bh(void *opaque)
{
    qemu_system_wakeup_request(QEMU_WAKEUP_REASON_OTHER);
}

static int assigned_initfn(struct PCIDevice *pci_dev)
{
    AssignedDevice *dev = DO_UPCAST(AssignedDevice, dev, pci_dev);
    uint8_t e_device, e_intx;
    int r;

    if (!kvm_enabled()) {
        error_report("pci-assign: error: requires KVM support");
        return -1;
    }

    {   /*  RHEL6.1 bz670787 */
        AssignedDevice *adev;
        int i = 0;

        QLIST_FOREACH(adev, &devs, next) {
            i++;
        }

        if (i >= MAX_DEV_ASSIGN_CMDLINE) {
            error_report("pci-assign: Maximum supported assigned devices (%d) "
                         "already attached\n", MAX_DEV_ASSIGN_CMDLINE);
            return -1;
        }
    }

    if (!dev->host.seg && !dev->host.bus && !dev->host.dev && !dev->host.func) {
        error_report("pci-assign: error: no host device specified");
        return -1;
    }

    if (get_real_device(dev, dev->host.seg, dev->host.bus,
			dev->host.dev, dev->host.func)) {
        error_report("pci-assign: Error: Couldn't get real device (%s)!",
                     dev->dev.qdev.id);
        goto out;
    }

    /* handle real device's MMIO/PIO BARs */
    if (assigned_dev_register_regions(dev->real_device.regions,
                                      dev->real_device.region_number,
                                      dev))
        goto out;

    /* handle interrupt routing */
    e_device = (dev->dev.devfn >> 3) & 0x1f;
    e_intx = dev->dev.config[0x3d] - 1;
    dev->intpin = e_intx;
    dev->run = 0;
    dev->girq = -1;
    dev->h_segnr = dev->host.seg;
    dev->h_busnr = dev->host.bus;
    dev->h_devfn = PCI_DEVFN(dev->host.dev, dev->host.func);

    if (assigned_device_pci_cap_init(pci_dev) < 0)
        goto out;

    /* assign device to guest */
    r = assign_device(dev);
    if (r < 0)
        goto out;

    /* assign irq for the device */
    r = assign_irq(dev);
    if (r < 0)
        goto assigned_out;

    /* intercept MSI-X entry page in the MMIO */
    if (dev->cap.available & ASSIGNED_DEVICE_CAP_MSIX)
        if (assigned_dev_register_msix_mmio(dev))
            goto assigned_out;

    assigned_dev_load_option_rom(dev);

    if (QLIST_EMPTY(&devs)) {
        suspend_notifier.notify = assigned_suspend_notify;
        qemu_register_suspend_notifier(&suspend_notifier);
        suspend_bh = qemu_bh_new(assigned_suspend_bh, dev);
    }
    QLIST_INSERT_HEAD(&devs, dev, next);

    add_boot_device_path(dev->bootindex, &pci_dev->qdev, NULL);

    /* Register a vmsd so that we can mark it unmigratable. */
    vmstate_register(&dev->dev.qdev, 0, &vmstate_assigned_device, dev);
    register_device_unmigratable(&dev->dev.qdev,
                                 vmstate_assigned_device.name, dev);


    return 0;

assigned_out:
    deassign_device(dev);
out:
    free_assigned_device(dev);
    return -1;
}

static int assigned_exitfn(struct PCIDevice *pci_dev)
{
    AssignedDevice *dev = DO_UPCAST(AssignedDevice, dev, pci_dev);

    vmstate_unregister(&dev->dev.qdev, &vmstate_assigned_device, dev);
    QLIST_REMOVE(dev, next);
    if (QLIST_EMPTY(&devs)) {
        qemu_bh_delete(suspend_bh);
        qemu_unregister_suspend_notifier(&suspend_notifier);
    }
    deassign_device(dev);
    free_assigned_device(dev);
    return 0;
}

static int parse_hostaddr(DeviceState *dev, Property *prop, const char *str)
{
    PCIHostDevice *ptr = qdev_get_prop_ptr(dev, prop);
    int rc;

    rc = pci_parse_host_devaddr(str, &ptr->seg, &ptr->bus, &ptr->dev, &ptr->func);
    if (rc != 0)
        return -1;
    return 0;
}

static int print_hostaddr(DeviceState *dev, Property *prop, char *dest, size_t len)
{
    PCIHostDevice *ptr = qdev_get_prop_ptr(dev, prop);

    if (ptr->seg) {
        return snprintf(dest, len, "%04x:%02x:%02x.%x", ptr->seg, ptr->bus,
                        ptr->dev, ptr->func);
    } else {
        return snprintf(dest, len, "%02x:%02x.%x", ptr->bus, ptr->dev, ptr->func);
    }
}

PropertyInfo qdev_prop_hostaddr = {
    .name  = "pci-hostaddr",
    .type  = -1,
    .size  = sizeof(PCIHostDevice),
    .parse = parse_hostaddr,
    .print = print_hostaddr,
};

static PCIDeviceInfo assign_info = {
    .qdev.name    = "pci-assign",
    .qdev.desc    = "pass through host pci devices to the guest",
    .qdev.size    = sizeof(AssignedDevice),
    .qdev.reset   = reset_assigned_device,
    .init         = assigned_initfn,
    .exit         = assigned_exitfn,
    .config_read  = assigned_dev_pci_read_config,
    .config_write = assigned_dev_pci_write_config,
    .qdev.props   = (Property[]) {
        DEFINE_PROP("host", AssignedDevice, host, qdev_prop_hostaddr, PCIHostDevice),
        DEFINE_PROP_BIT("iommu", AssignedDevice, features,
                        ASSIGNED_DEVICE_USE_IOMMU_BIT, true),
        DEFINE_PROP_BIT("prefer_msi", AssignedDevice, features,
                        ASSIGNED_DEVICE_PREFER_MSI_BIT, true),
        DEFINE_PROP_INT32("bootindex", AssignedDevice, bootindex, -1),
        DEFINE_PROP_STRING("configfd", AssignedDevice, configfd_name),
        DEFINE_PROP_END_OF_LIST(),
    },
};

static void assign_register_devices(void)
{
    pci_qdev_register(&assign_info);
}

device_init(assign_register_devices)


/*
 * Syntax to assign device:
 *
 * -pcidevice host=[seg:]bus:dev.func[,dma=none][,name=Foo]
 *
 * Example:
 * -pcidevice host=00:13.0,dma=pvdma
 *
 * dma can currently only be 'none' to disable iommu support.
 */
QemuOpts *add_assigned_device(const char *arg)
{
    QemuOpts *opts = NULL;
    char host[64], id[64], dma[8];
    int r;

    r = get_param_value(host, sizeof(host), "host", arg);
    if (!r)
         goto bad;
    r = get_param_value(id, sizeof(id), "id", arg);
    if (!r)
        r = get_param_value(id, sizeof(id), "name", arg);
    if (!r)
        r = get_param_value(id, sizeof(id), "host", arg);

    opts = qemu_opts_create(&qemu_device_opts, id, 0);
    if (!opts)
        goto bad;
    qemu_opt_set(opts, "driver", "pci-assign");
    qemu_opt_set(opts, "host", host);

#ifdef KVM_CAP_IOMMU
    r = get_param_value(dma, sizeof(dma), "dma", arg);
    if (r && !strncmp(dma, "none", 4))
        qemu_opt_set(opts, "iommu", "0");
#endif
    qemu_opts_print(opts, NULL);
    return opts;

bad:
    fprintf(stderr, "pcidevice argument parse error; "
            "please check the help text for usage\n");
    if (opts)
        qemu_opts_del(opts);
    return NULL;
}

void add_assigned_devices(PCIBus *bus, const char **devices, int n_devices)
{
    QemuOpts *opts;
    int i;

    for (i = 0; i < n_devices; i++) {
        opts = add_assigned_device(devices[i]);
        if (opts == NULL) {
            fprintf(stderr, "Could not add assigned device %s\n", devices[i]);
            exit(1);
        }
        /* generic code will call qdev_device_add() for the device */
    }
}

/*
 * Scan the assigned devices for the devices that have an option ROM, and then
 * load the corresponding ROM data to RAM. If an error occurs while loading an
 * option ROM, we just ignore that option ROM and continue with the next one.
 */
static void assigned_dev_load_option_rom(AssignedDevice *dev)
{
    char name[32], rom_file[64];
    FILE *fp;
    uint8_t val;
    struct stat st;
    void *ptr;

    /* If loading ROM from file, pci handles it */
    if (dev->dev.romfile || !dev->dev.rom_bar)
        return;

    snprintf(rom_file, sizeof(rom_file),
             "/sys/bus/pci/devices/%04x:%02x:%02x.%01x/rom",
             dev->host.seg, dev->host.bus, dev->host.dev, dev->host.func);

    if (stat(rom_file, &st)) {
        return;
    }

    if (access(rom_file, F_OK)) {
        fprintf(stderr, "pci-assign: Insufficient privileges for %s\n",
                rom_file);
        return;
    }

    /* Write "1" to the ROM file to enable it */
    fp = fopen(rom_file, "r+");
    if (fp == NULL) {
        return;
    }
    val = 1;
    if (fwrite(&val, 1, 1, fp) != 1) {
        goto close_rom;
    }
    fseek(fp, 0, SEEK_SET);

    snprintf(name, sizeof(name), "%s.rom", dev->dev.qdev.info->name);
    dev->dev.rom_offset = qemu_ram_alloc(&dev->dev.qdev, name, st.st_size);
    ptr = qemu_get_ram_ptr(dev->dev.rom_offset);
    memset(ptr, 0xff, st.st_size);

    if (!fread(ptr, 1, st.st_size, fp)) {
        fprintf(stderr, "pci-assign: Cannot read from host %s\n"
                "\tDevice option ROM contents are probably invalid "
                "(check dmesg).\n\tSkip option ROM probe with rombar=0, "
                "or load from file with romfile=\n", rom_file);
        qemu_ram_free(dev->dev.rom_offset);
        dev->dev.rom_offset = 0;
        goto close_rom;
    }

    pci_register_bar(&dev->dev, PCI_ROM_SLOT,
                     st.st_size, 0, pci_map_option_rom);
close_rom:
    /* Write "0" to disable ROM */
    fseek(fp, 0, SEEK_SET);
    val = 0;
    if (!fwrite(&val, 1, 1, fp)) {
        DEBUG("%s\n", "Failed to disable pci-sysfs rom file");
    }
    fclose(fp);
}
