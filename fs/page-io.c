/*
 * Copyright (C) 2018 Saptarshi Sen
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <linux/fs.h>
#include <linux/bio.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mpage.h>
#include <linux/kernel.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>

#include "luci.h"
#include "compress.h"

#include "trace.h"
EXPORT_TRACEPOINT_SYMBOL_GPL(luci_scan_pgtree_dirty_pages);
EXPORT_TRACEPOINT_SYMBOL_GPL(luci_write_extents);
EXPORT_TRACEPOINT_SYMBOL_GPL(luci_bio_complete);
EXPORT_TRACEPOINT_SYMBOL_GPL(luci_write_extent_begin);
EXPORT_TRACEPOINT_SYMBOL_GPL(luci_write_extent_end);
EXPORT_TRACEPOINT_SYMBOL_GPL(luci_end_bio_write);

//#define LUCI_BIO_CHECKSUM //only when you are paranoid

#define WBC_FMT  "wbc: (%llu-%llu) dirty :%lu cyclic :%u sync_mode :%u"

#define WBC_ARGS(wbc) wbc->range_start, wbc->range_end, wbc->nr_to_write, wbc->range_cyclic, wbc->sync_mode

/* compression engine stats */
atomic64_t pages_ingested;
atomic64_t pages_notcompressed;
atomic64_t pages_notcompressible;
atomic64_t pages_wellcompressed;

static void
luci_release_backing_pages(struct pagevec *pvec)
{
    int i;

    for (i = 0; i < pagevec_count(pvec); i++) {
        struct page *page = pvec->pages[i];

        BUG_ON(page == NULL);

        if (PageWriteback(page))
                end_page_writeback(page);

        if (PageLocked(page))
                unlock_page(page);

        put_page(page); // grab_cache_page bumps ref count.
    }
}

/*
 * Compressed pages freed here, and must be run in process context.
 * Should be run only after processing completes on compressed pages.
 * TBD: Check for status associated with each bvec page
 */
static void
#ifdef HAVE_NEW_BIO_END
luci_end_compressed_bio_read(struct bio *bio)
#else
luci_end_compressed_bio_read(struct bio *bio, int error)
#endif
{
    int i = 0;
    struct bio_vec *bvec;
    bio_for_each_segment_all(bvec, bio, i) {
        struct page *page = bvec->bv_page;
        BUG_ON(page->mapping != NULL);
        BUG_ON(PageLocked(page));
        put_page(page);
    }
}

/*
 * We do not set any writeback flag, so end_page_writeback(page) not needed.
 * return page back to mempool
 * TBD: Check for status associated with each bvec page
 */
static void
#ifdef HAVE_NEW_BIO_END
luci_end_bio_write_compressed(struct bio *bio)
#else
luci_end_bio_write_compressed(struct bio *bio, int error)
#endif
{
    int i = 0;
    u32 crc = ~0U;
    struct page *page;
    struct bio_vec *bvec;
    struct luci_compressed_bio_data *bdata;
#ifdef LUCI_BIO_CHECKSUM
    size_t totalb, minb;
#endif

    page = bio->bi_io_vec[0].bv_page;
    bdata = (struct luci_compressed_bio_data *) (page->private);
    BUG_ON(bdata == NULL);
    BUG_ON(bdata->ws == NULL);
    BUG_ON(bdata->ext_work == NULL);
    BUG_ON(bdata->ext_work->pvec == NULL);
#ifdef LUCI_BIO_CHECKSUM
    totalb = bdata->total_out;
#endif

    bio_for_each_segment_all(bvec, bio, i) {
        page = bvec->bv_page;
        BUG_ON(page_has_buffers(page));
        BUG_ON(PageLocked(page));
        BUG_ON(PageWriteback(page));
        luci_zlib_compress.remit_workspace(bdata->ws, page);
#ifdef LUCI_BIO_CHECKSUM
        BUG_ON(totalb <= 0);
        minb = min((ssize_t)totalb, (ssize_t)PAGE_SIZE);
        crc = luci_compute_page_cksum(page, 0, minb, crc);
        totalb -= minb;
#endif
    }
    luci_release_backing_pages(bdata->ext_work->pvec);
    kfree(bdata->ext_work->pvec);
    kfree(bdata->ext_work);
    kfree(bdata);
#ifdef HAVE_TRACEPOINT_ENABLED
    if (trace_luci_bio_complete_enabled())
        trace_luci_bio_complete(bio, error, crc);
#else
    trace_luci_bio_complete(bio, error, crc);
#endif
    bio_put(bio);
}

/*
 * For regular writes, we perform end_writeback
 * TBD: In case write fails, check for PageError, we redirty the page
 */
