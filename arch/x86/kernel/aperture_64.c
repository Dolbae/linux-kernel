/*
 * Firmware replacement code.
 *
 * Work around broken BIOSes that don't set an aperture, only set the
 * aperture in the AGP bridge, or set too small aperture.
 *
 * If all fails map the aperture over some low memory.  This is cheaper than
 * doing bounce buffering. The memory is lost. This is done at early boot
 * because only the bootmem allocator can allocate 32+MB.
 *
 * Copyright 2002 Andi Kleen, SuSE Labs.
 */
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/mmzone.h>
#include <linux/pci_ids.h>
#include <linux/pci.h>
#include <linux/bitops.h>
#include <linux/ioport.h>
#include <linux/suspend.h>
#include <linux/kmemleak.h>
#include <asm/e820.h>
#include <asm/io.h>
#include <asm/iommu.h>
#include <asm/gart.h>
#include <asm/pci-direct.h>
#include <asm/dma.h>
#include <asm/amd_nb.h>
#include <asm/x86_init.h>

/*
 * Using 512M as goal, in case kexec will load kernel_big
 * that will do the on-position decompress, and could overlap with
 * with the gart aperture that is used.
 * Sequence:
 * kernel_small
 * ==> kexec (with kdump trigger path or gart still enabled)
 * ==> kernel_small (gart area become e820_reserved)
 * ==> kexec (with kdump trigger path or gart still enabled)
 * ==> kerne_big (uncompressed size will be big than 64M or 128M)
 * So don't use 512M below as gart iommu, leave the space for kernel
 * code for safe.
 */
#define GART_MIN_ADDR	(512ULL << 20)
#define GART_MAX_ADDR	(1ULL   << 32)

int gart_iommu_aperture;
int gart_iommu_aperture_disabled __initdata;
int gart_iommu_aperture_allowed __initdata;

int fallback_aper_order __initdata = 1; /* 64MB */
int fallback_aper_force __initdata;

int fix_aperture __initdata = 1;

static struct resource gart_resource = {
	.name	= "GART",
	.flags	= IORESOURCE_MEM,
};

static void __init insert_aperture_resource(u32 aper_base, u32 aper_size)
{
	gart_resource.start = aper_base;
	gart_resource.end = aper_base + aper_size - 1;
	insert_resource(&iomem_resource, &gart_resource);
}

/* This code runs before the PCI subsystem is initialized, so just
   access the northbridge directly. */

static u32 __init allocate_aperture(void)
{
	u32 aper_size;
	unsigned long addr;

	/* aper_size should <= 1G */
	if (fallback_aper_order > 5)
		fallback_aper_order = 5;
	aper_size = (32 * 1024 * 1024) << fallback_aper_order;

	/*
	 * Aperture has to be naturally aligned. This means a 2GB aperture
	 * won't have much chance of finding a place in the lower 4GB of
	 * memory. Unfortunately we cannot move it up because that would
	 * make the IOMMU useless.
	 */
	addr = memblock_find_in_range(GART_MIN_ADDR, GART_MAX_ADDR,
				      aper_size, aper_size);
	if (addr == MEMBLOCK_ERROR || addr + aper_size > GART_MAX_ADDR) {
		printk(KERN_ERR
			"Cannot allocate aperture memory hole (%lx,%uK)\n",
				addr, aper_size>>10);
		return 0;
	}
	memblock_x86_reserve_range(addr, addr + aper_size, "aperture64");
	/*
	 * Kmemleak should not scan this block as it may not be mapped via the
	 * kernel direct mapping.
	 */
	kmemleak_ignore(phys_to_virt(addr));
	printk(KERN_INFO "Mapping aperture over %d KB of RAM @ %lx\n",
			aper_size >> 10, addr);
	insert_aperture_resource((u32)addr, aper_size);
	register_nosave_region(addr >> PAGE_SHIFT,
			       (addr+aper_size) >> PAGE_SHIFT);

	return (u32)addr;
}


