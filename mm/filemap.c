/*
 *	linux/mm/filemap.c
 *
 * Copyright (C) 1994-1999  Linus Torvalds
 */

/*
 * This file handles the generic file mmap semantics used by
 * most "normal" filesystems (but you don't /have/ to use this:
 * the NFS filesystem used to do this differently, for example)
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/compiler.h>
#include <linux/fs.h>
#include <linux/aio.h>
#include <linux/kernel_stat.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/hash.h>
#include <linux/writeback.h>
#include <linux/pagevec.h>
#include <linux/blkdev.h>
#include <linux/security.h>
/*
 * This is needed for the following functions:
 *  - try_to_release_page
 *  - block_invalidatepage
 *  - page_has_buffers
 *  - generic_osync_inode
 *
 * FIXME: remove all knowledge of the buffer layer from this file
 */
#include <linux/buffer_head.h>

#include <asm/uaccess.h>
#include <asm/mman.h>

/*
 * Shared mappings implemented 30.11.1994. It's not fully working yet,
 * though.
 *
 * Shared mappings now work. 15.8.1995  Bruno.
 *
 * finished 'unifying' the page and buffer cache and SMP-threaded the
 * page-cache, 21.05.1999, Ingo Molnar <mingo@redhat.com>
 *
 * SMP-threaded pagemap-LRU 1999, Andrea Arcangeli <andrea@suse.de>
 */

/*
 * Lock ordering:
 *
 *  ->i_shared_lock		(vmtruncate)
 *    ->private_lock		(__free_pte->__set_page_dirty_buffers)
 *      ->swap_list_lock
 *        ->swap_device_lock	(exclusive_swap_page, others)
 *          ->mapping->page_lock
 *  ->inode_lock
 *    ->sb_lock			(fs/fs-writeback.c)
 *  ->page_table_lock
 *    ->swap_device_lock	(try_to_unmap_one)
 *    ->private_lock		(try_to_unmap_one)
 *    ->page_lock		(try_to_unmap_one)
 */

/*
 * Remove a page from the page cache and free it. Caller has to make
 * sure the page is locked and that nobody else uses it - or that usage
 * is safe.  The caller must hold a write_lock on the mapping's page_lock.
 */
void __remove_from_page_cache(struct page *page)
{
	struct address_space *mapping = page->mapping;

	BUG_ON(PageDirty(page) && !PageSwapCache(page));

	radix_tree_delete(&mapping->page_tree, page->index);
	list_del(&page->list);
	page->mapping = NULL;

	mapping->nrpages--;
	dec_page_state(nr_pagecache);
}

void remove_from_page_cache(struct page *page)
{
	struct address_space *mapping = page->mapping;

	if (unlikely(!PageLocked(page)))
		PAGE_BUG(page);

	write_lock(&mapping->page_lock);
	__remove_from_page_cache(page);
	write_unlock(&mapping->page_lock);
}

static inline int sync_page(struct page *page)
{
	struct address_space *mapping = page->mapping;

	if (mapping && mapping->a_ops && mapping->a_ops->sync_page)
		return mapping->a_ops->sync_page(page);
	return 0;
}

/*
 * In-memory filesystems have to fail their
 * writepage function - and this has to be
 * worked around in the VM layer..
 *
 * We
 *  - mark the page dirty again (but do NOT
 *    add it back to the inode dirty list, as
 *    that would livelock in fdatasync)
 *  - activate the page so that the page stealer
 *    doesn't try to write it out over and over
 *    again.
 *
 * NOTE!  The livelock in fdatasync went away, due to io_pages.
 * So this function can now call set_page_dirty().
 */
int fail_writepage(struct page *page)
{
	/* Only activate on memory-pressure, not fsync.. */
	if (current->flags & PF_MEMALLOC) {
		if (!PageActive(page))
			activate_page(page);
		if (!PageReferenced(page))
			SetPageReferenced(page);
	}

	unlock_page(page);
	return -EAGAIN;		/* It will be set dirty again */
}
EXPORT_SYMBOL(fail_writepage);

/**
 * filemap_fdatawrite - start writeback against all of a mapping's dirty pages
 * @mapping: address space structure to write
 *
 * This is a "data integrity" operation, as opposed to a regular memory
 * cleansing writeback.  The difference between these two operations is that
 * if a dirty page/buffer is encountered, it must be waited upon, and not just
 * skipped over.
 *
 * The PF_SYNC flag is set across this operation and the various functions
 * which care about this distinction must use called_for_sync() to find out
 * which behaviour they should implement.
 */
int filemap_fdatawrite(struct address_space *mapping)
{
	int ret;
	struct writeback_control wbc = {
		.sync_mode = WB_SYNC_ALL,
		.nr_to_write = mapping->nrpages * 2,
	};

	current->flags |= PF_SYNC;
	ret = do_writepages(mapping, &wbc);
	current->flags &= ~PF_SYNC;
	return ret;
}

/**
 * filemap_fdatawait - walk the list of locked pages of the given address
 *                     space and wait for all of them.
 * @mapping: address space structure to wait for
 */
int filemap_fdatawait(struct address_space * mapping)
{
	int ret = 0;

	write_lock(&mapping->page_lock);

        while (!list_empty(&mapping->locked_pages)) {
		struct page *page;

		page = list_entry(mapping->locked_pages.next,struct page,list);
		list_del(&page->list);
		if (PageDirty(page))
			list_add(&page->list, &mapping->dirty_pages);
		else
			list_add(&page->list, &mapping->clean_pages);

		if (!PageWriteback(page))
			continue;

		page_cache_get(page);
		write_unlock(&mapping->page_lock);

		wait_on_page_writeback(page);
		if (PageError(page))
			ret = -EIO;

		page_cache_release(page);
		write_lock(&mapping->page_lock);
	}
	write_unlock(&mapping->page_lock);
	return ret;
}

/*
 * This adds a page to the page cache, starting out as locked, unreferenced,
 * not uptodate and with no errors.
 *
 * This function is used for two things: adding newly allocated pagecache
 * pages and for moving existing anon pages into swapcache.
 *
 * In the case of pagecache pages, the page is new, so we can just run
 * SetPageLocked() against it.  The other page state flags were set by
 * rmqueue()
 *
 * In the case of swapcache, try_to_swap_out() has already locked the page, so
 * SetPageLocked() is ugly-but-OK there too.  The required page state has been
 * set up by swap_out_add_to_swap_cache().
 *
 * This function does not add the page to the LRU.  The caller must do that.
 */
