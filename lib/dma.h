/*
 * Copyright (c) 2019 Nutanix Inc. All rights reserved.
 *
 * Authors: Mike Cui <cui@nutanix.com>
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *      * Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *      * Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 *      * Neither the name of Nutanix nor the names of its contributors may be
 *        used to endorse or promote products derived from this software without
 *        specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 *  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *  OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 *  DAMAGE.
 *
 */

#ifndef LIB_VFIO_USER_DMA_H
#define LIB_VFIO_USER_DMA_H

/*
 * FIXME check whether DMA regions must be page aligned. If so then the
 * implementation can be greatly simpified.
 */

/*
 * This library emulates a DMA controller for a device emulation application to
 * perform DMA operations on a foreign memory space.
 *
 * Concepts:
 * - A DMA controller has its own 64-bit DMA address space.
 * - Foreign memory is made available to the DMA controller in linear chunks
 *   called memory regions.
 * - Each memory region is backed by a file descriptor and
 *   is registered with the DMA controllers at a unique, non-overlapping
 *   linear span of the DMA address space.
 * - To perform DMA, the application should first build a scatter-gather
 *   list (sglist) of dma_sg_t from DMA addresses. Then the sglist
 *   can be mapped using dma_map_sg() into the process's virtual address space
 *   as an iovec for direct access, and unmapped using dma_unmap_sg() when done.
 *   Every region is mapped into the application's virtual address space
 *   at registration time with R/W permissions.
 *   dma_map_sg() ignores all protection bits and only does lookups and
 *   returns pointers to the previously mapped regions. dma_unmap_sg() is
 *   effectively a no-op.
 */

#ifdef DMA_MAP_PROTECTED
#undef DMA_MAP_FAST
#define DMA_MAP_FAST_IMPL 0
#else
#define DMA_MAP_FAST_IMPL 1
#endif

#include <assert.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>

#include "libvfio-user.h"
#include "common.h"

#define iov_end(iov) ((iov)->iov_base + (iov)->iov_len)

struct vfu_ctx;

typedef struct {
    vfu_dma_info_t info;
    int fd;                     // File descriptor to mmap
    off_t offset;               // File offset
    int refcnt;                 // Number of users of this region
    char *dirty_bitmap;         // Dirty page bitmap
} dma_memory_region_t;

typedef struct {
    int max_regions;
    int nregions;
    struct vfu_ctx *vfu_ctx;
    size_t dirty_pgsize;        // Dirty page granularity
    dma_memory_region_t regions[0];
} dma_controller_t;

dma_controller_t *
dma_controller_create(vfu_ctx_t *vfu_ctx, int max_regions);

void
dma_controller_remove_regions(dma_controller_t *dma);

void
dma_controller_destroy(dma_controller_t *dma);

/* Registers a new memory region.
 * Returns:
 * - On success, a non-negative region number
 * - On failure, a negative integer (-x - 1) where x is the region number
 *   where this region would have been mapped to if the call could succeed
 *   (e.g. due to conflict with existing region).
 */
MOCK_DECLARE(int, dma_controller_add_region, dma_controller_t *dma,
             vfu_dma_addr_t dma_addr, size_t size, int fd, off_t offset,
             uint32_t prot);

MOCK_DECLARE(int, dma_controller_remove_region, dma_controller_t *dma,
             vfu_dma_addr_t dma_addr, size_t size,
             vfu_dma_unregister_cb_t *dma_unregister, void *data);

MOCK_DECLARE(void, dma_controller_unmap_region, dma_controller_t *dma,
             dma_memory_region_t *region);

// Helper for dma_addr_to_sg() slow path.
int
_dma_addr_sg_split(const dma_controller_t *dma,
                   vfu_dma_addr_t dma_addr, uint32_t len,
                   dma_sg_t *sg, int max_sg, int prot);

static bool
_dma_should_mark_dirty(const dma_controller_t *dma, int prot)
{
    assert(dma != NULL);

    return (prot & PROT_WRITE) == PROT_WRITE && dma->dirty_pgsize > 0;
}

static size_t
_get_pgstart(size_t pgsize, void *base_addr, uint64_t offset)
{
    return (offset - (uint64_t)base_addr) / pgsize;
}

static size_t
_get_pgend(size_t pgsize, uint64_t len, size_t start)
{
    return start + (len / pgsize) + (len % pgsize != 0) - 1;
}

static void
_dma_bitmap_get_pgrange(const dma_controller_t *dma,
                           const dma_memory_region_t *region,
                           const dma_sg_t *sg, size_t *start, size_t *end)
{
    assert(dma != NULL);
    assert(region != NULL);
    assert(sg != NULL);
    assert(start != NULL);
    assert(end != NULL);

    *start = _get_pgstart(dma->dirty_pgsize, region->info.iova.iov_base, sg->offset);
    *end = _get_pgend(dma->dirty_pgsize, sg->length, *start);
}

