// SPDX-License-Identifier: MIT
/*
 * Copyright © 2014 Intel Corporation
 */

#include <linux/circ_buf.h>

#include "gem/i915_gem_context.h"
#include "gt/gen8_engine_cs.h"
#include "gt/intel_breadcrumbs.h"
#include "gt/intel_context.h"
#include "gt/intel_engine_pm.h"
#include "gt/intel_engine_heartbeat.h"
#include "gt/intel_gpu_commands.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_irq.h"
#include "gt/intel_gt_pm.h"
#include "gt/intel_gt_requests.h"
#include "gt/intel_lrc.h"
#include "gt/intel_lrc_reg.h"
#include "gt/intel_mocs.h"
#include "gt/intel_ring.h"

#include "intel_guc_submission.h"

#include "i915_drv.h"
#include "i915_trace.h"

/**
 * DOC: GuC-based command submission
 *
 * The Scratch registers:
 * There are 16 MMIO-based registers start from 0xC180. The kernel driver writes
 * a value to the action register (SOFT_SCRATCH_0) along with any data. It then
 * triggers an interrupt on the GuC via another register write (0xC4C8).
 * Firmware writes a success/fail code back to the action register after
 * processes the request. The kernel driver polls waiting for this update and
 * then proceeds.
 *
 * Command Transport buffers (CTBs):
 * Covered in detail in other sections but CTBs (host-to-guc, H2G, guc-to-host
 * G2H) are a message interface between the i915 and GuC used to controls
 * submissions.
 *
 * Context registration:
 * Before a context can be submitted it must be registered with the GuC via a
 * H2G. A unique guc_id is associated with each context. The context is either
 * registered at request creation time (normal operation) or at submission time
 * (abnormal operation, e.g. after a reset).
 *
 * Context submission:
 * The i915 updates the LRC tail value in memory. Either a schedule enable H2G
 * or context submit H2G is used to submit a context.
 *
 * Context unpin:
 * To unpin a context a H2G is used to disable scheduling and when the
 * corresponding G2H returns indicating the scheduling disable operation has
 * completed it is safe to unpin the context. While a disable is in flight it
 * isn't safe to resubmit the context so a fence is used to stall all future
 * requests until the G2H is returned.
 *
 * Context deregistration:
 * Before a context can be destroyed or we steal its guc_id we must deregister
 * the context with the GuC via H2G. If stealing the guc_id it isn't safe to
 * submit anything to this guc_id until the deregister completes so a fence is
 * used to stall all requests associated with this guc_ids until the
 * corresponding G2H returns indicating the guc_id has been deregistered.
 *
 * guc_ids:
 * Unique number associated with private GuC context data passed in during
 * context registration / submission / deregistration. 64k available. Simple ida
 * is used for allocation.
 *
 * Stealing guc_ids:
 * If no guc_ids are available they can be stolen from another context at
 * request creation time if that context is unpinned. If a guc_id can't be found
 * we punt this problem to the user as we believe this is near impossible to hit
 * during normal use cases.
 *
 * Locking:
 * In the GuC submission code we have 3 basic spin locks which protect
 * everything. Details about each below.
 *
 * sched_engine->lock
 * This is the submission lock for all contexts that share a i915 schedule
 * engine (sched_engine), thus only 1 context which share a sched_engine can be
 * submitting at a time. Currently only 1 sched_engine used for all of GuC
 * submission but that could change in the future.
 *
 * guc->submission_state.lock
 * Global lock for GuC submission state. Protects guc_ids and destroyed contexts
 * list.
 *
 * ce->guc_state.lock
 * Protects everything under ce->guc_state. Ensures that a context is in the
 * correct state before issuing a H2G. e.g. We don't issue a schedule disable
 * on disabled context (bad idea), we don't issue schedule enable when a
 * schedule disable is inflight, etc... Also protects list of inflight requests
 * on the context and the priority management state. Lock individual to each
 * context.
 *
 * Lock ordering rules:
 * sched_engine->lock -> ce->guc_state.lock
 * guc->submission_state.lock -> ce->guc_state.lock
 *
 * Reset races:
 * When a GPU full reset is triggered it is assumed that some G2H responses to
 * a H2G can be lost as the GuC is likely toast. Losing these G2H can prove to
 * fatal as we do certain operations upon receiving a G2H (e.g. destroy
 * contexts, release guc_ids, etc...). Luckly when this occurs we can scrub
 * context state and cleanup appropriately, however this is quite racey. To
 * avoid races the rules are check for submission being disabled (i.e. check for
 * mid reset) with the appropriate lock being held. If submission is disabled
 * don't send the H2G or update the context state. The reset code must disable
 * submission and grab all these locks before scrubbing for the missing G2H.
 */

/* GuC Virtual Engine */
struct guc_virtual_engine {
	struct intel_engine_cs base;
	struct intel_context context;
};

static struct intel_context *
guc_create_virtual(struct intel_engine_cs **siblings, unsigned int count,
		   unsigned long flags);

static struct intel_context *
guc_create_parallel(struct intel_engine_cs **engines,
		    unsigned int num_siblings,
		    unsigned int width);

#define GUC_REQUEST_SIZE 64 /* bytes */

/*
 * We reserve 1/16 of the guc_ids for multi-lrc as these need to be contiguous
 * per the GuC submission interface. A different allocation algorithm is used
 * (bitmap vs. ida) between multi-lrc and single-lrc hence the reason to
 * partition the guc_id space. We believe the number of multi-lrc contexts in
 * use should be low and 1/16 should be sufficient. Minimum of 32 guc_ids for
 * multi-lrc.
 */
#define NUMBER_MULTI_LRC_GUC_ID(guc) \
	((guc)->submission_state.num_guc_ids / 16 > 32 ? \
	 (guc)->submission_state.num_guc_ids / 16 : 32)

/*
 * Below is a set of functions which control the GuC scheduling state which
 * require a lock.
 */
#define SCHED_STATE_WAIT_FOR_DEREGISTER_TO_REGISTER	BIT(0)
#define SCHED_STATE_DESTROYED				BIT(1)
#define SCHED_STATE_PENDING_DISABLE			BIT(2)
#define SCHED_STATE_BANNED				BIT(3)
#define SCHED_STATE_ENABLED				BIT(4)
#define SCHED_STATE_PENDING_ENABLE			BIT(5)
#define SCHED_STATE_REGISTERED				BIT(6)
#define SCHED_STATE_BLOCKED_SHIFT			7
#define SCHED_STATE_BLOCKED		BIT(SCHED_STATE_BLOCKED_SHIFT)
#define SCHED_STATE_BLOCKED_MASK	(0xfff << SCHED_STATE_BLOCKED_SHIFT)
static void init_sched_state(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state &= SCHED_STATE_BLOCKED_MASK;
}

__maybe_unused
static bool sched_state_is_init(struct intel_context *ce)
{
	/*
	 * XXX: Kernel contexts can have SCHED_STATE_NO_LOCK_REGISTERED after
	 * suspend.
	 */
	return !(ce->guc_state.sched_state &=
		 ~(SCHED_STATE_BLOCKED_MASK | SCHED_STATE_REGISTERED));
}

static bool
context_wait_for_deregister_to_register(struct intel_context *ce)
{
	return ce->guc_state.sched_state &
		SCHED_STATE_WAIT_FOR_DEREGISTER_TO_REGISTER;
}

static void
set_context_wait_for_deregister_to_register(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state |=
		SCHED_STATE_WAIT_FOR_DEREGISTER_TO_REGISTER;
}

static void
clr_context_wait_for_deregister_to_register(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state &=
		~SCHED_STATE_WAIT_FOR_DEREGISTER_TO_REGISTER;
}

static bool
context_destroyed(struct intel_context *ce)
{
	return ce->guc_state.sched_state & SCHED_STATE_DESTROYED;
}

static void
set_context_destroyed(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state |= SCHED_STATE_DESTROYED;
}

static bool context_pending_disable(struct intel_context *ce)
{
	return ce->guc_state.sched_state & SCHED_STATE_PENDING_DISABLE;
}

static void set_context_pending_disable(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state |= SCHED_STATE_PENDING_DISABLE;
}

static void clr_context_pending_disable(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state &= ~SCHED_STATE_PENDING_DISABLE;
}

static bool context_banned(struct intel_context *ce)
{
	return ce->guc_state.sched_state & SCHED_STATE_BANNED;
}

static void set_context_banned(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state |= SCHED_STATE_BANNED;
}

static void clr_context_banned(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state &= ~SCHED_STATE_BANNED;
}

static bool context_enabled(struct intel_context *ce)
{
	return ce->guc_state.sched_state & SCHED_STATE_ENABLED;
}

static void set_context_enabled(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state |= SCHED_STATE_ENABLED;
}

static void clr_context_enabled(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state &= ~SCHED_STATE_ENABLED;
}

static bool context_pending_enable(struct intel_context *ce)
{
	return ce->guc_state.sched_state & SCHED_STATE_PENDING_ENABLE;
}

static void set_context_pending_enable(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state |= SCHED_STATE_PENDING_ENABLE;
}

static void clr_context_pending_enable(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state &= ~SCHED_STATE_PENDING_ENABLE;
}

static bool context_registered(struct intel_context *ce)
{
	return ce->guc_state.sched_state & SCHED_STATE_REGISTERED;
}

static void set_context_registered(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state |= SCHED_STATE_REGISTERED;
}

static void clr_context_registered(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	ce->guc_state.sched_state &= ~SCHED_STATE_REGISTERED;
}

static u32 context_blocked(struct intel_context *ce)
{
	return (ce->guc_state.sched_state & SCHED_STATE_BLOCKED_MASK) >>
		SCHED_STATE_BLOCKED_SHIFT;
}

static void incr_context_blocked(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);

	ce->guc_state.sched_state += SCHED_STATE_BLOCKED;

	GEM_BUG_ON(!context_blocked(ce));	/* Overflow check */
}

static void decr_context_blocked(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);

	GEM_BUG_ON(!context_blocked(ce));	/* Underflow check */

	ce->guc_state.sched_state -= SCHED_STATE_BLOCKED;
}

static bool context_has_committed_requests(struct intel_context *ce)
{
	return !!ce->guc_state.number_committed_requests;
}

static void incr_context_committed_requests(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	++ce->guc_state.number_committed_requests;
	GEM_BUG_ON(ce->guc_state.number_committed_requests < 0);
}

static void decr_context_committed_requests(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	--ce->guc_state.number_committed_requests;
	GEM_BUG_ON(ce->guc_state.number_committed_requests < 0);
}

static struct intel_context *
request_to_scheduling_context(struct i915_request *rq)
{
	return intel_context_to_parent(rq->context);
}

static bool context_guc_id_invalid(struct intel_context *ce)
{
	return ce->guc_id.id == GUC_INVALID_LRC_ID;
}

static void set_context_guc_id_invalid(struct intel_context *ce)
{
	ce->guc_id.id = GUC_INVALID_LRC_ID;
}

static struct intel_guc *ce_to_guc(struct intel_context *ce)
{
	return &ce->engine->gt->uc.guc;
}

static struct i915_priolist *to_priolist(struct rb_node *rb)
{
	return rb_entry(rb, struct i915_priolist, node);
}

/*
 * When using multi-lrc submission an extra page in the context state is
 * reserved for the process descriptor, work queue, and preempt BB boundary
 * handshake between the parent + childlren contexts.
 *
 * The layout of this page is below:
 * 0						guc_process_desc
 * + sizeof(struct guc_process_desc)		child go
 * + CACHELINE_BYTES				child join ...
 * + CACHELINE_BYTES ...
 * ...						unused
 * PAGE_SIZE / 2				work queue start
 * ...						work queue
 * PAGE_SIZE - 1				work queue end
 */
#define WQ_OFFSET	(PAGE_SIZE / 2)
static u32 __get_process_desc_offset(struct intel_context *ce)
{
	GEM_BUG_ON(!ce->parent_page);

	return ce->parent_page * PAGE_SIZE;
}

static u32 __get_wq_offset(struct intel_context *ce)
{
	return __get_process_desc_offset(ce) + WQ_OFFSET;
}

static struct guc_process_desc *
__get_process_desc(struct intel_context *ce)
{
	return (struct guc_process_desc *)
		(ce->lrc_reg_state +
		 ((__get_process_desc_offset(ce) -
		   LRC_STATE_OFFSET) / sizeof(u32)));
}

static u32 *get_wq_pointer(struct guc_process_desc *desc,
			   struct intel_context *ce,
			   u32 wqi_size)
{
	/*
	 * Check for space in work queue. Caching a value of head pointer in
	 * intel_context structure in order reduce the number accesses to shared
	 * GPU memory which may be across a PCIe bus.
	 */
#define AVAILABLE_SPACE	\
	CIRC_SPACE(ce->guc_wqi_tail, ce->guc_wqi_head, GUC_WQ_SIZE)
	if (wqi_size > AVAILABLE_SPACE) {
		ce->guc_wqi_head = READ_ONCE(desc->head);

		if (wqi_size > AVAILABLE_SPACE)
			return NULL;
	}
#undef AVAILABLE_SPACE

	return ((u32 *)__get_process_desc(ce)) +
		((WQ_OFFSET + ce->guc_wqi_tail) / sizeof(u32));
}

static struct guc_lrc_desc *__get_lrc_desc(struct intel_guc *guc, u32 index)
{
	struct guc_lrc_desc *base = guc->lrc_desc_pool_vaddr;

	GEM_BUG_ON(index >= guc->submission_state.max_guc_ids);

	return &base[index];
}

static struct intel_context *__get_context(struct intel_guc *guc, u32 id)
{
	struct intel_context *ce = xa_load(&guc->context_lookup, id);

	GEM_BUG_ON(id >= guc->submission_state.max_guc_ids);

	return ce;
}

static int guc_lrc_desc_pool_create(struct intel_guc *guc)
{
	u32 size;
	int ret;

	size = PAGE_ALIGN(sizeof(struct guc_lrc_desc) *
			  guc->submission_state.max_guc_ids);
	ret = intel_guc_allocate_and_map_vma(guc, size, &guc->lrc_desc_pool,
					     (void **)&guc->lrc_desc_pool_vaddr);
	if (ret)
		return ret;

	return 0;
}

static void guc_lrc_desc_pool_destroy(struct intel_guc *guc)
{
	guc->lrc_desc_pool_vaddr = NULL;
	i915_vma_unpin_and_release(&guc->lrc_desc_pool, I915_VMA_RELEASE_MAP);
}

static bool guc_submission_initialized(struct intel_guc *guc)
{
	return !!guc->lrc_desc_pool_vaddr;
}

static void reset_lrc_desc(struct intel_guc *guc, u32 id)
{
	if (likely(guc_submission_initialized(guc))) {
		struct guc_lrc_desc *desc = __get_lrc_desc(guc, id);
		unsigned long flags;

		memset(desc, 0, sizeof(*desc));

		/*
		 * xarray API doesn't have xa_erase_irqsave wrapper, so calling
		 * the lower level functions directly.
		 */
		xa_lock_irqsave(&guc->context_lookup, flags);
		__xa_erase(&guc->context_lookup, id);
		xa_unlock_irqrestore(&guc->context_lookup, flags);
	}
}

static bool lrc_desc_registered(struct intel_guc *guc, u32 id)
{
	return __get_context(guc, id);
}

static void set_lrc_desc_registered(struct intel_guc *guc, u32 id,
				    struct intel_context *ce)
{
	unsigned long flags;

	/*
	 * xarray API doesn't have xa_save_irqsave wrapper, so calling the
	 * lower level functions directly.
	 */
	xa_lock_irqsave(&guc->context_lookup, flags);
	__xa_store(&guc->context_lookup, id, ce, GFP_ATOMIC);
	xa_unlock_irqrestore(&guc->context_lookup, flags);
}

static void decr_outstanding_submission_g2h(struct intel_guc *guc)
{
	if (atomic_dec_and_test(&guc->outstanding_submission_g2h))
		wake_up_all(&guc->ct.wq);
}

static int guc_submission_send_busy_loop(struct intel_guc *guc,
					 const u32 *action,
					 u32 len,
					 u32 g2h_len_dw,
					 bool loop)
{
	int err;

	if (g2h_len_dw)
		atomic_inc(&guc->outstanding_submission_g2h);

	err = intel_guc_send_busy_loop(guc, action, len, g2h_len_dw, loop);
	if (err == -EBUSY && g2h_len_dw)
		decr_outstanding_submission_g2h(guc);

	return err;
}

int intel_guc_wait_for_pending_msg(struct intel_guc *guc,
				   atomic_t *wait_var,
				   bool interruptible,
				   long timeout)
{
	const int state = interruptible ?
		TASK_INTERRUPTIBLE : TASK_UNINTERRUPTIBLE;
	DEFINE_WAIT(wait);

	might_sleep();
	GEM_BUG_ON(timeout < 0);

	if (!atomic_read(wait_var))
		return 0;

	if (!timeout)
		return -ETIME;

	for (;;) {
		prepare_to_wait(&guc->ct.wq, &wait, state);

		if (!atomic_read(wait_var))
			break;

		if (signal_pending_state(state, current)) {
			timeout = -EINTR;
			break;
		}

		if (!timeout) {
			timeout = -ETIME;
			break;
		}

		timeout = io_schedule_timeout(timeout);
	}
	finish_wait(&guc->ct.wq, &wait);

	return (timeout < 0) ? timeout : 0;
}

int intel_guc_wait_for_idle(struct intel_guc *guc, long timeout)
{
	if (!intel_uc_uses_guc_submission(&guc_to_gt(guc)->uc))
		return 0;

	return intel_guc_wait_for_pending_msg(guc,
					      &guc->outstanding_submission_g2h,
					      true, timeout);
}

static int guc_lrc_desc_pin(struct intel_context *ce, bool loop);