static void
#ifdef HAVE_NEW_BIO_END
luci_end_bio_write(struct bio *bio)
#else
luci_end_bio_write(struct bio *bio, int error)
#endif
{
    int i;
    u32 crc = ~0U;
    struct page *page;
    struct bio_vec *bvec;

    #ifndef  HAVE_NEW_BIO_END
    BUG_ON(error);
    #endif
    bio_for_each_segment_all(bvec, bio, i) {
        page = bvec->bv_page;
        BUG_ON(page_has_buffers(page)); // L0 blocks are no_bh based

        trace_luci_end_bio_write(page);

        if (PageWriteback(page))
            end_page_writeback(page);

        if (!PageLocked(page))
                continue;

        // grab_cache_page locks page and bumps ref count.
        // regular page is already unlocked by write_end.
        unlock_page(page);
        put_page(page);
    }
#ifdef HAVE_TRACEPOINT_ENABLED
    if (trace_luci_bio_complete_enabled())
        trace_luci_bio_complete(bio, error, crc);
#else
    trace_luci_bio_complete(bio, error, crc);
#endif
    bio_put(bio);
}

/*
 * allocates bio for read/write submission
 */
static struct bio *
luci_bio_alloc(struct block_device *bdev, unsigned long start,
               unsigned long nr_pages_out)
{
    struct bio *bio;

    BUG_ON(nr_pages_out > BIO_MAX_PAGES);

    bio = bio_alloc(GFP_NOFS, nr_pages_out);
    if (!bio) {
        return NULL;
    }

    bio->bi_vcnt = 0;

    #ifdef HAVE_BIO_SETDEV_NEW
    bio_set_dev(bio, bdev);
    #else
    bio->bi_bdev = bdev;
    #endif

    #ifdef HAVE_BIO_ITER
    bio->bi_iter.bi_sector = start >> 9;
    #else
    bio->bi_sector = start >> 9;
    #endif
    return bio;
}

/*
 * construct bio vecs for each PAGE of compressed output
 * Note these pages are anon and do not belong to page cache
 *
     1. scsi_lib panics for zero phy segments
     2. align size to device sector, otherwise device rejects write
 */
static struct bio*
luci_construct_bio(struct inode *inode,
                   struct page **pages,
                   unsigned long total,
                   unsigned long disk_start,
                   bool write)
{
    int i, err = 0;
    struct bio *bio;
    unsigned int curr_bytes;
    unsigned long sector_bytes, nr_pages;
    struct block_device *bdev = inode->i_sb->s_bdev;

    sector_bytes = sector_align(total);
    nr_pages = (sector_bytes + PAGE_SIZE - 1)/PAGE_SIZE;

    bio = luci_bio_alloc(bdev, disk_start, nr_pages);
    if (!bio) {
        err = -ENOMEM;
        luci_err_inode(inode, "bio alloc failed\n");
        goto exit;
    }

    for (i = 0; i < nr_pages; i++) {
       BUG_ON(!pages[i]);
       BUG_ON(sector_bytes == 0);

       curr_bytes = min(sector_bytes, (unsigned long)PAGE_SIZE);
       if (bio_add_page(bio, pages[i], curr_bytes, 0) < curr_bytes)  {
           err = -EIO;
           bio_put(bio);
           luci_err_inode(inode, "cannot add page, bio is full\n");
           goto exit;
       }
       sector_bytes -= (unsigned long)curr_bytes;
       luci_info("added page %p to bio, len :%u", pages[i], curr_bytes);
    }

    BUG_ON(sector_bytes);

    #ifdef NEW_BIO_SUBMIT
    bio->bi_opf = write ? REQ_OP_WRITE : REQ_OP_READ;
    #endif
    return bio;

exit:
    return ERR_PTR(err);
}

/*
 * talks to bio layer.
 * bio function to build and submit io for compressed/uncompressed pages.
 */
static int
luci_prepare_and_submit_bio(struct inode *inode,
                            struct page **pages,
                            unsigned long total_out,
                            unsigned long disk_start,
                            bool compressed,
                            struct luci_compressed_bio_data *bdata)
{
    ktime_t start;
    struct bio *bio;
    struct page *page;

    bio = luci_construct_bio(inode, pages, total_out, disk_start, true);
    BUG_ON(IS_ERR(bio));

    page = bio_page(bio);
    if (compressed) {
        BUG_ON(bdata == NULL);
        page->private = (unsigned long) bdata;
    }

    bio->bi_end_io = compressed ?
                     luci_end_bio_write_compressed : luci_end_bio_write;

    luci_bio_dump(bio, "submitting bio write");