int add_to_page_cache(struct page *page,
		struct address_space *mapping, pgoff_t offset)
{
	int error;

	page_cache_get(page);
	write_lock(&mapping->page_lock);
	error = radix_tree_insert(&mapping->page_tree, offset, page);
	if (!error) {
		SetPageLocked(page);
		___add_to_page_cache(page, mapping, offset);
	} else {
		page_cache_release(page);
	}
	write_unlock(&mapping->page_lock);
	return error;
}

int add_to_page_cache_lru(struct page *page,
		struct address_space *mapping, pgoff_t offset)
{
	int ret = add_to_page_cache(page, mapping, offset);
	if (ret == 0)
		lru_cache_add(page);
	return ret;
}

/*
 * This adds the requested page to the page cache if it isn't already there,
 * and schedules an I/O to read in its contents from disk.
 */
static int FASTCALL(page_cache_read(struct file * file, unsigned long offset));
static int page_cache_read(struct file * file, unsigned long offset)
{
	struct address_space *mapping = file->f_dentry->d_inode->i_mapping;
	struct page *page; 
	int error;

	page = page_cache_alloc_cold(mapping);
	if (!page)
		return -ENOMEM;

	error = add_to_page_cache_lru(page, mapping, offset);
	if (!error) {
		error = mapping->a_ops->readpage(file, page);
		page_cache_release(page);
		return error;
	}

	/*
	 * We arrive here in the unlikely event that someone 
	 * raced with us and added our page to the cache first
	 * or we are out of memory for radix-tree nodes.
	 */
	page_cache_release(page);
	return error == -EEXIST ? 0 : error;
}

/*
 * In order to wait for pages to become available there must be
 * waitqueues associated with pages. By using a hash table of
 * waitqueues where the bucket discipline is to maintain all
 * waiters on the same queue and wake all when any of the pages
 * become available, and for the woken contexts to check to be
 * sure the appropriate page became available, this saves space
 * at a cost of "thundering herd" phenomena during rare hash
 * collisions.
 */
static inline wait_queue_head_t *page_waitqueue(struct page *page)
{
	const struct zone *zone = page_zone(page);

	return &zone->wait_table[hash_ptr(page, zone->wait_table_bits)];
}

void wait_on_page_bit(struct page *page, int bit_nr)
{
	wait_queue_head_t *waitqueue = page_waitqueue(page);
	DEFINE_WAIT(wait);

	do {
		prepare_to_wait(waitqueue, &wait, TASK_UNINTERRUPTIBLE);
		sync_page(page);
		if (test_bit(bit_nr, &page->flags))
			io_schedule();
	} while (test_bit(bit_nr, &page->flags));
	finish_wait(waitqueue, &wait);
}
EXPORT_SYMBOL(wait_on_page_bit);

/**
 * unlock_page() - unlock a locked page
 *
 * @page: the page
 *
 * Unlocks the page and wakes up sleepers in ___wait_on_page_locked().
 * Also wakes sleepers in wait_on_page_writeback() because the wakeup
 * mechananism between PageLocked pages and PageWriteback pages is shared.
 * But that's OK - sleepers in wait_on_page_writeback() just go back to sleep.
 *
 * The first mb is necessary to safely close the critical section opened by the
 * TestSetPageLocked(), the second mb is necessary to enforce ordering between
 * the clear_bit and the read of the waitqueue (to avoid SMP races with a
 * parallel wait_on_page_locked()).
 */
void unlock_page(struct page *page)
{
	wait_queue_head_t *waitqueue = page_waitqueue(page);
	smp_mb__before_clear_bit();
	if (!TestClearPageLocked(page))
		BUG();
	smp_mb__after_clear_bit(); 
	if (waitqueue_active(waitqueue))
		wake_up_all(waitqueue);
}

/*
 * End writeback against a page.
 */
void end_page_writeback(struct page *page)
{
	wait_queue_head_t *waitqueue = page_waitqueue(page);
	smp_mb__before_clear_bit();
	if (!TestClearPageWriteback(page))
		BUG();
	smp_mb__after_clear_bit(); 
	if (waitqueue_active(waitqueue))
		wake_up_all(waitqueue);
}
EXPORT_SYMBOL(end_page_writeback);

/*
 * Get a lock on the page, assuming we need to sleep to get it.
 *
 * Ugly: running sync_page() in state TASK_UNINTERRUPTIBLE is scary.  If some
 * random driver's requestfn sets TASK_RUNNING, we could busywait.  However
 * chances are that on the second loop, the block layer's plug list is empty,
 * so sync_page() will then return in state TASK_UNINTERRUPTIBLE.
 */
void __lock_page(struct page *page)
{
	wait_queue_head_t *wqh = page_waitqueue(page);
	DEFINE_WAIT(wait);

	while (TestSetPageLocked(page)) {
		prepare_to_wait(wqh, &wait, TASK_UNINTERRUPTIBLE);
		sync_page(page);
		if (PageLocked(page))
			io_schedule();
	}
	finish_wait(wqh, &wait);
}
EXPORT_SYMBOL(__lock_page);

/*
 * a rather lightweight function, finding and getting a reference to a
 * hashed page atomically.
 */
struct page * find_get_page(struct address_space *mapping, unsigned long offset)
{
	struct page *page;

	/*
	 * We scan the hash list read-only. Addition to and removal from
	 * the hash-list needs a held write-lock.
	 */
	read_lock(&mapping->page_lock);
	page = radix_tree_lookup(&mapping->page_tree, offset);
	if (page)
		page_cache_get(page);
	read_unlock(&mapping->page_lock);
	return page;
}

/*
 * Same as above, but trylock it instead of incrementing the count.
 */
struct page *find_trylock_page(struct address_space *mapping, unsigned long offset)
{
	struct page *page;

	read_lock(&mapping->page_lock);
	page = radix_tree_lookup(&mapping->page_tree, offset);
	if (page && TestSetPageLocked(page))
		page = NULL;
	read_unlock(&mapping->page_lock);
	return page;
}

