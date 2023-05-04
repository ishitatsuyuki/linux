/* SPDX-License-Identifier: GPL-2.0 OR MIT */

#include <drm/drm_exec.h>
#include <drm/drm_gem.h>
#include <linux/dma-resv.h>

/**
 * DOC: Overview
 *
 * This component mainly abstracts the retry loop necessary for locking
 * multiple GEM objects while preparing hardware operations (e.g. command
 * submissions, page table updates etc..).
 *
 * If a contention is detected while locking a GEM object the cleanup procedure
 * unlocks all previously locked GEM objects and locks the contended one first
 * before locking any further objects.
 *
 * After an object is locked fences slots can optionally be reserved on the
 * dma_resv object inside the GEM object.
 *
 * A typical usage pattern should look like this::
 *
 *	struct drm_gem_object *obj;
 *	struct drm_exec exec;
 *	unsigned long index;
 *	int ret;
 *
 *	drm_exec_init(&exec, true);
 *	drm_exec_while_not_all_locked(&exec) {
 *		ret = drm_exec_prepare_obj(&exec, boA, 1);
 *		drm_exec_continue_on_contention(&exec);
 *		if (ret)
 *			goto error;
 *
 *		ret = drm_exec_prepare_obj(&exec, boB, 1);
 *		drm_exec_continue_on_contention(&exec);
 *		if (ret)
 *			goto error;
 *	}
 *
 *	drm_exec_for_each_locked_object(&exec, index, obj) {
 *		dma_resv_add_fence(obj->resv, fence, DMA_RESV_USAGE_READ);
 *		...
 *	}
 *	drm_exec_fini(&exec);
 *
 * See struct dma_exec for more details.
 */

/* Dummy value used to initially enter the retry loop */
#define DRM_EXEC_DUMMY (void*)~0

/* Unlock all objects and drop references */
static void drm_exec_unlock_all(struct drm_exec *exec)
{
	struct drm_gem_object *obj;
	unsigned long index;

	drm_exec_for_each_locked_object(exec, index, obj) {
		dma_resv_unlock(obj->resv);
		drm_gem_object_put(obj);
	}

	drm_gem_object_put(exec->prelocked);
	exec->prelocked = NULL;
}

/**
 * drm_exec_init - initialize a drm_exec object
 * @exec: the drm_exec object to initialize
 * @interruptible: if locks should be acquired interruptible
 *
 * Initialize the object and make sure that we can track locked objects.
 */
void drm_exec_init(struct drm_exec *exec, bool interruptible)
{
	exec->interruptible = interruptible;
	exec->objects = kmalloc(PAGE_SIZE, GFP_KERNEL);

	/* If allocation here fails, just delay that till the first use */
	exec->max_objects = exec->objects ? PAGE_SIZE / sizeof(void *) : 0;
	exec->num_objects = 0;
	exec->contended = DRM_EXEC_DUMMY;
	exec->prelocked = NULL;
}
EXPORT_SYMBOL(drm_exec_init);

/**
 * drm_exec_fini - finalize a drm_exec object
 * @exec: the drm_exec object to finalize
 *
 * Unlock all locked objects, drop the references to objects and free all memory
 * used for tracking the state.
 */
void drm_exec_fini(struct drm_exec *exec)
{
	drm_exec_unlock_all(exec);
	kvfree(exec->objects);
	if (exec->contended != DRM_EXEC_DUMMY) {
		drm_gem_object_put(exec->contended);
		ww_acquire_fini(&exec->ticket);
	}
}
EXPORT_SYMBOL(drm_exec_fini);

/**
 * drm_exec_cleanup - cleanup when contention is detected
 * @exec: the drm_exec object to cleanup
 *
 * Cleanup the current state and return true if we should stay inside the retry
 * loop, false if there wasn't any contention detected and we can keep the
 * objects locked.
 */
bool drm_exec_cleanup(struct drm_exec *exec)
{
	if (likely(!exec->contended)) {
		ww_acquire_done(&exec->ticket);
		return false;
	}

	if (likely(exec->contended == DRM_EXEC_DUMMY)) {
		exec->contended = NULL;
		ww_acquire_init(&exec->ticket, &reservation_ww_class);
		return true;
	}

	drm_exec_unlock_all(exec);
	exec->num_objects = 0;
	return true;
}
EXPORT_SYMBOL(drm_exec_cleanup);