static int __guc_add_request(struct intel_guc *guc, struct i915_request *rq)
{
	int err = 0;
	struct intel_context *ce = request_to_scheduling_context(rq);
	u32 action[3];
	int len = 0;
	u32 g2h_len_dw = 0;
	bool enabled;

	lockdep_assert_held(&rq->engine->sched_engine->lock);

	/*
	 * Corner case where requests were sitting in the priority list or a
	 * request resubmitted after the context was banned.
	 */
	if (unlikely(intel_context_is_banned(ce))) {
		i915_request_put(i915_request_mark_eio(rq));
		intel_engine_signal_breadcrumbs(ce->engine);
		return 0;
	}

	GEM_BUG_ON(!atomic_read(&ce->guc_id.ref));
	GEM_BUG_ON(context_guc_id_invalid(ce));

	spin_lock(&ce->guc_state.lock);

	/*
	 * The request / context will be run on the hardware when scheduling
	 * gets enabled in the unblock. For multi-lrc we still submit the
	 * context to move the LRC tails.
	 */
	if (unlikely(context_blocked(ce) && !intel_context_is_parent(ce)))
		goto out;

	enabled = context_enabled(ce) || context_blocked(ce);

	if (!enabled) {
		action[len++] = INTEL_GUC_ACTION_SCHED_CONTEXT_MODE_SET;
		action[len++] = ce->guc_id.id;
		action[len++] = GUC_CONTEXT_ENABLE;
		set_context_pending_enable(ce);
		intel_context_get(ce);
		g2h_len_dw = G2H_LEN_DW_SCHED_CONTEXT_MODE_SET;
	} else {
		action[len++] = INTEL_GUC_ACTION_SCHED_CONTEXT;
		action[len++] = ce->guc_id.id;
	}

	err = intel_guc_send_nb(guc, action, len, g2h_len_dw);
	if (!enabled && !err) {
		trace_intel_context_sched_enable(ce);
		atomic_inc(&guc->outstanding_submission_g2h);
		set_context_enabled(ce);

		/*
		 * Without multi-lrc KMD does the submission step (moving the
		 * lrc tail) so enabling scheduling is sufficient to submit the
		 * context. This isn't the case in multi-lrc submission as the
		 * GuC needs to move the tails, hence the need for another H2G
		 * to submit a multi-lrc context after enabling scheduling.
		 */
		if (intel_context_is_parent(ce)) {
			action[0] = INTEL_GUC_ACTION_SCHED_CONTEXT;
			err = intel_guc_send_nb(guc, action, len - 1, 0);
		}
	} else if (!enabled) {
		clr_context_pending_enable(ce);
		intel_context_put(ce);
	}
	if (likely(!err))
		trace_i915_request_guc_submit(rq);

out:
	spin_unlock(&ce->guc_state.lock);
	return err;
}

static int guc_add_request(struct intel_guc *guc, struct i915_request *rq)
{
	int ret = __guc_add_request(guc, rq);

	if (unlikely(ret == -EBUSY)) {
		guc->stalled_request = rq;
		guc->submission_stall_reason = STALL_ADD_REQUEST;
	}

	return ret;
}

static void guc_set_lrc_tail(struct i915_request *rq)
{
	rq->context->lrc_reg_state[CTX_RING_TAIL] =
		intel_ring_set_tail(rq->ring, rq->tail);
}

static int rq_prio(const struct i915_request *rq)
{
	return rq->sched.attr.priority;
}

static inline bool is_multi_lrc(struct intel_context *ce)
{
	return intel_context_is_parallel(ce);
}

static bool is_multi_lrc_rq(struct i915_request *rq)
{
	return intel_context_is_parallel(rq->context);
}

static bool can_merge_rq(struct i915_request *rq,
			 struct i915_request *last)
{
	return request_to_scheduling_context(rq) ==
		request_to_scheduling_context(last);
}

static u32 wq_space_until_wrap(struct intel_context *ce)
{
	return (GUC_WQ_SIZE - ce->guc_wqi_tail);
}

static void write_wqi(struct guc_process_desc *desc,
		      struct intel_context *ce,
		      u32 wqi_size)
{
	/*
	 * Ensure WQE are visible before updating tail
	 */
	intel_guc_write_barrier(ce_to_guc(ce));

	ce->guc_wqi_tail = (ce->guc_wqi_tail + wqi_size) & (GUC_WQ_SIZE - 1);
	WRITE_ONCE(desc->tail, ce->guc_wqi_tail);
}

static int guc_wq_noop_append(struct intel_context *ce)
{
	struct guc_process_desc *desc = __get_process_desc(ce);
	u32 *wqi = get_wq_pointer(desc, ce, wq_space_until_wrap(ce));

	if (!wqi)
		return -EBUSY;

	*wqi = WQ_TYPE_NOOP |
		((wq_space_until_wrap(ce) / sizeof(u32) - 1) << WQ_LEN_SHIFT);
	ce->guc_wqi_tail = 0;

	return 0;
}

static int __guc_wq_item_append(struct i915_request *rq)
{
	struct intel_context *ce = request_to_scheduling_context(rq);
	struct intel_context *child;
	struct guc_process_desc *desc = __get_process_desc(ce);
	unsigned int wqi_size = (ce->guc_number_children + 4) *
		sizeof(u32);
	u32 *wqi;
	int ret;

	/* Ensure context is in correct state updating work queue */
	GEM_BUG_ON(!atomic_read(&ce->guc_id.ref));
	GEM_BUG_ON(context_guc_id_invalid(ce));
	GEM_BUG_ON(context_wait_for_deregister_to_register(ce));
	GEM_BUG_ON(!lrc_desc_registered(ce_to_guc(ce), ce->guc_id.id));

	/* Insert NOOP if this work queue item will wrap the tail pointer. */
	if (wqi_size > wq_space_until_wrap(ce)) {
		ret = guc_wq_noop_append(ce);
		if (ret)
			return ret;
	}

	wqi = get_wq_pointer(desc, ce, wqi_size);
	if (!wqi)
		return -EBUSY;

	*wqi++ = WQ_TYPE_MULTI_LRC |
		((wqi_size / sizeof(u32) - 1) << WQ_LEN_SHIFT);
	*wqi++ = ce->lrc.lrca;
	*wqi++ = (ce->guc_id.id << WQ_GUC_ID_SHIFT) |
		 ((ce->ring->tail / sizeof(u64)) << WQ_RING_TAIL_SHIFT);
	*wqi++ = 0;	/* fence_id */
	for_each_child(ce, child)
		*wqi++ = child->ring->tail / sizeof(u64);

	write_wqi(desc, ce, wqi_size);

	return 0;
}

static int guc_wq_item_append(struct intel_guc *guc,
			      struct i915_request *rq)
{
	struct intel_context *ce = request_to_scheduling_context(rq);
	int ret = 0;

	if (likely(!intel_context_is_banned(ce))) {
		ret = __guc_wq_item_append(rq);

		if (unlikely(ret == -EBUSY)) {
			guc->stalled_request = rq;
			guc->submission_stall_reason = STALL_MOVE_LRC_TAIL;
		}
	}

	return ret;
}

static bool multi_lrc_submit(struct i915_request *rq)
{
	struct intel_context *ce = request_to_scheduling_context(rq);

	intel_ring_set_tail(rq->ring, rq->tail);

	/*
	 * We expect the front end (execbuf IOCTL) to set this flag on the last
	 * request generated from a multi-BB submission. This indicates to the
	 * backend (GuC interface) that we should submit this context thus
	 * submitting all the requests generated in parallel.
	 */
	return test_bit(I915_FENCE_FLAG_SUBMIT_PARALLEL, &rq->fence.flags) ||
		intel_context_is_banned(ce);
}

static int guc_dequeue_one_context(struct intel_guc *guc)
{
	struct i915_sched_engine * const sched_engine = guc->sched_engine;
	struct i915_request *last = NULL;
	bool submit = false;
	struct rb_node *rb;
	int ret;

	lockdep_assert_held(&sched_engine->lock);

	if (guc->stalled_request) {
		submit = true;
		last = guc->stalled_request;

		switch (guc->submission_stall_reason) {
		case STALL_REGISTER_CONTEXT:
			goto register_context;
		case STALL_MOVE_LRC_TAIL:
			goto move_lrc_tail;
		case STALL_ADD_REQUEST:
			goto add_request;
		default:
			MISSING_CASE(guc->submission_stall_reason);
		}
	}

	while ((rb = rb_first_cached(&sched_engine->queue))) {
		struct i915_priolist *p = to_priolist(rb);
		struct i915_request *rq, *rn;

		priolist_for_each_request_consume(rq, rn, p) {
			if (last && !can_merge_rq(rq, last))
				goto register_context;

			list_del_init(&rq->sched.link);

			__i915_request_submit(rq);

			trace_i915_request_in(rq, 0);
			last = rq;

			if (is_multi_lrc_rq(rq)) {
				/*
				 * We need to coalesce all multi-lrc requests in
				 * a relationship into a single H2G. We are
				 * guaranteed that all of these requests will be
				 * submitted sequentially.
				 */
				if (multi_lrc_submit(rq)) {
					submit = true;
					goto register_context;
				}
			} else {
				submit = true;
			}
		}

		rb_erase_cached(&p->node, &sched_engine->queue);
		i915_priolist_free(p);
	}

register_context:
	if (submit) {
		struct intel_context *ce = request_to_scheduling_context(last);

		if (unlikely(!lrc_desc_registered(guc, ce->guc_id.id) &&
			     !intel_context_is_banned(ce))) {
			ret = guc_lrc_desc_pin(ce, false);
			if (unlikely(ret == -EPIPE)) {
				goto deadlk;
			} else if (ret == -EBUSY) {
				guc->stalled_request = last;
				guc->submission_stall_reason =
					STALL_REGISTER_CONTEXT;
				goto schedule_tasklet;
			} else if (ret != 0) {
				GEM_WARN_ON(ret);	/* Unexpected */
				goto deadlk;
			}
		}

move_lrc_tail:
		if (is_multi_lrc_rq(last)) {
			ret = guc_wq_item_append(guc, last);
			if (ret == -EBUSY) {
				goto schedule_tasklet;
			} else if (ret != 0) {
				GEM_WARN_ON(ret);	/* Unexpected */
				goto deadlk;
			}
		} else {
			guc_set_lrc_tail(last);
		}

add_request:
		ret = guc_add_request(guc, last);
		if (unlikely(ret == -EPIPE)) {
			goto deadlk;
		} else if (ret == -EBUSY) {
			goto schedule_tasklet;
		} else if (ret != 0) {
			GEM_WARN_ON(ret);	/* Unexpected */
			goto deadlk;
		}
	}

	guc->stalled_request = NULL;
	guc->submission_stall_reason = STALL_NONE;
	return submit;

deadlk:
	sched_engine->tasklet.callback = NULL;
	tasklet_disable_nosync(&sched_engine->tasklet);
	return false;

schedule_tasklet:
	tasklet_schedule(&sched_engine->tasklet);
	return false;
}

static void guc_submission_tasklet(struct tasklet_struct *t)
{
	struct i915_sched_engine *sched_engine =
		from_tasklet(sched_engine, t, tasklet);
	unsigned long flags;
	bool loop;

	spin_lock_irqsave(&sched_engine->lock, flags);

	do {
		loop = guc_dequeue_one_context(sched_engine->private_data);
	} while (loop);

	i915_sched_engine_reset_on_empty(sched_engine);

	spin_unlock_irqrestore(&sched_engine->lock, flags);
}

static void cs_irq_handler(struct intel_engine_cs *engine, u16 iir)
{
	if (iir & GT_RENDER_USER_INTERRUPT)
		intel_engine_signal_breadcrumbs(engine);
}

static void __guc_context_destroy(struct intel_context *ce);
static void release_guc_id(struct intel_guc *guc, struct intel_context *ce);
static void guc_signal_context_fence(struct intel_context *ce);
static void guc_cancel_context_requests(struct intel_context *ce);
static void guc_blocked_fence_complete(struct intel_context *ce);

static void scrub_guc_desc_for_outstanding_g2h(struct intel_guc *guc)
{
	struct intel_context *ce;
	unsigned long index, flags;
	bool pending_disable, pending_enable, deregister, destroyed, banned;

	xa_lock_irqsave(&guc->context_lookup, flags);
	xa_for_each(&guc->context_lookup, index, ce) {
		/*
		 * Corner case where the ref count on the object is zero but and
		 * deregister G2H was lost. In this case we don't touch the ref
		 * count and finish the destroy of the context.
		 */
		bool do_put = kref_get_unless_zero(&ce->ref);

		xa_unlock(&guc->context_lookup);

		spin_lock(&ce->guc_state.lock);

		/*
		 * Once we are at this point submission_disabled() is guaranteed
		 * to be visible to all callers who set the below flags (see above
		 * flush and flushes in reset_prepare). If submission_disabled()
		 * is set, the caller shouldn't set these flags.
		 */

		destroyed = context_destroyed(ce);
		pending_enable = context_pending_enable(ce);
		pending_disable = context_pending_disable(ce);
		deregister = context_wait_for_deregister_to_register(ce);
		banned = context_banned(ce);
		init_sched_state(ce);

		spin_unlock(&ce->guc_state.lock);

		GEM_BUG_ON(!do_put && !destroyed);

		if (pending_enable || destroyed || deregister) {
			decr_outstanding_submission_g2h(guc);
			if (deregister)
				guc_signal_context_fence(ce);
			if (destroyed) {
				intel_gt_pm_put_async(guc_to_gt(guc));
				release_guc_id(guc, ce);
				__guc_context_destroy(ce);
			}
			if (pending_enable || deregister)
				intel_context_put(ce);
		}

		/* Not mutualy exclusive with above if statement. */
		if (pending_disable) {
			guc_signal_context_fence(ce);
			if (banned) {
				guc_cancel_context_requests(ce);
				intel_engine_signal_breadcrumbs(ce->engine);
			}
			intel_context_sched_disable_unpin(ce);
			decr_outstanding_submission_g2h(guc);

			spin_lock(&ce->guc_state.lock);
			guc_blocked_fence_complete(ce);
			spin_unlock(&ce->guc_state.lock);

			intel_context_put(ce);
		}

		if (do_put)
			intel_context_put(ce);
		xa_lock(&guc->context_lookup);
	}
	xa_unlock_irqrestore(&guc->context_lookup, flags);
}

static bool
submission_disabled(struct intel_guc *guc)
{
	struct i915_sched_engine * const sched_engine = guc->sched_engine;

	return unlikely(!sched_engine ||
			!__tasklet_is_enabled(&sched_engine->tasklet));
}

static void disable_submission(struct intel_guc *guc)
{
	struct i915_sched_engine * const sched_engine = guc->sched_engine;

	if (__tasklet_is_enabled(&sched_engine->tasklet)) {
		GEM_BUG_ON(!guc->ct.enabled);
		__tasklet_disable_sync_once(&sched_engine->tasklet);
		sched_engine->tasklet.callback = NULL;
	}
}

static void enable_submission(struct intel_guc *guc)
{
	struct i915_sched_engine * const sched_engine = guc->sched_engine;
	unsigned long flags;

	spin_lock_irqsave(&guc->sched_engine->lock, flags);
	sched_engine->tasklet.callback = guc_submission_tasklet;
	wmb();	/* Make sure callback visible */
	if (!__tasklet_is_enabled(&sched_engine->tasklet) &&
	    __tasklet_enable(&sched_engine->tasklet)) {
		GEM_BUG_ON(!guc->ct.enabled);

		/* And kick in case we missed a new request submission. */
		tasklet_hi_schedule(&sched_engine->tasklet);
	}
	spin_unlock_irqrestore(&guc->sched_engine->lock, flags);
}

static void guc_flush_submissions(struct intel_guc *guc)
{
	struct i915_sched_engine * const sched_engine = guc->sched_engine;
	unsigned long flags;

	spin_lock_irqsave(&sched_engine->lock, flags);
	spin_unlock_irqrestore(&sched_engine->lock, flags);
}

static void guc_flush_destroyed_contexts(struct intel_guc *guc);

void intel_guc_submission_reset_prepare(struct intel_guc *guc)
{
	if (unlikely(!guc_submission_initialized(guc))) {
		/* Reset called during driver load? GuC not yet initialised! */
		return;
	}

	intel_gt_park_heartbeats(guc_to_gt(guc));
	disable_submission(guc);
	guc->interrupts.disable(guc);

	/* Flush IRQ handler */
	spin_lock_irq(&guc_to_gt(guc)->irq_lock);
	spin_unlock_irq(&guc_to_gt(guc)->irq_lock);

	flush_work(&guc->ct.requests.worker);
	guc_flush_destroyed_contexts(guc);

	scrub_guc_desc_for_outstanding_g2h(guc);
}

static struct intel_engine_cs *
guc_virtual_get_sibling(struct intel_engine_cs *ve, unsigned int sibling)
{
	struct intel_engine_cs *engine;
	intel_engine_mask_t tmp, mask = ve->mask;
	unsigned int num_siblings = 0;

	for_each_engine_masked(engine, ve->gt, mask, tmp)
		if (num_siblings++ == sibling)
			return engine;

	return NULL;
}

static struct intel_engine_cs *
__context_to_physical_engine(struct intel_context *ce)
{
	struct intel_engine_cs *engine = ce->engine;

	if (intel_engine_is_virtual(engine))
		engine = guc_virtual_get_sibling(engine, 0);

	return engine;
}

static void guc_reset_state(struct intel_context *ce, u32 head, bool scrub)
{
	struct intel_engine_cs *engine = __context_to_physical_engine(ce);

	if (intel_context_is_banned(ce))
		return;

	GEM_BUG_ON(!intel_context_is_pinned(ce));

	/*
	 * We want a simple context + ring to execute the breadcrumb update.
	 * We cannot rely on the context being intact across the GPU hang,
	 * so clear it and rebuild just what we need for the breadcrumb.
	 * All pending requests for this context will be zapped, and any
	 * future request will be after userspace has had the opportunity
	 * to recreate its own state.
	 */
	if (scrub)
		lrc_init_regs(ce, engine, true);

	/* Rerun the request; its payload has been neutered (if guilty). */
	lrc_update_regs(ce, engine, head);
}

static void guc_reset_nop(struct intel_engine_cs *engine)
{
}