/**
 * find_lock_page - locate, pin and lock a pagecache page
 *
 * @mapping - the address_space to search
 * @offset - the page index
 *
 * Locates the desired pagecache page, locks it, increments its reference
 * count and returns its address.
 *
 * Returns zero if the page was not present. find_lock_page() may sleep.
 */
struct page *find_lock_page(struct address_space *mapping,
				unsigned long offset)
{
	struct page *page;

	read_lock(&mapping->page_lock);
repeat:
	page = radix_tree_lookup(&mapping->page_tree, offset);
	if (page) {
		page_cache_get(page);
		if (TestSetPageLocked(page)) {
			read_unlock(&mapping->page_lock);
			lock_page(page);
			read_lock(&mapping->page_lock);

			/* Has the page been truncated while we slept? */
			if (page->mapping != mapping || page->index != offset) {
				unlock_page(page);
				page_cache_release(page);
				goto repeat;
			}
		}
	}
	read_unlock(&mapping->page_lock);
	return page;
}

/**
 * find_or_create_page - locate or add a pagecache page
 *
 * @mapping - the page's address_space
 * @index - the page's index into the mapping
 * @gfp_mask - page allocation mode
 *
 * Locates a page in the pagecache.  If the page is not present, a new page
 * is allocated using @gfp_mask and is added to the pagecache and to the VM's
 * LRU list.  The returned page is locked and has its reference count
 * incremented.
 *
 * find_or_create_page() may sleep, even if @gfp_flags specifies an atomic
 * allocation!
 *
 * find_or_create_page() returns the desired page's address, or zero on
 * memory exhaustion.
 */
struct page *find_or_create_page(struct address_space *mapping,
		unsigned long index, unsigned int gfp_mask)
{
	struct page *page, *cached_page = NULL;
	int err;
repeat:
	page = find_lock_page(mapping, index);
	if (!page) {
		if (!cached_page) {
			cached_page = alloc_page(gfp_mask);
			if (!cached_page)
				return NULL;
		}
		err = add_to_page_cache_lru(cached_page, mapping, index);
		if (!err) {
			page = cached_page;
			cached_page = NULL;
		} else if (err == -EEXIST)
			goto repeat;
	}
	if (cached_page)
		page_cache_release(cached_page);
	return page;
}

/**
 * find_get_pages - gang pagecache lookup
 * @mapping:	The address_space to search
 * @start:	The starting page index
 * @nr_pages:	The maximum number of pages
 * @pages:	Where the resulting pages are placed
 *
 * find_get_pages() will search for and return a group of up to
 * @nr_pages pages in the mapping.  The pages are placed at @pages.
 * find_get_pages() takes a reference against the returned pages.
 *
 * The search returns a group of mapping-contiguous pages with ascending
 * indexes.  There may be holes in the indices due to not-present pages.
 *
 * find_get_pages() returns the number of pages which were found.
 */
unsigned int find_get_pages(struct address_space *mapping, pgoff_t start,
			    unsigned int nr_pages, struct page **pages)
{
	unsigned int i;
	unsigned int ret;

	read_lock(&mapping->page_lock);
	ret = radix_tree_gang_lookup(&mapping->page_tree,
				(void **)pages, start, nr_pages);
	for (i = 0; i < ret; i++)
		page_cache_get(pages[i]);
	read_unlock(&mapping->page_lock);
	return ret;
}

/*
 * Same as grab_cache_page, but do not wait if the page is unavailable.
 * This is intended for speculative data generators, where the data can
 * be regenerated if the page couldn't be grabbed.  This routine should
 * be safe to call while holding the lock for another page.
 *
 * Clear __GFP_FS when allocating the page to avoid recursion into the fs
 * and deadlock against the caller's locked page.
 */
struct page *
grab_cache_page_nowait(struct address_space *mapping, unsigned long index)
{
	struct page *page = find_get_page(mapping, index);

	if (page) {
		if (!TestSetPageLocked(page))
			return page;
		page_cache_release(page);
		return NULL;
	}
	page = alloc_pages(mapping->gfp_mask & ~__GFP_FS, 0);
	if (page && add_to_page_cache_lru(page, mapping, index)) {
		page_cache_release(page);
		page = NULL;
	}
	return page;
}

/*
 * Mark a page as having seen activity.
 *
 * inactive,unreferenced	->	inactive,referenced
 * inactive,referenced		->	active,unreferenced
 * active,unreferenced		->	active,referenced
 */
void mark_page_accessed(struct page *page)
{
	if (!PageActive(page) && PageReferenced(page)) {
		activate_page(page);
		ClearPageReferenced(page);
		return;
	} else if (!PageReferenced(page)) {
		SetPageReferenced(page);
	}
}

/*
 * This is a generic file read routine, and uses the
 * inode->i_op->readpage() function for the actual low-level
 * stuff.
 *
 * This is really ugly. But the goto's actually try to clarify some
 * of the logic when it comes to error handling etc.
 * - note the struct file * is only passed for the use of readpage
 */
