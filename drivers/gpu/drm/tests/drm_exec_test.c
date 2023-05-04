// SPDX-License-Identifier: MIT
/*
 * Copyright © 2019 Intel Corporation
 */

#define pr_fmt(fmt) "drm_exec: " fmt

#include <kunit/test.h>

#include <linux/module.h>
#include <linux/prime_numbers.h>

#include <drm/drm_exec.h>
#include <drm/drm_device.h>
#include <drm/drm_gem.h>

#include "../lib/drm_random.h"

static struct drm_device dev;

static void drm_exec_sanitycheck(struct kunit *test)
{
	struct drm_exec exec;

	drm_exec_init(&exec, true);
	drm_exec_fini(&exec);
	pr_info("%s - ok!\n", __func__);
}

static void drm_exec_lock1(struct kunit *test)
{
	struct drm_gem_object gobj = { };
	struct drm_exec exec;
	int ret;

	drm_gem_private_object_init(&dev, &gobj, PAGE_SIZE);

	drm_exec_init(&exec, true);
	drm_exec_while_not_all_locked(&exec) {
		ret = drm_exec_prepare_obj(&exec, &gobj, 1);
		drm_exec_continue_on_contention(&exec);
		if (ret) {
			drm_exec_fini(&exec);
			pr_err("%s - err %d!\n", __func__, ret);
			return;
		}
	}
	drm_exec_fini(&exec);
	pr_info("%s - ok!\n", __func__);
}

static void drm_exec_lock_array(struct kunit *test)
{
	struct drm_gem_object gobj1 = { };
	struct drm_gem_object gobj2 = { };
	struct drm_gem_object *array[] = { &gobj1, &gobj2 };
	struct drm_exec exec;
	int ret;

	drm_gem_private_object_init(&dev, &gobj1, PAGE_SIZE);
	drm_gem_private_object_init(&dev, &gobj2, PAGE_SIZE);

	drm_exec_init(&exec, true);
	ret = drm_exec_prepare_array(&exec, array, ARRAY_SIZE(array), 0);
	if (ret) {
		drm_exec_fini(&exec);
		pr_err("%s - err %d!\n", __func__, ret);
		return;
	}
	drm_exec_fini(&exec);
	pr_info("%s - ok!\n", __func__);
}

static int drm_exec_suite_init(struct kunit_suite *suite)
{
	kunit_info(suite, "Testing DRM exec manager\n");
	return 0;
}

static struct kunit_case drm_exec_tests[] = {
	KUNIT_CASE(drm_exec_sanitycheck),
	KUNIT_CASE(drm_exec_lock1),
	KUNIT_CASE(drm_exec_lock_array),
	{}
};

static struct kunit_suite drm_exec_test_suite = {
	.name = "drm_exec",
	.suite_init = drm_exec_suite_init,
	.test_cases = drm_exec_tests,
};

kunit_test_suite(drm_exec_test_suite);

MODULE_AUTHOR("AMD");
MODULE_LICENSE("GPL and additional rights");
