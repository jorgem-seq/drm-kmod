/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#ifndef __I915_GEM_CONTEXT_TYPES_H__
#define __I915_GEM_CONTEXT_TYPES_H__

#include <linux/atomic.h>
#include <linux/list.h>
#include <linux/llist.h>
#include <linux/kref.h>
#include <linux/mutex.h>
#include <linux/radix-tree.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <linux/types.h>

#include "i915_scheduler.h"
#include "intel_context_types.h"

struct pid;

struct drm_i915_private;
struct drm_i915_file_private;
struct i915_hw_ppgtt;
struct i915_timeline;
struct intel_ring;

/**
 * struct i915_gem_context - client state
 *
 * The struct i915_gem_context represents the combined view of the driver and
 * logical hardware state for a particular client.
 */
struct i915_gem_context {
	/** i915: i915 device backpointer */
	struct drm_i915_private *i915;

	/** file_priv: owning file descriptor */
	struct drm_i915_file_private *file_priv;

	/**
	 * @ppgtt: unique address space (GTT)
	 *
	 * In full-ppgtt mode, each context has its own address space ensuring
	 * complete seperation of one client from all others.
	 *
	 * In other modes, this is a NULL pointer with the expectation that
	 * the caller uses the shared global GTT.
	 */
	struct i915_hw_ppgtt *ppgtt;

	/**
	 * @pid: process id of creator
	 *
	 * Note that who created the context may not be the principle user,
	 * as the context may be shared across a local socket. However,
	 * that should only affect the default context, all contexts created
	 * explicitly by the client are expected to be isolated.
	 */
#ifdef __FreeBSD__
	pid_t pid;
#else
	struct pid *pid;
#endif

	/**
	 * @name: arbitrary name
	 *
	 * A name is constructed for the context from the creator's process
	 * name, pid and user handle in order to uniquely identify the
	 * context in messages.
	 */
	const char *name;

	/** link: place with &drm_i915_private.context_list */
	struct list_head link;
	struct llist_node free_link;

	/**
	 * @ref: reference count
	 *
	 * A reference to a context is held by both the client who created it
	 * and on each request submitted to the hardware using the request
	 * (to ensure the hardware has access to the state until it has
	 * finished all pending writes). See i915_gem_context_get() and
	 * i915_gem_context_put() for access.
	 */
	struct kref ref;

	/**
	 * @rcu: rcu_head for deferred freeing.
	 */
	struct rcu_head rcu;

	/**
	 * @user_flags: small set of booleans controlled by the user
	 */
	unsigned long user_flags;
#define UCONTEXT_NO_ZEROMAP		0
#define UCONTEXT_NO_ERROR_CAPTURE	1
#define UCONTEXT_BANNABLE		2
#define UCONTEXT_RECOVERABLE		3

	/**
	 * @flags: small set of booleans
	 */
	unsigned long flags;
#define CONTEXT_BANNED			0
#define CONTEXT_CLOSED			1
#define CONTEXT_FORCE_SINGLE_SUBMISSION	2

	/**
	 * @hw_id: - unique identifier for the context
	 *
	 * The hardware needs to uniquely identify the context for a few
	 * functions like fault reporting, PASID, scheduling. The
	 * &drm_i915_private.context_hw_ida is used to assign a unqiue
	 * id for the lifetime of the context.
	 *
	 * @hw_id_pin_count: - number of times this context had been pinned
	 * for use (should be, at most, once per engine).
	 *
	 * @hw_id_link: - all contexts with an assigned id are tracked
	 * for possible repossession.
	 */
	unsigned int hw_id;
	atomic_t hw_id_pin_count;
	struct list_head hw_id_link;

	struct list_head active_engines;
	struct mutex mutex;

	/**
	 * @user_handle: userspace identifier
	 *
	 * A unique per-file identifier is generated from
	 * &drm_i915_file_private.contexts.
	 */
	u32 user_handle;
#define DEFAULT_CONTEXT_HANDLE 0

	struct i915_sched_attr sched;

	/** hw_contexts: per-engine logical HW state */
	struct rb_root hw_contexts;
	spinlock_t hw_contexts_lock;

	/** ring_size: size for allocating the per-engine ring buffer */
	u32 ring_size;
	/** desc_template: invariant fields for the HW context descriptor */
	u32 desc_template;

	/** guilty_count: How many times this context has caused a GPU hang. */
	atomic_t guilty_count;
	/**
	 * @active_count: How many times this context was active during a GPU
	 * hang, but did not cause it.
	 */
	atomic_t active_count;

	/**
	 * @hang_timestamp: The last time(s) this context caused a GPU hang
	 */
	unsigned long hang_timestamp[2];
#define CONTEXT_FAST_HANG_JIFFIES (120 * HZ) /* 3 hangs within 120s? Banned! */

	/** remap_slice: Bitmask of cache lines that need remapping */
	u8 remap_slice;

	/** handles_vma: rbtree to look up our context specific obj/vma for
	 * the user handle. (user handles are per fd, but the binding is
	 * per vm, which may be one per context or shared with the global GTT)
	 */
	struct radix_tree_root handles_vma;

	/** handles_list: reverse list of all the rbtree entries in use for
	 * this context, which allows us to free all the allocations on
	 * context close.
	 */
	struct list_head handles_list;
};

#endif /* __I915_GEM_CONTEXT_TYPES_H__ */