    start = ktime_get();
    #ifdef NEW_BIO_SUBMIT
    bio->bi_opf = REQ_OP_WRITE;
    submit_bio(bio);
    #else
    submit_bio(WRITE, bio);
    #endif
    UPDATE_AVG_LATENCY_NS(dbgfsparam.avg_io_lat, start);
    return 0;
}

/*
 * Worker thread function.
 *
 *  1. Applies compression heuristics to cluster pages.
 *  2. if compression possible, compreses the cluser, and creates a compressed bio.
 *  3. Otherwise, creates a bio from regular pages cached in page tree
 *  4. Issues async IO
 *  5. Updates bmap after bio completion
 */

static void
__luci_compress_extent_and_write(struct work_struct *work)
{
    ktime_t start;
    int i, err, delta;
    bool compressed = true, redirty_page = false;
    struct list_head *ws = NULL;
    struct inode *inode;
    unsigned extent;
    struct page **page_array, *pageout;
    struct extent_write_work *ext_work;
    unsigned long start_compr_block, disk_start, nr_blocks;
    unsigned long nr_pages_out, total_in, total_out;
    blkptr bp_array[EXTENT_NRBLOCKS_MAX]; // [-Waggressive-loop-optimizations]
    u32 crc32[EXTENT_NRBLOCKS_MAX], crc32_extent = 0;
    struct luci_compressed_bio_data *bio_data = NULL;

    memset((char *)crc32, 0, sizeof(u32) * EXTENT_NRBLOCKS_MAX);

    ext_work = container_of(work, struct extent_write_work, work);

    /* We are nobh. See *_write_end */
    BUG_ON(page_has_buffers(ext_work->begin_page));
    BUG_ON(pagevec_count(ext_work->pvec) != EXTENT_NRPAGE);

    inode = ext_work->begin_page->mapping->host;
    extent = luci_extent_no(page_index(ext_work->begin_page));
    pageout = ext_work->pageout;

    page_array = kzalloc(EXTENT_NRPAGE * sizeof(struct page *), GFP_NOFS);
    if (!page_array) {
        luci_err_inode(inode, "failed to allocate page extent");
        return;
    }

    atomic64_add(EXTENT_NRPAGE, &pages_ingested);

#ifdef LUCI_COMPRESSION_HEURISTICS
    // apply heuristics
    if (!can_compress(ext_work->begin_page)) {
            atomic64_add(EXTENT_NRPAGE, &pages_notcompressible);
            goto notcompressible;
    }
#endif

    // for direct-blocks avoid compression, this keeps bmap
    // deletion operations simple by not spreading compressed
    // extents across direct/indirect blocks.
    if (extent < LUCI_NDIR_BLOCKS) {
            atomic64_add(EXTENT_NRPAGE, &pages_notcompressible);
            goto notcompressible;
    }

    // start compression
    start = ktime_get();

    total_in = EXTENT_SIZE,
    ws = luci_get_compression_context();
    if (IS_ERR(ws)) {
        luci_err_inode(inode, "failed to alloc workspace");
        goto write_error;
    }

    total_out = EXTENT_SIZE;
    nr_pages_out = EXTENT_NRPAGE;
    err = ctxpool.op->compress_pages(ws,
                                     ext_work->begin_page->mapping,
                                     page_offset(ext_work->begin_page),
                                     page_array,
                                     &nr_pages_out,
                                     &total_in,
                                     &total_out);

    luci_put_compression_context(ws);

    if (!err) {
        unsigned cr;

        compressed = true;
        BUG_ON(nr_pages_out == 0);
        bio_data = kzalloc(sizeof(struct luci_compressed_bio_data), GFP_NOFS);
        if (!bio_data) {
                luci_err_inode(inode, "failed to allocate bio data for cluster");
                goto write_error;
        }
        bio_data->ext_work = ext_work;
        bio_data->ws = ws;
        bio_data->total_out = total_out;
        crc32_extent = luci_compute_pages_cksum(page_array, nr_pages_out, total_out);
        cr = ((EXTENT_SIZE - total_out) * 100)/EXTENT_SIZE;
        if (cr >= COMPRESS_RATIO_LIMIT)
                atomic64_add(EXTENT_NRPAGE, &pages_wellcompressed);

        UPDATE_AVG_LATENCY_NS(dbgfsparam.avg_deflate_lat, start);
        LUCI_COMPRESS_RESULT(extent,
                             page_index(ext_work->begin_page),
                             total_in,
                             total_out);
    } else {
        while (nr_pages_out--) {
             BUG_ON(!page_array[nr_pages_out]);
             luci_zlib_compress.remit_workspace(ws, page_array[nr_pages_out]);
        }

notcompressible:
        compressed = false;
        total_out = EXTENT_SIZE;
        nr_pages_out = EXTENT_NRPAGE;
        for (i = 0; i < nr_pages_out; i++) {
            page_array[i] = ext_work->pvec->pages[i];
            crc32[i] = luci_compute_page_cksum(page_array[i], 0, PAGE_SIZE, ~0U);
        }
        atomic64_add(EXTENT_NRPAGE, &pages_notcompressed);
        luci_info_inode(inode, "cannot compress extent, do regular write");
    }

    nr_blocks = (total_out + LUCI_BLOCK_SIZE(inode->i_sb) - 1) >>
                 LUCI_BLOCK_SIZE_BITS(inode->i_sb);

    //mutex_lock(&(LUCI_I(inode)->truncate_mutex));

    if (luci_new_block(inode, nr_blocks, &start_compr_block) < 0) {
        panic("failed block allocation for extent %u, nr_blocks :%lu",
               extent, nr_blocks);
    }

    //mutex_unlock(&(LUCI_I(inode)->truncate_mutex));

    for (i = 0; i < EXTENT_NRBLOCKS_MAX; i++) {
        if (compressed)
            bp_reset(&bp_array[i],
                     start_compr_block,
                     total_out,
                     LUCI_COMPR_FLAG,
                     crc32_extent);
        else
            bp_reset(&bp_array[i],
                     start_compr_block + i,
                     0,
                     0,
                     crc32[i]);
    }

    // Write block map meta data. We COW on a new write.
    delta = luci_bmap_update_extent_bp(ext_work->begin_page, inode, bp_array);

    // update physical file size
    LUCI_I(inode)->i_size_comp += delta;

    luci_info_inode(inode, "block compressed(%d) extent(%d) size=%llu, "
        "delta=%d", compressed, extent, LUCI_I(inode)->i_size_comp, delta);

    // Write Data Block
    disk_start = start_compr_block * LUCI_BLOCK_SIZE(inode->i_sb);
    if (luci_prepare_and_submit_bio(inode,
                                    page_array,
                                    total_out,
                                    disk_start,
                                    compressed,
                                    compressed ? bio_data : NULL) < 0) {
        redirty_page = true;
        luci_err_inode(inode, "submit write error for extent %u", extent);
        goto write_error;
    } else {
        luci_info_inode(inode, "submit write ok for extent %u(page=%lu)",
                extent, page_index(ext_work->begin_page));
        goto release;
    }

write_error:
    if (compressed) {
        while (nr_pages_out--)
            luci_zlib_compress.remit_workspace(ws, page_array[nr_pages_out]);
    }

release:

    if (page_array)
        kfree(page_array);

    if (!compressed) {
        if (ext_work->pvec)
                kfree(ext_work->pvec);
        kfree(ext_work);
        return;
    }

    // backing pages will be released later after io completion
    if (pageout)
        put_page(pageout);
}