static void guc_rewind_nop(struct intel_engine_cs *engine, bool stalled)
{
}

static void
__unwind_incomplete_requests(struct intel_context *ce)
{
	struct i915_request *rq, *rn;
	struct list_head *pl;
	int prio = I915_PRIORITY_INVALID;
	struct i915_sched_engine * const sched_engine =
		ce->engine->sched_engine;
	unsigned long flags;

	spin_lock_irqsave(&sched_engine->lock, flags);
	spin_lock(&ce->guc_state.lock);
	list_for_each_entry_safe_reverse(rq, rn,
					 &ce->guc_state.requests,
					 sched.link) {
		if (i915_request_completed(rq))
			continue;

		list_del_init(&rq->sched.link);
		__i915_request_unsubmit(rq);

		/* Push the request back into the queue for later resubmission. */
		GEM_BUG_ON(rq_prio(rq) == I915_PRIORITY_INVALID);
		if (rq_prio(rq) != prio) {
			prio = rq_prio(rq);
			pl = i915_sched_lookup_priolist(sched_engine, prio);
		}
		GEM_BUG_ON(i915_sched_engine_is_empty(sched_engine));

		list_add(&rq->sched.link, pl);
		set_bit(I915_FENCE_FLAG_PQUEUE, &rq->fence.flags);
	}
	spin_unlock(&ce->guc_state.lock);
	spin_unlock_irqrestore(&sched_engine->lock, flags);
}

static void __guc_reset_context(struct intel_context *ce, bool stalled)
{
	bool local_stalled;
	struct i915_request *rq;
	unsigned long flags;
	u32 head;
	int i, number_children = ce->guc_number_children;
	bool skip = false;
	struct intel_context *parent = ce;

	intel_context_get(ce);

	/*
	 * GuC will implicitly mark the context as non-schedulable when it sends
	 * the reset notification. Make sure our state reflects this change. The
	 * context will be marked enabled on resubmission.
	 *
	 * XXX: If the context is reset as a result of the request cancellation
	 * this G2H is received after the schedule disable complete G2H which is
	 * likely wrong as this creates a race between the request cancellation
	 * code re-submitting the context and this G2H handler. This likely
	 * should be fixed in the GuC but until if / when that gets fixed we
	 * need to workaround this. Convert this function to a NOP if a pending
	 * enable is in flight as this indicates that a request cancellation has
	 * occurred.
	 */
	spin_lock_irqsave(&ce->guc_state.lock, flags);
	if (likely(!context_pending_enable(ce))) {
		clr_context_enabled(ce);
	} else {
		skip = true;
	}
	spin_unlock_irqrestore(&ce->guc_state.lock, flags);
	if (unlikely(skip))
		goto out_put;

	for (i = 0; i < number_children + 1; ++i) {
		if (!intel_context_is_pinned(ce))
			goto next_context;

		local_stalled = false;
		rq = intel_context_find_active_request(ce);
		if (!rq) {
			head = ce->ring->tail;
			goto out_replay;
		}

		GEM_BUG_ON(i915_active_is_idle(&ce->active));
		head = intel_ring_wrap(ce->ring, rq->head);

		if (i915_request_started(rq))
			local_stalled = true;

		__i915_request_reset(rq, local_stalled && stalled);
out_replay:
		guc_reset_state(ce, head, local_stalled && stalled);
next_context:
		if (i != number_children)
			ce = list_next_entry(ce, guc_child_link);
	}

	__unwind_incomplete_requests(parent);
out_put:
	intel_context_put(parent);
}

void intel_guc_submission_reset(struct intel_guc *guc, bool stalled)
{
	struct intel_context *ce;
	unsigned long index;
	unsigned long flags;

	if (unlikely(!guc_submission_initialized(guc))) {
		/* Reset called during driver load? GuC not yet initialised! */
		return;
	}

	xa_lock_irqsave(&guc->context_lookup, flags);
	xa_for_each(&guc->context_lookup, index, ce) {
		if (!kref_get_unless_zero(&ce->ref))
			continue;

		xa_unlock(&guc->context_lookup);

		if (intel_context_is_pinned(ce) &&
		    !intel_context_is_child(ce))
			__guc_reset_context(ce, stalled);

		intel_context_put(ce);

		xa_lock(&guc->context_lookup);
	}
	xa_unlock_irqrestore(&guc->context_lookup, flags);

	/* GuC is blown away, drop all references to contexts */
	xa_destroy(&guc->context_lookup);
}

static void guc_cancel_context_requests(struct intel_context *ce)
{
	struct i915_sched_engine *sched_engine = ce_to_guc(ce)->sched_engine;
	struct i915_request *rq;
	unsigned long flags;

	/* Mark all executing requests as skipped. */
	spin_lock_irqsave(&sched_engine->lock, flags);
	spin_lock(&ce->guc_state.lock);
	list_for_each_entry(rq, &ce->guc_state.requests, sched.link)
		i915_request_put(i915_request_mark_eio(rq));
	spin_unlock(&ce->guc_state.lock);
	spin_unlock_irqrestore(&sched_engine->lock, flags);
}

static void
guc_cancel_sched_engine_requests(struct i915_sched_engine *sched_engine)
{
	struct i915_request *rq, *rn;
	struct rb_node *rb;
	unsigned long flags;

	/* Can be called during boot if GuC fails to load */
	if (!sched_engine)
		return;

	/*
	 * Before we call engine->cancel_requests(), we should have exclusive
	 * access to the submission state. This is arranged for us by the
	 * caller disabling the interrupt generation, the tasklet and other
	 * threads that may then access the same state, giving us a free hand
	 * to reset state. However, we still need to let lockdep be aware that
	 * we know this state may be accessed in hardirq context, so we
	 * disable the irq around this manipulation and we want to keep
	 * the spinlock focused on its duties and not accidentally conflate
	 * coverage to the submission's irq state. (Similarly, although we
	 * shouldn't need to disable irq around the manipulation of the
	 * submission's irq state, we also wish to remind ourselves that
	 * it is irq state.)
	 */
	spin_lock_irqsave(&sched_engine->lock, flags);

	/* Flush the queued requests to the timeline list (for retiring). */
	while ((rb = rb_first_cached(&sched_engine->queue))) {
		struct i915_priolist *p = to_priolist(rb);

		priolist_for_each_request_consume(rq, rn, p) {
			list_del_init(&rq->sched.link);

			__i915_request_submit(rq);

			i915_request_put(i915_request_mark_eio(rq));
		}

		rb_erase_cached(&p->node, &sched_engine->queue);
		i915_priolist_free(p);
	}

	/* Remaining _unready_ requests will be nop'ed when submitted */

	sched_engine->queue_priority_hint = INT_MIN;
	sched_engine->queue = RB_ROOT_CACHED;

	spin_unlock_irqrestore(&sched_engine->lock, flags);
}

void intel_guc_submission_cancel_requests(struct intel_guc *guc)
{
	struct intel_context *ce;
	unsigned long index;
	unsigned long flags;

	xa_lock_irqsave(&guc->context_lookup, flags);
	xa_for_each(&guc->context_lookup, index, ce) {
		if (!kref_get_unless_zero(&ce->ref))
			continue;

		xa_unlock(&guc->context_lookup);

		if (intel_context_is_pinned(ce) &&
		    !intel_context_is_child(ce))
			guc_cancel_context_requests(ce);

		intel_context_put(ce);

		xa_lock(&guc->context_lookup);
	}
	xa_unlock_irqrestore(&guc->context_lookup, flags);

	guc_cancel_sched_engine_requests(guc->sched_engine);

	/* GuC is blown away, drop all references to contexts */
	xa_destroy(&guc->context_lookup);
}

void intel_guc_submission_reset_finish(struct intel_guc *guc)
{
	/* Reset called during driver load or during wedge? */
	if (unlikely(!guc_submission_initialized(guc) ||
		     test_bit(I915_WEDGED, &guc_to_gt(guc)->reset.flags))) {
		return;
	}

	/*
	 * Technically possible for either of these values to be non-zero here,
	 * but very unlikely + harmless. Regardless let's add a warn so we can
	 * see in CI if this happens frequently / a precursor to taking down the
	 * machine.
	 */
	GEM_WARN_ON(atomic_read(&guc->outstanding_submission_g2h));
	atomic_set(&guc->outstanding_submission_g2h, 0);

	intel_guc_global_policies_update(guc);
	enable_submission(guc);
	intel_gt_unpark_heartbeats(guc_to_gt(guc));
}

int intel_guc_submission_limit_ids(struct intel_guc *guc, u32 limit)
{
	if (limit > GUC_MAX_LRC_DESCRIPTORS)
		return -E2BIG;

	if (!ida_is_empty(&guc->submission_state.guc_ids))
		return -ETXTBSY;

	guc->submission_state.max_guc_ids = limit;
	guc->submission_state.num_guc_ids = min(limit, guc->submission_state.num_guc_ids);
	return 0;
}

static void destroyed_worker_func(struct work_struct *w);

/*
 * Set up the memory resources to be shared with the GuC (via the GGTT)
 * at firmware loading time.
 */
int intel_guc_submission_init(struct intel_guc *guc)
{
	int ret;

	if (guc->lrc_desc_pool)
		return 0;

	ret = guc_lrc_desc_pool_create(guc);
	if (ret)
		return ret;
	/*
	 * Keep static analysers happy, let them know that we allocated the
	 * vma after testing that it didn't exist earlier.
	 */
	GEM_BUG_ON(!guc->lrc_desc_pool);

	xa_init_flags(&guc->context_lookup, XA_FLAGS_LOCK_IRQ);
	xa_init_flags(&guc->tlb_lookup, XA_FLAGS_ALLOC);

	spin_lock_init(&guc->submission_state.lock);
	INIT_LIST_HEAD(&guc->submission_state.guc_id_list);
	ida_init(&guc->submission_state.guc_ids);
	INIT_LIST_HEAD(&guc->submission_state.destroyed_contexts);
	intel_gt_pm_unpark_work_init(&guc->submission_state.destroyed_worker,
				     destroyed_worker_func);
	guc->submission_state.guc_ids_bitmap =
		bitmap_zalloc(NUMBER_MULTI_LRC_GUC_ID(guc), GFP_KERNEL);
	if (!guc->submission_state.guc_ids_bitmap)
		return -ENOMEM;

	return 0;
}

void intel_guc_submission_fini(struct intel_guc *guc)
{
	if (!guc->lrc_desc_pool)
		return;

	guc_lrc_desc_pool_destroy(guc);
	guc_flush_destroyed_contexts(guc);
	i915_sched_engine_put(guc->sched_engine);
	bitmap_free(guc->submission_state.guc_ids_bitmap);
	xa_destroy(&guc->tlb_lookup);
}

static void queue_request(struct i915_sched_engine *sched_engine,
			  struct i915_request *rq,
			  int prio)
{
	GEM_BUG_ON(!list_empty(&rq->sched.link));
	list_add_tail(&rq->sched.link,
		      i915_sched_lookup_priolist(sched_engine, prio));
	set_bit(I915_FENCE_FLAG_PQUEUE, &rq->fence.flags);
	tasklet_hi_schedule(&sched_engine->tasklet);
}

static int guc_bypass_tasklet_submit(struct intel_guc *guc,
				     struct i915_request *rq)
{
	int ret;

	__i915_request_submit(rq);

	trace_i915_request_in(rq, 0);

	if (is_multi_lrc_rq(rq)) {
		if (multi_lrc_submit(rq)) {
			ret = guc_wq_item_append(guc, rq);
			if (!ret)
				ret = guc_add_request(guc, rq);
		}
	} else {
		guc_set_lrc_tail(rq);
		ret = guc_add_request(guc, rq);
	}

	if (unlikely(ret == -EPIPE))
		disable_submission(guc);

	return ret;
}

bool need_tasklet(struct intel_guc *guc, struct i915_request *rq)
{
	struct i915_sched_engine *sched_engine = rq->engine->sched_engine;
	struct intel_context *ce = request_to_scheduling_context(rq);

	return submission_disabled(guc) || guc->stalled_request ||
		!i915_sched_engine_is_empty(sched_engine) ||
		!lrc_desc_registered(guc, ce->guc_id.id);
}

static void guc_submit_request(struct i915_request *rq)
{
	struct i915_sched_engine *sched_engine = rq->engine->sched_engine;
	struct intel_guc *guc = &rq->engine->gt->uc.guc;
	unsigned long flags;

	/* Will be called from irq-context when using foreign fences. */
	spin_lock_irqsave(&sched_engine->lock, flags);

	if (need_tasklet(guc, rq))
		queue_request(sched_engine, rq, rq_prio(rq));
	else if (guc_bypass_tasklet_submit(guc, rq) == -EBUSY)
		tasklet_hi_schedule(&sched_engine->tasklet);

	spin_unlock_irqrestore(&sched_engine->lock, flags);
}

static int new_guc_id(struct intel_guc *guc, struct intel_context *ce)
{
	int ret;

	GEM_BUG_ON(intel_context_is_child(ce));

	if (intel_context_is_parent(ce))
		ret = bitmap_find_free_region(guc->submission_state.guc_ids_bitmap,
					      NUMBER_MULTI_LRC_GUC_ID(guc),
					      order_base_2(ce->guc_number_children
							   + 1));
	else
		ret = ida_simple_get(&guc->submission_state.guc_ids,
				     NUMBER_MULTI_LRC_GUC_ID(guc),
				     guc->submission_state.num_guc_ids,
				     GFP_KERNEL | __GFP_RETRY_MAYFAIL |
				     __GFP_NOWARN);
	if (unlikely(ret < 0))
		return ret;

	ce->guc_id.id = ret;
	return 0;
}

static void __release_guc_id(struct intel_guc *guc, struct intel_context *ce)
{
	GEM_BUG_ON(intel_context_is_child(ce));

	if (!context_guc_id_invalid(ce)) {
		if (intel_context_is_parent(ce))
			bitmap_release_region(guc->submission_state.guc_ids_bitmap,
					      ce->guc_id.id,
					      order_base_2(ce->guc_number_children
							   + 1));
		else
			ida_simple_remove(&guc->submission_state.guc_ids,
					  ce->guc_id.id);
		reset_lrc_desc(guc, ce->guc_id.id);
		set_context_guc_id_invalid(ce);
	}
	if (!list_empty(&ce->guc_id.link))
		list_del_init(&ce->guc_id.link);
}

static void release_guc_id(struct intel_guc *guc, struct intel_context *ce)
{
	unsigned long flags;

	spin_lock_irqsave(&guc->submission_state.lock, flags);
	__release_guc_id(guc, ce);
	spin_unlock_irqrestore(&guc->submission_state.lock, flags);
}

static int steal_guc_id(struct intel_guc *guc, struct intel_context *ce)
{
	struct intel_context *cn;

	lockdep_assert_held(&guc->submission_state.lock);
	GEM_BUG_ON(intel_context_is_child(ce));
	GEM_BUG_ON(intel_context_is_parent(ce));

	if (!list_empty(&guc->submission_state.guc_id_list)) {
		cn = list_first_entry(&guc->submission_state.guc_id_list,
				      struct intel_context,
				      guc_id.link);

		GEM_BUG_ON(atomic_read(&cn->guc_id.ref));
		GEM_BUG_ON(context_guc_id_invalid(cn));
		GEM_BUG_ON(intel_context_is_child(cn));
		GEM_BUG_ON(intel_context_is_parent(cn));

		list_del_init(&cn->guc_id.link);
		ce->guc_id = cn->guc_id;
		clr_context_registered(cn);
		set_context_guc_id_invalid(cn);

		return 0;
	} else {
		return -EAGAIN;
	}
}

static int assign_guc_id(struct intel_guc *guc, struct intel_context *ce)
{
	int ret;

	lockdep_assert_held(&guc->submission_state.lock);
	GEM_BUG_ON(intel_context_is_child(ce));

	ret = new_guc_id(guc, ce);
	if (unlikely(ret < 0)) {
		if (intel_context_is_parent(ce))
			return -ENOSPC;

		ret = steal_guc_id(guc, ce);
		if (ret < 0)
			return ret;
	}

	if (intel_context_is_parent(ce)) {
		struct intel_context *child;
		int i = 1;

		for_each_child(ce, child)
			child->guc_id.id = ce->guc_id.id + i++;
	}

	return 0;
}

#define PIN_GUC_ID_TRIES	4
static int pin_guc_id(struct intel_guc *guc, struct intel_context *ce)
{
	int ret = 0;
	unsigned long flags, tries = PIN_GUC_ID_TRIES;

	GEM_BUG_ON(atomic_read(&ce->guc_id.ref));

try_again:
	spin_lock_irqsave(&guc->submission_state.lock, flags);

	might_lock(&ce->guc_state.lock);

	if (context_guc_id_invalid(ce)) {
		ret = assign_guc_id(guc, ce);
		if (ret)
			goto out_unlock;
		ret = 1;	/* Indidcates newly assigned guc_id */
	}
	if (!list_empty(&ce->guc_id.link))
		list_del_init(&ce->guc_id.link);
	atomic_inc(&ce->guc_id.ref);

out_unlock:
	spin_unlock_irqrestore(&guc->submission_state.lock, flags);

	/*
	 * -EAGAIN indicates no guc_id are available, let's retire any
	 * outstanding requests to see if that frees up a guc_id. If the first
	 * retire didn't help, insert a sleep with the timeslice duration before
	 * attempting to retire more requests. Double the sleep period each
	 * subsequent pass before finally giving up. The sleep period has max of
	 * 100ms and minimum of 1ms.
	 */
	if (ret == -EAGAIN && --tries) {
		if (PIN_GUC_ID_TRIES - tries > 1) {
			unsigned int timeslice_shifted =
				ce->engine->props.timeslice_duration_ms <<
				(PIN_GUC_ID_TRIES - tries - 2);
			unsigned int max = min_t(unsigned int, 100,
						 timeslice_shifted);

			msleep(max_t(unsigned int, max, 1));
		}
		intel_gt_retire_requests(guc_to_gt(guc));
		goto try_again;
	}

	return ret;
}

