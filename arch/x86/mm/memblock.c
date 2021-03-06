#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/bitops.h>
#include <linux/memblock.h>
#include <linux/bootmem.h>
#include <linux/mm.h>
#include <linux/range.h>

/* Check for already reserved areas */
/* memblock에서 검색하는 영역(addr~last)이 예약할수(reserved) 없는지 체크 */
bool __init memblock_x86_check_reserved_size(u64 *addrp, u64 *sizep, u64 align)
{
	struct memblock_region *r;
	u64 addr = *addrp, last;
	u64 size = *sizep;
	bool changed = false;

again:
	last = addr + size;
	for_each_memblock(reserved, r) { /* 빙빙돈다. */
		/* 체크하려는 영역(addr~last)의 뒤에 memblock이 겹치는 경우 해당영역 삭제 */
		if (last > r->base && addr < r->base) {
			size = r->base - addr;
			changed = true;
			goto again;
		}
		/* 체크하려는 블럭의 앞에 memblock이 겹치는 경우 짤라준다. */
		if (last > (r->base + r->size) && addr < (r->base + r->size)) {
			addr = round_up(r->base + r->size, align);
			size = last - addr;
			changed = true;
			goto again;
		}
		/* 체크하려는 블럭이 memblock에 포함되는 경우
		 * 사이즈를 0으로 만들어서 예약 불가능하게 만든다.
		 */
		if (last <= (r->base + r->size) && addr >= r->base) {
			*sizep = 0;
			return false;
		}
	}
	if (changed) {		/* 변했으면 다시 대입 */
		*addrp = addr;
		*sizep = size;
	}
	return changed;
}

/*
 * Find next free range after start, and size is returned in *sizep
 */
/* start부터 연속적으로 할당 가능한 메모리 크기를 구해넣고
 * align된 시작주소를 반환한다.
 */
u64 __init memblock_x86_find_in_range_size(u64 start, u64 *sizep, u64 align)
{
	struct memblock_region *r;

	for_each_memblock(memory, r) {
		u64 ei_start = r->base;
		u64 ei_last = ei_start + r->size;
		u64 addr;
		/* align 올림 한다. */
		addr = round_up(ei_start, align);
		if (addr < start)
			addr = round_up(start, align);
		/* 겹치지 않으면 패스 */
		if (addr >= ei_last)
			continue;
		/* 이 memblock에서 할당가능한 크기 */
		*sizep = ei_last - addr;
		/* reserved 되어있는지 체크 */
		while (memblock_x86_check_reserved_size(&addr, sizep, align))
			;

		if (*sizep)
			return addr; /* 시작주소 리턴 */
	}

	return MEMBLOCK_ERROR;
}
/* memblock에서  */
static __init struct range *find_range_array(int count)
{
	u64 end, size, mem;
	struct range *range;

	size = sizeof(struct range) * count;
	end = memblock.current_limit;

	mem = memblock_find_in_range(0, end, size, sizeof(struct range));
	if (mem == MEMBLOCK_ERROR)
		panic("can not find more space for range array");

	/*
	 * This range is tempoaray, so don't reserve it, it will not be
	 * overlapped because We will not alloccate new buffer before
	 * We discard this one
	 */
	range = __va(mem);
	memset(range, 0, size);

	return range;
}

static void __init memblock_x86_subtract_reserved(struct range *range, int az)
{
	u64 final_start, final_end;
	struct memblock_region *r;

	/* Take out region array itself at first*/
	memblock_free_reserved_regions();

	memblock_dbg("Subtract (%ld early reservations)\n", memblock.reserved.cnt);

	for_each_memblock(reserved, r) {
		memblock_dbg("  [%010llx-%010llx]\n", (u64)r->base, (u64)r->base + r->size - 1);
		final_start = PFN_DOWN(r->base);
		final_end = PFN_UP(r->base + r->size);
		if (final_start >= final_end)
			continue;
		subtract_range(range, az, final_start, final_end);
	}

	/* Put region array back ? */
	memblock_reserve_reserved_regions();
}

struct count_data {
	int nr;
};

static int __init count_work_fn(unsigned long start_pfn,
				unsigned long end_pfn, void *datax)
{
	struct count_data *data = datax;

	data->nr++;

	return 0;
}