/*
 *  Initialize work item for background compression and write
 */
static struct extent_write_work *
luci_init_work(struct pagevec *pvec, struct page *pageout)
{
    struct extent_write_work *work;

    work = kmalloc(sizeof(struct extent_write_work), GFP_NOFS);
    if (!work) {
        luci_err("cannot allocate work item\n");
        return NULL;
    }

    BUG_ON(pvec->pages[0] == NULL);
    work->pvec = pvec;
    work->pageout = pageout;
    work->begin_page = pvec->pages[0];
    INIT_WORK(&work->work, __luci_compress_extent_and_write);
    return work;
}

/*
 * Core routine which converts page to an extent write
 * This is common code exercised by writepages and writepage.
 * For identifying writepage, we pass the page itself.
 *
 * @pageout param can be NULL if invoked via writepages
 */

struct pagevec *
luci_scan_pgtree_dirty_pages(struct address_space *mapping,
                             struct page *pageout,
                             pgoff_t *index,
                             struct writeback_control *wbc)
{
    unsigned i, nr_pages, nr_dirty, tag, extent;
    pgoff_t end_index, next_index = *index;
    struct page *page = NULL;
    struct pagevec *pvec;
    struct inode *inode = mapping->host;

    pvec = kzalloc(sizeof(struct pagevec), GFP_NOFS);
    if (!pvec) {
        luci_err_inode(inode, "failed to allocate pagevec");
        return ERR_PTR(-ENOMEM);
    }

    if (!IS_ALIGNED(*index, EXTENT_NRPAGE))
        next_index = ALIGN_DOWN(*index, EXTENT_NRPAGE);
    else
        next_index = *index;

    end_index = next_index + EXTENT_NRPAGE - 1;

    if ((wbc->sync_mode == WB_SYNC_ALL || wbc->tagged_writepages)) {
        tag = PAGECACHE_TAG_TOWRITE;
        tag_pages_for_writeback(mapping, next_index, end_index); // tag state prior WRITEBACK
    } else
        tag = PAGECACHE_TAG_DIRTY;

