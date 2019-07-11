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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/ktrace.h#4 $
 */

#ifndef KTRACE_H
#define KTRACE_H

#include <linux/device-mapper.h>

#include "common.h"
#include "trace.h"

struct kernel_layer;
struct kvio;

// Implement event sampling once per N.
struct sample_counter {
	unsigned int interval;
	unsigned int tick;
	spinlock_t lock;
};

/**
 * Flag indicating whether newly created VDO devices should record trace info.
 **/
extern bool trace_recording;

/**
 * Updates the counter state and returns true once each time the
 * sampling interval is reached.
 *
 * @param counter    The sampling counter info
 *
 * @return whether to do sampling on this invocation
 **/
bool sample_this_one(struct sample_counter *counter);

/**
 * Initialize trace data in the KernelLayer
 *
 * @param layer  The KernelLayer
 *
 * @return VDO_SUCCESS, or an error code
 **/
int trace_kernel_layer_init(struct kernel_layer *layer);

/**
 * Initialize the mutex used when logging latency tracing data.
 **/
void initialize_trace_logging_once(void);

/**
 * Allocate a trace buffer
 *
 * @param layer          The KernelLayer
 * @param trace_pointer  The trace buffer is returned here
 *
 * @return VDO_SUCCESS or an error code
 **/
int alloc_trace_from_pool(struct kernel_layer *layer, Trace **trace_pointer);

/**
 * Free a trace buffer
 *
 * @param layer  The KernelLayer
 * @param trace  The trace buffer
 **/
void free_trace_to_pool(struct kernel_layer *layer, Trace *trace);

/**
 * Log the trace at kvio freeing time
 *
 * @param kvio  The kvio structure
 **/
void log_kvio_trace(struct kvio *kvio);

#endif /* KTRACE_H */
