#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/bio.h>
#include <linux/pagemap.h>

#include "luci.h"

void luci_pageflags_dump(struct page* page, const char *msg)
{
    luci_info("%s : page=%lu Writeback :%d Dirty :%d Uptodate %d",
              msg,
              page->index,
              PageWriteback(page),
              PageDirty(page),
              PageUptodate(page));
}

void luci_dump_bytes(const char *msg,
                     struct page *page,
                     unsigned int len)
{
    void *kaddr;
    bool map_page;

    if (!dbgfsparam.tracedata)
        return;

    map_page = ((page_file_mapping(page)) || page_mapped(page)) ? false : true;

    if (map_page)
        kmap(page);

    kaddr = page_address(page);

    print_hex_dump(KERN_INFO, msg,
                              DUMP_PREFIX_OFFSET,
                              16,
                              1,
                              kaddr,
                              len,
                              true);

    if (map_page)
        kunmap(page); // note :kunmap_atomic takes kaddr
}

void luci_bio_dump(struct bio * bio, const char *msg)
{
#ifdef HAVE_BIO_ITER
     luci_dbg("%s bio : bi_max_vecs :%u bi_vcnt :%d bi_size :%u bi_sector :%lu"
        " bytes: %u index :%lu\n", msg,
                                   bio->bi_max_vecs,
                                   bio->bi_vcnt,
                                   bio->bi_iter.bi_size,
                                   bio->bi_iter.bi_sector,
                                   bio_cur_bytes(bio),
                                   page_index(bio_page(bio)));
#else
     luci_dbg("%s bio : bi_max_vecs :%u bi_vcnt :%d bi_size :%u bi_sector :%lu"
        " bytes :%u index :%lu\n", msg,
                                   bio->bi_max_vecs,
                                   bio->bi_vcnt,
                                   bio->bi_size,
                                   bio->bi_sector,
                                   bio_cur_bytes(bio),
                                   page_index(bio_page(bio)));
#endif
     //luci_dump_bytes("bio page", bio_page(bio), PAGE_SIZE);
}

void copy_pages(struct page *dst_page,
                struct page *src_page,
                unsigned long dst_off,
                unsigned long src_off,
                unsigned long len)
{
    int must_memmove = 0;
    char *src_kaddr;
    char *dst_kaddr = page_address(dst_page);

    if (dst_page != src_page)
        src_kaddr = page_address(src_page);
    else {
        src_kaddr = dst_kaddr;
        if (areas_overlap(src_off, dst_off, len))
            must_memmove = 1;
    }

    if (must_memmove)
        memmove(dst_kaddr + dst_off, src_kaddr + src_off, len);
    else
        memcpy(dst_kaddr + dst_off, src_kaddr + src_off, len);
}

bool areas_overlap(unsigned long src,
                   unsigned long dst,
                   unsigned long len)
{
    unsigned long distance = (src > dst) ? src - dst : dst - src;
    return distance < len;
}