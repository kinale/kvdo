// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "bio.h"

#include "logger.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "permassert.h"

#include "atomic-stats.h"
#include "kernel-types.h"
#include "vdo.h"
#include "vio.h"

/*
 * Copy bio data to a buffer
 */
void vdo_bio_copy_data_in(struct bio *bio, char *data_ptr)
{
	struct bio_vec biovec;
	struct bvec_iter iter;

	bio_for_each_segment(biovec, bio, iter) {
		memcpy_from_bvec(data_ptr, &biovec);
		data_ptr += biovec.bv_len;
	}
}

/*
 * Copy a buffer into a bio's data
 */
void vdo_bio_copy_data_out(struct bio *bio, char *data_ptr)
{
	struct bio_vec biovec;
	struct bvec_iter iter;

	bio_for_each_segment(biovec, bio, iter) {
		memcpy_to_bvec(&biovec, data_ptr);
		data_ptr += biovec.bv_len;
	}
}

void vdo_free_bio(struct bio *bio)
{
	if (bio == NULL) {
		return;
	}

	bio_uninit(bio);
	UDS_FREE(UDS_FORGET(bio));
}

/*----------------------------------------------------------------*/
/*
 * Various counting functions for statistics.
 * These are used for bios coming into VDO, as well as bios generated by VDO.
 */
void vdo_count_bios(struct atomic_bio_stats *bio_stats, struct bio *bio)
{
	if (((bio->bi_opf & REQ_PREFLUSH) != 0) &&
	    (bio->bi_iter.bi_size == 0)) {
		atomic64_inc(&bio_stats->empty_flush);
		atomic64_inc(&bio_stats->flush);
		return;
	}

	switch (bio_op(bio)) {
	case REQ_OP_WRITE:
		atomic64_inc(&bio_stats->write);
		break;
	case REQ_OP_READ:
		atomic64_inc(&bio_stats->read);
		break;
	case REQ_OP_DISCARD:
		atomic64_inc(&bio_stats->discard);
		break;
		/*
		 * All other operations are filtered out in dmvdo.c, or
		 * not created by VDO, so shouldn't exist.
		 */
	default:
		ASSERT_LOG_ONLY(0, "Bio operation %d not a write, read, discard, or empty flush",
				bio_op(bio));
	}

	if ((bio->bi_opf & REQ_PREFLUSH) != 0) {
		atomic64_inc(&bio_stats->flush);
	}
	if (bio->bi_opf & REQ_FUA) {
		atomic64_inc(&bio_stats->fua);
	}
}

static void count_all_bios_completed(struct vio *vio, struct bio *bio)
{
	struct atomic_statistics *stats = &vdo_from_vio(vio)->stats;

	if (is_data_vio(vio)) {
		vdo_count_bios(&stats->bios_out_completed, bio);
		return;
	}

	vdo_count_bios(&stats->bios_meta_completed, bio);
	if (vio->type == VIO_TYPE_RECOVERY_JOURNAL) {
		vdo_count_bios(&stats->bios_journal_completed, bio);
	} else if (vio->type == VIO_TYPE_BLOCK_MAP) {
		vdo_count_bios(&stats->bios_page_cache_completed, bio);
	}
}

void vdo_count_completed_bios(struct bio *bio)
{
	struct vio *vio = (struct vio *) bio->bi_private;
	atomic64_inc(&vdo_from_vio(vio)->stats.bios_completed);
	count_all_bios_completed(vio, bio);
}

/*----------------------------------------------------------------*/

/*
 * Completes a bio relating to a vio, causing the vio completion callback
 * to be invoked.
 *
 * This is used as the bi_end_io function for most of the bios created within
 * VDO and submitted to the storage device. Exceptions are the flush code and
 * the read-block code, both of which need to do work after the I/O completes.
 */
