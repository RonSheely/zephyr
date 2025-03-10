/* SPDX-License-Identifier: MIT */
/*
 ****************************************************************************
 * (C) 2006 - Cambridge University
 * (C) 2021-2024 - EPAM Systems
 ****************************************************************************
 *
 *        File: gnttab.c
 *      Author: Steven Smith (sos22@cam.ac.uk)
 *     Changes: Grzegorz Milos (gm281@cam.ac.uk)
 *
 *        Date: July 2006
 *
 * Environment: Xen Minimal OS
 * Description: Simple grant tables implementation. About as stupid as it's
 *  possible to be and still work.
 *
 ****************************************************************************
 */
#include <zephyr/arch/arm64/hypercall.h>
#include <zephyr/xen/generic.h>
#include <zephyr/xen/gnttab.h>
#include <zephyr/xen/public/grant_table.h>
#include <zephyr/xen/public/memory.h>
#include <zephyr/xen/public/xen.h>
#include <zephyr/sys/barrier.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/device_mmio.h>

LOG_MODULE_REGISTER(xen_gnttab);

/* Timeout for grant table ops retrying */
#define GOP_RETRY_DELAY 200

#define GNTTAB_GREF_USED	(UINT32_MAX - 1)
#define GNTTAB_SIZE		(CONFIG_NR_GRANT_FRAMES * XEN_PAGE_SIZE)
#define NR_GRANT_ENTRIES	(GNTTAB_SIZE / sizeof(grant_entry_v1_t))

BUILD_ASSERT(GNTTAB_SIZE <= DT_REG_SIZE_BY_IDX(DT_INST(0, xen_xen), 0),
	     "Number of grant frames is bigger than grant table DT region!");
BUILD_ASSERT(GNTTAB_SIZE <= CONFIG_KERNEL_VM_SIZE);

static struct gnttab {
	struct k_sem sem;
	grant_entry_v1_t *table;
	grant_ref_t gref_list[NR_GRANT_ENTRIES];
} gnttab;

static grant_ref_t get_free_entry(void)
{
	grant_ref_t gref;
	unsigned int flags;

	k_sem_take(&gnttab.sem, K_FOREVER);

	flags = irq_lock();
	gref = gnttab.gref_list[0];
	__ASSERT((gref >= GNTTAB_NR_RESERVED_ENTRIES &&
		gref < NR_GRANT_ENTRIES), "Invalid gref = %d", gref);
	gnttab.gref_list[0] = gnttab.gref_list[gref];
	gnttab.gref_list[gref] = GNTTAB_GREF_USED;
	irq_unlock(flags);

	return gref;
}

static void put_free_entry(grant_ref_t gref)
{
	unsigned int flags;

	flags = irq_lock();
	if (gnttab.gref_list[gref] != GNTTAB_GREF_USED) {
		LOG_WRN("Trying to put already free gref = %u", gref);

		return;
	}

	gnttab.gref_list[gref] = gnttab.gref_list[0];
	gnttab.gref_list[0] = gref;

	irq_unlock(flags);

	k_sem_give(&gnttab.sem);
}

static void gnttab_grant_permit_access(grant_ref_t gref, domid_t domid,
		unsigned long gfn, bool readonly)
{
	uint16_t flags = GTF_permit_access;

	if (readonly) {
		flags |= GTF_readonly;
	}

	gnttab.table[gref].frame = gfn;
	gnttab.table[gref].domid = domid;
	/* Need to be sure that gfn and domid will be set before flags */
	barrier_dmem_fence_full();

	gnttab.table[gref].flags = flags;
}

grant_ref_t gnttab_grant_access(domid_t domid, unsigned long gfn,
		bool readonly)
{
	grant_ref_t gref = get_free_entry();

	gnttab_grant_permit_access(gref, domid, gfn, readonly);

	return gref;
}

/* Reset flags to zero in order to stop using the grant */
static int gnttab_reset_flags(grant_ref_t gref)
{
	uint16_t flags, nflags;
	uint16_t *pflags;

	pflags = &gnttab.table[gref].flags;
	nflags = *pflags;

	do {
		flags = nflags;
		if (flags & (GTF_reading | GTF_writing)) {
			LOG_WRN("gref = %u still in use! (0x%x)\n",
				gref, flags);
			return 1;
		}
		nflags = synch_cmpxchg(pflags, flags, 0);
	} while (nflags != flags);

	return 0;
}