static void
_dma_mark_dirty(const dma_controller_t *dma, const dma_memory_region_t *region,
                dma_sg_t *sg)
{
    size_t i, start, end;

    assert(dma != NULL);
    assert(region != NULL);
    assert(sg != NULL);
    assert(region->dirty_bitmap != NULL);

    _dma_bitmap_get_pgrange(dma, region, sg, &start, &end);
    for (i = start; i <= end; i++) {
        region->dirty_bitmap[i / CHAR_BIT] |= 1 << (i % CHAR_BIT);
    }
}

static inline int
dma_init_sg(const dma_controller_t *dma, dma_sg_t *sg, vfu_dma_addr_t dma_addr,
            uint32_t len, int prot, int region_index)
{
    const dma_memory_region_t *const region = &dma->regions[region_index];

    if ((prot & PROT_WRITE) && !(region->info.prot & PROT_WRITE)) {
        errno = EACCES;
        return -1;
    }

    sg->dma_addr = region->info.iova.iov_base;
    sg->region = region_index;
    sg->offset = dma_addr - region->info.iova.iov_base;
    sg->length = len;
    if (_dma_should_mark_dirty(dma, prot)) {
        _dma_mark_dirty(dma, region, sg);
    }
    sg->mappable = (region->info.vaddr != NULL);

    return 0;
}

/* Takes a linear dma address span and returns a sg list suitable for DMA.
 * A single linear dma address span may need to be split into multiple
 * scatter gather regions due to limitations of how memory can be mapped.
 *
 * Returns:
 * - On success, number of scatter gather entries created.
 * - On failure:
 *     -1 if
 *          - the DMA address span is invalid
 *          - protection violation (errno=EACCES)
 *     (-x - 1) if @max_sg is too small, where x is the number of sg entries
 *     necessary to complete this request.
 */
static inline int
dma_addr_to_sg(const dma_controller_t *dma,
               vfu_dma_addr_t dma_addr, size_t len,
               dma_sg_t *sg, int max_sg, int prot)
{
    static __thread int region_hint;
    int cnt, ret;

    const dma_memory_region_t *const region = &dma->regions[region_hint];
    const void *region_end = iov_end(&region->info.iova);

    // Fast path: single region.
    if (likely(max_sg > 0 && len > 0 &&
               dma_addr >= region->info.iova.iov_base &&
               dma_addr + len <= region_end &&
               region_hint < dma->nregions)) {
        ret = dma_init_sg(dma, sg, dma_addr, len, prot, region_hint);
        if (ret < 0) {
            return ret;
        }

        return 1;
    }
    // Slow path: search through regions.
    cnt = _dma_addr_sg_split(dma, dma_addr, len, sg, max_sg, prot);
    if (likely(cnt > 0)) {
        region_hint = sg->region;
    }
    return cnt;
}

static inline int
dma_map_sg(dma_controller_t *dma, const dma_sg_t *sg, struct iovec *iov,
           int cnt)
{
    dma_memory_region_t *region;
    int i;

    assert(dma != NULL);
    assert(sg != NULL);
    assert(iov != NULL);

    for (i = 0; i < cnt; i++) {
        if (sg[i].region >= dma->nregions) {
            return -EINVAL;
        }
        region = &dma->regions[sg[i].region];

        if (region->info.vaddr == NULL) {
            return -EFAULT;
        }

        vfu_log(dma->vfu_ctx, LOG_DEBUG, "map %p-%p",
                sg->dma_addr + sg->offset,
                sg->dma_addr + sg->offset + sg->length);
        iov[i].iov_base = region->info.vaddr + sg[i].offset;
        iov[i].iov_len = sg[i].length;
        region->refcnt++;
    }

    return 0;
}

static inline void
dma_unmap_sg(dma_controller_t *dma, const dma_sg_t *sg,
	     UNUSED struct iovec *iov, int cnt)
{
    int i;

    for (i = 0; i < cnt; i++) {
        dma_memory_region_t *r;
        /*
         * FIXME this double loop will be removed if we replace the array with
         * tfind(3)
         */
        for (r = dma->regions;
             r < dma->regions + dma->nregions &&
             r->info.iova.iov_base != sg[i].dma_addr;
             r++);
        if (r > dma->regions + dma->nregions) {
            /* bad region */
            continue;
        }
        vfu_log(dma->vfu_ctx, LOG_DEBUG, "unmap %p-%p",
                sg[i].dma_addr + sg[i].offset,
                sg[i].dma_addr + sg[i].offset + sg[i].length);
        r->refcnt--;
    }
    return;
}

int
dma_controller_dirty_page_logging_start(dma_controller_t *dma, size_t pgsize);

int
dma_controller_dirty_page_logging_stop(dma_controller_t *dma);

int
dma_controller_dirty_page_get(dma_controller_t *dma, vfu_dma_addr_t addr,
                              int len, size_t pgsize, size_t size, char **data);

#endif /* LIB_VFIO_USER_DMA_H */

/* ex: set tabstop=4 shiftwidth=4 softtabstop=4 expandtab: */