    // scan for tag
#ifdef HAVE_PAGEVEC_INIT_NEW
    pagevec_init(pvec);

    nr_pages = pagevec_lookup_tag(pvec, mapping, &next_index, tag);
#else
    pagevec_init(pvec, 0);

    nr_pages = pagevec_lookup_tag(pvec, mapping, &next_index, tag, EXTENT_NRPAGE);
#endif

    BUG_ON(pagevec_count(pvec) != nr_pages);

    // page tree is clean
    if (!nr_pages) {
        pagevec_release(pvec);
        kfree(pvec);
        luci_info_inode(inode, "page tree is clean, nr_pages = 0");
        return NULL; // next index is not updated
    }

    // search if dirty pages are part of this extent
    // NOTE: Fixed missing writes for pages not from this extent
    extent = luci_extent_no(*index);

    for (i = 0, nr_dirty = 0; i < pagevec_count(pvec); i++) {
        page = pvec->pages[i];

        if (extent != luci_extent_no(page_index(page))) {
            next_index = page_index(page);
            break;
        }

        // dirty page must have most latest/uptodate data
        BUG_ON(!PageUptodate(page));

        // Page is already been under writeback
        if (PageWriteback(page))
            wait_for_stable_page(page);

        // this is not expected!!!
        if (!PageDirty(page))
            SetPageDirty(page);

        nr_dirty++;
    }

    pagevec_release(pvec); // drop all refs from pagevec lookup

    if (!nr_dirty) {
        kfree(pvec);
        *index = next_index;
        luci_info_inode(inode, "dirty page does not belong to this "
            "extent(%u), next index %lu\n", extent, next_index);
        return NULL; // next_index is updated
    }

    if (nr_dirty != EXTENT_NRPAGE)
        pr_warn("pagevec does not have all extent pages :%u!", nr_dirty);

    // extent has dirty pages, lock pages in the extent here
    for (i = 0; i < EXTENT_NRPAGE; i++) {
repeat:
        if ((page = grab_cache_page_nowait(mapping, *index + i)) == NULL) {
            cond_resched();
            goto repeat;
        }

        if (PageDirty(page))
            clear_page_dirty_for_io(page);
        set_page_writeback(page);

        pagevec_add(pvec, page); // does not take a refcount
        luci_pgtrack(page, "locked page for write");

#ifdef HAVE_TRACEPOINT_ENABLED
        if (trace_luci_scan_pgtree_dirty_pages_enabled())
#endif
                trace_luci_scan_pgtree_dirty_pages(inode, next_index, page);

    }

    luci_info_inode(inode, "dirty pages:%u in extent %u(%lu)", nr_dirty,
        extent, *index);

    *index = next_index;
    wbc->nr_to_write -= nr_dirty;
    dbgfsparam.nrwrites += nr_dirty;
    return pvec;
}
EXPORT_SYMBOL_GPL(luci_scan_pgtree_dirty_pages);

/*
 * This is invoked by shrink_page_list. See : shrink_page_list and pageout
 * Initiates a work item for this extent
 */
int
luci_write_extent(struct page *page, struct writeback_control *wbc)
{
    int err = 0;
    struct pagevec *pvec;
    struct extent_write_work *wrk;
    pgoff_t next_index = page_index(page);
    struct inode *inode = page->mapping->host;

    /*
     * Disabling PageDirty.
     *
     * Notes from mm/writeback:
     * pageout->clear_page_dirty_for_io.
     *
     * Clear a page's dirty flag, while caring for dirty memory accounting.
     * Returns true if the page was previously dirty.
     *
     * This is for preparing to put the page under writeout.  We leave the page
     * tagged as dirty in the radix tree so that a concurrent write-for-sync
     * can discover it via a PAGECACHE_TAG_DIRTY walk.  The ->writepage
     * implementation will run either set_page_writeback() or set_page_dirty(),
     * at which stage we bring the page's dirty flag and radix-tree dirty tag
     * back into sync.
     *
     * This incoherency between the page's dirty flag and radix-tree tag is
     * unfortunate, but it only exists while the page is locked.
     */

    //BUG_ON(!PageDirty(page));

    BUG_ON(PagePrivate(page));
    pvec = luci_scan_pgtree_dirty_pages(page->mapping,
                                        page,
                                        &next_index,
                                        wbc);
    if (pvec && !IS_ERR(pvec)) {
        if ((wrk = luci_init_work(pvec, page)) == NULL) {
            kfree(pvec);
            goto exit;
        }
        queue_work(LUCI_SB(inode->i_sb)->comp_write_wq, &wrk->work);
        dbgfsparam.nrbatches++;
    } else {
exit:
        err = -EIO;
        if (PageLocked(page))
             unlock_page(page);
    }

    return err;
}