void do_generic_mapping_read(struct address_space *mapping,
			     struct file_ra_state *ra,
			     struct file * filp,
			     loff_t *ppos,
			     read_descriptor_t * desc,
			     read_actor_t actor)
{
	struct inode *inode = mapping->host;
	unsigned long index, offset;
	struct page *cached_page;
	int error;

	cached_page = NULL;
	index = *ppos >> PAGE_CACHE_SHIFT;
	offset = *ppos & ~PAGE_CACHE_MASK;

	for (;;) {
		struct page *page;
		unsigned long end_index, nr, ret;

		end_index = inode->i_size >> PAGE_CACHE_SHIFT;
			
		if (index > end_index)
			break;
		nr = PAGE_CACHE_SIZE;
		if (index == end_index) {
			nr = inode->i_size & ~PAGE_CACHE_MASK;
			if (nr <= offset)
				break;
		}

		page_cache_readahead(mapping, ra, filp, index);

		nr = nr - offset;

		/*
		 * Try to find the data in the page cache..
		 */
find_page:
		read_lock(&mapping->page_lock);
		page = radix_tree_lookup(&mapping->page_tree, index);
		if (!page) {
			read_unlock(&mapping->page_lock);
			handle_ra_miss(mapping,ra);
			goto no_cached_page;
		}
		page_cache_get(page);
		read_unlock(&mapping->page_lock);

		if (!PageUptodate(page))
			goto page_not_up_to_date;
page_ok:
		/* If users can be writing to this page using arbitrary
		 * virtual addresses, take care about potential aliasing
		 * before reading the page on the kernel side.
		 */
		if (!list_empty(&mapping->i_mmap_shared))
			flush_dcache_page(page);

		/*
		 * Mark the page accessed if we read the beginning.
		 */
		if (!offset)
			mark_page_accessed(page);

		/*
		 * Ok, we have the page, and it's up-to-date, so
		 * now we can copy it to user space...
		 *
		 * The actor routine returns how many bytes were actually used..
		 * NOTE! This may not be the same as how much of a user buffer
		 * we filled up (we may be padding etc), so we can only update
		 * "pos" here (the actor routine has to update the user buffer
		 * pointers and the remaining count).
		 */
		ret = actor(desc, page, offset, nr);
		offset += ret;
		index += offset >> PAGE_CACHE_SHIFT;
		offset &= ~PAGE_CACHE_MASK;

		page_cache_release(page);
		if (ret == nr && desc->count)
			continue;
		break;

page_not_up_to_date:
		if (PageUptodate(page))
			goto page_ok;

		/* Get exclusive access to the page ... */
		lock_page(page);

		/* Did it get unhashed before we got the lock? */
		if (!page->mapping) {
			unlock_page(page);
			page_cache_release(page);
			continue;
		}

		/* Did somebody else fill it already? */
		if (PageUptodate(page)) {
			unlock_page(page);
			goto page_ok;
		}

readpage:
		/* ... and start the actual read. The read will unlock the page. */
		error = mapping->a_ops->readpage(filp, page);

		if (!error) {
			if (PageUptodate(page))
				goto page_ok;
			wait_on_page_locked(page);
			if (PageUptodate(page))
				goto page_ok;
			error = -EIO;
		}

		/* UHHUH! A synchronous read error occurred. Report it */
		desc->error = error;
		page_cache_release(page);
		break;

no_cached_page:
		/*
		 * Ok, it wasn't cached, so we need to create a new
		 * page..
		 */
		if (!cached_page) {
			cached_page = page_cache_alloc_cold(mapping);
			if (!cached_page) {
				desc->error = -ENOMEM;
				break;
			}
		}
		error = add_to_page_cache_lru(cached_page, mapping, index);
		if (error) {
			if (error == -EEXIST)
				goto find_page;
			desc->error = error;
			break;
		}
		page = cached_page;
		cached_page = NULL;
		goto readpage;
	}

	*ppos = ((loff_t) index << PAGE_CACHE_SHIFT) + offset;
	if (cached_page)
		page_cache_release(cached_page);
	UPDATE_ATIME(inode);
}

/*
 * Fault a userspace page into pagetables.  Return non-zero on a fault.
 *
 * FIXME: this assumes that two userspace pages are always sufficient.  That's
 * not true if PAGE_CACHE_SIZE > PAGE_SIZE.
 */
static inline int fault_in_pages_writeable(char *uaddr, int size)
{
	int ret;

	/*
	 * Writing zeroes into userspace here is OK, because we know that if
	 * the zero gets there, we'll be overwriting it.
	 */
	ret = __put_user(0, uaddr);
	if (ret == 0) {
		char *end = uaddr + size - 1;

		/*
		 * If the page was already mapped, this will get a cache miss
		 * for sure, so try to avoid doing it.
		 */
		if (((unsigned long)uaddr & PAGE_MASK) !=
				((unsigned long)end & PAGE_MASK))
		 	ret = __put_user(0, end);
	}
	return ret;
}

static inline void fault_in_pages_readable(const char *uaddr, int size)
{
	volatile char c;
	int ret;

	ret = __get_user(c, (char *)uaddr);
	if (ret == 0) {
		const char *end = uaddr + size - 1;

		if (((unsigned long)uaddr & PAGE_MASK) !=
				((unsigned long)end & PAGE_MASK))
		 	__get_user(c, (char *)end);
	}
}

int file_read_actor(read_descriptor_t *desc, struct page *page,
			unsigned long offset, unsigned long size)
{
	char *kaddr;
	unsigned long left, count = desc->count;

	if (size > count)
		size = count;

	/*
	 * Faults on the destination of a read are common, so do it before
	 * taking the kmap.
	 */
	if (!fault_in_pages_writeable(desc->buf, size)) {
		kaddr = kmap_atomic(page, KM_USER0);
		left = __copy_to_user(desc->buf, kaddr + offset, size);
		kunmap_atomic(kaddr, KM_USER0);
		if (left == 0)
			goto success;
	}

	/* Do it the slow way */
	kaddr = kmap(page);
	left = __copy_to_user(desc->buf, kaddr + offset, size);
	kunmap(page);

	if (left) {
		size -= left;
		desc->error = -EFAULT;
	}
success:
	desc->count = count - size;
	desc->written += size;
	desc->buf += size;
	return size;
}

/*
 * This is the "read()" routine for all filesystems
 * that can use the page cache directly.
 */