static void unpin_guc_id(struct intel_guc *guc, struct intel_context *ce)
{
	unsigned long flags;

	GEM_BUG_ON(atomic_read(&ce->guc_id.ref) < 0);
	GEM_BUG_ON(intel_context_is_child(ce));

	if (unlikely(context_guc_id_invalid(ce) ||
		     intel_context_is_parent(ce)))
		return;

	spin_lock_irqsave(&guc->submission_state.lock, flags);
	if (!context_guc_id_invalid(ce) && list_empty(&ce->guc_id.link) &&
	    !atomic_read(&ce->guc_id.ref))
		list_add_tail(&ce->guc_id.link,
			      &guc->submission_state.guc_id_list);
	spin_unlock_irqrestore(&guc->submission_state.lock, flags);
}

static int __guc_action_register_multi_lrc(struct intel_guc *guc,
					   struct intel_context *ce,
					   u32 guc_id,
					   u32 offset,
					   bool loop)
{
	struct intel_context *child;
	u32 action[4 + MAX_ENGINE_INSTANCE];
	int len = 0;

	GEM_BUG_ON(ce->guc_number_children > MAX_ENGINE_INSTANCE);

	action[len++] = INTEL_GUC_ACTION_REGISTER_CONTEXT_MULTI_LRC;
	action[len++] = guc_id;
	action[len++] = ce->guc_number_children + 1;
	action[len++] = offset;
	for_each_child(ce, child) {
		offset += sizeof(struct guc_lrc_desc);
		action[len++] = offset;
	}

	return guc_submission_send_busy_loop(guc, action, len, 0, loop);
}

static int __guc_action_register_context(struct intel_guc *guc,
					 u32 guc_id,
					 u32 offset,
					 bool loop)
{
	u32 action[] = {
		INTEL_GUC_ACTION_REGISTER_CONTEXT,
		guc_id,
		offset,
	};

	return guc_submission_send_busy_loop(guc, action, ARRAY_SIZE(action),
					     0, loop);
}

static int register_context(struct intel_context *ce, bool loop)
{
	struct intel_guc *guc = ce_to_guc(ce);
	u32 offset = intel_guc_ggtt_offset(guc, guc->lrc_desc_pool) +
		ce->guc_id.id * sizeof(struct guc_lrc_desc);
	int ret;

	GEM_BUG_ON(intel_context_is_child(ce));
	trace_intel_context_register(ce);

	if (intel_context_is_parent(ce))
		ret = __guc_action_register_multi_lrc(guc, ce, ce->guc_id.id,
						      offset, loop);
	else
		ret = __guc_action_register_context(guc, ce->guc_id.id, offset,
						    loop);
	if (likely(!ret)) {
		unsigned long flags;

		spin_lock_irqsave(&ce->guc_state.lock, flags);
		set_context_registered(ce);
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);
	}

	return ret;
}

static int __guc_action_deregister_context(struct intel_guc *guc,
					   u32 guc_id,
					   bool loop)
{
	u32 action[] = {
		INTEL_GUC_ACTION_DEREGISTER_CONTEXT,
		guc_id,
	};

	return guc_submission_send_busy_loop(guc, action, ARRAY_SIZE(action),
					     G2H_LEN_DW_DEREGISTER_CONTEXT,
					     loop);
}

static int deregister_context(struct intel_context *ce, u32 guc_id, bool loop)
{
	struct intel_guc *guc = ce_to_guc(ce);

	GEM_BUG_ON(intel_context_is_child(ce));
	trace_intel_context_deregister(ce);

	return __guc_action_deregister_context(guc, guc_id, loop);
}

static inline void clear_children_join_go_memory(struct intel_context *ce)
{
	u32 *mem = (u32 *)(__get_process_desc(ce) + 1);
	u8 i;

	for (i = 0; i < ce->guc_number_children + 1; ++i)
		mem[i * (CACHELINE_BYTES / sizeof(u32))] = 0;
}

static inline u32 get_children_go_value(struct intel_context *ce)
{
	u32 *mem = (u32 *)(__get_process_desc(ce) + 1);

	return mem[0];
}

static inline u32 get_children_join_value(struct intel_context *ce,
					  u8 child_index)
{
	u32 *mem = (u32 *)(__get_process_desc(ce) + 1);

	return mem[(child_index + 1) * (CACHELINE_BYTES / sizeof(u32))];
}

static void guc_context_policy_init(struct intel_engine_cs *engine,
				    struct guc_lrc_desc *desc)
{
	desc->policy_flags = 0;

	if (engine->flags & I915_ENGINE_WANT_FORCED_PREEMPTION)
		desc->policy_flags |= CONTEXT_POLICY_FLAG_PREEMPT_TO_IDLE;

	/* NB: For both of these, zero means disabled. */
	desc->execution_quantum = engine->props.timeslice_duration_ms * 1000;
	desc->preemption_timeout = engine->props.preempt_timeout_ms * 1000;
}

static int guc_lrc_desc_pin(struct intel_context *ce, bool loop)
{
	struct intel_engine_cs *engine = ce->engine;
	struct intel_runtime_pm *runtime_pm = engine->uncore->rpm;
	struct intel_guc *guc = &engine->gt->uc.guc;
	u32 desc_idx = ce->guc_id.id;
	struct guc_lrc_desc *desc;
	bool context_registered;
	intel_wakeref_t wakeref;
	struct intel_context *child;
	int ret = 0;

	GEM_BUG_ON(!engine->mask);
	GEM_BUG_ON(!sched_state_is_init(ce));

	/*
	 * Ensure LRC + CT vmas are is same region as write barrier is done
	 * based on CT vma region.
	 */
	GEM_BUG_ON(i915_gem_object_is_lmem(guc->ct.vma->obj) !=
		   i915_gem_object_is_lmem(ce->ring->vma->obj));

	context_registered = lrc_desc_registered(guc, desc_idx);

	reset_lrc_desc(guc, desc_idx);
	set_lrc_desc_registered(guc, desc_idx, ce);

	desc = __get_lrc_desc(guc, desc_idx);
	desc->engine_class = engine_class_to_guc_class(engine->class);
	desc->engine_submit_mask = engine->logical_mask;
	desc->hw_context_desc = ce->lrc.lrca;
	desc->priority = ce->guc_state.prio;
	desc->context_flags = CONTEXT_REGISTRATION_FLAG_KMD;
	guc_context_policy_init(engine, desc);

	/*
	 * Context is a parent, we need to register a process descriptor
	 * describing a work queue and register all child contexts.
	 */
	if (intel_context_is_parent(ce)) {
		struct guc_process_desc *pdesc;

		ce->guc_wqi_tail = 0;
		ce->guc_wqi_head = 0;

		desc->process_desc = i915_ggtt_offset(ce->state) +
			__get_process_desc_offset(ce);
		desc->wq_addr = i915_ggtt_offset(ce->state) +
			__get_wq_offset(ce);
		desc->wq_size = GUC_WQ_SIZE;

		pdesc = __get_process_desc(ce);
		memset(pdesc, 0, sizeof(*(pdesc)));
		pdesc->stage_id = ce->guc_id.id;
		pdesc->wq_base_addr = desc->wq_addr;
		pdesc->wq_size_bytes = desc->wq_size;
		pdesc->priority = GUC_CLIENT_PRIORITY_KMD_NORMAL;
		pdesc->wq_status = WQ_STATUS_ACTIVE;

		for_each_child(ce, child) {
			desc = __get_lrc_desc(guc, child->guc_id.id);

			desc->engine_class =
				engine_class_to_guc_class(engine->class);
			desc->hw_context_desc = child->lrc.lrca;
			desc->priority = GUC_CLIENT_PRIORITY_KMD_NORMAL;
			desc->context_flags = CONTEXT_REGISTRATION_FLAG_KMD;
			guc_context_policy_init(engine, desc);
		}

		clear_children_join_go_memory(ce);
	}

	/*
	 * The context_lookup xarray is used to determine if the hardware
	 * context is currently registered. There are two cases in which it
	 * could be registered either the guc_id has been stolen from another
	 * context or the lrc descriptor address of this context has changed. In
	 * either case the context needs to be deregistered with the GuC before
	 * registering this context.
	 */
	if (context_registered) {
		bool disabled;
		unsigned long flags;

		trace_intel_context_steal_guc_id(ce);
		GEM_BUG_ON(!loop);

		/* Seal race with Reset */
		spin_lock_irqsave(&ce->guc_state.lock, flags);
		disabled = submission_disabled(guc);
		if (likely(!disabled)) {
			set_context_wait_for_deregister_to_register(ce);
			intel_context_get(ce);
		}
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);
		if (unlikely(disabled)) {
			reset_lrc_desc(guc, desc_idx);
			return 0;	/* Will get registered later */
		}

		/*
		 * If stealing the guc_id, this ce has the same guc_id as the
		 * context whose guc_id was stolen.
		 */
		with_intel_runtime_pm(runtime_pm, wakeref)
			ret = deregister_context(ce, ce->guc_id.id, loop);
		if (unlikely(ret == -ENODEV)) {
			ret = 0;	/* Will get registered later */
		}
	} else {
		with_intel_runtime_pm(runtime_pm, wakeref)
			ret = register_context(ce, loop);
		if (unlikely(ret == -EBUSY)) {
			reset_lrc_desc(guc, desc_idx);
		} else if (unlikely(ret == -ENODEV)) {
			reset_lrc_desc(guc, desc_idx);
			ret = 0;	/* Will get registered later */
		}
	}

	return ret;
}

static int __guc_context_pre_pin(struct intel_context *ce,
				 struct intel_engine_cs *engine,
				 struct i915_gem_ww_ctx *ww,
				 void **vaddr)
{
	return lrc_pre_pin(ce, engine, ww, vaddr);
}

static int __guc_context_pin(struct intel_context *ce,
			     struct intel_engine_cs *engine,
			     void *vaddr)
{
	if (i915_ggtt_offset(ce->state) !=
	    (ce->lrc.lrca & CTX_GTT_ADDRESS_MASK))
		set_bit(CONTEXT_LRCA_DIRTY, &ce->flags);

	/*
	 * GuC context gets pinned in guc_request_alloc. See that function for
	 * explaination of why.
	 */

	return lrc_pin(ce, engine, vaddr);
}

static int guc_context_pre_pin(struct intel_context *ce,
			       struct i915_gem_ww_ctx *ww,
			       void **vaddr)
{
	return __guc_context_pre_pin(ce, ce->engine, ww, vaddr);
}

static int guc_context_pin(struct intel_context *ce, void *vaddr)
{
	int ret = __guc_context_pin(ce, ce->engine, vaddr);

	if (likely(!ret && !intel_context_is_barrier(ce)))
		intel_engine_pm_get(ce->engine);

	return ret;
}

static void guc_context_unpin(struct intel_context *ce)
{
	struct intel_guc *guc = ce_to_guc(ce);

	unpin_guc_id(guc, ce);
	lrc_unpin(ce);

	if (likely(!intel_context_is_barrier(ce)))
		intel_engine_pm_put_async(ce->engine);
}

static void guc_context_post_unpin(struct intel_context *ce)
{
	lrc_post_unpin(ce);
}

static void __guc_context_sched_enable(struct intel_guc *guc,
				       struct intel_context *ce)
{
	u32 action[] = {
		INTEL_GUC_ACTION_SCHED_CONTEXT_MODE_SET,
		ce->guc_id.id,
		GUC_CONTEXT_ENABLE
	};

	trace_intel_context_sched_enable(ce);

	guc_submission_send_busy_loop(guc, action, ARRAY_SIZE(action),
				      G2H_LEN_DW_SCHED_CONTEXT_MODE_SET, true);
}

static void __guc_context_sched_disable(struct intel_guc *guc,
					struct intel_context *ce,
					u16 guc_id)
{
	u32 action[] = {
		INTEL_GUC_ACTION_SCHED_CONTEXT_MODE_SET,
		guc_id,	/* ce->guc_id.id not stable */
		GUC_CONTEXT_DISABLE
	};

	GEM_BUG_ON(guc_id == GUC_INVALID_LRC_ID);

	GEM_BUG_ON(intel_context_is_child(ce));
	trace_intel_context_sched_disable(ce);

	guc_submission_send_busy_loop(guc, action, ARRAY_SIZE(action),
				      G2H_LEN_DW_SCHED_CONTEXT_MODE_SET, true);
}

static void guc_blocked_fence_complete(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);

	if (!i915_sw_fence_done(&ce->guc_state.blocked_fence))
		i915_sw_fence_complete(&ce->guc_state.blocked_fence);
}

static void guc_blocked_fence_reinit(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);
	GEM_BUG_ON(!i915_sw_fence_done(&ce->guc_state.blocked_fence));

	/*
	 * This fence is always complete unless a pending schedule disable is
	 * outstanding. We arm the fence here and complete it when we receive
	 * the pending schedule disable complete message.
	 */
	i915_sw_fence_fini(&ce->guc_state.blocked_fence);
	i915_sw_fence_reinit(&ce->guc_state.blocked_fence);
	i915_sw_fence_await(&ce->guc_state.blocked_fence);
	i915_sw_fence_commit(&ce->guc_state.blocked_fence);
}

static u16 prep_context_pending_disable(struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);

	set_context_pending_disable(ce);
	clr_context_enabled(ce);
	guc_blocked_fence_reinit(ce);
	intel_context_get(ce);

	return ce->guc_id.id;
}

static struct i915_sw_fence *guc_context_block(struct intel_context *ce)
{
	struct intel_guc *guc = ce_to_guc(ce);
	unsigned long flags;
	struct intel_runtime_pm *runtime_pm = ce->engine->uncore->rpm;
	intel_wakeref_t wakeref;
	u16 guc_id;
	bool enabled;

	GEM_BUG_ON(intel_context_is_child(ce));

	spin_lock_irqsave(&ce->guc_state.lock, flags);

	incr_context_blocked(ce);

	enabled = context_enabled(ce);
	if (unlikely(!enabled || submission_disabled(guc))) {
		if (enabled)
			clr_context_enabled(ce);
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);
		return &ce->guc_state.blocked_fence;
	}

	/*
	 * We add +2 here as the schedule disable complete CTB handler calls
	 * intel_context_sched_disable_unpin (-2 to pin_count).
	 */
	atomic_add(2, &ce->pin_count);

	guc_id = prep_context_pending_disable(ce);

	spin_unlock_irqrestore(&ce->guc_state.lock, flags);

	with_intel_runtime_pm(runtime_pm, wakeref)
		__guc_context_sched_disable(guc, ce, guc_id);

	return &ce->guc_state.blocked_fence;
}

static void guc_context_unblock(struct intel_context *ce)
{
	struct intel_guc *guc = ce_to_guc(ce);
	unsigned long flags;
	struct intel_runtime_pm *runtime_pm = ce->engine->uncore->rpm;
	intel_wakeref_t wakeref;
	bool enable;

	GEM_BUG_ON(context_enabled(ce));
	GEM_BUG_ON(intel_context_is_child(ce));

	spin_lock_irqsave(&ce->guc_state.lock, flags);

	if (unlikely(submission_disabled(guc) ||
		     intel_context_is_banned(ce) ||
		     context_guc_id_invalid(ce) ||
		     !lrc_desc_registered(guc, ce->guc_id.id) ||
		     !intel_context_is_pinned(ce) ||
		     context_pending_disable(ce) ||
		     context_blocked(ce) > 1)) {
		enable = false;
	} else {
		enable = true;
		set_context_pending_enable(ce);
		set_context_enabled(ce);
		intel_context_get(ce);
	}

	decr_context_blocked(ce);

	spin_unlock_irqrestore(&ce->guc_state.lock, flags);

	if (enable) {
		with_intel_runtime_pm(runtime_pm, wakeref)
			__guc_context_sched_enable(guc, ce);
	}
}

static void guc_context_cancel_request(struct intel_context *ce,
				       struct i915_request *rq)
{
	struct intel_context *block_context =
		request_to_scheduling_context(rq);

	if (i915_sw_fence_signaled(&rq->submit)) {
		struct i915_sw_fence *fence;

		intel_context_get(ce);
		fence = guc_context_block(block_context);
		i915_sw_fence_wait(fence);
		if (!i915_request_completed(rq)) {
			__i915_request_skip(rq);
			guc_reset_state(ce, intel_ring_wrap(ce->ring, rq->head),
					true);
		}

		/*
		 * XXX: Racey if context is reset, see comment in
		 * __guc_reset_context().
		 */
		flush_work(&ce_to_guc(ce)->ct.requests.worker);

		guc_context_unblock(block_context);
		intel_context_put(ce);
	}
}

static void __guc_context_set_preemption_timeout(struct intel_guc *guc,
						 u16 guc_id,
						 u32 preemption_timeout)
{
	u32 action[] = {
		INTEL_GUC_ACTION_SET_CONTEXT_PREEMPTION_TIMEOUT,
		guc_id,
		preemption_timeout
	};

	intel_guc_send_busy_loop(guc, action, ARRAY_SIZE(action), 0, true);
}

static void guc_context_ban(struct intel_context *ce, struct i915_request *rq)
{
	struct intel_guc *guc = ce_to_guc(ce);
	struct intel_runtime_pm *runtime_pm =
		&ce->engine->gt->i915->runtime_pm;
	intel_wakeref_t wakeref;
	unsigned long flags;

	GEM_BUG_ON(intel_context_is_child(ce));

	guc_flush_submissions(guc);

	spin_lock_irqsave(&ce->guc_state.lock, flags);
	set_context_banned(ce);

	if (submission_disabled(guc) ||
	    (!context_enabled(ce) && !context_pending_disable(ce))) {
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);

		guc_cancel_context_requests(ce);
		intel_engine_signal_breadcrumbs(ce->engine);
	} else if (!context_pending_disable(ce)) {
		u16 guc_id;

		/*
		 * We add +2 here as the schedule disable complete CTB handler
		 * calls intel_context_sched_disable_unpin (-2 to pin_count).
		 */
		atomic_add(2, &ce->pin_count);

		guc_id = prep_context_pending_disable(ce);
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);

		/*
		 * In addition to disabling scheduling, set the preemption
		 * timeout to the minimum value (1 us) so the banned context
		 * gets kicked off the HW ASAP.
		 */
		with_intel_runtime_pm(runtime_pm, wakeref) {
			__guc_context_set_preemption_timeout(guc, guc_id, 1);
			__guc_context_sched_disable(guc, ce, guc_id);
		}
	} else {
		if (!context_guc_id_invalid(ce))
			with_intel_runtime_pm(runtime_pm, wakeref)
				__guc_context_set_preemption_timeout(guc,
								     ce->guc_id.id,
								     1);
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);
	}
}