/*
 * This is invoked in the context of vmscan.
 * Scans inode page tree, identify and initiate work per dirty extent.
 */
int luci_write_extents(struct address_space *mapping,
                       struct writeback_control *wbc)
{
    int err = 0;
    bool cycled, done = false;
    struct pagevec *pvec;
    struct extent_write_work *wrk;
    pgoff_t start_index, end_index, prv_index, next_index;
    struct inode *inode = mapping->host;
    unsigned long nr_dirty = wbc->nr_to_write;

    if (wbc->range_cyclic) {
        start_index = mapping->writeback_index;
        end_index = ULONG_MAX >> PAGE_SHIFT;
        cycled = (start_index == 0) ? true : false;
    } else {
        start_index = wbc->range_start >> PAGE_SHIFT;
        end_index = wbc->range_end >> PAGE_SHIFT;
        cycled = true;
    }

    BUG_ON(inode == NULL);
    luci_dbg_inode(inode, "writing pages start_index :%lu "WBC_FMT,
                           start_index, WBC_ARGS(wbc));

repeat:
    next_index = start_index;
    do  {
        prv_index = next_index;
        pvec = luci_scan_pgtree_dirty_pages(mapping, NULL, &next_index, wbc);
        if (pvec && !IS_ERR(pvec)) {
            wrk = luci_init_work(pvec, NULL);
            if (!wrk) {
                err = -EIO;
                kfree(pvec);
                luci_err_inode(inode, "out-of-memory for work\n");
                goto exit;
            }
            queue_work(LUCI_SB(inode->i_sb)->comp_write_wq, &wrk->work);
            dbgfsparam.nrbatches++;
        } else
            BUG_ON(pvec != NULL);

        if (prv_index == next_index)
            done = true;

        cond_resched();
    } while (!done && wbc->nr_to_write > 0 && next_index < end_index);

    // we hit end but there's pending work, cycle back
    if (!done && !cycled) {
        cycled = 1;
        start_index = 0;
        end_index = mapping->writeback_index - 1;
        goto repeat;
    }

    // we still have stuff dirty, but that's all we can do for now
    if (wbc->nr_to_write > 0 && wbc->range_cyclic)
        mapping->writeback_index = done ? 0 : next_index;

exit:
    luci_info_inode(inode, "exiting writepages, range(%lu-%lu) nr_pending_write :%lu\n",
        start_index, next_index, wbc->nr_to_write);

#ifdef HAVE_TRACEPOINT_ENABLED
    if (trace_luci_write_extents_enabled())
#endif
	trace_luci_write_extents(inode, nr_dirty, wbc->nr_to_write);

    return err;
}
EXPORT_SYMBOL_GPL(luci_write_extents);

/*
 * Give a page where data will be copied. The page will be locked.
 * This is for buffered writes. Currently, we do not handle partial writes.
 * Once compressed read is implemented, we can handle this case correctly.
 */
int
luci_write_extent_begin(struct address_space *mapping,
                        loff_t pos, unsigned len, unsigned flags,
                        struct page **pagep)
{
    int i;
    struct pagevec pvec;
    struct page *page = NULL;
    pgoff_t index_begin, index = pos >> PAGE_CACHE_SHIFT;
    struct inode *inode = mapping->host;

    // vfs limits len to page size
    if (len > PAGE_SIZE) {
        luci_err("write length exceeds page size!");
        return -EINVAL;
    }

    // prepare cluster for compression
    if (!IS_ALIGNED(index, EXTENT_NRPAGE))
        index_begin = ALIGN_DOWN(index, EXTENT_NRPAGE);
    else
        index_begin = index;

    pagevec_init(&pvec, 0);

    // Find or create a page and returned the locked page.
    for (i = 0; i < EXTENT_NRPAGE; i++) {
        page = grab_cache_page_write_begin(mapping, index_begin + i, flags);
        BUG_ON(page == NULL);
        BUG_ON(!PageLocked(page));

        // page-tree page is not yet mapped
        if (!PageUptodate(page)) {
            mapping->a_ops->readpage(NULL, page);
            if (!PageLocked(page))
               lock_page(page);
            BUG_ON(!PageUptodate(page));
            //put_page(page);
        }

        if ((index_begin + i) == index)
            *pagep = page;

        pagevec_add(&pvec, page);
    }