static int __init count_early_node_map(int nodeid)
{
	struct count_data data;

	data.nr = 0;
	work_with_active_regions(nodeid, count_work_fn, &data);

	return data.nr;
}

int __init __get_free_all_memory_range(struct range **rangep, int nodeid,
			 unsigned long start_pfn, unsigned long end_pfn)
{
	int count;
	struct range *range;
	int nr_range;

	count = (memblock.reserved.cnt + count_early_node_map(nodeid)) * 2;

	range = find_range_array(count);
	nr_range = 0;

	/*
	 * Use early_node_map[] and memblock.reserved.region to get range array
	 * at first
	 */
	nr_range = add_from_early_node_map(range, count, nr_range, nodeid);
	subtract_range(range, count, 0, start_pfn);
	subtract_range(range, count, end_pfn, -1ULL);

	memblock_x86_subtract_reserved(range, count);
	nr_range = clean_sort_range(range, count);

	*rangep = range;
	return nr_range;
}

int __init get_free_all_memory_range(struct range **rangep, int nodeid)
{
	unsigned long end_pfn = -1UL;

#ifdef CONFIG_X86_32
	end_pfn = max_low_pfn;
#endif
	return __get_free_all_memory_range(rangep, nodeid, 0, end_pfn);
}
/**
 * memblock 구조체(memroy, reserved)에서 range(addr~limit)에 속하는 크기를 구한다.
 * get_free가 참이면 free한 공간만을 구한다.
 */
static u64 __init __memblock_x86_memory_in_range(u64 addr, u64 limit, bool get_free)
{
	int i, count;
	struct range *range;
	int nr_range;
	u64 final_start, final_end;
	u64 free_size;
	struct memblock_region *r;
	/* 예약된 영역을 subtract 하면서 쪼개질수 있기 때문에 공간을 넉넉하게 잡는것 같다. */
	count = (memblock.reserved.cnt + memblock.memory.cnt) * 2;
	/* memblock의 뒤쪽에서 count만큼의 공간을 찾는다. */
	range = find_range_array(count);
	nr_range = 0;

	addr = PFN_UP(addr);
	limit = PFN_DOWN(limit);
	/* memblock중 memory에 해당하는 블럭을 탐색  */
	for_each_memblock(memory, r) {
		/* Page 단위로 사용하기 위해 Page 단위로 정렬 */
		final_start = PFN_UP(r->base);
		final_end = PFN_DOWN(r->base + r->size);
		/* 올림, 내림 했을때 이상이 있으면 패스 */
		if (final_start >= final_end)
			continue;
		/* 인자로 들어온 영역을 밖이면 패스 */
		if (final_start >= limit || final_end <= addr)
			continue;

		nr_range = add_range(range, count, nr_range, final_start, final_end);
	}
	/* 인자값으로 들어온 영역에 속하지 않으면 뺀다. */
	subtract_range(range, count, 0, addr);
	subtract_range(range, count, limit, -1ULL);

	/* Subtract memblock.reserved.region in range ? */
	/* get_free 비트가 켜있으면 예약된 공간을 빼서 사용가능한 부분만 남긴다. */
	if (!get_free)
		goto sort_and_count_them;
	for_each_memblock(reserved, r) {
		/* 영역을 뺄때는 반대로 시작지점을 내리고 끝을 올린다. */
		final_start = PFN_DOWN(r->base);
		final_end = PFN_UP(r->base + r->size);
		if (final_start >= final_end)
			continue;
		if (final_start >= limit || final_end <= addr)
			continue;

		subtract_range(range, count, final_start, final_end);
	}

sort_and_count_them:
	/* 빈 range 삭제와 정렬. nr_range는 갯수 */
	nr_range = clean_sort_range(range, count);

	free_size = 0;
	/* 크기를 구한다. */
	for (i = 0; i < nr_range; i++)
		free_size += range[i].end - range[i].start;

	return free_size << PAGE_SHIFT;
}
/* 해당 range중 free한 memblock 공간을 구한다. */
u64 __init memblock_x86_free_memory_in_range(u64 addr, u64 limit)
{
	return __memblock_x86_memory_in_range(addr, limit, true);
}
/* memblock에서 해당 range에 속하는 크기를 구한다. */
u64 __init memblock_x86_memory_in_range(u64 addr, u64 limit)
{
	return __memblock_x86_memory_in_range(addr, limit, false);
}