static void guc_context_sched_disable(struct intel_context *ce)
{
	struct intel_guc *guc = ce_to_guc(ce);
	unsigned long flags;
	struct intel_runtime_pm *runtime_pm = &ce->engine->gt->i915->runtime_pm;
	intel_wakeref_t wakeref;
	u16 guc_id;
	bool enabled;

	GEM_BUG_ON(intel_context_is_child(ce));

	if (submission_disabled(guc) || context_guc_id_invalid(ce) ||
	    !lrc_desc_registered(guc, ce->guc_id.id)) {
		spin_lock_irqsave(&ce->guc_state.lock, flags);
		clr_context_enabled(ce);
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);
		goto unpin;
	}

	if (!context_enabled(ce))
		goto unpin;

	spin_lock_irqsave(&ce->guc_state.lock, flags);

	/*
	 * We have to check if the context has been disabled by another thread,
	 * check if submssion has been disabled to seal a race with reset and
	 * finally check if any more requests have been committed to the
	 * context ensursing that a request doesn't slip through the
	 * 'context_pending_disable' fence.
	 */
	enabled = context_enabled(ce);
	if (unlikely(!enabled || submission_disabled(guc))) {
		if (enabled)
			clr_context_enabled(ce);
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);
		goto unpin;
	}
	if (unlikely(context_has_committed_requests(ce))) {
		intel_context_sched_disable_unpin(ce);
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);
		return;
	}
	guc_id = prep_context_pending_disable(ce);

	spin_unlock_irqrestore(&ce->guc_state.lock, flags);

	with_intel_runtime_pm(runtime_pm, wakeref)
		__guc_context_sched_disable(guc, ce, guc_id);

	return;
unpin:
	intel_context_sched_disable_unpin(ce);
}

static void guc_lrc_desc_unpin(struct intel_context *ce)
{
	struct intel_guc *guc = ce_to_guc(ce);
	struct intel_gt *gt = guc_to_gt(guc);
	unsigned long flags;
	bool disabled;

	GEM_BUG_ON(!intel_gt_pm_is_awake(gt));
	GEM_BUG_ON(!lrc_desc_registered(guc, ce->guc_id.id));
	GEM_BUG_ON(ce != __get_context(guc, ce->guc_id.id));
	GEM_BUG_ON(context_enabled(ce));

	/* Seal race with Reset */
	spin_lock_irqsave(&ce->guc_state.lock, flags);
	disabled = submission_disabled(guc);
	if (likely(!disabled)) {
		__intel_gt_pm_get(gt);
		set_context_destroyed(ce);
		clr_context_registered(ce);
	}
	spin_unlock_irqrestore(&ce->guc_state.lock, flags);
	if (unlikely(disabled)) {
		release_guc_id(guc, ce);
		__guc_context_destroy(ce);
		return;
	}

	deregister_context(ce, ce->guc_id.id, true);
}

static void __guc_context_destroy(struct intel_context *ce)
{
	GEM_BUG_ON(ce->guc_state.prio_count[GUC_CLIENT_PRIORITY_KMD_HIGH] ||
		   ce->guc_state.prio_count[GUC_CLIENT_PRIORITY_HIGH] ||
		   ce->guc_state.prio_count[GUC_CLIENT_PRIORITY_KMD_NORMAL] ||
		   ce->guc_state.prio_count[GUC_CLIENT_PRIORITY_NORMAL]);
	GEM_BUG_ON(ce->guc_state.number_committed_requests);

	lrc_fini(ce);
	intel_context_fini(ce);

	if (intel_engine_is_virtual(ce->engine)) {
		struct guc_virtual_engine *ve =
			container_of(ce, typeof(*ve), context);

		if (ve->base.breadcrumbs)
			intel_breadcrumbs_put(ve->base.breadcrumbs);

		kfree(ve);
	} else {
		intel_context_free(ce);
	}
}

static void guc_flush_destroyed_contexts(struct intel_guc *guc)
{
	struct intel_context *ce, *cn;
	unsigned long flags;

	GEM_BUG_ON(!submission_disabled(guc) &&
		   guc_submission_initialized(guc));

	spin_lock_irqsave(&guc->submission_state.lock, flags);
	list_for_each_entry_safe(ce, cn,
				 &guc->submission_state.destroyed_contexts,
				 destroyed_link) {
		list_del_init(&ce->destroyed_link);
		__release_guc_id(guc, ce);
		__guc_context_destroy(ce);
	}
	spin_unlock_irqrestore(&guc->submission_state.lock, flags);
}

static void deregister_destroyed_contexts(struct intel_guc *guc)
{
	struct intel_context *ce, *cn;
	unsigned long flags;

	spin_lock_irqsave(&guc->submission_state.lock, flags);
	list_for_each_entry_safe(ce, cn,
				 &guc->submission_state.destroyed_contexts,
				 destroyed_link) {
		list_del_init(&ce->destroyed_link);
		spin_unlock_irqrestore(&guc->submission_state.lock, flags);
		guc_lrc_desc_unpin(ce);
		spin_lock_irqsave(&guc->submission_state.lock, flags);
	}
	spin_unlock_irqrestore(&guc->submission_state.lock, flags);
}

static void destroyed_worker_func(struct work_struct *w)
{
	struct intel_gt_pm_unpark_work *destroyed_worker =
		container_of(w, struct intel_gt_pm_unpark_work, worker);
	struct intel_guc *guc = container_of(destroyed_worker, struct intel_guc,
					     submission_state.destroyed_worker);
	struct intel_gt *gt = guc_to_gt(guc);
	int tmp;

	with_intel_gt_pm_if_awake(gt, tmp)
		deregister_destroyed_contexts(guc);

	if (!list_empty(&guc->submission_state.destroyed_contexts))
		intel_gt_pm_unpark_work_add(gt, destroyed_worker);
}

static void guc_context_destroy(struct kref *kref)
{
	struct intel_context *ce = container_of(kref, typeof(*ce), ref);
	struct intel_guc *guc = ce_to_guc(ce);
	unsigned long flags;
	bool destroy;

	/*
	 * If the guc_id is invalid this context has been stolen and we can free
	 * it immediately. Also can be freed immediately if the context is not
	 * registered with the GuC or the GuC is in the middle of a reset.
	 */
	spin_lock_irqsave(&guc->submission_state.lock, flags);
	destroy = submission_disabled(guc) || context_guc_id_invalid(ce) ||
		!lrc_desc_registered(guc, ce->guc_id.id);
	if (likely(!destroy)) {
		if (!list_empty(&ce->guc_id.link))
			list_del_init(&ce->guc_id.link);
		list_add_tail(&ce->destroyed_link,
			      &guc->submission_state.destroyed_contexts);
	} else {
		__release_guc_id(guc, ce);
	}
	spin_unlock_irqrestore(&guc->submission_state.lock, flags);
	if (unlikely(destroy)) {
		__guc_context_destroy(ce);
		return;
	}

	/*
	 * We use a worker to issue the H2G to deregister the context as we can
	 * take the GT PM for the first time which isn't allowed from an atomic
	 * context.
	 */
	intel_gt_pm_unpark_work_add(guc_to_gt(guc),
				    &guc->submission_state.destroyed_worker);
}

static int guc_context_alloc(struct intel_context *ce)
{
	return lrc_alloc(ce, ce->engine);
}

static void guc_context_set_prio(struct intel_guc *guc,
				 struct intel_context *ce,
				 u8 prio)
{
	u32 action[] = {
		INTEL_GUC_ACTION_SET_CONTEXT_PRIORITY,
		ce->guc_id.id,
		prio,
	};

	GEM_BUG_ON(prio < GUC_CLIENT_PRIORITY_KMD_HIGH ||
		   prio > GUC_CLIENT_PRIORITY_NORMAL);
	lockdep_assert_held(&ce->guc_state.lock);

	if (ce->guc_state.prio == prio || submission_disabled(guc) ||
	    !context_registered(ce)) {
		ce->guc_state.prio = prio;
		return;
	}

	guc_submission_send_busy_loop(guc, action, ARRAY_SIZE(action), 0, true);

	ce->guc_state.prio = prio;
	trace_intel_context_set_prio(ce);
}

static u8 map_i915_prio_to_guc_prio(int prio)
{
	if (prio == I915_PRIORITY_NORMAL)
		return GUC_CLIENT_PRIORITY_KMD_NORMAL;
	else if (prio < I915_PRIORITY_NORMAL)
		return GUC_CLIENT_PRIORITY_NORMAL;
	else if (prio < I915_PRIORITY_DISPLAY)
		return GUC_CLIENT_PRIORITY_HIGH;
	else
		return GUC_CLIENT_PRIORITY_KMD_HIGH;
}

static void add_context_inflight_prio(struct intel_context *ce, u8 guc_prio)
{
	lockdep_assert_held(&ce->guc_state.lock);
	GEM_BUG_ON(guc_prio >= ARRAY_SIZE(ce->guc_state.prio_count));

	++ce->guc_state.prio_count[guc_prio];

	/* Overflow protection */
	GEM_WARN_ON(!ce->guc_state.prio_count[guc_prio]);
}

static void sub_context_inflight_prio(struct intel_context *ce, u8 guc_prio)
{
	lockdep_assert_held(&ce->guc_state.lock);
	GEM_BUG_ON(guc_prio >= ARRAY_SIZE(ce->guc_state.prio_count));

	/* Underflow protection */
	GEM_WARN_ON(!ce->guc_state.prio_count[guc_prio]);

	--ce->guc_state.prio_count[guc_prio];
}

static void update_context_prio(struct intel_context *ce)
{
	struct intel_guc *guc = &ce->engine->gt->uc.guc;
	int i;

	BUILD_BUG_ON(GUC_CLIENT_PRIORITY_KMD_HIGH != 0);
	BUILD_BUG_ON(GUC_CLIENT_PRIORITY_KMD_HIGH > GUC_CLIENT_PRIORITY_NORMAL);

	lockdep_assert_held(&ce->guc_state.lock);

	for (i = 0; i < ARRAY_SIZE(ce->guc_state.prio_count); ++i) {
		if (ce->guc_state.prio_count[i]) {
			guc_context_set_prio(guc, ce, i);
			break;
		}
	}
}

static bool new_guc_prio_higher(u8 old_guc_prio, u8 new_guc_prio)
{
	/* Lower value is higher priority */
	return new_guc_prio < old_guc_prio;
}

static void add_to_context(struct i915_request *rq)
{
	struct intel_context *ce = request_to_scheduling_context(rq);
	u8 new_guc_prio = map_i915_prio_to_guc_prio(rq_prio(rq));

	GEM_BUG_ON(intel_context_is_child(ce));
	GEM_BUG_ON(rq->guc_prio == GUC_PRIO_FINI);

	spin_lock(&ce->guc_state.lock);
	list_move_tail(&rq->sched.link, &ce->guc_state.requests);

	if (rq->guc_prio == GUC_PRIO_INIT) {
		rq->guc_prio = new_guc_prio;
		add_context_inflight_prio(ce, rq->guc_prio);
	} else if (new_guc_prio_higher(rq->guc_prio, new_guc_prio)) {
		sub_context_inflight_prio(ce, rq->guc_prio);
		rq->guc_prio = new_guc_prio;
		add_context_inflight_prio(ce, rq->guc_prio);
	}
	update_context_prio(ce);

	spin_unlock(&ce->guc_state.lock);
}

static void guc_prio_fini(struct i915_request *rq, struct intel_context *ce)
{
	lockdep_assert_held(&ce->guc_state.lock);

	if (rq->guc_prio != GUC_PRIO_INIT &&
	    rq->guc_prio != GUC_PRIO_FINI) {
		sub_context_inflight_prio(ce, rq->guc_prio);
		update_context_prio(ce);
	}
	rq->guc_prio = GUC_PRIO_FINI;
}

static void remove_from_context(struct i915_request *rq)
{
	struct intel_context *ce = request_to_scheduling_context(rq);

	GEM_BUG_ON(intel_context_is_child(ce));

	spin_lock_irq(&ce->guc_state.lock);

	list_del_init(&rq->sched.link);
	clear_bit(I915_FENCE_FLAG_PQUEUE, &rq->fence.flags);

	/* Prevent further __await_execution() registering a cb, then flush */
	set_bit(I915_FENCE_FLAG_ACTIVE, &rq->fence.flags);

	guc_prio_fini(rq, ce);

	decr_context_committed_requests(ce);

	spin_unlock_irq(&ce->guc_state.lock);

	atomic_dec(&ce->guc_id.ref);
	i915_request_notify_execute_cb_imm(rq);
}

static const struct intel_context_ops guc_context_ops = {
	.alloc = guc_context_alloc,

	.pre_pin = guc_context_pre_pin,
	.pin = guc_context_pin,
	.unpin = guc_context_unpin,
	.post_unpin = guc_context_post_unpin,

	.ban = guc_context_ban,

	.cancel_request = guc_context_cancel_request,

	.enter = intel_context_enter_engine,
	.exit = intel_context_exit_engine,

	.sched_disable = guc_context_sched_disable,

	.reset = lrc_reset,
	.destroy = guc_context_destroy,

	.create_virtual = guc_create_virtual,
	.create_parallel = guc_create_parallel,
};

static void submit_work_cb(struct irq_work *wrk)
{
	struct i915_request *rq = container_of(wrk, typeof(*rq), submit_work);

	might_lock(&rq->engine->sched_engine->lock);
	i915_sw_fence_complete(&rq->submit);
}

static void __guc_signal_context_fence(struct intel_context *ce)
{
	struct i915_request *rq;

	lockdep_assert_held(&ce->guc_state.lock);

	if (!list_empty(&ce->guc_state.fences))
		trace_intel_context_fence_release(ce);

	/*
	 * Use an IRQ to ensure locking order of sched_engine->lock ->
	 * ce->guc_state.lock is preserved.
	 */
	list_for_each_entry(rq, &ce->guc_state.fences, guc_fence_link)
		irq_work_queue(&rq->submit_work);

	INIT_LIST_HEAD(&ce->guc_state.fences);
}

static void guc_signal_context_fence(struct intel_context *ce)
{
	unsigned long flags;

	GEM_BUG_ON(intel_context_is_child(ce));

	spin_lock_irqsave(&ce->guc_state.lock, flags);
	clr_context_wait_for_deregister_to_register(ce);
	__guc_signal_context_fence(ce);
	spin_unlock_irqrestore(&ce->guc_state.lock, flags);
}

static bool context_needs_register(struct intel_context *ce, bool new_guc_id)
{
	return (new_guc_id || test_bit(CONTEXT_LRCA_DIRTY, &ce->flags) ||
		!lrc_desc_registered(ce_to_guc(ce), ce->guc_id.id)) &&
		!submission_disabled(ce_to_guc(ce));
}

static void guc_context_init(struct intel_context *ce)
{
	const struct i915_gem_context *ctx;
	int prio = I915_CONTEXT_DEFAULT_PRIORITY;

	rcu_read_lock();
	ctx = rcu_dereference(ce->gem_context);
	if (ctx)
		prio = ctx->sched.priority;
	rcu_read_unlock();

	ce->guc_state.prio = map_i915_prio_to_guc_prio(prio);
}

static int guc_request_alloc(struct i915_request *rq)
{
	struct intel_context *ce = request_to_scheduling_context(rq);
	struct intel_guc *guc = ce_to_guc(ce);
	unsigned long flags;
	int ret;

	GEM_BUG_ON(!intel_context_is_pinned(rq->context));

	/*
	 * Flush enough space to reduce the likelihood of waiting after
	 * we start building the request - in which case we will just
	 * have to repeat work.
	 */
	rq->reserved_space += GUC_REQUEST_SIZE;

	/*
	 * Note that after this point, we have committed to using
	 * this request as it is being used to both track the
	 * state of engine initialisation and liveness of the
	 * golden renderstate above. Think twice before you try
	 * to cancel/unwind this request now.
	 */

	/* Unconditionally invalidate GPU caches and TLBs. */
	ret = rq->engine->emit_flush(rq, EMIT_INVALIDATE);
	if (ret)
		return ret;

	rq->reserved_space -= GUC_REQUEST_SIZE;

	if (unlikely(!test_bit(CONTEXT_GUC_INIT, &ce->flags)))
		guc_context_init(ce);

	/*
	 * Call pin_guc_id here rather than in the pinning step as with
	 * dma_resv, contexts can be repeatedly pinned / unpinned trashing the
	 * guc_id and creating horrible race conditions. This is especially bad
	 * when guc_id are being stolen due to over subscription. By the time
	 * this function is reached, it is guaranteed that the guc_id will be
	 * persistent until the generated request is retired. Thus, sealing these
	 * race conditions. It is still safe to fail here if guc_id are
	 * exhausted and return -EAGAIN to the user indicating that they can try
	 * again in the future.
	 *
	 * There is no need for a lock here as the timeline mutex (or
	 * parallel_submit mutex in the case of multi-lrc) ensures at most one
	 * context can be executing this code path at once. The guc_id_ref is
	 * incremented once for every request in flight and decremented on each
	 * retire. When it is zero, a lock around the increment (in pin_guc_id)
	 * is needed to seal a race with unpin_guc_id.
	 */
	if (atomic_add_unless(&ce->guc_id.ref, 1, 0))
		goto out;

	ret = pin_guc_id(guc, ce);	/* returns 1 if new guc_id assigned */
	if (unlikely(ret < 0))
		return ret;
	if (context_needs_register(ce, !!ret)) {
		ret = guc_lrc_desc_pin(ce, true);
		if (unlikely(ret)) {	/* unwind */
			if (ret == -EPIPE) {
				disable_submission(guc);
				goto out;	/* GPU will be reset */
			}
			atomic_dec(&ce->guc_id.ref);
			unpin_guc_id(guc, ce);
			return ret;
		}
	}

	clear_bit(CONTEXT_LRCA_DIRTY, &ce->flags);

out:
	/*
	 * We block all requests on this context if a G2H is pending for a
	 * schedule disable or context deregistration as the GuC will fail a
	 * schedule enable or context registration if either G2H is pending
	 * respectfully. Once a G2H returns, the fence is released that is
	 * blocking these requests (see guc_signal_context_fence).
	 */
	spin_lock_irqsave(&ce->guc_state.lock, flags);
	if (context_wait_for_deregister_to_register(ce) ||
	    context_pending_disable(ce)) {
		init_irq_work(&rq->submit_work, submit_work_cb);
		i915_sw_fence_await(&rq->submit);

		list_add_tail(&rq->guc_fence_link, &ce->guc_state.fences);
	}
	incr_context_committed_requests(ce);
	spin_unlock_irqrestore(&ce->guc_state.lock, flags);

	return 0;
}

