/*
 * Copyright (c) 2019 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/kvio.c#18 $
 */

#include "kvio.h"


#include "logger.h"
#include "memoryAlloc.h"

#include "numUtils.h"
#include "vdo.h"
#include "waitQueue.h"

#include "bio.h"
#include "ioSubmitter.h"
#include "kvdoFlush.h"

/**
 * A function to tell vdo that we have completed the requested async
 * operation for a vio
 *
 * @param item    The work item of the VIO to complete
 **/
static void kvdo_handle_vio_callback(struct kvdo_work_item *item)
{
	struct kvio *kvio = work_item_as_kvio(item);
	runCallback(vioAsCompletion(kvio->vio));
}

/**********************************************************************/
void kvdo_enqueue_vio_callback(struct kvio *kvio)
{
	enqueue_kvio(kvio,
                     kvdo_handle_vio_callback,
                     (KvdoWorkFunction)vioAsCompletion(kvio->vio)->callback,
                     REQ_Q_ACTION_VIO_CALLBACK);
}

/**********************************************************************/
void kvdo_continue_kvio(struct kvio *kvio, int error)
{
	if (unlikely(error != VDO_SUCCESS)) {
		setCompletionResult(vioAsCompletion(kvio->vio), error);
	}
	kvdo_enqueue_vio_callback(kvio);
}

/**********************************************************************/
// noinline ensures systemtap can hook in here
static noinline void maybe_log_kvio_trace(struct kvio *kvio)
{
	if (kvio->layer->trace_logging) {
		log_kvio_trace(kvio);
	}
}

/**********************************************************************/
static void free_kvio(struct kvio **kvio_ptr)
{
	struct kvio *kvio = *kvio_ptr;
	if (kvio == NULL) {
		return;
	}

	if (unlikely(kvio->vio->trace != NULL)) {
		maybe_log_kvio_trace(kvio);
		FREE(kvio->vio->trace);
	}

	free_bio(kvio->bio, kvio->layer);
	FREE(kvio);
	*kvio_ptr = NULL;
}

/**********************************************************************/
void free_metadata_kvio(struct metadata_kvio **metadata_kvio_ptr)
{
	free_kvio((struct kvio **)metadata_kvio_ptr);
}

/**********************************************************************/
void free_compressed_write_kvio(
	struct compressed_write_kvio **compressed_write_kvio_ptr)
{
	free_kvio((struct kvio **)compressed_write_kvio_ptr);
}

/**********************************************************************/
void writeCompressedBlock(AllocatingVIO *allocating_vio)
{
	// This method assumes that compressed writes never set the flush or
	// FUA bits.
	struct compressed_write_kvio *compressed_write_kvio =
		allocating_vio_as_compressed_write_kvio(allocating_vio);
	struct kvio *kvio =
		compressed_write_kvio_as_kvio(compressed_write_kvio);
	struct bio *bio = kvio->bio;
	reset_bio(bio, kvio->layer);
	set_bio_operation_write(bio);
	set_bio_sector(bio, block_to_sector(kvio->layer, kvio->vio->physical));
	vdo_submit_bio(bio, BIO_Q_ACTION_COMPRESSED_DATA);
}

/**
 * Get the BioQueue action for a metadata VIO based on that VIO's priority.
 *
 * @param vio  The VIO
 *
 * @return The action with which to submit the VIO's bio.
 **/
static inline bio_q_action get_metadata_action(VIO *vio)
{
	return ((vio->priority == VIO_PRIORITY_HIGH) ? BIO_Q_ACTION_HIGH :
						       BIO_Q_ACTION_METADATA);
}