/* Find a PCI capability */
static u32 __init find_cap(int bus, int slot, int func, int cap)
{
	int bytes;
	u8 pos;
	/**
	 * status 레지스터에서 0x10(CAP_LIST)가 없으면 0을 리턴
	 * Capabilities List - If set to 1 the device implements the pointer
	 * for a New Capabilities Linked list at offset 0x34;
	 * otherwise, the linked list is not available. 
	 */
	if (!(read_pci_config_16(bus, slot, func, PCI_STATUS) &
						PCI_STATUS_CAP_LIST))
		return 0;
	/* cap list 다음 레지스터 위치를 가져온다. */
	pos = read_pci_config_byte(bus, slot, func, PCI_CAPABILITY_LIST);
	/* 0x3c까지는 예약되어 있기 때문에 리스트는 40이후부터 있다. null이면 끝 */
	for (bytes = 0; bytes < 48 && pos >= 0x40; bytes++) {
		u8 id;

		pos &= ~3;	/* 4로 정렬 */
		/**
		 * linked 리스트에서 device? vendor? id중 1바이트만 얻어온다.
		 * 아마도 vendor id의 최하위 바이트
		 * http://www.pcisig.com/specifications/conventional/pci_30/ECN_Conventional_Adv_Caps_27Jul06.pdf
		 * http://www.ece.mtu.edu/faculty/btdavis/courses/mtu_ee3173_f04/papers/PCI_22.pdf
		 */
		/* capabilities ID 자세한건 위 문서를 참조 */
		id = read_pci_config_byte(bus, slot, func, pos+PCI_CAP_LIST_ID);
		if (id == 0xff)	/* 0xff는 reserved 영역 */
			break;
		if (id == cap)	/* cap 값을 찾았으면 해당 위치(레지스터)를 리턴 */
			return pos;
		/* 다음 리스트 위치 */
		pos = read_pci_config_byte(bus, slot, func,
						pos+PCI_CAP_LIST_NEXT);
	}
	return 0;
}

/* Read a standard AGPv3 bridge header */
static u32 __init read_agp(int bus, int slot, int func, int cap, u32 *order)
{
	u32 apsize;
	u32 apsizereg;
	int nbits;
	u32 aper_low, aper_hi;
	u64 aper;
	u32 old_order;
	/**
	 * http://download.intel.com/support/motherboards/desktop/sb/agp30.pdf
	 * 스펙 참조
	 */
	/* AGP 정보 출력 */
	printk(KERN_INFO "AGP bridge at %02x:%02x:%02x\n", bus, slot, func);
	/* +0x14의 하위 12비트는 램 크기다.  */
	apsizereg = read_pci_config_16(bus, slot, func, cap + 0x14);
	if (apsizereg == 0xffffffff) {
		printk(KERN_ERR "APSIZE in AGP bridge unreadable\n");
		return 0;
	}

	/* old_order could be the value from NB gart setting */
	old_order = *order;
	/* 12bits 실제값을 가져온다. */
	apsize = apsizereg & 0xfff;
	/* Some BIOS use weird encodings not in the AGPv3 table. */
	if (apsize & 0xff)	/* 128M까지 값이면 확인겸 상위 비트를 켜준다. */
		apsize |= 0xf00;
	nbits = hweight16(apsize); /* 1인 비트수를 복잡하게 센다. */
	*order = 7 - nbits;	   /* 위쪽 4비트를 켜주기 때문에 7개면 딱 32개다. */
	if ((int)*order < 0) /* < 32MB */
		*order = 0;

	aper_low = read_pci_config(bus, slot, func, 0x10);
	aper_hi = read_pci_config(bus, slot, func, 0x14);
	/* base address 아래 22비트는 없앤다. */
	aper = (aper_low & ~((1<<22)-1)) | ((u64)aper_hi << 32);

	/*
	 * On some sick chips, APSIZE is 0. It means it wants 4G
	 * so let double check that order, and lets trust AMD NB settings:
	 */
	/* aper는 베이스 주소, 인자로 넘어온 크기를 출력  */
	printk(KERN_INFO "Aperture from AGP @ %Lx old size %u MB\n",
			aper, 32 << old_order);
	/* BASE addr 에  (32M+order) shift만큼 더했을때 4G가 넘으면 인자로 넘어온 옛값을 사용 */
	if (aper + (32ULL<<(20 + *order)) > 0x100000000ULL) {
		printk(KERN_INFO "Aperture size %u MB (APSIZE %x) is not right, using settings from NB\n",
				32 << *order, apsizereg);
		*order = old_order;
	}
	/* 문제없으면 AGP의 메모리 출력 */
	printk(KERN_INFO "Aperture from AGP @ %Lx size %u MB (APSIZE %x)\n",
			aper, 32 << *order, apsizereg);
	/* 베이스 주소, 메모리 크기, 단위 */
	if (!aperture_valid(aper, (32*1024*1024) << *order, 32<<20))
		return 0;
	/* 메모리 주소를 리턴 */
	return (u32)aper;
}