static int guc_virtual_context_pre_pin(struct intel_context *ce,
				       struct i915_gem_ww_ctx *ww,
				       void **vaddr)
{
	struct intel_engine_cs *engine = guc_virtual_get_sibling(ce->engine, 0);

	return __guc_context_pre_pin(ce, engine, ww, vaddr);
}

static int guc_virtual_context_pin(struct intel_context *ce, void *vaddr)
{
	struct intel_engine_cs *engine = guc_virtual_get_sibling(ce->engine, 0);
	int ret = __guc_context_pin(ce, engine, vaddr);
	intel_engine_mask_t tmp, mask = ce->engine->mask;

	if (likely(!ret))
		for_each_engine_masked(engine, ce->engine->gt, mask, tmp)
			intel_engine_pm_get(engine);

	return ret;
}

static void guc_virtual_context_unpin(struct intel_context *ce)
{
	intel_engine_mask_t tmp, mask = ce->engine->mask;
	struct intel_engine_cs *engine;
	struct intel_guc *guc = ce_to_guc(ce);

	GEM_BUG_ON(context_enabled(ce));
	GEM_BUG_ON(intel_context_is_barrier(ce));

	unpin_guc_id(guc, ce);
	lrc_unpin(ce);

	for_each_engine_masked(engine, ce->engine->gt, mask, tmp)
		intel_engine_pm_put_async(engine);
}

static void guc_virtual_context_enter(struct intel_context *ce)
{
	intel_engine_mask_t tmp, mask = ce->engine->mask;
	struct intel_engine_cs *engine;

	for_each_engine_masked(engine, ce->engine->gt, mask, tmp)
		intel_engine_pm_get(engine);

	intel_timeline_enter(ce->timeline);
}

static void guc_virtual_context_exit(struct intel_context *ce)
{
	intel_engine_mask_t tmp, mask = ce->engine->mask;
	struct intel_engine_cs *engine;

	for_each_engine_masked(engine, ce->engine->gt, mask, tmp)
		intel_engine_pm_put(engine);

	intel_timeline_exit(ce->timeline);
}

static int guc_virtual_context_alloc(struct intel_context *ce)
{
	struct intel_engine_cs *engine = guc_virtual_get_sibling(ce->engine, 0);

	return lrc_alloc(ce, engine);
}

static const struct intel_context_ops virtual_guc_context_ops = {
	.alloc = guc_virtual_context_alloc,

	.pre_pin = guc_virtual_context_pre_pin,
	.pin = guc_virtual_context_pin,
	.unpin = guc_virtual_context_unpin,
	.post_unpin = guc_context_post_unpin,

	.ban = guc_context_ban,

	.cancel_request = guc_context_cancel_request,

	.enter = guc_virtual_context_enter,
	.exit = guc_virtual_context_exit,

	.sched_disable = guc_context_sched_disable,

	.destroy = guc_context_destroy,

	.get_sibling = guc_virtual_get_sibling,
};

static int guc_parent_context_pin(struct intel_context *ce, void *vaddr)
{
	struct intel_engine_cs *engine = guc_virtual_get_sibling(ce->engine, 0);
	struct intel_guc *guc = ce_to_guc(ce);
	int ret;

	GEM_BUG_ON(!intel_context_is_parent(ce));
	GEM_BUG_ON(!intel_engine_is_virtual(ce->engine));

	ret = pin_guc_id(guc, ce);
	if (unlikely(ret < 0))
		return ret;

	return __guc_context_pin(ce, engine, vaddr);
}

static int guc_child_context_pin(struct intel_context *ce, void *vaddr)
{
	struct intel_engine_cs *engine = guc_virtual_get_sibling(ce->engine, 0);

	GEM_BUG_ON(!intel_context_is_child(ce));
	GEM_BUG_ON(!intel_engine_is_virtual(ce->engine));

	__intel_context_pin(ce->parent);
	return __guc_context_pin(ce, engine, vaddr);
}

static void guc_parent_context_unpin(struct intel_context *ce)
{
	struct intel_guc *guc = ce_to_guc(ce);

	GEM_BUG_ON(context_enabled(ce));
	GEM_BUG_ON(intel_context_is_barrier(ce));
	GEM_BUG_ON(!intel_context_is_parent(ce));
	GEM_BUG_ON(!intel_engine_is_virtual(ce->engine));

	unpin_guc_id(guc, ce);
	lrc_unpin(ce);
}

static void guc_child_context_unpin(struct intel_context *ce)
{
	GEM_BUG_ON(context_enabled(ce));
	GEM_BUG_ON(intel_context_is_barrier(ce));
	GEM_BUG_ON(!intel_context_is_child(ce));
	GEM_BUG_ON(!intel_engine_is_virtual(ce->engine));

	lrc_unpin(ce);
}

static void guc_child_context_post_unpin(struct intel_context *ce)
{
	GEM_BUG_ON(!intel_context_is_child(ce));
	GEM_BUG_ON(!intel_context_is_pinned(ce->parent));
	GEM_BUG_ON(!intel_engine_is_virtual(ce->engine));

	lrc_post_unpin(ce);
	intel_context_unpin(ce->parent);
}

static void guc_child_context_destroy(struct kref *kref)
{
	struct intel_context *ce = container_of(kref, typeof(*ce), ref);

	__guc_context_destroy(ce);
}

static const struct intel_context_ops virtual_parent_context_ops = {
	.alloc = guc_virtual_context_alloc,

	.pre_pin = guc_context_pre_pin,
	.pin = guc_parent_context_pin,
	.unpin = guc_parent_context_unpin,
	.post_unpin = guc_context_post_unpin,

	.ban = guc_context_ban,

	.cancel_request = guc_context_cancel_request,

	.enter = guc_virtual_context_enter,
	.exit = guc_virtual_context_exit,

	.sched_disable = guc_context_sched_disable,

	.destroy = guc_context_destroy,

	.get_sibling = guc_virtual_get_sibling,
};

static const struct intel_context_ops virtual_child_context_ops = {
	.alloc = guc_virtual_context_alloc,

	.pre_pin = guc_context_pre_pin,
	.pin = guc_child_context_pin,
	.unpin = guc_child_context_unpin,
	.post_unpin = guc_child_context_post_unpin,

	.cancel_request = guc_context_cancel_request,

	.enter = guc_virtual_context_enter,
	.exit = guc_virtual_context_exit,

	.destroy = guc_child_context_destroy,

	.get_sibling = guc_virtual_get_sibling,
};

/*
 * The below override of the breadcrumbs is enabled when the user configures a
 * context for parallel submission (multi-lrc, parent-child).
 *
 * The overridden breadcrumbs implements an algorithm which allows the GuC to
 * safely preempt all the hw contexts configured for parallel submission
 * between each BB. The contract between the i915 and GuC is if the parent
 * context can be preempted, all the children can be preempted, and the GuC will
 * always try to preempt the parent before the children. A handshake between the
 * parent / children breadcrumbs ensures the i915 holds up its end of the deal
 * creating a window to preempt between each set of BBs.
 */
static int emit_bb_start_parent_no_preempt_mid_batch(struct i915_request *rq,
						     u64 offset, u32 len,
						     const unsigned int flags);
static int emit_bb_start_child_no_preempt_mid_batch(struct i915_request *rq,
						    u64 offset, u32 len,
						    const unsigned int flags);
static u32 *
emit_fini_breadcrumb_parent_no_preempt_mid_batch(struct i915_request *rq,
						 u32 *cs);
static u32 *
emit_fini_breadcrumb_child_no_preempt_mid_batch(struct i915_request *rq,
						u32 *cs);

static struct intel_context *
guc_create_parallel(struct intel_engine_cs **engines,
		    unsigned int num_siblings,
		    unsigned int width)
{
	struct intel_engine_cs **siblings = NULL;
	struct intel_context *parent = NULL, *ce, *err;
	int i, j;

	siblings = kmalloc_array(num_siblings,
				 sizeof(*siblings),
				 GFP_KERNEL);
	if (!siblings)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < width; ++i) {
		for (j = 0; j < num_siblings; ++j)
			siblings[j] = engines[i * num_siblings + j];

		ce = intel_engine_create_virtual(siblings, num_siblings,
						 FORCE_VIRTUAL);
		if (!ce) {
			err = ERR_PTR(-ENOMEM);
			goto unwind;
		}

		if (i == 0) {
			parent = ce;
			parent->ops = &virtual_parent_context_ops;
		} else {
			ce->ops = &virtual_child_context_ops;
			intel_context_bind_parent_child(parent, ce);
		}
	}

	parent->fence_context = dma_fence_context_alloc(1);

	parent->engine->emit_bb_start =
		emit_bb_start_parent_no_preempt_mid_batch;
	parent->engine->emit_fini_breadcrumb =
		emit_fini_breadcrumb_parent_no_preempt_mid_batch;
	parent->engine->emit_fini_breadcrumb_dw =
		12 + 4 * parent->guc_number_children;
	for_each_child(parent, ce) {
		ce->engine->emit_bb_start =
			emit_bb_start_child_no_preempt_mid_batch;
		ce->engine->emit_fini_breadcrumb =
			emit_fini_breadcrumb_child_no_preempt_mid_batch;
		ce->engine->emit_fini_breadcrumb_dw = 16;
	}

	kfree(siblings);
	return parent;

unwind:
	if (parent)
		intel_context_put(parent);
	kfree(siblings);
	return err;
}

static bool
guc_irq_enable_breadcrumbs(struct intel_breadcrumbs *b)
{
	struct intel_engine_cs *sibling;
	intel_engine_mask_t tmp, mask = b->engine_mask;
	bool result = false;

	for_each_engine_masked(sibling, b->irq_engine->gt, mask, tmp)
		result |= intel_engine_irq_enable(sibling);

	return result;
}

static void
guc_irq_disable_breadcrumbs(struct intel_breadcrumbs *b)
{
	struct intel_engine_cs *sibling;
	intel_engine_mask_t tmp, mask = b->engine_mask;

	for_each_engine_masked(sibling, b->irq_engine->gt, mask, tmp)
		intel_engine_irq_disable(sibling);
}

static void guc_init_breadcrumbs(struct intel_engine_cs *engine)
{
	int i;

	/*
	 * In GuC submission mode we do not know which physical engine a request
	 * will be scheduled on, this creates a problem because the breadcrumb
	 * interrupt is per physical engine. To work around this we attach
	 * requests and direct all breadcrumb interrupts to the first instance
	 * of an engine per class. In addition all breadcrumb interrupts are
	 * enabled / disabled across an engine class in unison.
	 */
	for (i = 0; i < MAX_ENGINE_INSTANCE; ++i) {
		struct intel_engine_cs *sibling =
			engine->gt->engine_class[engine->class][i];

		if (sibling) {
			if (engine->breadcrumbs != sibling->breadcrumbs) {
				intel_breadcrumbs_put(engine->breadcrumbs);
				engine->breadcrumbs =
					intel_breadcrumbs_get(sibling->breadcrumbs);
			}
			break;
		}
	}

	if (engine->breadcrumbs) {
		engine->breadcrumbs->engine_mask |= engine->mask;
		engine->breadcrumbs->irq_enable = guc_irq_enable_breadcrumbs;
		engine->breadcrumbs->irq_disable = guc_irq_disable_breadcrumbs;
	}
}

static void guc_bump_inflight_request_prio(struct i915_request *rq,
					   int prio)
{
	struct intel_context *ce = request_to_scheduling_context(rq);
	u8 new_guc_prio = map_i915_prio_to_guc_prio(prio);

	/* Short circuit function */
	if (prio < I915_PRIORITY_NORMAL ||
	    rq->guc_prio == GUC_PRIO_FINI ||
	    (rq->guc_prio != GUC_PRIO_INIT &&
	     !new_guc_prio_higher(rq->guc_prio, new_guc_prio)))
		return;

	spin_lock(&ce->guc_state.lock);
	if (rq->guc_prio != GUC_PRIO_FINI) {
		if (rq->guc_prio != GUC_PRIO_INIT)
			sub_context_inflight_prio(ce, rq->guc_prio);
		rq->guc_prio = new_guc_prio;
		add_context_inflight_prio(ce, rq->guc_prio);
		update_context_prio(ce);
	}
	spin_unlock(&ce->guc_state.lock);
}

static void guc_retire_inflight_request_prio(struct i915_request *rq)
{
	struct intel_context *ce = request_to_scheduling_context(rq);

	spin_lock(&ce->guc_state.lock);
	guc_prio_fini(rq, ce);
	spin_unlock(&ce->guc_state.lock);
}

static void sanitize_hwsp(struct intel_engine_cs *engine)
{
	struct intel_timeline *tl;

	list_for_each_entry(tl, &engine->status_page.timelines, engine_link)
		intel_timeline_reset_seqno(tl);
}

static void guc_sanitize(struct intel_engine_cs *engine)
{
	/*
	 * Poison residual state on resume, in case the suspend didn't!
	 *
	 * We have to assume that across suspend/resume (or other loss
	 * of control) that the contents of our pinned buffers has been
	 * lost, replaced by garbage. Since this doesn't always happen,
	 * let's poison such state so that we more quickly spot when
	 * we falsely assume it has been preserved.
	 */
	if (IS_ENABLED(CONFIG_DRM_I915_DEBUG_GEM))
		memset(engine->status_page.addr, POISON_INUSE, PAGE_SIZE);

	/*
	 * The kernel_context HWSP is stored in the status_page. As above,
	 * that may be lost on resume/initialisation, and so we need to
	 * reset the value in the HWSP.
	 */
	sanitize_hwsp(engine);

	/* And scrub the dirty cachelines for the HWSP */
	clflush_cache_range(engine->status_page.addr, PAGE_SIZE);
}

static void setup_hwsp(struct intel_engine_cs *engine)
{
	intel_engine_set_hwsp_writemask(engine, ~0u); /* HWSTAM */

	ENGINE_WRITE_FW(engine,
			RING_HWS_PGA,
			i915_ggtt_offset(engine->status_page.vma));
}

static void start_engine(struct intel_engine_cs *engine)
{
	ENGINE_WRITE_FW(engine,
			RING_MODE_GEN7,
			_MASKED_BIT_ENABLE(GEN11_GFX_DISABLE_LEGACY_MODE));

	ENGINE_WRITE_FW(engine, RING_MI_MODE, _MASKED_BIT_DISABLE(STOP_RING));
	ENGINE_POSTING_READ(engine, RING_MI_MODE);
}

static int guc_resume(struct intel_engine_cs *engine)
{
	assert_forcewakes_active(engine->uncore, FORCEWAKE_ALL);

	intel_mocs_init_engine(engine);

	intel_breadcrumbs_reset(engine->breadcrumbs);

	setup_hwsp(engine);
	start_engine(engine);

	return 0;
}

static int vf_guc_resume(struct intel_engine_cs *engine)
{
	intel_breadcrumbs_reset(engine->breadcrumbs);
	return 0;
}

static int gen12_rcs_resume(struct intel_engine_cs *engine)
{
	int ret;

	ret = guc_resume(engine);
	if (ret)
		return ret;

	/*
	 * Multi Context programming.
	 * just need to program this register once no matter how many CCS
	 * engines there are. Since some of the CCS engines might be fused off,
	 * we can't do this as part of the init of a specific CCS and we do
	 * it during RCS init instead. RCS and all CCS engines are reset
	 * together, so post-reset re-init is covered as well.
	 */
	if (CCS_MASK(engine->gt))
		intel_uncore_write(engine->uncore, GEN12_RCU_MODE,
			   _MASKED_BIT_ENABLE(GEN12_RCU_MODE_CCS_ENABLE));

       return 0;
}

static bool guc_sched_engine_disabled(struct i915_sched_engine *sched_engine)
{
	return !sched_engine->tasklet.callback;
}

static void guc_set_default_submission(struct intel_engine_cs *engine)
{
	engine->submit_request = guc_submit_request;
}

static void guc_kernel_context_pin(struct intel_guc *guc,
				   struct intel_context *ce)
{
	if (context_guc_id_invalid(ce))
		pin_guc_id(guc, ce);
	guc_lrc_desc_pin(ce, true);
}

static void guc_init_lrc_mapping(struct intel_guc *guc)
{
	struct intel_gt *gt = guc_to_gt(guc);
	struct intel_engine_cs *engine;
	enum intel_engine_id id;

	/* make sure all descriptors are clean... */
	xa_destroy(&guc->context_lookup);

	/*
	 * Some contexts might have been pinned before we enabled GuC
	 * submission, so we need to add them to the GuC bookeeping.
	 * Also, after a reset the of the GuC we want to make sure that the
	 * information shared with GuC is properly reset. The kernel LRCs are
	 * not attached to the gem_context, so they need to be added separately.
	 *
	 * Note: we purposefully do not check the return of guc_lrc_desc_pin,
	 * because that function can only fail if a reset is just starting. This
	 * is at the end of reset so presumably another reset isn't happening
	 * and even it did this code would be run again.
	 */

	for_each_engine(engine, gt, id)
		if (engine->kernel_context)
			guc_kernel_context_pin(guc, engine->kernel_context);
}