int gnttab_end_access(grant_ref_t gref)
{
	int rc;

	__ASSERT((gref >= GNTTAB_NR_RESERVED_ENTRIES &&
		gref < NR_GRANT_ENTRIES), "Invalid gref = %d", gref);

	rc = gnttab_reset_flags(gref);
	if (!rc) {
		return rc;
	}

	put_free_entry(gref);

	return 0;
}

int32_t gnttab_alloc_and_grant(void **map, bool readonly)
{
	void *page;
	unsigned long gfn;
	grant_ref_t gref;

	__ASSERT_NO_MSG(map != NULL);

	page = k_aligned_alloc(XEN_PAGE_SIZE, XEN_PAGE_SIZE);
	if (page == NULL) {
		return -ENOMEM;
	}

	gfn = xen_virt_to_gfn(page);
	gref = gnttab_grant_access(0, gfn, readonly);

	*map = page;

	return gref;
}

static void gop_eagain_retry(int cmd, struct gnttab_map_grant_ref *gref)
{
	unsigned int step = 10, delay = step;
	int16_t *status = &gref->status;

	do {
		HYPERVISOR_grant_table_op(cmd, gref, 1);
		if (*status == GNTST_eagain) {
			k_sleep(K_MSEC(delay));
		}

		delay += step;
	} while ((*status == GNTST_eagain) && (delay < GOP_RETRY_DELAY));

	if (delay >= GOP_RETRY_DELAY) {
		LOG_ERR("Failed to map grant, timeout reached\n");
		*status = GNTST_bad_page;
	}
}

void *gnttab_get_page(void)
{
	int ret;
	void *page_addr;
	struct xen_remove_from_physmap rfpm;

	page_addr = k_aligned_alloc(XEN_PAGE_SIZE, XEN_PAGE_SIZE);
	if (!page_addr) {
		LOG_WRN("Failed to allocate memory for gnttab page!\n");
		return NULL;
	}

	rfpm.domid = DOMID_SELF;
	rfpm.gpfn = xen_virt_to_gfn(page_addr);

	/*
	 * GNTTABOP_map_grant_ref will simply replace the entry in the P2M
	 * and not release any RAM that may have been associated with
	 * page_addr, so we release this memory before mapping.
	 */
	ret = HYPERVISOR_memory_op(XENMEM_remove_from_physmap, &rfpm);
	if (ret) {
		LOG_WRN("Failed to remove gnttab page from physmap, ret = %d\n", ret);
		return NULL;
	}

	return page_addr;
}

void gnttab_put_page(void *page_addr)
{
	int ret, nr_extents = 1;
	struct xen_memory_reservation reservation;
	xen_pfn_t page = xen_virt_to_gfn(page_addr);

	/*
	 * After unmapping there will be a 4Kb holes in address space
	 * at 'page_addr' positions. To keep it contiguous and be able
	 * to return such addresses to memory allocator we need to
	 * populate memory on unmapped positions here.
	 */
	memset(&reservation, 0, sizeof(reservation));
	reservation.domid = DOMID_SELF;
	reservation.extent_order = 0;
	reservation.nr_extents = nr_extents;
	set_xen_guest_handle(reservation.extent_start, &page);

	ret = HYPERVISOR_memory_op(XENMEM_populate_physmap, &reservation);
	if (ret != nr_extents) {
		LOG_WRN("failed to populate physmap on gfn = 0x%llx, ret = %d\n",
			page, ret);
		return;
	}

	k_free(page_addr);
}

int gnttab_map_refs(struct gnttab_map_grant_ref *map_ops, unsigned int count)
{
	int i, ret;

	ret = HYPERVISOR_grant_table_op(GNTTABOP_map_grant_ref, map_ops, count);
	if (ret) {
		return ret;
	}

	for (i = 0; i < count; i++) {
		switch (map_ops[i].status) {
		case GNTST_no_device_space:
			LOG_WRN("map_grant_ref failed, no device space for page #%d\n", i);
			break;

		case GNTST_eagain:
			/* Operation not done; need to try again */
			gop_eagain_retry(GNTTABOP_map_grant_ref, &map_ops[i]);
			/* Need to re-check status for current page */
			i--;

			break;

		default:
			break;
		}
	}

	return 0;
}