/*
 * Look for an AGP bridge. Windows only expects the aperture in the
 * AGP bridge and some BIOS forget to initialize the Northbridge(memory controler , graphic card ) too.
 * Southbridge (soundcard, ... etc)
 * Work around this here.
 *
 * Do an PCI bus scan by hand because we're running before the PCI
 * subsystem.
 *
 * All AMD AGP bridges are AGPv3 compliant, so we can do this scan
 * generically. It's probably overkill to always scan all slots because
 * the AGP bridges should be always an own bus on the HT hierarchy,
 * but do it here for future safety.
 * The AGP3.0 specification only requires the PCI Port controller to
 * provide a means for PCI masters to have peer-to-peer
 * write access to the PCI target that resides on the AGP3.0 Port.
 */
static u32 __init search_agp_bridge(u32 *order, int *valid_agp)
{
	int bus, slot, func;

	/**
	 * Poor man's PCI discovery
	 * 31 30 - 24 23 - 16 15 - 11 10 - 8 7 - 2 1 - 0
	 * Enable Bit Reserved Bus Number Device Number Function Number Register Number 00
	 * http://www.tpikorea.com/micronet-intime-techguide-PCI%20configuration.htm
	 */
	for (bus = 0; bus < 256; bus++) {
		for (slot = 0; slot < 32; slot++) {
			for (func = 0; func < 8; func++) {
				u32 class, cap;
				u8 type;
				class = read_pci_config(bus, slot, func,
							PCI_CLASS_REVISION);
				if (class == 0xffffffff)
					break;
				/* PCI_CLASS_REVISION(08)의 값은
				 * Class code/ Subclass/ Prog IF/ Revision ID 로
				 * 상위 16비트의 class와 subclass값을 switch한다.
				 */
				switch (class >> 16) {
				case PCI_CLASS_BRIDGE_HOST:
				case PCI_CLASS_BRIDGE_OTHER: /* needed? */
					/* AGP bridge? */
					/* capabilities list에서 AGP를 찾는다. */
					cap = find_cap(bus, slot, func,
							PCI_CAP_ID_AGP);
					if (!cap)
						break;
					*valid_agp = 1;
					/* 시스템에 agp가 존재하면 read_agp를 호출한다.
					 * memory 주소를 리턴, 에러면 NULL
					 */
					return read_agp(bus, slot, func, cap,
							order);
				}

				/* No multi-function device? */
				/* Multiple function bit(최상위비트) 확인 */
				type = read_pci_config_byte(bus, slot, func,
							       PCI_HEADER_TYPE);
				/* Multiple function이 켜있으면 나머지 function은 무시한다. */
				if (!(type & 0x80))
					break;
			}
		}
	}
	printk(KERN_INFO "No AGP bridge found\n");

	return 0;
}

static int gart_fix_e820 __initdata = 1;

static int __init parse_gart_mem(char *p)
{
	if (!p)
		return -EINVAL;

	if (!strncmp(p, "off", 3))
		gart_fix_e820 = 0;
	else if (!strncmp(p, "on", 2))
		gart_fix_e820 = 1;

	return 0;
}
early_param("gart_fix_e820", parse_gart_mem);
/*
 * Graphics address remapping table
 * 비디오 메모리를 시스템 메모리에 remapping 해서 직접 억세스 가능하게 한다.
 */