    for (i = 0; i < pagevec_count(&pvec); i++) {
            page = pvec.pages[i];
            if (!PageLocked(page))
                    lock_page(page);
    }

#ifdef HAVE_TRACEPOINT_ENABLED
    if (trace_luci_write_extent_begin_enabled()) {
        u32 crc = luci_compute_page_cksum(*pagep, 0, len, ~0U);
        trace_luci_write_extent_begin(inode, pos, len, flags, crc);
    }
#endif

    luci_pgtrack(page, "grabbed page for inode %lu off %llu-%u",
        inode->i_ino, pos, len);
    return 0;
}

/*
 * Data is copied from user space to page.
 * Set appropriate flags and unlock the page. Tag page tress dirty here
 * Note file inode size is updated here
 *
 * FIXME : even on marking a page descriptor dirty, on writepages,
 * page dirty flag is reset at time (confirmed via log)
 */
int
luci_write_extent_end(struct address_space *mapping,
                      loff_t pos,
                      unsigned len,
                      unsigned flags,
                      struct page *pagep)
{
    int i, n;
    struct pagevec pvec;
    struct page *page, *pages[EXTENT_NRPAGE];
    struct inode *inode = mapping->host;
    pgoff_t index_begin, index = pos >> PAGE_CACHE_SHIFT;

    if (!IS_ALIGNED(index, EXTENT_NRPAGE))
        index_begin = ALIGN_DOWN(index, EXTENT_NRPAGE);
    else
        index_begin = index;

    n = find_get_pages_contig(mapping, index_begin, EXTENT_NRPAGE, pages);
    BUG_ON(n != EXTENT_NRPAGE);

    pagevec_init(&pvec, 0);

    for (i = 0; i < EXTENT_NRPAGE; i++) {
        page = pages[i];

        BUG_ON(!PageLocked(page));
        SetPageUptodate(page);

        if (!PageDirty(page))
           __set_page_dirty_nobuffers(page);

        unlock_page(page);
        put_page(page);
        pagevec_add(&pvec, page);
    }

    for (i = 0; i < pagevec_count(&pvec); i++) {
            page = pvec.pages[i];
            put_page(page);
    }

    luci_pgtrack(pagep, "copied cache page(%lu) for inode %lu off %llu-%u",
                             page_index(pagep),
                             inode->i_ino,
                             pos,
                             len);

    if (pos + len > inode->i_size) {
        i_size_write(inode, pos + len);
        mark_inode_dirty(inode);
        luci_dbg_inode(inode, "updating inode new size %llu", inode->i_size);
    }

#ifdef HAVE_TRACEPOINT_ENABLED
    if (trace_luci_write_extent_end_enabled()) {
        u32 crc = luci_compute_page_cksum(pagep, 0, len, ~0U);
        trace_luci_write_extent_end(inode, pos, len, flags, crc);
    }
#endif

    // Ensure we trigger page writeback once, dirty pages exceeds threshold
    //balance_dirty_pages_ratelimited(mapping);

    return len;
}

/*
 * read a compressed page
 * Fixed :pass disk start to bio prepare, not blockno
 */