static ssize_t
__generic_file_aio_read(struct kiocb *iocb, const struct iovec *iov,
		unsigned long nr_segs, loff_t *ppos)
{
	struct file *filp = iocb->ki_filp;
	ssize_t retval;
	unsigned long seg;
	size_t count;

	count = 0;
	for (seg = 0; seg < nr_segs; seg++) {
		const struct iovec *iv = &iov[seg];

		/*
		 * If any segment has a negative length, or the cumulative
		 * length ever wraps negative then return -EINVAL.
		 */
		count += iv->iov_len;
		if (unlikely((ssize_t)(count|iv->iov_len) < 0))
			return -EINVAL;
		if (access_ok(VERIFY_WRITE, iv->iov_base, iv->iov_len))
			continue;
		if (seg == 0)
			return -EFAULT;
		nr_segs = seg;
		break;
	}

	/* coalesce the iovecs and go direct-to-BIO for O_DIRECT */
	if (filp->f_flags & O_DIRECT) {
		loff_t pos = *ppos, size;
		struct address_space *mapping;
		struct inode *inode;

		mapping = filp->f_dentry->d_inode->i_mapping;
		inode = mapping->host;
		retval = 0;
		if (!count)
			goto out; /* skip atime */
		size = inode->i_size;
		if (pos < size) {
			if (pos + count > size) {
				count = size - pos;
				nr_segs = iov_shorten((struct iovec *)iov,
							nr_segs, count);
			}
			retval = generic_file_direct_IO(READ, filp,
					iov, pos, nr_segs);
			if (retval > 0)
				*ppos = pos + retval;
		}
		UPDATE_ATIME(filp->f_dentry->d_inode);
		goto out;
	}

	retval = 0;
	if (count) {
		for (seg = 0; seg < nr_segs; seg++) {
			read_descriptor_t desc;

			desc.written = 0;
			desc.buf = iov[seg].iov_base;
			desc.count = iov[seg].iov_len;
			if (desc.count == 0)
				continue;
			desc.error = 0;
			do_generic_file_read(filp,ppos,&desc,file_read_actor);
			retval += desc.written;
			if (!retval) {
				retval = desc.error;
				break;
			}
		}
	}
out:
	return retval;
}

ssize_t
generic_file_aio_read(struct kiocb *iocb, char *buf, size_t count, loff_t pos)
{
	struct iovec local_iov = { .iov_base = buf, .iov_len = count };

	BUG_ON(iocb->ki_pos != pos);
	return __generic_file_aio_read(iocb, &local_iov, 1, &iocb->ki_pos);
}
EXPORT_SYMBOL(generic_file_aio_read);

ssize_t
generic_file_read(struct file *filp, char *buf, size_t count, loff_t *ppos)
{
	struct iovec local_iov = { .iov_base = buf, .iov_len = count };
	struct kiocb kiocb;
	ssize_t ret;

	init_sync_kiocb(&kiocb, filp);
	ret = __generic_file_aio_read(&kiocb, &local_iov, 1, ppos);
	if (-EIOCBQUEUED == ret)
		ret = wait_on_sync_kiocb(&kiocb);
	return ret;
}

int file_send_actor(read_descriptor_t * desc, struct page *page, unsigned long offset, unsigned long size)
{
	ssize_t written;
	unsigned long count = desc->count;
	struct file *file = (struct file *) desc->buf;

	if (size > count)
		size = count;

	written = file->f_op->sendpage(file, page, offset,
				       size, &file->f_pos, size<count);
	if (written < 0) {
		desc->error = written;
		written = 0;
	}
	desc->count = count - written;
	desc->written += written;
	return written;
}

ssize_t generic_file_sendfile(struct file *out_file, struct file *in_file,
			      loff_t *ppos, size_t count)
{
	read_descriptor_t desc;

	if (!count)
		return 0;

	desc.written = 0;
	desc.count = count;
	desc.buf = (char *)out_file;
	desc.error = 0;

	do_generic_file_read(in_file, ppos, &desc, file_send_actor);
	if (desc.written)
		return desc.written;
	return desc.error;
}

static ssize_t
do_readahead(struct address_space *mapping, struct file *filp,
	     unsigned long index, unsigned long nr)
{
	unsigned long max;
	unsigned long active;
	unsigned long inactive;

	if (!mapping || !mapping->a_ops || !mapping->a_ops->readpage)
		return -EINVAL;

	/* Limit it to a sane percentage of the inactive list.. */
	get_zone_counts(&active, &inactive);
	max = inactive / 2;
	if (nr > max)
		nr = max;

	do_page_cache_readahead(mapping, filp, index, nr);
	return 0;
}

asmlinkage ssize_t sys_readahead(int fd, loff_t offset, size_t count)
{
	ssize_t ret;
	struct file *file;

	ret = -EBADF;
	file = fget(fd);
	if (file) {
		if (file->f_mode & FMODE_READ) {
			struct address_space *mapping = file->f_dentry->d_inode->i_mapping;
			unsigned long start = offset >> PAGE_CACHE_SHIFT;
			unsigned long end = (offset + count - 1) >> PAGE_CACHE_SHIFT;
			unsigned long len = end - start + 1;
			ret = do_readahead(mapping, file, start, len);
		}
		fput(file);
	}
	return ret;
}

/*
 * filemap_nopage() is invoked via the vma operations vector for a
 * mapped memory region to read in file data during a page fault.
 *
 * The goto's are kind of ugly, but this streamlines the normal case of having
 * it in the page cache, and handles the special cases reasonably without
 * having a lot of duplicated code.
 */

struct page * filemap_nopage(struct vm_area_struct * area, unsigned long address, int unused)
{
	int error;
	struct file *file = area->vm_file;
	struct address_space *mapping = file->f_dentry->d_inode->i_mapping;
	struct file_ra_state *ra = &file->f_ra;
	struct inode *inode = mapping->host;
	struct page *page;
	unsigned long size, pgoff, endoff;
	int did_readahead;