void __init early_gart_iommu_check(void)
{
	/*
	 * in case it is enabled before, esp for kexec/kdump,
	 * previous kernel already enable that. memset called
	 * by allocate_aperture/__alloc_bootmem_nopanic cause restart.
	 * or second kernel have different position for GART hole. and new
	 * kernel could use hole as RAM that is still used by GART set by
	 * first kernel
	 * or BIOS forget to put that in reserved.
	 * try to update e820 to make that region as reserved.
	 */
	u32 agp_aper_order = 0;
	int i, fix, slot, valid_agp = 0;
	u32 ctl;
	u32 aper_size = 0, aper_order = 0, last_aper_order = 0;
	u64 aper_base = 0, last_aper_base = 0;
	int aper_enabled = 0, last_aper_enabled = 0, last_valid = 0;
	/// early가 아니면 리턴
	if (!early_pci_allowed())
		return;

	/* This is mostly duplicate of iommu_hole_init */
	/* agp를 탐색한다. */
	search_agp_bridge(&agp_aper_order, &valid_agp);
	/* AMD 노스브릿지를 쭈욱 검사 */
	fix = 0;
	for (i = 0; amd_nb_bus_dev_ranges[i].dev_limit; i++) {
		int bus;
		int dev_base, dev_limit;

		bus = amd_nb_bus_dev_ranges[i].bus;
		dev_base = amd_nb_bus_dev_ranges[i].dev_base;
		dev_limit = amd_nb_bus_dev_ranges[i].dev_limit;

		for (slot = dev_base; slot < dev_limit; slot++) {
			if (!early_is_amd_nb(read_pci_config(bus, slot, 3, 0x00)))
				continue;

			ctl = read_pci_config(bus, slot, 3, AMD64_GARTAPERTURECTL);
			aper_enabled = ctl & GARTEN;
			aper_order = (ctl >> 1) & 7;
			aper_size = (32 * 1024 * 1024) << aper_order;
			aper_base = read_pci_config(bus, slot, 3, AMD64_GARTAPERTUREBASE) & 0x7fff;
			aper_base <<= 25;

			if (last_valid) {
				if ((aper_order != last_aper_order) ||
				    (aper_base != last_aper_base) ||
				    (aper_enabled != last_aper_enabled)) {
					fix = 1;
					break;
				}
			}

			last_aper_order = aper_order;
			last_aper_base = aper_base;
			last_aper_enabled = aper_enabled;
			last_valid = 1;
		}
	}
	/* 값이 없으면 리턴 */
	if (!fix && !aper_enabled)
		return;
	/* 역시 크기제한 4G이상이 아니어야 한다. */
	if (!aper_base || !aper_size || aper_base + aper_size > 0x100000000UL)
		fix = 1;

	if (gart_fix_e820 && !fix && aper_enabled) {
		/* 겹치는 영역이 있으면 등록하고 update(sanitize) */
		if (e820_any_mapped(aper_base, aper_base + aper_size,
				    E820_RAM)) {
			/* reserve it, so we can reuse it in second kernel */
			printk(KERN_INFO "update e820 for GART\n");
			/* aper 영역을 예약한다. */
			e820_add_region(aper_base, aper_size, E820_RESERVED);
			update_e820();
		}
	}

	if (valid_agp)		/* agp가 유효하면 리턴 */
		return;

	/* disable them all at first */
	/* agp가 없으면 range를 다 돌면서 disable한다. */
	for (i = 0; i < amd_nb_bus_dev_ranges[i].dev_limit; i++) {
		int bus;
		int dev_base, dev_limit;

		bus = amd_nb_bus_dev_ranges[i].bus;
		dev_base = amd_nb_bus_dev_ranges[i].dev_base;
		dev_limit = amd_nb_bus_dev_ranges[i].dev_limit;
		for (slot = dev_base; slot < dev_limit; slot++) {
			if (!early_is_amd_nb(read_pci_config(bus, slot, 3, 0x00)))
				continue;

			ctl = read_pci_config(bus, slot, 3, AMD64_GARTAPERTURECTL);
			ctl &= ~GARTEN;
			/* 1을 mask 하고 다시 써준다. 아마도 0번 비트가 disable */
			write_pci_config(bus, slot, 3, AMD64_GARTAPERTURECTL, ctl);
		}
	}

}

static int __initdata printed_gart_size_msg;