void vdo_complete_async_bio(struct bio *bio)
{
	struct vio *vio = (struct vio *) bio->bi_private;

	vdo_count_completed_bios(bio);
	continue_vio(vio, vdo_get_bio_result(bio));
}

/*
 * Set bio properties for a VDO read or write. The vio associated with the bio
 * may be NULL.
 */
void vdo_set_bio_properties(struct bio *bio,
			    struct vio *vio,
			    bio_end_io_t callback,
			    unsigned int bi_opf,
			    physical_block_number_t pbn)
{
	bio->bi_private = vio;
	bio->bi_end_io = callback;
	bio->bi_opf = bi_opf;
	if ((vio != NULL) && (pbn != VDO_GEOMETRY_BLOCK_LOCATION)) {
		pbn -= vdo_from_vio(vio)->geometry.bio_offset;
	}
	bio->bi_iter.bi_sector = pbn * VDO_SECTORS_PER_BLOCK;
}

/*
 * Prepares the bio to perform IO with the specified buffer.
 * May only be used on a VDO-allocated bio, as it assumes the bio
 * wraps a 4k buffer that is 4k aligned, but there does not have
 * to be a vio associated with the bio.
 */
int vdo_reset_bio_with_buffer(struct bio *bio,
			      char *data,
			      struct vio *vio,
			      bio_end_io_t callback,
			      unsigned int bi_opf,
			      physical_block_number_t pbn)
{
	int bvec_count, result, offset, len, i;
	unsigned short blocks;

	if (vio == NULL) {
		blocks = 1;
	} else if (vio->type == VIO_TYPE_DATA) {
		result = ASSERT((vio->block_count == 1),
				"Data vios may not span multiple blocks");
		if (result != VDO_SUCCESS) {
			return result;
		}

		blocks = 1;
	} else {
		blocks = vio->block_count;
	}

#ifdef RHEL_RELEASE_CODE
#define USE_ALTERNATE (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(9,1))
#else
#define USE_ALTERNATE (LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0))
#endif

#if USE_ALTERNATE
	bio_reset(bio);
#else
	bio_reset(bio, bio->bi_bdev, bi_opf);
#endif
	vdo_set_bio_properties(bio, vio, callback, bi_opf, pbn);
	if (data == NULL) {
		return VDO_SUCCESS;
	}

	bio->bi_io_vec = bio->bi_inline_vecs;
	bio->bi_max_vecs = blocks + 1;
	len = VDO_BLOCK_SIZE * blocks;
	offset = offset_in_page(data);
	bvec_count = DIV_ROUND_UP(offset + len, PAGE_SIZE);

	/*
	 * If we knew that data was always on one page, or contiguous pages,
	 * we wouldn't need the loop. But if we're using vmalloc, it's not
	 * impossible that the data is in different pages that can't be
	 * merged in bio_add_page...
	 */
	for (i = 0; (i < bvec_count) && (len > 0); i++) {
		struct page *page;
		int bytes_added;
		int bytes = PAGE_SIZE - offset;

		if (bytes > len) {
			bytes = len;
		}

		page = is_vmalloc_addr(data) ? vmalloc_to_page(data)
					     : virt_to_page(data);
		bytes_added = bio_add_page(bio, page, bytes, offset);

		if (bytes_added != bytes) {
			return uds_log_error_strerror(VDO_BIO_CREATION_FAILED,
						      "Could only add %i bytes to bio",
						       bytes_added);
		}

		data += bytes;
		len -= bytes;
		offset = 0;
	}

	return VDO_SUCCESS;
}

int vdo_create_multi_block_bio(block_count_t size, struct bio **bio_ptr)
{
	struct bio *bio = NULL;
	int result = UDS_ALLOCATE_EXTENDED(struct bio,
					   size + 1,
					   struct bio_vec,
					   "bio",
					   &bio);
	if (result != VDO_SUCCESS) {
		return result;
	}

	*bio_ptr = bio;
	return VDO_SUCCESS;
}