/* Track the locked object in the array */
static int drm_exec_obj_locked(struct drm_exec *exec,
			       struct drm_gem_object *obj)
{
	if (unlikely(exec->num_objects == exec->max_objects)) {
		size_t size = exec->max_objects * sizeof(void *);
		void *tmp;

		tmp = kvrealloc(exec->objects, size, size + PAGE_SIZE,
				GFP_KERNEL);
		if (!tmp)
			return -ENOMEM;

		exec->objects = tmp;
		exec->max_objects += PAGE_SIZE / sizeof(void *);
	}
	drm_gem_object_get(obj);
	exec->objects[exec->num_objects++] = obj;

	return 0;
}

/* Make sure the contended object is locked first */
static int drm_exec_lock_contended(struct drm_exec *exec)
{
	struct drm_gem_object *obj = exec->contended;
	int ret;

	if (likely(!obj))
		return 0;

	if (exec->interruptible) {
		ret = dma_resv_lock_slow_interruptible(obj->resv,
						       &exec->ticket);
		if (unlikely(ret))
			goto error_dropref;
	} else {
		dma_resv_lock_slow(obj->resv, &exec->ticket);
	}

	ret = drm_exec_obj_locked(exec, obj);
	if (unlikely(ret)) {
		dma_resv_unlock(obj->resv);
		goto error_dropref;
	}

	swap(exec->prelocked, obj);

error_dropref:
	/* Always cleanup the contention so that error handling can kick in */
	drm_gem_object_put(obj);
	exec->contended = NULL;
	return ret;
}

/**
 * drm_exec_prepare_obj - prepare a GEM object for use
 * @exec: the drm_exec object with the state
 * @obj: the GEM object to prepare
 * @num_fences: how many fences to reserve
 *
 * Prepare a GEM object for use by locking it and reserving fence slots. All
 * successfully locked objects are put into the locked container.
 *
 * Returns: -EDEADLK if a contention is detected, -EALREADY when object is
 * already locked, -ENOMEM when memory allocation failed and zero for success.
 */
int drm_exec_prepare_obj(struct drm_exec *exec, struct drm_gem_object *obj,
			 unsigned int num_fences)
{
	int ret;

	ret = drm_exec_lock_contended(exec);
	if (unlikely(ret))
		return ret;

	if (exec->prelocked == obj) {
		drm_gem_object_put(exec->prelocked);
		exec->prelocked = NULL;

		return dma_resv_reserve_fences(obj->resv, num_fences);
	}

	if (exec->interruptible)
		ret = dma_resv_lock_interruptible(obj->resv, &exec->ticket);
	else
		ret = dma_resv_lock(obj->resv, &exec->ticket);

	if (unlikely(ret == -EDEADLK)) {
		drm_gem_object_get(obj);
		exec->contended = obj;
		return -EDEADLK;
	}

	if (unlikely(ret))
		return ret;

	ret = drm_exec_obj_locked(exec, obj);
	if (ret)
		goto error_unlock;

	/* Keep locked when reserving fences fails */
	return dma_resv_reserve_fences(obj->resv, num_fences);

error_unlock:
	dma_resv_unlock(obj->resv);
	return ret;
}
EXPORT_SYMBOL(drm_exec_prepare_obj);

/**
 * drm_exec_prepare_array - helper to prepare an array of objects
 * @exec: the drm_exec object with the state
 * @objects: array of GEM object to prepare
 * @num_objects: number of GEM objects in the array
 * @num_fences: number of fences to reserve on each GEM object
 *
 * Prepares all GEM objects in an array, handles contention but aports on first
 * error otherwise. Reserves @num_fences on each GEM object after locking it.
 *
 * Returns: -EALREADY when object is already locked, -ENOMEM when memory
 * allocation failed and zero for success.
 */
int drm_exec_prepare_array(struct drm_exec *exec,
			   struct drm_gem_object **objects,
			   unsigned int num_objects,
			   unsigned int num_fences)
{
	int ret;

	drm_exec_while_not_all_locked(exec) {
		for (unsigned int i = 0; i < num_objects; ++i) {
			ret = drm_exec_prepare_obj(exec, objects[i],
						   num_fences);
			drm_exec_break_on_contention(exec);
			if (unlikely(ret))
				return ret;
		}
	}

	return 0;
}
EXPORT_SYMBOL(drm_exec_prepare_array);

MODULE_DESCRIPTION("DRM execution context");
MODULE_LICENSE("Dual MIT/GPL");