int __init gart_iommu_hole_init(void)
{
	u32 agp_aper_base = 0, agp_aper_order = 0;
	u32 aper_size, aper_alloc = 0, aper_order = 0, last_aper_order = 0;
	u64 aper_base, last_aper_base = 0;
	int fix, slot, valid_agp = 0;
	int i, node;

	if (gart_iommu_aperture_disabled || !fix_aperture ||
	    !early_pci_allowed())
		return -ENODEV;

	printk(KERN_INFO  "Checking aperture...\n");

	if (!fallback_aper_force)
		agp_aper_base = search_agp_bridge(&agp_aper_order, &valid_agp);

	fix = 0;
	node = 0;
	for (i = 0; i < amd_nb_bus_dev_ranges[i].dev_limit; i++) {
		int bus;
		int dev_base, dev_limit;
		u32 ctl;

		bus = amd_nb_bus_dev_ranges[i].bus;
		dev_base = amd_nb_bus_dev_ranges[i].dev_base;
		dev_limit = amd_nb_bus_dev_ranges[i].dev_limit;

		for (slot = dev_base; slot < dev_limit; slot++) {
			if (!early_is_amd_nb(read_pci_config(bus, slot, 3, 0x00)))
				continue;

			iommu_detected = 1;
			gart_iommu_aperture = 1;
			x86_init.iommu.iommu_init = gart_iommu_init;

			ctl = read_pci_config(bus, slot, 3,
					      AMD64_GARTAPERTURECTL);

			/*
			 * Before we do anything else disable the GART. It may
			 * still be enabled if we boot into a crash-kernel here.
			 * Reconfiguring the GART while it is enabled could have
			 * unknown side-effects.
			 */
			ctl &= ~GARTEN;
			write_pci_config(bus, slot, 3, AMD64_GARTAPERTURECTL, ctl);

			aper_order = (ctl >> 1) & 7;
			aper_size = (32 * 1024 * 1024) << aper_order;
			aper_base = read_pci_config(bus, slot, 3, AMD64_GARTAPERTUREBASE) & 0x7fff;
			aper_base <<= 25;

			printk(KERN_INFO "Node %d: aperture @ %Lx size %u MB\n",
					node, aper_base, aper_size >> 20);
			node++;

			if (!aperture_valid(aper_base, aper_size, 64<<20)) {
				if (valid_agp && agp_aper_base &&
				    agp_aper_base == aper_base &&
				    agp_aper_order == aper_order) {
					/* the same between two setting from NB and agp */
					if (!no_iommu &&
					    max_pfn > MAX_DMA32_PFN &&
					    !printed_gart_size_msg) {
						printk(KERN_ERR "you are using iommu with agp, but GART size is less than 64M\n");
						printk(KERN_ERR "please increase GART size in your BIOS setup\n");
						printk(KERN_ERR "if BIOS doesn't have that option, contact your HW vendor!\n");
						printed_gart_size_msg = 1;
					}
				} else {
					fix = 1;
					goto out;
				}
			}

			if ((last_aper_order && aper_order != last_aper_order) ||
			    (last_aper_base && aper_base != last_aper_base)) {
				fix = 1;
				goto out;
			}
			last_aper_order = aper_order;
			last_aper_base = aper_base;
		}
	}

out:
	if (!fix && !fallback_aper_force) {
		if (last_aper_base) {
			unsigned long n = (32 * 1024 * 1024) << last_aper_order;

			insert_aperture_resource((u32)last_aper_base, n);
			return 1;
		}
		return 0;
	}

	if (!fallback_aper_force) {
		aper_alloc = agp_aper_base;
		aper_order = agp_aper_order;
	}

	if (aper_alloc) {
		/* Got the aperture from the AGP bridge */
	} else if ((!no_iommu && max_pfn > MAX_DMA32_PFN) ||
		   force_iommu ||
		   valid_agp ||
		   fallback_aper_force) {
		printk(KERN_INFO
			"Your BIOS doesn't leave a aperture memory hole\n");
		printk(KERN_INFO
			"Please enable the IOMMU option in the BIOS setup\n");
		printk(KERN_INFO
			"This costs you %d MB of RAM\n",
				32 << fallback_aper_order);

		aper_order = fallback_aper_order;
		aper_alloc = allocate_aperture();
		if (!aper_alloc) {
			/*
			 * Could disable AGP and IOMMU here, but it's
			 * probably not worth it. But the later users
			 * cannot deal with bad apertures and turning
			 * on the aperture over memory causes very
			 * strange problems, so it's better to panic
			 * early.
			 */
			panic("Not enough memory for aperture");
		}
	} else {
		return 0;
	}

	/* Fix up the north bridges */
	for (i = 0; i < amd_nb_bus_dev_ranges[i].dev_limit; i++) {
		int bus, dev_base, dev_limit;

		/*
		 * Don't enable translation yet but enable GART IO and CPU
		 * accesses and set DISTLBWALKPRB since GART table memory is UC.
		 */
		u32 ctl = aper_order << 1;

		bus = amd_nb_bus_dev_ranges[i].bus;
		dev_base = amd_nb_bus_dev_ranges[i].dev_base;
		dev_limit = amd_nb_bus_dev_ranges[i].dev_limit;
		for (slot = dev_base; slot < dev_limit; slot++) {
			if (!early_is_amd_nb(read_pci_config(bus, slot, 3, 0x00)))
				continue;

			write_pci_config(bus, slot, 3, AMD64_GARTAPERTURECTL, ctl);
			write_pci_config(bus, slot, 3, AMD64_GARTAPERTUREBASE, aper_alloc >> 25);
		}
	}

	set_up_gart_resume(aper_order, aper_alloc);

	return 1;
}