/**********************************************************************/
void submitMetadataVIO(VIO *vio)
{
	struct kvio *kvio = metadata_kvio_as_kvio(vio_as_metadata_kvio(vio));
	struct bio *bio = kvio->bio;
	reset_bio(bio, kvio->layer);

	set_bio_sector(bio, block_to_sector(kvio->layer, vio->physical));

	// Metadata I/Os bypass the read cache.
	if (isReadVIO(vio)) {
		ASSERT_LOG_ONLY(!vioRequiresFlushBefore(vio),
				"read VIO does not require flush before");
		vioAddTraceRecord(vio, THIS_LOCATION("$F;io=readMeta"));
		set_bio_operation_read(bio);
	} else if (vioRequiresFlushBefore(vio)) {
		set_bio_operation_write(bio);
		set_bio_operation_flag_preflush(bio);
		vioAddTraceRecord(vio, THIS_LOCATION("$F;io=flushWriteMeta"));
	} else {
		set_bio_operation_write(bio);
		vioAddTraceRecord(vio, THIS_LOCATION("$F;io=writeMeta"));
	}

	if (vioRequiresFlushAfter(vio)) {
		set_bio_operation_flag_fua(bio);
	}
	vdo_submit_bio(bio, get_metadata_action(vio));
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
/**
 * Handle the completion of a base-code initiated flush by continuing the flush
 * VIO.
 *
 * @param bio    The bio to complete
 **/
static void complete_flush_bio(struct bio *bio)
#else
/**
 * Handle the completion of a base-code initiated flush by continuing the flush
 * VIO.
 *
 * @param bio    The bio to complete
 * @param error  Possible error from underlying block device
 **/
static void complete_flush_bio(struct bio *bio, int error)
#endif
{
	struct kvio *kvio = (struct kvio *)bio->bi_private;
	// Restore the bio's notion of its own data.
	reset_bio(bio, kvio->layer);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
	kvdo_continue_kvio(kvio, get_bio_result(bio));
#else
	kvdo_continue_kvio(kvio, error);
#endif
}

/**********************************************************************/
void kvdo_flush_vio(VIO *vio)
{
	struct kvio *kvio = metadata_kvio_as_kvio(vio_as_metadata_kvio(vio));
	struct bio *bio = kvio->bio;
	struct kernel_layer *layer = kvio->layer;
	reset_bio(bio, layer);
	prepare_flush_bio(bio,
			  kvio,
			  get_kernel_layer_bdev(layer),
			  complete_flush_bio);
	vdo_submit_bio(bio, get_metadata_action(vio));
}

/*
 * Hook for a SystemTap probe to potentially restrict the choices
 * of which VIOs should have their latencies tracked.
 *
 * Normally returns true. Even if true is returned, sample_this_one may
 * cut down the monitored VIOs by some fraction so as to reduce the
 * impact on system performance.
 *
 * Must be "noinline" so that SystemTap can find the return
 * instruction and modify the return value.
 *
 * @param kvio   The kvio being initialized
 * @param layer  The kernel layer
 * @param bio    The incoming I/O request
 *
 * @return whether it's useful to track latency for VIOs looking like
 *         this one
 */
static noinline bool sample_this_vio(struct kvio *kvio,
				     struct kernel_layer *layer,
				     struct bio *bio)
{
	bool result = true;
	// Ensure the arguments and result exist at the same time, for
	// SystemTap.
	__asm__ __volatile__(""
			     : "=g"(result)
			     : "0"(result), "g"(kvio), "g"(layer), "g"(bio)
			     : "memory");
	return result;
}

/**********************************************************************/
void initialize_kvio(struct kvio *kvio,
		     struct kernel_layer *layer,
		     VIOType vio_type,
		     VIOPriority priority,
		     void *parent,
		     struct bio *bio)
{
	if (layer->vioTraceRecording && sample_this_vio(kvio, layer, bio) &&
	    sample_this_one(&layer->trace_sample_counter)) {
		int result =
			(isDataVIOType(vio_type) ?
				 alloc_trace_from_pool(layer, &kvio->vio->trace) :
				 ALLOCATE(1, Trace, "trace",
					  &kvio->vio->trace));
		if (result != VDO_SUCCESS) {
			logError("trace record allocation failure %d", result);
		}
	}

	kvio->bio = bio;
	kvio->layer = layer;
	if (bio != NULL) {
		bio->bi_private = kvio;
	}

	initializeVIO(kvio->vio,
		      vio_type,
		      priority,
		      parent,
		      get_vdo(&layer->kvdo),
		      &layer->common);

	// XXX: The "init" label should be replaced depending on the
	// write/read/flush path followed.
	kvio_add_trace_record(kvio, THIS_LOCATION("$F;io=?init;j=normal"));

	VDOCompletion *completion = vioAsCompletion(kvio->vio);
	kvio->enqueueable.enqueueable.completion = completion;
	completion->enqueueable = &kvio->enqueueable.enqueueable;
}

/**
 * Construct a metadata kvio.
 *
 * @param [in]  layer             The physical layer
 * @param [in]  vio_type          The type of VIO to create
 * @param [in]  priority          The relative priority to assign to the
 *                                metadata_kvio
 * @param [in]  parent            The parent of the metadata_kvio completion
 * @param [in]  bio               The bio to associate with this metadata_kvio
 * @param [out] metadata_kvio_ptr  A pointer to hold the new metadata_kvio
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result)) static int
make_metadata_kvio(struct kernel_layer *layer,
		   VIOType vio_type,
		   VIOPriority priority,
		   void *parent,
		   struct bio *bio,
		   struct metadata_kvio **metadata_kvio_ptr)
{
	// If struct metadata_kvio grows past 256 bytes, we'll lose benefits of
	// VDOSTORY-176.
	STATIC_ASSERT(sizeof(struct metadata_kvio) <= 256);

	// Metadata VIOs should use direct allocation and not use the buffer
	// pool, which is reserved for submissions from the linux block layer.
	struct metadata_kvio *metadata_kvio;
	int result =
		ALLOCATE(1, struct metadata_kvio, __func__, &metadata_kvio);
	if (result != VDO_SUCCESS) {
		logError("metadata kvio allocation failure %d", result);
		return result;
	}

	struct kvio *kvio = &metadata_kvio->kvio;
	kvio->vio = &metadata_kvio->vio;
	initialize_kvio(kvio, layer, vio_type, priority, parent, bio);
	*metadata_kvio_ptr = metadata_kvio;
	return VDO_SUCCESS;
}

/**
 * Construct a struct compressed_write_kvio.
 *
 * @param [in]  layer                      The physical layer
 * @param [in]  parent                     The parent of the
 *                                         compressed_write_kvio completion
 * @param [in]  bio                        The bio to associate with this
 *                                         compressed_write_kvio
 * @param [out] compressed_write_kvio_ptr  A pointer to hold the new
 *                                         compressed_write_kvio
 *
 * @return VDO_SUCCESS or an error
 **/
__attribute__((warn_unused_result))
static int make_compressed_write_kvio(struct kernel_layer *layer,
				      void *parent,
				      struct bio *bio,
				      struct compressed_write_kvio **compressed_write_kvio_ptr)
{
	// Compressed write VIOs should use direct allocation and not use the
	// buffer pool, which is reserved for submissions from the linux block
	// layer.
	struct compressed_write_kvio *compressed_write_kvio;
	int result = ALLOCATE(1, struct compressed_write_kvio, __func__,
			      &compressed_write_kvio);
	if (result != VDO_SUCCESS) {
		logError("compressed write kvio allocation failure %d", result);
		return result;
	}

	struct kvio *kvio = &compressed_write_kvio->kvio;
	kvio->vio = allocatingVIOAsVIO(&compressed_write_kvio->allocating_vio);
	initialize_kvio(kvio,
			layer,
			VIO_TYPE_COMPRESSED_BLOCK,
			VIO_PRIORITY_COMPRESSED_DATA,
			parent,
			bio);
	*compressed_write_kvio_ptr = compressed_write_kvio;
	return VDO_SUCCESS;
}

/**********************************************************************/
int kvdo_create_metadata_vio(PhysicalLayer *layer,
			     VIOType vio_type,
			     VIOPriority priority,
			     void *parent,
			     char *data,
			     VIO **vio_ptr)
{
	int result = ASSERT(isMetadataVIOType(vio_type),
			    "%d is a metadata type",
			    vio_type);
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct bio *bio;
	struct kernel_layer *kernel_layer = as_kernel_layer(layer);
	result = create_bio(kernel_layer, data, &bio);
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct metadata_kvio *metadata_kvio;
	result = make_metadata_kvio(kernel_layer, vio_type, priority, parent,
				    bio, &metadata_kvio);
	if (result != VDO_SUCCESS) {
		free_bio(bio, kernel_layer);
		return result;
	}

	*vio_ptr = &metadata_kvio->vio;
	return VDO_SUCCESS;
}

/**********************************************************************/
int kvdo_create_compressed_write_vio(PhysicalLayer *layer,
				     void *parent,
				     char *data,
				     AllocatingVIO **allocating_vio_ptr)
{
	struct bio *bio;
	struct kernel_layer *kernel_layer = as_kernel_layer(layer);
	int result = create_bio(kernel_layer, data, &bio);
	if (result != VDO_SUCCESS) {
		return result;
	}

	struct compressed_write_kvio *compressed_write_kvio;
	result = make_compressed_write_kvio(kernel_layer, parent, bio,
					    &compressed_write_kvio);
	if (result != VDO_SUCCESS) {
		free_bio(bio, kernel_layer);
		return result;
	}

	*allocating_vio_ptr = &compressed_write_kvio->allocating_vio;
	return VDO_SUCCESS;
}
