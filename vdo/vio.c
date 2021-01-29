/*
 * Copyright Red Hat
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vio.c#34 $
 */

#include "vio.h"

#include "logger.h"

#include "dataVIO.h"
#include "vdoInternal.h"

#include <linux/ratelimit.h>

/**********************************************************************/
void free_vio(struct vio **vio_ptr)
{
	struct vio *vio = *vio_ptr;
	if (vio == NULL) {
		return;
	}

	destroy_vio(vio_ptr);
}

/**********************************************************************/
void initialize_vio(struct vio *vio,
		    struct bio *bio,
		    enum vio_type vio_type,
		    enum vio_priority priority,
		    struct vdo_completion *parent,
		    struct vdo *vdo,
		    char *data)
{
	struct vdo_completion *completion = vio_as_completion(vio);

	vio->bio = bio;
	vio->vdo = vdo;
	vio->type = vio_type;
	vio->priority = priority;
	vio->data = data;

	initialize_completion(completion, VIO_COMPLETION, vdo->layer);
	completion->parent = parent;
}

/**********************************************************************/
void vio_done_callback(struct vdo_completion *completion)
{
	struct vio *vio = as_vio(completion);
	completion->callback = vio->callback;
	completion->error_handler = vio->error_handler;
	complete_completion(completion);
}

/**********************************************************************/
void get_vio_operation_description(const struct vio *vio, char *buffer)
{
	int buffer_remaining = VIO_OPERATION_DESCRIPTION_MAX_LENGTH;

	static const char *operations[] = {
		[VIO_UNSPECIFIED_OPERATION] = "empty",
		[VIO_READ]		    = "read",
		[VIO_WRITE]		    = "write",
		[VIO_READ_MODIFY_WRITE]	    = "read-modify-write",
	};
	int written = snprintf(buffer, buffer_remaining, "%s",
		operations[vio->operation & VIO_READ_WRITE_MASK]);
	if ((written < 0) || (buffer_remaining < written)) {
		// Should never happen, but if it does, we've done as much
		// description as possible.
		return;
	}

	buffer += written;
	buffer_remaining -= written;

	if (vio->operation & VIO_FLUSH_BEFORE) {
		written = snprintf(buffer, buffer_remaining, "+preflush");
	}

	if ((written < 0) || (buffer_remaining < written)) {
		// Should never happen, but if it does, we've done as much
		// description as possible.
		return;
	}

	buffer += written;
	buffer_remaining -= written;

	if (vio->operation & VIO_FLUSH_AFTER) {
		snprintf(buffer, buffer_remaining, "+postflush");
	}

	STATIC_ASSERT(sizeof("write+preflush+postflush") <=
		      VIO_OPERATION_DESCRIPTION_MAX_LENGTH);
}

/**********************************************************************/
void update_vio_error_stats(struct vio *vio, const char *format, ...)
{
	static DEFINE_RATELIMIT_STATE(error_limiter,
				      DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);

	va_list args;
	int priority;

	int result = vio_as_completion(vio)->result;
	switch (result) {
	case VDO_READ_ONLY:
		atomic64_add(1, &vio->vdo->error_stats.read_only_error_count);
		return;

	case VDO_NO_SPACE:
		atomic64_add(1, &vio->vdo->error_stats.no_space_error_count);
		priority = LOG_DEBUG;
		break;

	default:
		priority = LOG_ERR;
	}

	if (!__ratelimit(&error_limiter)) {
		return;
	}

	va_start(args, format);
	vlog_strerror(priority, result, format, args);
	va_end(args);
}

/**
 * Handle an error from a metadata I/O.
 *
 * @param completion  The vio
 **/
static void handle_metadata_io_error(struct vdo_completion *completion)
{
	struct vio *vio = as_vio(completion);
	char vio_operation[VIO_OPERATION_DESCRIPTION_MAX_LENGTH];
	get_vio_operation_description(vio, vio_operation);
	update_vio_error_stats(vio,
			       "Completing %s vio of type %u for physical block %llu with error",
			       vio_operation,
			       vio->type,
			       vio->physical);
	vio_done_callback(completion);
}

/**********************************************************************/
void launch_metadata_vio(struct vio *vio,
			 physical_block_number_t physical,
			 vdo_action *callback,
			 vdo_action *error_handler,
			 enum vio_operation operation)
{
	struct vdo_completion *completion = vio_as_completion(vio);

	vio->operation = operation;
	vio->physical = physical;
	vio->callback = callback;
	vio->error_handler = error_handler;

	reset_completion(completion);
	completion->callback = vio_done_callback;
	completion->error_handler = handle_metadata_io_error;

	submit_metadata_vio(vio);
}