int gnttab_unmap_refs(struct gnttab_unmap_grant_ref *unmap_ops, unsigned int count)
{
	return HYPERVISOR_grant_table_op(GNTTABOP_unmap_grant_ref, unmap_ops, count);
}


static const char * const gnttab_error_msgs[] = GNTTABOP_error_msgs;

const char *gnttabop_error(int16_t status)
{
	status = -status;
	if (status < 0 || (uint16_t) status >= ARRAY_SIZE(gnttab_error_msgs)) {
		return "bad status";
	} else {
		return gnttab_error_msgs[status];
	}
}

/* Picked from Linux implementation */
#define LEGACY_MAX_GNT_FRAMES_SUPPORTED		4
static unsigned long gnttab_get_max_frames(void)
{
	int ret;
	struct gnttab_query_size q = {
		.dom = DOMID_SELF,
	};

	ret = HYPERVISOR_grant_table_op(GNTTABOP_query_size, &q, 1);
	if ((ret < 0) || (q.status != GNTST_okay)) {
		return LEGACY_MAX_GNT_FRAMES_SUPPORTED;
	}

	return q.max_nr_frames;
}

static int gnttab_init(void)
{
	grant_ref_t gref;
	struct xen_add_to_physmap xatp;
	int rc = 0, i;
	unsigned long xen_max_grant_frames;
	uintptr_t gnttab_base = DT_REG_ADDR_BY_IDX(DT_INST(0, xen_xen), 0);
	mm_reg_t gnttab_reg;

	xen_max_grant_frames = gnttab_get_max_frames();
	if (xen_max_grant_frames < CONFIG_NR_GRANT_FRAMES) {
		LOG_ERR("Xen max_grant_frames is less than CONFIG_NR_GRANT_FRAMES!");
		k_panic();
	}

	/* Will be taken/given during gnt_refs allocation/release */
	k_sem_init(&gnttab.sem, NR_GRANT_ENTRIES - GNTTAB_NR_RESERVED_ENTRIES,
		   NR_GRANT_ENTRIES - GNTTAB_NR_RESERVED_ENTRIES);

	/* Initialize O(1) allocator, gnttab.gref_list[0] always shows first free entry */
	gnttab.gref_list[0] = GNTTAB_NR_RESERVED_ENTRIES;
	gnttab.gref_list[NR_GRANT_ENTRIES - 1] = 0;
	for (gref = GNTTAB_NR_RESERVED_ENTRIES; gref < NR_GRANT_ENTRIES - 1; gref++) {
		gnttab.gref_list[gref] = gref + 1;
	}

	for (i = CONFIG_NR_GRANT_FRAMES - 1; i >= 0; i--) {
		xatp.domid = DOMID_SELF;
		xatp.size = 0;
		xatp.space = XENMAPSPACE_grant_table;
		xatp.idx = i;
		xatp.gpfn = xen_virt_to_gfn(gnttab_base) + i;
		rc = HYPERVISOR_memory_op(XENMEM_add_to_physmap, &xatp);
		__ASSERT(!rc, "add_to_physmap failed; status = %d\n", rc);
	}

	/*
	 * Xen DT region reserved for grant table (first reg in hypervisor node)
	 * may be much bigger than CONFIG_NR_GRANT_FRAMES multiplied by page size.
	 * Thus, we need to map only part of region, that is limited by config.
	 * The size of this part is calculated in GNTTAB_SIZE macro and used as
	 * parameter for device_map()
	 */
	device_map(&gnttab_reg, gnttab_base, GNTTAB_SIZE, K_MEM_CACHE_WB | K_MEM_PERM_RW);
	gnttab.table = (grant_entry_v1_t *)gnttab_reg;

	LOG_DBG("%s: grant table mapped\n", __func__);

	return 0;
}

SYS_INIT(gnttab_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);