static void guc_release(struct intel_engine_cs *engine)
{
	engine->sanitize = NULL; /* no longer in control, nothing to sanitize */

	intel_engine_cleanup_common(engine);
	lrc_fini_wa_ctx(engine);
}

static void virtual_guc_bump_serial(struct intel_engine_cs *engine)
{
	struct intel_engine_cs *e;
	intel_engine_mask_t tmp, mask = engine->mask;

	for_each_engine_masked(e, engine->gt, mask, tmp)
		e->serial++;
}

static void guc_default_vfuncs(struct intel_engine_cs *engine)
{
	/* Default vfuncs which can be overridden by each engine. */

	engine->resume = guc_resume;

	engine->cops = &guc_context_ops;
	engine->request_alloc = guc_request_alloc;
	engine->add_active_request = add_to_context;
	engine->remove_active_request = remove_from_context;

	engine->sched_engine->schedule = i915_schedule;

	engine->reset.prepare = guc_reset_nop;
	engine->reset.rewind = guc_rewind_nop;
	engine->reset.cancel = guc_reset_nop;
	engine->reset.finish = guc_reset_nop;

	engine->emit_flush = gen8_emit_flush_xcs;
	engine->emit_init_breadcrumb = gen8_emit_init_breadcrumb;
	engine->emit_fini_breadcrumb = gen8_emit_fini_breadcrumb_xcs;
	if (GRAPHICS_VER(engine->i915) >= 12) {
		engine->emit_fini_breadcrumb = gen12_emit_fini_breadcrumb_xcs;
		engine->emit_flush = gen12_emit_flush_xcs;
	}
	engine->set_default_submission = guc_set_default_submission;

	engine->flags |= I915_ENGINE_HAS_PREEMPTION;
	engine->flags |= I915_ENGINE_HAS_TIMESLICES;

	/*
	 * TODO: GuC supports timeslicing and semaphores as well, but they're
	 * handled by the firmware so some minor tweaks are required before
	 * enabling.
	 *
	 * engine->flags |= I915_ENGINE_HAS_SEMAPHORES;
	 */

	engine->emit_bb_start = gen8_emit_bb_start;
}

static void rcs_submission_override(struct intel_engine_cs *engine)
{
	switch (GRAPHICS_VER(engine->i915)) {
	case 12:
		engine->emit_flush = gen12_emit_flush_rcs;
		engine->emit_fini_breadcrumb = gen12_emit_fini_breadcrumb_rcs;
		break;
	case 11:
		engine->emit_flush = gen11_emit_flush_rcs;
		engine->emit_fini_breadcrumb = gen11_emit_fini_breadcrumb_rcs;
		break;
	default:
		engine->emit_flush = gen8_emit_flush_rcs;
		engine->emit_fini_breadcrumb = gen8_emit_fini_breadcrumb_rcs;
		break;
	}

	if (engine->class == RENDER_CLASS)
		engine->resume = gen12_rcs_resume;
}

static void guc_default_irqs(struct intel_engine_cs *engine)
{
	engine->irq_keep_mask = GT_RENDER_USER_INTERRUPT;
	intel_engine_set_irq_handler(engine, cs_irq_handler);
}

static void guc_sched_engine_destroy(struct kref *kref)
{
	struct i915_sched_engine *sched_engine =
		container_of(kref, typeof(*sched_engine), ref);
	struct intel_guc *guc = sched_engine->private_data;

	guc->sched_engine = NULL;
	tasklet_kill(&sched_engine->tasklet); /* flush the callback */
	kfree(sched_engine);
}

int intel_guc_submission_setup(struct intel_engine_cs *engine)
{
	struct drm_i915_private *i915 = engine->i915;
	struct intel_guc *guc = &engine->gt->uc.guc;

	/*
	 * The setup relies on several assumptions (e.g. irqs always enabled)
	 * that are only valid on gen11+
	 */
	GEM_BUG_ON(GRAPHICS_VER(i915) < 11);

	if (!guc->sched_engine) {
		guc->sched_engine = i915_sched_engine_create(ENGINE_VIRTUAL);
		if (!guc->sched_engine)
			return -ENOMEM;

		guc->sched_engine->schedule = i915_schedule;
		guc->sched_engine->disabled = guc_sched_engine_disabled;
		guc->sched_engine->private_data = guc;
		guc->sched_engine->destroy = guc_sched_engine_destroy;
		guc->sched_engine->bump_inflight_request_prio =
			guc_bump_inflight_request_prio;
		guc->sched_engine->retire_inflight_request_prio =
			guc_retire_inflight_request_prio;
		tasklet_setup(&guc->sched_engine->tasklet,
			      guc_submission_tasklet);
	}
	i915_sched_engine_put(engine->sched_engine);
	engine->sched_engine = i915_sched_engine_get(guc->sched_engine);

	guc_default_vfuncs(engine);
	guc_default_irqs(engine);
	guc_init_breadcrumbs(engine);

	if (engine->class == RENDER_CLASS ||
	    engine->class == COMPUTE_CLASS)
		rcs_submission_override(engine);

	if (IS_SRIOV_VF(engine->i915))
		engine->resume = vf_guc_resume;

	lrc_init_wa_ctx(engine);

	/* Finally, take ownership and responsibility for cleanup! */
	engine->sanitize = guc_sanitize;
	engine->release = guc_release;

	return 0;
}

void intel_guc_submission_enable(struct intel_guc *guc)
{
	guc_init_lrc_mapping(guc);
}

void intel_guc_submission_disable(struct intel_guc *guc)
{
	/* Note: By the time we're here, GuC may have already been reset */
}

static bool __guc_submission_supported(struct intel_guc *guc)
{
	/* GuC submission is unavailable for pre-Gen11 */
	return intel_guc_is_supported(guc) &&
	       GRAPHICS_VER(guc_to_gt(guc)->i915) >= 11;
}

static bool __guc_submission_selected(struct intel_guc *guc)
{
	struct drm_i915_private *i915 = guc_to_gt(guc)->i915;

	if (!intel_guc_submission_is_supported(guc))
		return false;

	return i915->params.enable_guc & ENABLE_GUC_SUBMISSION;
}

void intel_guc_submission_init_early(struct intel_guc *guc)
{
	guc->submission_state.max_guc_ids = GUC_MAX_LRC_DESCRIPTORS;
	guc->submission_state.num_guc_ids = GUC_MAX_LRC_DESCRIPTORS;
	guc->submission_supported = __guc_submission_supported(guc);
	guc->submission_selected = __guc_submission_selected(guc);
}

static inline u32 get_children_go_addr(struct intel_context *ce)
{
	GEM_BUG_ON(!intel_context_is_parent(ce));

	return i915_ggtt_offset(ce->state) +
		__get_process_desc_offset(ce) +
		sizeof(struct guc_process_desc);
}

static inline u32 get_children_join_addr(struct intel_context *ce,
					 u8 child_index)
{
	GEM_BUG_ON(!intel_context_is_parent(ce));

	return get_children_go_addr(ce) + (child_index + 1) * CACHELINE_BYTES;
}

#define PARENT_GO_BB			1
#define PARENT_GO_FINI_BREADCRUMB	0
#define CHILD_GO_BB			1
#define CHILD_GO_FINI_BREADCRUMB	0
static int emit_bb_start_parent_no_preempt_mid_batch(struct i915_request *rq,
						     u64 offset, u32 len,
						     const unsigned int flags)
{
	struct intel_context *ce = rq->context;
	u32 *cs;
	u8 i;

	GEM_BUG_ON(!intel_context_is_parent(ce));

	cs = intel_ring_begin(rq, 10 + 4 * ce->guc_number_children);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	/* Wait on chidlren */
	for (i = 0; i < ce->guc_number_children; ++i) {
		*cs++ = (MI_SEMAPHORE_WAIT |
			 MI_SEMAPHORE_GLOBAL_GTT |
			 MI_SEMAPHORE_POLL |
			 MI_SEMAPHORE_SAD_EQ_SDD);
		*cs++ = PARENT_GO_BB;
		*cs++ = get_children_join_addr(ce, i);
		*cs++ = 0;
	}

	/* Turn off preemption */
	*cs++ = MI_ARB_ON_OFF | MI_ARB_DISABLE;
	*cs++ = MI_NOOP;

	/* Tell children go */
	cs = gen8_emit_ggtt_write(cs,
				  CHILD_GO_BB,
				  get_children_go_addr(ce),
				  0);

	/* Jump to batch */
	*cs++ = MI_BATCH_BUFFER_START_GEN8 |
		(flags & I915_DISPATCH_SECURE ? 0 : BIT(8));
	*cs++ = lower_32_bits(offset);
	*cs++ = upper_32_bits(offset);
	*cs++ = MI_NOOP;

	intel_ring_advance(rq, cs);

	return 0;
}

static int emit_bb_start_child_no_preempt_mid_batch(struct i915_request *rq,
						    u64 offset, u32 len,
						    const unsigned int flags)
{
	struct intel_context *ce = rq->context;
	u32 *cs;

	GEM_BUG_ON(!intel_context_is_child(ce));

	cs = intel_ring_begin(rq, 12);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	/* Signal parent */
	cs = gen8_emit_ggtt_write(cs,
				  PARENT_GO_BB,
				  get_children_join_addr(ce->parent,
							 ce->guc_child_index),
				  0);

	/* Wait parent on for go */
	*cs++ = (MI_SEMAPHORE_WAIT |
		 MI_SEMAPHORE_GLOBAL_GTT |
		 MI_SEMAPHORE_POLL |
		 MI_SEMAPHORE_SAD_EQ_SDD);
	*cs++ = CHILD_GO_BB;
	*cs++ = get_children_go_addr(ce->parent);
	*cs++ = 0;

	/* Turn off preemption */
	*cs++ = MI_ARB_ON_OFF | MI_ARB_DISABLE;

	/* Jump to batch */
	*cs++ = MI_BATCH_BUFFER_START_GEN8 |
		(flags & I915_DISPATCH_SECURE ? 0 : BIT(8));
	*cs++ = lower_32_bits(offset);
	*cs++ = upper_32_bits(offset);

	intel_ring_advance(rq, cs);

	return 0;
}

static u32 *
__emit_fini_breadcrumb_parent_no_preempt_mid_batch(struct i915_request *rq,
						   u32 *cs)
{
	struct intel_context *ce = rq->context;
	u8 i;

	GEM_BUG_ON(!intel_context_is_parent(ce));

	/* Wait on children */
	for (i = 0; i < ce->guc_number_children; ++i) {
		*cs++ = (MI_SEMAPHORE_WAIT |
			 MI_SEMAPHORE_GLOBAL_GTT |
			 MI_SEMAPHORE_POLL |
			 MI_SEMAPHORE_SAD_EQ_SDD);
		*cs++ = PARENT_GO_FINI_BREADCRUMB;
		*cs++ = get_children_join_addr(ce, i);
		*cs++ = 0;
	}

	/* Turn on preemption */
	*cs++ = MI_ARB_ON_OFF | MI_ARB_ENABLE;
	*cs++ = MI_NOOP;

	/* Tell children go */
	cs = gen8_emit_ggtt_write(cs,
				  CHILD_GO_FINI_BREADCRUMB,
				  get_children_go_addr(ce),
				  0);

	return cs;
}

/*
 * If this true, a submission of multi-lrc requests had an error and the
 * requests need to be skipped. The front end (execuf IOCTL) should've called
 * i915_request_skip which squashes the BB but we still need to emit the fini
 * breadrcrumbs seqno write. At this point we don't know how many of the
 * requests in the multi-lrc submission were generated so we can't do the
 * handshake between the parent and children (e.g. if 4 requests should be
 * generated but 2nd hit an error only 1 would be seen by the GuC backend).
 * Simply skip the handshake, but still emit the breadcrumbd seqno, if an error
 * has occurred on any of the requests in submission / relationship.
 */
static inline bool skip_handshake(struct i915_request *rq)
{
	return test_bit(I915_FENCE_FLAG_SKIP_PARALLEL, &rq->fence.flags);
}

static u32 *
emit_fini_breadcrumb_parent_no_preempt_mid_batch(struct i915_request *rq,
						 u32 *cs)
{
	struct intel_context *ce = rq->context;

	GEM_BUG_ON(!intel_context_is_parent(ce));

	if (unlikely(skip_handshake(rq))) {
		memset(cs, 0, sizeof(u32) *
		       (ce->engine->emit_fini_breadcrumb_dw - 6));
		cs += ce->engine->emit_fini_breadcrumb_dw - 6;
	} else {
		cs = __emit_fini_breadcrumb_parent_no_preempt_mid_batch(rq, cs);
	}

	/* Emit fini breadcrumb */
	cs = gen8_emit_ggtt_write(cs,
				  rq->fence.seqno,
				  i915_request_active_timeline(rq)->hwsp_offset,
				  0);

	/* User interrupt */
	*cs++ = MI_USER_INTERRUPT;
	*cs++ = MI_NOOP;

	rq->tail = intel_ring_offset(rq, cs);

	return cs;
}

static u32 *
__emit_fini_breadcrumb_child_no_preempt_mid_batch(struct i915_request *rq,
						  u32 *cs)
{
	struct intel_context *ce = rq->context;

	GEM_BUG_ON(!intel_context_is_child(ce));

	/* Turn on preemption */
	*cs++ = MI_ARB_ON_OFF | MI_ARB_ENABLE;
	*cs++ = MI_NOOP;

	/* Signal parent */
	cs = gen8_emit_ggtt_write(cs,
				  PARENT_GO_FINI_BREADCRUMB,
				  get_children_join_addr(ce->parent,
							 ce->guc_child_index),
				  0);

	/* Wait parent on for go */
	*cs++ = (MI_SEMAPHORE_WAIT |
		 MI_SEMAPHORE_GLOBAL_GTT |
		 MI_SEMAPHORE_POLL |
		 MI_SEMAPHORE_SAD_EQ_SDD);
	*cs++ = CHILD_GO_FINI_BREADCRUMB;
	*cs++ = get_children_go_addr(ce->parent);
	*cs++ = 0;

	return cs;
}

static u32 *
emit_fini_breadcrumb_child_no_preempt_mid_batch(struct i915_request *rq,
						u32 *cs)
{
	struct intel_context *ce = rq->context;

	GEM_BUG_ON(!intel_context_is_child(ce));

	if (unlikely(skip_handshake(rq))) {
		memset(cs, 0, sizeof(u32) *
		       (ce->engine->emit_fini_breadcrumb_dw - 6));
		cs += ce->engine->emit_fini_breadcrumb_dw - 6;
	} else {
		cs = __emit_fini_breadcrumb_child_no_preempt_mid_batch(rq, cs);
	}

	/* Emit fini breadcrumb */
	cs = gen8_emit_ggtt_write(cs,
				  rq->fence.seqno,
				  i915_request_active_timeline(rq)->hwsp_offset,
				  0);

	/* User interrupt */
	*cs++ = MI_USER_INTERRUPT;
	*cs++ = MI_NOOP;

	rq->tail = intel_ring_offset(rq, cs);

	return cs;
}

static struct intel_context *
g2h_context_lookup(struct intel_guc *guc, u32 desc_idx)
{
	struct intel_context *ce;

	if (unlikely(desc_idx >= guc->submission_state.max_guc_ids)) {
		drm_err(&guc_to_gt(guc)->i915->drm,
			"Invalid desc_idx %u, max %u",
			desc_idx, guc->submission_state.max_guc_ids);
		return NULL;
	}

	ce = __get_context(guc, desc_idx);
	if (unlikely(!ce)) {
		drm_err(&guc_to_gt(guc)->i915->drm,
			"Context is NULL, desc_idx %u", desc_idx);
		return NULL;
	}

	if (unlikely(intel_context_is_child(ce))) {
		drm_err(&guc_to_gt(guc)->i915->drm,
			"Context is child, desc_idx %u", desc_idx);
		return NULL;
	}

	return ce;
}

static void wait_wake_outstanding_tlb_g2h(struct intel_guc *guc, u32 seqno)
{
	struct intel_guc_tlb_wait *wait;
	unsigned long flags;

	xa_lock_irqsave(&guc->tlb_lookup, flags);
	wait = xa_load(&guc->tlb_lookup, seqno);

	/* We received a response after the waiting task did exit with a timeout */
	if (unlikely(!wait))
		drm_dbg(&guc_to_gt(guc)->i915->drm, "Stale tlb invalidation response with seqno %d\n", seqno);

	if (wait) {
		WRITE_ONCE(wait->status, 0);
		smp_mb();
		wake_up_process(wait->tsk);
	}
	xa_unlock_irqrestore(&guc->tlb_lookup, flags);
}

void  intel_guc_tlb_invalidation_done_process_msg(struct intel_guc *guc, u32 seqno)
{
	wait_wake_outstanding_tlb_g2h(guc, seqno);
}

int intel_guc_deregister_done_process_msg(struct intel_guc *guc,
					  const u32 *msg,
					  u32 len)
{
	struct intel_context *ce;
	u32 desc_idx = msg[0];

	if (unlikely(len < 1)) {
		drm_err(&guc_to_gt(guc)->i915->drm, "Invalid length %u", len);
		return -EPROTO;
	}

	ce = g2h_context_lookup(guc, desc_idx);
	if (unlikely(!ce))
		return -EPROTO;

	trace_intel_context_deregister_done(ce);

#ifdef CONFIG_DRM_I915_SELFTEST
	if (unlikely(ce->drop_deregister)) {
		ce->drop_deregister = false;
		return 0;
	}
#endif

	if (context_wait_for_deregister_to_register(ce)) {
		struct intel_runtime_pm *runtime_pm =
			&ce->engine->gt->i915->runtime_pm;
		intel_wakeref_t wakeref;

		/*
		 * Previous owner of this guc_id has been deregistered, now safe
		 * register this context.
		 */
		with_intel_runtime_pm(runtime_pm, wakeref)
			register_context(ce, true);
		guc_signal_context_fence(ce);
		intel_context_put(ce);
	} else if (context_destroyed(ce)) {
		/* Context has been destroyed */
		intel_gt_pm_put_async(guc_to_gt(guc));
		release_guc_id(guc, ce);
		__guc_context_destroy(ce);
	}

	decr_outstanding_submission_g2h(guc);

	return 0;
}