	pgoff = ((address - area->vm_start) >> PAGE_CACHE_SHIFT) + area->vm_pgoff;
	endoff = ((area->vm_end - area->vm_start) >> PAGE_CACHE_SHIFT) + area->vm_pgoff;

retry_all:
	/*
	 * An external ptracer can access pages that normally aren't
	 * accessible..
	 */
	size = (inode->i_size + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;
	if ((pgoff >= size) && (area->vm_mm == current->mm))
		return NULL;

	/*
	 * The "size" of the file, as far as mmap is concerned, isn't bigger
	 * than the mapping
	 */
	if (size > endoff)
		size = endoff;

	did_readahead = 0;

	/*
	 * The readahead code wants to be told about each and every page
	 * so it can build and shrink its windows appropriately
	 */
	if (VM_SequentialReadHint(area)) {
		did_readahead = 1;
		page_cache_readahead(mapping, ra, file, pgoff);
	}

	/*
	 * If the offset is outside the mapping size we're off the end
	 * of a privately mapped file, so we need to map a zero page.
	 */
	if ((pgoff < size) && !VM_RandomReadHint(area)) {
		did_readahead = 1;
		page_cache_readaround(mapping, ra, file, pgoff);
	}

	/*
	 * Do we have something in the page cache already?
	 */
retry_find:
	page = find_get_page(mapping, pgoff);
	if (!page) {
		if (did_readahead) {
			handle_ra_miss(mapping,ra);
			did_readahead = 0;
		}
		goto no_cached_page;
	}

	/*
	 * Ok, found a page in the page cache, now we need to check
	 * that it's up-to-date.
	 */
	if (!PageUptodate(page))
		goto page_not_uptodate;

success:
	/*
	 * Found the page and have a reference on it, need to check sharing
	 * and possibly copy it over to another page..
	 */
	mark_page_accessed(page);
	flush_page_to_ram(page);
	return page;

no_cached_page:
	/*
	 * We're only likely to ever get here if MADV_RANDOM is in
	 * effect.
	 */
	error = page_cache_read(file, pgoff);

	/*
	 * The page we want has now been added to the page cache.
	 * In the unlikely event that someone removed it in the
	 * meantime, we'll just come back here and read it again.
	 */
	if (error >= 0)
		goto retry_find;

	/*
	 * An error return from page_cache_read can result if the
	 * system is low on memory, or a problem occurs while trying
	 * to schedule I/O.
	 */
	if (error == -ENOMEM)
		return NOPAGE_OOM;
	return NULL;

page_not_uptodate:
	inc_page_state(pgmajfault);
	lock_page(page);

	/* Did it get unhashed while we waited for it? */
	if (!page->mapping) {
		unlock_page(page);
		page_cache_release(page);
		goto retry_all;
	}

	/* Did somebody else get it up-to-date? */
	if (PageUptodate(page)) {
		unlock_page(page);
		goto success;
	}

	if (!mapping->a_ops->readpage(file, page)) {
		wait_on_page_locked(page);
		if (PageUptodate(page))
			goto success;
	}

	/*
	 * Umm, take care of errors if the page isn't up-to-date.
	 * Try to re-read it _once_. We do this synchronously,
	 * because there really aren't any performance issues here
	 * and we need to check for errors.
	 */
	lock_page(page);

	/* Somebody truncated the page on us? */
	if (!page->mapping) {
		unlock_page(page);
		page_cache_release(page);
		goto retry_all;
	}

	/* Somebody else successfully read it in? */
	if (PageUptodate(page)) {
		unlock_page(page);
		goto success;
	}
	ClearPageError(page);
	if (!mapping->a_ops->readpage(file, page)) {
		wait_on_page_locked(page);
		if (PageUptodate(page))
			goto success;
	}

	/*
	 * Things didn't work out. Return zero to tell the
	 * mm layer so, possibly freeing the page cache page first.
	 */
	page_cache_release(page);
	return NULL;
}

static struct vm_operations_struct generic_file_vm_ops = {
	.nopage		= filemap_nopage,
};

/* This is used for a general mmap of a disk file */

int generic_file_mmap(struct file * file, struct vm_area_struct * vma)
{
	struct address_space *mapping = file->f_dentry->d_inode->i_mapping;
	struct inode *inode = mapping->host;

	if ((vma->vm_flags & VM_SHARED) && (vma->vm_flags & VM_MAYWRITE)) {
		if (!mapping->a_ops->writepage)
			return -EINVAL;
	}
	if (!mapping->a_ops->readpage)
		return -ENOEXEC;
	UPDATE_ATIME(inode);
	vma->vm_ops = &generic_file_vm_ops;
	return 0;
}

static inline struct page *__read_cache_page(struct address_space *mapping,
				unsigned long index,
				int (*filler)(void *,struct page*),
				void *data)
{
	struct page *page, *cached_page = NULL;
	int err;
repeat:
	page = find_get_page(mapping, index);
	if (!page) {
		if (!cached_page) {
			cached_page = page_cache_alloc_cold(mapping);
			if (!cached_page)
				return ERR_PTR(-ENOMEM);
		}
		err = add_to_page_cache_lru(cached_page, mapping, index);
		if (err == -EEXIST)
			goto repeat;
		if (err < 0) {
			/* Presumably ENOMEM for radix tree node */
			page_cache_release(cached_page);
			return ERR_PTR(err);
		}
		page = cached_page;
		cached_page = NULL;
		err = filler(data, page);
		if (err < 0) {
			page_cache_release(page);
			page = ERR_PTR(err);
		}
	}
	if (cached_page)
		page_cache_release(cached_page);
	return page;
}

/*
 * Read into the page cache. If a page already exists,
 * and PageUptodate() is not set, try to fill the page.
 */
struct page *read_cache_page(struct address_space *mapping,
				unsigned long index,
				int (*filler)(void *,struct page*),
				void *data)
{
	struct page *page;
	int err;

retry:
	page = __read_cache_page(mapping, index, filler, data);
	if (IS_ERR(page))
		goto out;
	mark_page_accessed(page);
	if (PageUptodate(page))
		goto out;

	lock_page(page);
	if (!page->mapping) {
		unlock_page(page);
		page_cache_release(page);
		goto retry;
	}
	if (PageUptodate(page)) {
		unlock_page(page);
		goto out;
	}
	err = filler(data, page);
	if (err < 0) {
		page_cache_release(page);
		page = ERR_PTR(err);
	}
 out:
	return page;
}

/*
 * If the page was newly created, increment its refcount and add it to the
 * caller's lru-buffering pagevec.  This function is specifically for
 * generic_file_write().
 */
static inline struct page *
__grab_cache_page(struct address_space *mapping, unsigned long index,
			struct page **cached_page, struct pagevec *lru_pvec)
{
	int err;
	struct page *page;
repeat:
	page = find_lock_page(mapping, index);
	if (!page) {
		if (!*cached_page) {
			*cached_page = page_cache_alloc(mapping);
			if (!*cached_page)
				return NULL;
		}
		err = add_to_page_cache(*cached_page, mapping, index);
		if (err == -EEXIST)
			goto repeat;
		if (err == 0) {
			page = *cached_page;
			page_cache_get(page);
			if (!pagevec_add(lru_pvec, page))
				__pagevec_lru_add(lru_pvec);
			*cached_page = NULL;
		}
	}
	return page;
}

inline void remove_suid(struct dentry *dentry)
{
	struct iattr newattrs;
	struct inode *inode = dentry->d_inode;
	unsigned int mode = inode->i_mode & (S_ISUID|S_ISGID|S_IXGRP);

	if (!(mode & S_IXGRP))
		mode &= S_ISUID;

	/* was any of the uid bits set? */
	if (mode && !capable(CAP_FSETID)) {
		newattrs.ia_valid = ATTR_KILL_SUID | ATTR_KILL_SGID;
		notify_change(dentry, &newattrs);
	}
}

static inline int
filemap_copy_from_user(struct page *page, unsigned long offset,
			const char *buf, unsigned bytes)
{
	char *kaddr;
	int left;

	kaddr = kmap_atomic(page, KM_USER0);
	left = __copy_from_user(kaddr + offset, buf, bytes);
	kunmap_atomic(kaddr, KM_USER0);

	if (left != 0) {
		/* Do it the slow way */
		kaddr = kmap(page);
		left = __copy_from_user(kaddr + offset, buf, bytes);
		kunmap(page);
	}
	return left;
}

static inline int
__filemap_copy_from_user_iovec(char *vaddr, 
			const struct iovec *iov, size_t base, size_t bytes)
{
	int left = 0;

	while (bytes) {
		char *buf = iov->iov_base + base;
		int copy = min(bytes, iov->iov_len - base);
		base = 0;
		if ((left = __copy_from_user(vaddr, buf, copy)))
			break;
		bytes -= copy;
		vaddr += copy;
		iov++;
	}
	return left;
}

static inline int
filemap_copy_from_user_iovec(struct page *page, unsigned long offset,
			const struct iovec *iov, size_t base, size_t bytes)
{
	char *kaddr;
	int left;

	kaddr = kmap_atomic(page, KM_USER0);
	left = __filemap_copy_from_user_iovec(kaddr + offset, iov, base, bytes);
	kunmap_atomic(kaddr, KM_USER0);
	if (left != 0) {
		kaddr = kmap(page);
		left = __filemap_copy_from_user_iovec(kaddr + offset, iov, base, bytes);
		kunmap(page);
	}
	return left;
}

static inline void
filemap_set_next_iovec(const struct iovec **iovp, size_t *basep, size_t bytes)
{
	const struct iovec *iov = *iovp;
	size_t base = *basep;

	while (bytes) {
		int copy = min(bytes, iov->iov_len - base);
		bytes -= copy;
		base += copy;
		if (iov->iov_len == base) {
			iov++;
			base = 0;
		}
	}
	*iovp = iov;
	*basep = base;
}


/*
 * Write to a file through the page cache. 
 *
 * We put everything into the page cache prior to writing it. This is not a
 * problem when writing full pages. With partial pages, however, we first have
 * to read the data into the cache, then dirty the page, and finally schedule
 * it for writing by marking it dirty.
 *							okir@monad.swb.de
 */
ssize_t
generic_file_write_nolock(struct file *file, const struct iovec *iov,
				unsigned long nr_segs, loff_t *ppos)
{
	struct address_space * mapping = file->f_dentry->d_inode->i_mapping;
	struct address_space_operations *a_ops = mapping->a_ops;
	size_t ocount;		/* original count */
	size_t count;		/* after file limit checks */
	struct inode 	*inode = mapping->host;
	unsigned long	limit = current->rlim[RLIMIT_FSIZE].rlim_cur;
	long		status = 0;
	loff_t		pos;
	struct page	*page;
	struct page	*cached_page = NULL;
	ssize_t		written;
	int		err;
	size_t		bytes;
	time_t		time_now;
	struct pagevec	lru_pvec;
	const struct iovec *cur_iov = iov; /* current iovec */
	size_t		iov_base = 0;	   /* offset in the current iovec */
	unsigned long	seg;
	char		*buf;

	ocount = 0;
	for (seg = 0; seg < nr_segs; seg++) {
		const struct iovec *iv = &iov[seg];

		/*
		 * If any segment has a negative length, or the cumulative
		 * length ever wraps negative then return -EINVAL.
		 */
		ocount += iv->iov_len;
		if (unlikely((ssize_t)(ocount|iv->iov_len) < 0))
			return -EINVAL;
		if (access_ok(VERIFY_READ, iv->iov_base, iv->iov_len))
			continue;
		if (seg == 0)
			return -EFAULT;
		nr_segs = seg;
		break;
	}
	count = ocount;

	pos = *ppos;
	if (unlikely(pos < 0))
		return -EINVAL;

	/* We can write back this queue in page reclaim */
	current->backing_dev_info = mapping->backing_dev_info;

	pagevec_init(&lru_pvec, 0);

	if (unlikely(file->f_error)) {
		err = file->f_error;
		file->f_error = 0;
		goto out;
	}

	written = 0;

	/* FIXME: this is for backwards compatibility with 2.4 */
	if (!S_ISBLK(inode->i_mode) && file->f_flags & O_APPEND)
		pos = inode->i_size;

	/*
	 * Check whether we've reached the file size limit.
	 */
	if (unlikely(limit != RLIM_INFINITY)) {
		if (pos >= limit) {
			send_sig(SIGXFSZ, current, 0);
			err = -EFBIG;
			goto out;
		}
		if (pos > 0xFFFFFFFFULL || count > limit - (u32)pos) {
			/* send_sig(SIGXFSZ, current, 0); */
			count = limit - (u32)pos;
		}
	}

	/*
	 * LFS rule
	 */
	if (unlikely(pos + count > MAX_NON_LFS &&
				!(file->f_flags & O_LARGEFILE))) {
		if (pos >= MAX_NON_LFS) {
			send_sig(SIGXFSZ, current, 0);
			err = -EFBIG;
			goto out;
		}
		if (count > MAX_NON_LFS - (u32)pos) {
			/* send_sig(SIGXFSZ, current, 0); */
			count = MAX_NON_LFS - (u32)pos;
		}
	}

	/*
	 * Are we about to exceed the fs block limit ?
	 *
	 * If we have written data it becomes a short write.  If we have
	 * exceeded without writing data we send a signal and return EFBIG.
	 * Linus frestrict idea will clean these up nicely..
	 */
	if (likely(!S_ISBLK(inode->i_mode))) {
		if (unlikely(pos >= inode->i_sb->s_maxbytes)) {
			if (count || pos > inode->i_sb->s_maxbytes) {
				send_sig(SIGXFSZ, current, 0);
				err = -EFBIG;
				goto out;
			}
			/* zero-length writes at ->s_maxbytes are OK */
		}

		if (unlikely(pos + count > inode->i_sb->s_maxbytes))
			count = inode->i_sb->s_maxbytes - pos;
	} else {
		if (bdev_read_only(inode->i_bdev)) {
			err = -EPERM;
			goto out;
		}
		if (pos >= inode->i_size) {
			if (count || pos > inode->i_size) {
				err = -ENOSPC;
				goto out;
			}
		}

		if (pos + count > inode->i_size)
			count = inode->i_size - pos;
	}

	err = 0;
	if (count == 0)
		goto out;

	remove_suid(file->f_dentry);
	time_now = CURRENT_TIME;
	if (inode->i_ctime != time_now || inode->i_mtime != time_now) {
		inode->i_ctime = time_now;
		inode->i_mtime = time_now;
		mark_inode_dirty_sync(inode);
	}

	/* coalesce the iovecs and go direct-to-BIO for O_DIRECT */
	if (unlikely(file->f_flags & O_DIRECT)) {
		if (count != ocount)
			nr_segs = iov_shorten((struct iovec *)iov,
						nr_segs, count);
		written = generic_file_direct_IO(WRITE, file,
					iov, pos, nr_segs);
		if (written > 0) {
			loff_t end = pos + written;
			if (end > inode->i_size && !S_ISBLK(inode->i_mode)) {
				inode->i_size = end;
				mark_inode_dirty(inode);
			}
			*ppos = end;
		}
		/*
		 * Sync the fs metadata but not the minor inode changes and
		 * of course not the data as we did direct DMA for the IO.
		 */
		if (written >= 0 && file->f_flags & O_SYNC)
			status = generic_osync_inode(inode, OSYNC_METADATA);
		goto out_status;
	}

	buf = iov->iov_base;
	do {
		unsigned long index;
		unsigned long offset;
		long page_fault;

		offset = (pos & (PAGE_CACHE_SIZE -1)); /* Within page */
		index = pos >> PAGE_CACHE_SHIFT;
		bytes = PAGE_CACHE_SIZE - offset;
		if (bytes > count)
			bytes = count;

		/*
		 * Bring in the user page that we will copy from _first_.
		 * Otherwise there's a nasty deadlock on copying from the
		 * same page as we're writing to, without it being marked
		 * up-to-date.
		 */
		fault_in_pages_readable(buf, bytes);

		page = __grab_cache_page(mapping,index,&cached_page,&lru_pvec);
		if (!page) {
			status = -ENOMEM;
			break;
		}

		status = a_ops->prepare_write(file, page, offset, offset+bytes);
		if (unlikely(status)) {
			/*
			 * prepare_write() may have instantiated a few blocks
			 * outside i_size.  Trim these off again.
			 */
			unlock_page(page);
			page_cache_release(page);
			if (pos + bytes > inode->i_size)
				vmtruncate(inode, inode->i_size);
			break;
		}
		if (likely(nr_segs == 1))
			page_fault = filemap_copy_from_user(page, offset,
							buf, bytes);
		else
			page_fault = filemap_copy_from_user_iovec(page, offset,
						cur_iov, iov_base, bytes);
		flush_dcache_page(page);
		status = a_ops->commit_write(file, page, offset, offset+bytes);
		if (unlikely(page_fault)) {
			status = -EFAULT;
		} else {
			if (!status)
				status = bytes;

			if (status >= 0) {
				written += status;
				count -= status;
				pos += status;
				buf += status;
				if (unlikely(nr_segs > 1))
					filemap_set_next_iovec(&cur_iov,
							&iov_base, status);
			}
		}
		if (!PageReferenced(page))
			SetPageReferenced(page);
		unlock_page(page);
		page_cache_release(page);
		if (status < 0)
			break;
		balance_dirty_pages_ratelimited(mapping);
	} while (count);
	*ppos = pos;

	if (cached_page)
		page_cache_release(cached_page);

	/*
	 * For now, when the user asks for O_SYNC, we'll actually give O_DSYNC
	 */
	if (status >= 0) {
		if ((file->f_flags & O_SYNC) || IS_SYNC(inode))
			status = generic_osync_inode(inode,
					OSYNC_METADATA|OSYNC_DATA);
	}
	
out_status:	
	err = written ? written : status;
out:
	pagevec_lru_add(&lru_pvec);
	current->backing_dev_info = 0;
	return err;
}

ssize_t generic_file_aio_write(struct kiocb *iocb, const char *buf,
			       size_t count, loff_t pos)
{
	return generic_file_write(iocb->ki_filp, buf, count, &iocb->ki_pos);
}
EXPORT_SYMBOL(generic_file_aio_write);

ssize_t generic_file_write(struct file *file, const char *buf,
			   size_t count, loff_t *ppos)
{
	struct inode	*inode = file->f_dentry->d_inode->i_mapping->host;
	int		err;
	struct iovec local_iov = { .iov_base = (void *)buf, .iov_len = count };

	down(&inode->i_sem);
	err = generic_file_write_nolock(file, &local_iov, 1, ppos);
	up(&inode->i_sem);

	return err;
}

ssize_t generic_file_readv(struct file *filp, const struct iovec *iov,
			unsigned long nr_segs, loff_t *ppos)
{
	struct kiocb kiocb;
	ssize_t ret;

	init_sync_kiocb(&kiocb, filp);
	ret = __generic_file_aio_read(&kiocb, iov, nr_segs, ppos);
	if (-EIOCBQUEUED == ret)
		ret = wait_on_sync_kiocb(&kiocb);
	return ret;
}

ssize_t generic_file_writev(struct file *file, const struct iovec *iov,
			unsigned long nr_segs, loff_t * ppos) 
{
	struct inode *inode = file->f_dentry->d_inode;
	ssize_t ret;

	down(&inode->i_sem);
	ret = generic_file_write_nolock(file, iov, nr_segs, ppos);
	up(&inode->i_sem);
	return ret;
}