int luci_read_extent(struct page *page, blkptr *bp)
{
    int i, ret = 0;
    struct page *page_in, *page_out;
    struct list_head *ws;
    struct bio_vec *bvec;
    struct bio *comp_bio = NULL, *pgtree_bio = NULL;
    struct inode *inode = page->mapping->host;
    unsigned long total_in = COMPR_LEN(bp);
    unsigned aligned_bytes = sector_align(total_in);
    unsigned nr_pages = (aligned_bytes + PAGE_SIZE - 1)/PAGE_SIZE;
    unsigned long extent = luci_extent_no(page_index(page));
    unsigned long pg_index = extent * EXTENT_NRPAGE;
    u64 disk_start = bp->blockno * LUCI_BLOCK_SIZE(inode->i_sb);
    struct page *compressed_pages[EXTENT_NRPAGE], *pgtree_pages[EXTENT_NRPAGE];

    #ifdef DEBUG_COMPRESSION
    luci_info_inode(inode, "read, total_in :%lu aligned bytes :%u disk start "
                    ":%llu", total_in, aligned_bytes, disk_start);
    #endif

    memset((char*)pgtree_pages, 0, EXTENT_NRPAGE * sizeof(struct page *));
    memset((char*)compressed_pages, 0, EXTENT_NRPAGE * sizeof(struct page *));

    // allocate pages for reading compressed blocks
    for (i = 0; i < nr_pages; i++) {
        page_in = alloc_page(GFP_NOFS | __GFP_HIGHMEM | __GFP_ZERO);
        if (!page_in) {
            ret = -ENOMEM;
            luci_err("failed to allocate page for compressed read");
            goto free_readpages;
        }
        compressed_pages[i] = page_in;
    }

    comp_bio = luci_construct_bio(inode,
                                  compressed_pages,
                                  aligned_bytes,
                                  disk_start,
                                  false);
    if (IS_ERR(comp_bio)) {
        ret = -EIO;
        luci_err("failed to allocate comp_bio for read");
        goto free_readpages;
    }

    #ifdef NEW_BIO_SUBMIT
    ret = submit_bio_wait(comp_bio);
    #else
    ret = submit_bio_wait(READ_SYNC, comp_bio);
    #endif
    if (ret) {
    #ifdef HAVE_NEW_BIO_FLAGS
        luci_err("bio error status :0x%x, status :%d", comp_bio->bi_flags, ret);
    #else
        luci_err("bio error status :0x%lx, status :%d", comp_bio->bi_flags, ret);
    #endif
        goto free_readbio;
    }

    bio_for_each_segment_all(bvec, comp_bio, i)
        SetPageUptodate(bvec->bv_page);

    if (luci_validate_data_pages_cksum(compressed_pages, nr_pages, bp) == -EBADE) {
            luci_err("L0 checksum mismatch on read extent, block=%u-%u-%u\n",
                      bp->blockno, bp->flags, bp->length);
            goto free_compbio;
    }

    // gather page tree pages
    for (i = 0; i < EXTENT_NRPAGE; pg_index++, i++) {
        page_out = find_get_page(page->mapping, pg_index);
        if (!page_out) {
            luci_info_inode(inode, "page %lu not in cache, adding", pg_index);
            page_out = find_or_create_page(page->mapping, pg_index, GFP_KERNEL);
        }
        BUG_ON(!page_out);
        pgtree_pages[i] = page_out;
    }

    pgtree_bio = luci_construct_bio(inode, pgtree_pages, EXTENT_SIZE, 0, false);
    if (IS_ERR(pgtree_bio)) {
        ret = -EIO;
        luci_err("failed to allocate bio for inflate");
        goto free_compbio;
    }

    ws = luci_get_compression_context();
    if (IS_ERR(ws)) {
        ret = PTR_ERR(ws);
        luci_err_inode(inode, "failed to alloc workspace");
        goto free_compbio;
    }

    if (ctxpool.op->decompress_pages(ws, total_in, comp_bio, pgtree_bio) != 0)
        panic("decompress failed\n");

    luci_put_compression_context(ws);

free_compbio:
    #ifdef HAVE_NEW_BIO_END
    luci_end_compressed_bio_read(comp_bio);
    #else
    luci_end_compressed_bio_read(comp_bio, ret);
    #endif

    for (i = 0; i < EXTENT_NRPAGE; i++) {
        page_out = pgtree_pages[i];
        if (!page_out)
             break;
        SetPageUptodate(page_out);
        if (PageLocked(page_out)) // TBD: check if page can be at all locked
             unlock_page(page_out);

        put_page(page_out);
    }

    if(comp_bio)
        bio_put(comp_bio);

    if (pgtree_bio)
        bio_put(pgtree_bio);

    return ret;

free_readbio:
    if(comp_bio)
        bio_put(comp_bio);

free_readpages:
    for (i = 0; i < nr_pages; i++) {
        page_in = compressed_pages[i];
        put_page(page_in);
    }

    return ret;
}

static int luci_show_compression_stats(struct seq_file *m, void *data)
{
        unsigned long ingested, notcompressed, notcompressible, wellcompressed;

        ingested        = atomic64_read(&pages_ingested);
        notcompressed   = atomic64_read(&pages_notcompressed);
        notcompressible = atomic64_read(&pages_notcompressible);
        wellcompressed  = atomic64_read(&pages_wellcompressed);

        #ifdef  LUCI_COMPRESSION_HEURISTICS
        seq_printf(m, "pages ingested :%lu\npages notcompressed :%lu\n"
                      "pages notcompressible(heuristics) :%lu\npages wellcompressed(>%d%%) :%lu\n"
                      "pages notwellcompressed :%lu\n",
                      ingested, notcompressed, notcompressible, COMPRESS_RATIO_LIMIT,
                      wellcompressed, ingested - notcompressed - wellcompressed);
        #else
        seq_printf(m, "pages ingested :%lu\npages notcompressed :%lu\n"
                      "pages wellcompressed(>%d%%) :%lu\n",
                      ingested, notcompressed, COMPRESS_RATIO_LIMIT, wellcompressed);
        #endif
        return 0;
}

static int luci_debugfs_open(struct inode *inode, struct file *file)
{
        return single_open(file, luci_show_compression_stats, inode->i_private);
}

const struct file_operations luci_compression_stats_ops = {
        .open           = luci_debugfs_open,
        .read           = seq_read,
        .llseek         = no_llseek,
        .release        = single_release,
};