int intel_guc_sched_done_process_msg(struct intel_guc *guc,
				     const u32 *msg,
				     u32 len)
{
	struct intel_context *ce;
	unsigned long flags;
	u32 desc_idx = msg[0];

	if (unlikely(len < 2)) {
		drm_err(&guc_to_gt(guc)->i915->drm, "Invalid length %u", len);
		return -EPROTO;
	}

	ce = g2h_context_lookup(guc, desc_idx);
	if (unlikely(!ce))
		return -EPROTO;

	if (unlikely(context_destroyed(ce) ||
		     (!context_pending_enable(ce) &&
		     !context_pending_disable(ce)))) {
		drm_err(&guc_to_gt(guc)->i915->drm,
			"Bad context sched_state 0x%x, desc_idx %u",
			ce->guc_state.sched_state, desc_idx);
		return -EPROTO;
	}

	trace_intel_context_sched_done(ce);

	if (context_pending_enable(ce)) {
#ifdef CONFIG_DRM_I915_SELFTEST
		if (unlikely(ce->drop_schedule_enable)) {
			ce->drop_schedule_enable = false;
			return 0;
		}
#endif

		spin_lock_irqsave(&ce->guc_state.lock, flags);
		clr_context_pending_enable(ce);
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);
	} else if (context_pending_disable(ce)) {
		bool banned;

#ifdef CONFIG_DRM_I915_SELFTEST
		if (unlikely(ce->drop_schedule_disable)) {
			ce->drop_schedule_disable = false;
			return 0;
		}
#endif

		/*
		 * Unpin must be done before __guc_signal_context_fence,
		 * otherwise a race exists between the requests getting
		 * submitted + retired before this unpin completes resulting in
		 * the pin_count going to zero and the context still being
		 * enabled.
		 */
		intel_context_sched_disable_unpin(ce);

		spin_lock_irqsave(&ce->guc_state.lock, flags);
		banned = context_banned(ce);
		clr_context_banned(ce);
		clr_context_pending_disable(ce);
		__guc_signal_context_fence(ce);
		guc_blocked_fence_complete(ce);
		spin_unlock_irqrestore(&ce->guc_state.lock, flags);

		if (banned) {
			guc_cancel_context_requests(ce);
			intel_engine_signal_breadcrumbs(ce->engine);
		}
	}

	decr_outstanding_submission_g2h(guc);
	intel_context_put(ce);

	return 0;
}

static void capture_error_state(struct intel_guc *guc,
				struct intel_context *ce)
{
	struct intel_gt *gt = guc_to_gt(guc);
	struct drm_i915_private *i915 = gt->i915;
	struct intel_engine_cs *engine = __context_to_physical_engine(ce);
	intel_wakeref_t wakeref;

	intel_engine_set_hung_context(engine, ce);
	with_intel_runtime_pm(&i915->runtime_pm, wakeref)
		i915_capture_error_state(gt, engine->mask);
	atomic_inc(&i915->gpu_error.reset_engine_count[engine->uabi_class]);
}

static void guc_context_replay(struct intel_context *ce)
{
	struct i915_sched_engine *sched_engine = ce->engine->sched_engine;

	__guc_reset_context(ce, true);
	tasklet_hi_schedule(&sched_engine->tasklet);
}

static void guc_handle_context_reset(struct intel_guc *guc,
				     struct intel_context *ce)
{
	trace_intel_context_reset(ce);

	/*
	 * XXX: Racey if request cancellation has occurred, see comment in
	 * __guc_reset_context().
	 */
	if (likely(!intel_context_is_banned(ce) &&
		   !context_blocked(ce))) {
		capture_error_state(guc, ce);
		guc_context_replay(ce);
	}
}

int intel_guc_context_reset_process_msg(struct intel_guc *guc,
					const u32 *msg, u32 len)
{
	struct intel_context *ce;
	int desc_idx;
	unsigned long flags;

	if (unlikely(len != 1)) {
		drm_err(&guc_to_gt(guc)->i915->drm, "Invalid length %u", len);
		return -EPROTO;
	}

	desc_idx = msg[0];
	/*
	 * The context lookup uses the xarray but lookups only require an RCU lock
	 * not the full spinlock. So take the lock explicitly and keep it until the
	 * context has been reference count locked to ensure it can't be destroyed
	 * asynchronously until the reset is done.
	 */
	xa_lock_irqsave(&guc->context_lookup, flags);
	ce = g2h_context_lookup(guc, desc_idx);
	if (ce)
		intel_context_get(ce);
	xa_unlock_irqrestore(&guc->context_lookup, flags);

	if (unlikely(!ce))
		return -EPROTO;

	guc_handle_context_reset(guc, ce);
	intel_context_put(ce);

	return 0;
}

int intel_guc_error_capture_process_msg(struct intel_guc *guc,
					 const u32 *msg, u32 len)
{
	int status;

	if (unlikely(len != 1)) {
		drm_dbg(&guc_to_gt(guc)->i915->drm, "Invalid length %u", len);
		return -EPROTO;
	}

	status = msg[0];
	drm_info(&guc_to_gt(guc)->i915->drm, "Got error capture: status = %d", status);

	/* FIXME: Do something with the capture */

	return 0;
}

static struct intel_engine_cs *
guc_lookup_engine(struct intel_guc *guc, u8 guc_class, u8 instance)
{
	struct intel_gt *gt = guc_to_gt(guc);
	u8 engine_class = guc_class_to_engine_class(guc_class);

	/* Class index is checked in class converter */
	GEM_BUG_ON(instance > MAX_ENGINE_INSTANCE);

	return gt->engine_class[engine_class][instance];
}

int intel_guc_engine_failure_process_msg(struct intel_guc *guc,
					 const u32 *msg, u32 len)
{
	struct intel_engine_cs *engine;
	u8 guc_class, instance;
	u32 reason;

	if (unlikely(len != 3)) {
		drm_err(&guc_to_gt(guc)->i915->drm, "Invalid length %u", len);
		return -EPROTO;
	}

	guc_class = msg[0];
	instance = msg[1];
	reason = msg[2];

	engine = guc_lookup_engine(guc, guc_class, instance);
	if (unlikely(!engine)) {
		drm_err(&guc_to_gt(guc)->i915->drm,
			"Invalid engine %d:%d", guc_class, instance);
		return -EPROTO;
	}

	intel_gt_handle_error(guc_to_gt(guc), engine->mask,
			      I915_ERROR_CAPTURE,
			      "GuC failed to reset %s (reason=0x%08x)\n",
			      engine->name, reason);

	return 0;
}

void intel_guc_find_hung_context(struct intel_engine_cs *engine)
{
	struct intel_guc *guc = &engine->gt->uc.guc;
	struct intel_context *ce;
	struct i915_request *rq;
	unsigned long index;
	unsigned long flags;

	/* Reset called during driver load? GuC not yet initialised! */
	if (unlikely(!guc_submission_initialized(guc)))
		return;

	xa_lock_irqsave(&guc->context_lookup, flags);
	xa_for_each(&guc->context_lookup, index, ce) {
		if (!kref_get_unless_zero(&ce->ref))
			continue;

		xa_unlock(&guc->context_lookup);

		if (!intel_context_is_pinned(ce))
			goto next;

		if (intel_engine_is_virtual(ce->engine)) {
			if (!(ce->engine->mask & engine->mask))
				goto next;
		} else {
			if (ce->engine != engine)
				goto next;
		}

		list_for_each_entry(rq, &ce->guc_state.requests, sched.link) {
			if (i915_test_request_state(rq) != I915_REQUEST_ACTIVE)
				continue;

			intel_engine_set_hung_context(engine, ce);

			/* Can only cope with one hang at a time... */
			intel_context_put(ce);
			xa_lock(&guc->context_lookup);
			goto done;
		}
next:
		intel_context_put(ce);
		xa_lock(&guc->context_lookup);

	}
done:
	xa_unlock_irqrestore(&guc->context_lookup, flags);
}

void intel_guc_dump_active_requests(struct intel_engine_cs *engine,
				    struct i915_request *hung_rq,
				    struct drm_printer *m)
{
	struct intel_guc *guc = &engine->gt->uc.guc;
	struct intel_context *ce;
	unsigned long index;
	unsigned long flags;

	/* Reset called during driver load? GuC not yet initialised! */
	if (unlikely(!guc_submission_initialized(guc)))
		return;

	xa_lock_irqsave(&guc->context_lookup, flags);
	xa_for_each(&guc->context_lookup, index, ce) {
		if (!kref_get_unless_zero(&ce->ref))
			continue;

		xa_unlock(&guc->context_lookup);

		if (!intel_context_is_pinned(ce))
			goto next;

		if (intel_engine_is_virtual(ce->engine)) {
			if (!(ce->engine->mask & engine->mask))
				goto next;
		} else {
			if (ce->engine != engine)
				goto next;
		}

		spin_lock(&ce->guc_state.lock);
		intel_engine_dump_active_requests(&ce->guc_state.requests,
						  hung_rq, m);
		spin_unlock(&ce->guc_state.lock);

next:
		intel_context_put(ce);
		xa_lock(&guc->context_lookup);
	}
	xa_unlock_irqrestore(&guc->context_lookup, flags);
}

void intel_guc_submission_print_info(struct intel_guc *guc,
				     struct drm_printer *p)
{
	struct i915_sched_engine *sched_engine = guc->sched_engine;
	struct rb_node *rb;
	unsigned long flags;

	if (!sched_engine)
		return;

	drm_printf(p, "GuC Number Outstanding Submission G2H: %u\n",
		   atomic_read(&guc->outstanding_submission_g2h));
	drm_printf(p, "GuC Number GuC IDs: %u\n",
		   guc->submission_state.num_guc_ids);
	drm_printf(p, "GuC Max GuC IDs: %u\n",
		   guc->submission_state.max_guc_ids);
	drm_printf(p, "GuC tasklet count: %u\n\n",
		   atomic_read(&sched_engine->tasklet.count));

	spin_lock_irqsave(&sched_engine->lock, flags);
	drm_printf(p, "Requests in GuC submit tasklet:\n");
	for (rb = rb_first_cached(&sched_engine->queue); rb; rb = rb_next(rb)) {
		struct i915_priolist *pl = to_priolist(rb);
		struct i915_request *rq;

		priolist_for_each_request(rq, pl)
			drm_printf(p, "guc_id=%u, seqno=%llu\n",
				   rq->context->guc_id.id,
				   rq->fence.seqno);
	}
	spin_unlock_irqrestore(&sched_engine->lock, flags);
	drm_printf(p, "\n");
}

static void guc_log_context_priority(struct drm_printer *p,
				     struct intel_context *ce)
{
	int i;

	drm_printf(p, "\t\tPriority: %d\n", ce->guc_state.prio);
	drm_printf(p, "\t\tNumber Requests (lower index == higher priority)\n");
	for (i = GUC_CLIENT_PRIORITY_KMD_HIGH;
	     i < GUC_CLIENT_PRIORITY_NUM; ++i) {
		drm_printf(p, "\t\tNumber requests in priority band[%d]: %d\n",
			   i, ce->guc_state.prio_count[i]);
	}
	drm_printf(p, "\n");
}


static inline void guc_log_context(struct drm_printer *p,
				   struct intel_context *ce)
{
	drm_printf(p, "GuC lrc descriptor %u:\n", ce->guc_id.id);
	drm_printf(p, "\tHW Context Desc: 0x%08x\n", ce->lrc.lrca);
	drm_printf(p, "\t\tLRC Head: Internal %u, Memory %u\n",
		   ce->ring->head,
		   ce->lrc_reg_state[CTX_RING_HEAD]);
	drm_printf(p, "\t\tLRC Tail: Internal %u, Memory %u\n",
		   ce->ring->tail,
		   ce->lrc_reg_state[CTX_RING_TAIL]);
	drm_printf(p, "\t\tContext Pin Count: %u\n",
		   atomic_read(&ce->pin_count));
	drm_printf(p, "\t\tGuC ID Ref Count: %u\n",
		   atomic_read(&ce->guc_id.ref));
	drm_printf(p, "\t\tSchedule State: 0x%x\n\n",
		   ce->guc_state.sched_state);
}

void intel_guc_submission_print_context_info(struct intel_guc *guc,
					     struct drm_printer *p)
{
	struct intel_context *ce;
	unsigned long index;
	unsigned long flags;

	xa_lock_irqsave(&guc->context_lookup, flags);
	xa_for_each(&guc->context_lookup, index, ce) {
		GEM_BUG_ON(intel_context_is_child(ce));

		guc_log_context(p, ce);
		guc_log_context_priority(p, ce);

		if (intel_context_is_parent(ce)) {
			struct guc_process_desc *desc = __get_process_desc(ce);
			struct intel_context *child;

			drm_printf(p, "\t\tWQI Head: %u\n",
				   READ_ONCE(desc->head));
			drm_printf(p, "\t\tWQI Tail: %u\n",
				   READ_ONCE(desc->tail));
			drm_printf(p, "\t\tWQI Status: %u\n\n",
				   READ_ONCE(desc->wq_status));

			drm_printf(p, "\t\tNumber Children: %u\n\n",
				   ce->guc_number_children);
			if (ce->engine->emit_bb_start ==
			    emit_bb_start_parent_no_preempt_mid_batch) {
				u8 i;

				drm_printf(p, "\t\tChildren Go: %u\n\n",
					   get_children_go_value(ce));
				for (i = 0; i < ce->guc_number_children; ++i)
					drm_printf(p, "\t\tChildren Join: %u\n",
						   get_children_join_value(ce, i));
			}

			for_each_child(ce, child)
				guc_log_context(p, child);
		}
	}
	xa_unlock_irqrestore(&guc->context_lookup, flags);
}

static struct intel_context *
guc_create_virtual(struct intel_engine_cs **siblings, unsigned int count,
		   unsigned long flags)
{
	struct guc_virtual_engine *ve;
	struct intel_guc *guc;
	unsigned int n;
	int err;

	ve = kzalloc(sizeof(*ve), GFP_KERNEL);
	if (!ve)
		return ERR_PTR(-ENOMEM);

	guc = &siblings[0]->gt->uc.guc;

	ve->base.i915 = siblings[0]->i915;
	ve->base.gt = siblings[0]->gt;
	ve->base.uncore = siblings[0]->uncore;
	ve->base.id = -1;

	ve->base.uabi_class = I915_ENGINE_CLASS_INVALID;
	ve->base.instance = I915_ENGINE_CLASS_INVALID_VIRTUAL;
	ve->base.uabi_instance = I915_ENGINE_CLASS_INVALID_VIRTUAL;
	ve->base.saturated = ALL_ENGINES;

	snprintf(ve->base.name, sizeof(ve->base.name), "virtual");

	ve->base.sched_engine = i915_sched_engine_get(guc->sched_engine);

	ve->base.cops = &virtual_guc_context_ops;
	ve->base.request_alloc = guc_request_alloc;
	ve->base.bump_serial = virtual_guc_bump_serial;

	ve->base.submit_request = guc_submit_request;

	ve->base.flags = I915_ENGINE_IS_VIRTUAL;

	intel_context_init(&ve->context, &ve->base);

	for (n = 0; n < count; n++) {
		struct intel_engine_cs *sibling = siblings[n];

		GEM_BUG_ON(!is_power_of_2(sibling->mask));
		if (sibling->mask & ve->base.mask) {
			DRM_DEBUG("duplicate %s entry in load balancer\n",
				  sibling->name);
			err = -EINVAL;
			goto err_put;
		}

		ve->base.mask |= sibling->mask;
		ve->base.logical_mask |= sibling->logical_mask;

		if (n != 0 && ve->base.class != sibling->class) {
			DRM_DEBUG("invalid mixing of engine class, sibling %d, already %d\n",
				  sibling->class, ve->base.class);
			err = -EINVAL;
			goto err_put;
		} else if (n == 0) {
			ve->base.class = sibling->class;
			ve->base.uabi_class = sibling->uabi_class;
			snprintf(ve->base.name, sizeof(ve->base.name),
				 "v%dx%d", ve->base.class, count);
			ve->base.context_size = sibling->context_size;

			ve->base.add_active_request =
				sibling->add_active_request;
			ve->base.remove_active_request =
				sibling->remove_active_request;
			ve->base.emit_bb_start = sibling->emit_bb_start;
			ve->base.emit_flush = sibling->emit_flush;
			ve->base.emit_init_breadcrumb =
				sibling->emit_init_breadcrumb;
			ve->base.emit_fini_breadcrumb =
				sibling->emit_fini_breadcrumb;
			ve->base.emit_fini_breadcrumb_dw =
				sibling->emit_fini_breadcrumb_dw;
			ve->base.breadcrumbs =
				intel_breadcrumbs_get(sibling->breadcrumbs);

			ve->base.flags |= sibling->flags;

			ve->base.props.timeslice_duration_ms =
				sibling->props.timeslice_duration_ms;
			ve->base.props.preempt_timeout_ms =
				sibling->props.preempt_timeout_ms;
		}
	}

	return &ve->context;

err_put:
	intel_context_put(&ve->context);
	return ERR_PTR(err);
}

bool intel_guc_virtual_engine_has_heartbeat(const struct intel_engine_cs *ve)
{
	struct intel_engine_cs *engine;
	intel_engine_mask_t tmp, mask = ve->mask;

	for_each_engine_masked(engine, ve->gt, mask, tmp)
		if (READ_ONCE(engine->props.heartbeat_interval_ms))
			return true;

	return false;
}

#if IS_ENABLED(CONFIG_DRM_I915_SELFTEST)
#include "selftest_guc.c"
#include "selftest_guc_multi_lrc.c"
#endif