/* start부터 end까지 영역을 name의 스트링으로 예약한다. */
void __init memblock_x86_reserve_range(u64 start, u64 end, char *name)
{
	if (start == end)	/* 크기가 0이면 리턴 */
		return;
	/* 워닝! 워닝! */
	if (WARN_ONCE(start > end, "memblock_x86_reserve_range: wrong range [%#llx, %#llx)\n", start, end))
		return;

	memblock_dbg("    memblock_x86_reserve_range: [%#010llx-%#010llx] %16s\n", start, end - 1, name);

	memblock_reserve(start, end - start);
}

/* 해당 영역을 reserve에서 제거 */
void __init memblock_x86_free_range(u64 start, u64 end)
{
	/* 0이면 나간다 */
	if (start == end)
		return;

	if (WARN_ONCE(start > end, "memblock_x86_free_range: wrong range [%#llx, %#llx)\n", start, end))
		return;

	memblock_dbg("       memblock_x86_free_range: [%#010llx-%#010llx]\n", start, end - 1);

	memblock_free(start, end - start);
}

/*
 * Need to call this function after memblock_x86_register_active_regions,
 * so early_node_map[] is filled already.
 */
u64 __init memblock_x86_find_in_range_node(int nid, u64 start, u64 end, u64 size, u64 align)
{
	u64 addr;
	addr = find_memory_core_early(nid, size, align, start, end);
	if (addr != MEMBLOCK_ERROR)
		return addr;

	/* Fallback, should already have start end within node range */
	return memblock_find_in_range(start, end, size, align);
}

/*
 * Finds an active region in the address range from start_pfn to last_pfn and
 * returns its range in ei_startpfn and ei_endpfn for the memblock entry.
 */
/* ei에서 start_pfn과 last_pfn중에서 사용가능한 영역을 찾아,
 * ei_startpfn과 ei_endpfn에 저장 */
static int __init memblock_x86_find_active_region(const struct memblock_region *ei,
				  unsigned long start_pfn,
				  unsigned long last_pfn,
				  unsigned long *ei_startpfn,
				  unsigned long *ei_endpfn)
{
	u64 align = PAGE_SIZE;

	*ei_startpfn = round_up(ei->base, align) >> PAGE_SHIFT;
	*ei_endpfn = round_down(ei->base + ei->size, align) >> PAGE_SHIFT;

	/* Skip map entries smaller than a page */
	if (*ei_startpfn >= *ei_endpfn)
		return 0;

	/* Skip if map is outside the node */
	if (*ei_endpfn <= start_pfn || *ei_startpfn >= last_pfn)
		return 0;

	/* Check for overlaps */
	if (*ei_startpfn < start_pfn)
		*ei_startpfn = start_pfn;
	if (*ei_endpfn > last_pfn)
		*ei_endpfn = last_pfn;

	return 1;
}

/* Walk the memblock.memory map and register active regions within a node */
/* node에 사용가능한 memblock 영역을 등록 */
void __init memblock_x86_register_active_regions(int nid, unsigned long start_pfn,
					 unsigned long last_pfn)
{
	unsigned long ei_startpfn;
	unsigned long ei_endpfn;
	struct memblock_region *r;

	for_each_memblock(memory, r)
		if (memblock_x86_find_active_region(r, start_pfn, last_pfn,
					   &ei_startpfn, &ei_endpfn))
			add_active_range(nid, ei_startpfn, ei_endpfn);
}

/*
 * Find the hole size (in bytes) in the memory range.
 * @start: starting address of the memory range to scan
 * @end: ending address of the memory range to scan
 */
u64 __init memblock_x86_hole_size(u64 start, u64 end)
{
	unsigned long start_pfn = start >> PAGE_SHIFT;
	unsigned long last_pfn = end >> PAGE_SHIFT;
	unsigned long ei_startpfn, ei_endpfn, ram = 0;
	struct memblock_region *r;

	for_each_memblock(memory, r)
		if (memblock_x86_find_active_region(r, start_pfn, last_pfn,
					   &ei_startpfn, &ei_endpfn))
			ram += ei_endpfn - ei_startpfn;

	return end - start - ((u64)ram << PAGE_SHIFT);
}
