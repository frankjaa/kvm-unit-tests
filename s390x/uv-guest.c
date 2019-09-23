/*
 * Guest Ultravisor Call tests
 *
 * Copyright (c) 2020 IBM Corp
 *
 * Authors:
 *  Janosch Frank <frankja@linux.ibm.com>
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2.
 */

#include <libcflat.h>
#include <alloc_page.h>
#include <asm/page.h>
#include <asm/asm-offsets.h>
#include <asm/interrupt.h>
#include <asm/facility.h>
#include <asm/uv.h>

static unsigned long page;

static inline int share(unsigned long addr, u16 cmd)
{
	struct uv_cb_share uvcb = {
		.header.cmd = cmd,
		.header.len = sizeof(uvcb),
		.paddr = addr
	};

	uv_call(0, (u64)&uvcb);
	return uvcb.header.rc;
}

static inline int uv_set_shared(unsigned long addr)
{
	return share(addr, UVC_CMD_SET_SHARED_ACCESS);
}

static inline int uv_remove_shared(unsigned long addr)
{
	return share(addr, UVC_CMD_REMOVE_SHARED_ACCESS);
}

static void test_priv(void)
{
	struct uv_cb_header uvcb = {};

	report_prefix_push("privileged");

	report_prefix_push("query");
	expect_pgm_int();
	uvcb.cmd = UVC_CMD_QUI;
	uvcb.len = sizeof(struct uv_cb_qui);
	enter_pstate();
	uv_call(0, (u64)&uvcb);
	check_pgm_int_code(PGM_INT_CODE_PRIVILEGED_OPERATION);
	report_prefix_pop();

	report_prefix_push("share");
	expect_pgm_int();
	enter_pstate();
	uv_set_shared((unsigned long)page);
	check_pgm_int_code(PGM_INT_CODE_PRIVILEGED_OPERATION);
	report_prefix_pop();

	report_prefix_push("unshare");
	expect_pgm_int();
	enter_pstate();
	uv_remove_shared((unsigned long)page);
	check_pgm_int_code(PGM_INT_CODE_PRIVILEGED_OPERATION);
	report_prefix_pop();

	report_prefix_pop();
}

static void test_query(void)
{
	struct uv_cb_qui uvcb = {
		.header.cmd = UVC_CMD_QUI,
		.header.len = sizeof(uvcb) - 8,
	};
	int cc;

	report_prefix_push("query");
	cc = uv_call(0, (u64)&uvcb);
	report(cc == 1 && uvcb.header.rc == UVC_RC_INV_LEN, "length");

	uvcb.header.len = sizeof(uvcb);
	cc = uv_call(0, (u64)&uvcb);
	report(cc == 0 && uvcb.header.rc == UVC_RC_EXECUTED, "successful query");

	/*
	 * These bits have been introduced with the very first
	 * Ultravisor version and are expected to always be available
	 * because they are basic building blocks.
	 */
	report(uvcb.inst_calls_list[0] & (1UL << (63 - BIT_UVC_CMD_QUI)),
	       "query indicated");
	report(uvcb.inst_calls_list[0] & (1UL << (63 - BIT_UVC_CMD_SET_SHARED_ACCESS)),
	       "share indicated");
	report(uvcb.inst_calls_list[0] & (1UL << (63 - BIT_UVC_CMD_REMOVE_SHARED_ACCESS)),
	       "unshare indicated");
	report_prefix_pop();
}

static void test_sharing(void)
{
	struct uv_cb_share uvcb = {
		.header.cmd = UVC_CMD_SET_SHARED_ACCESS,
		.header.len = sizeof(uvcb) - 8,
	};
	int cc;

	report_prefix_push("share");
	cc = uv_call(0, (u64)&uvcb);
	report(cc == 1 && uvcb.header.rc == UVC_RC_INV_LEN, "length");
	report(uv_set_shared(page) == UVC_RC_EXECUTED, "share");
	report_prefix_pop();

	report_prefix_push("unshare");
	uvcb.header.cmd = UVC_CMD_REMOVE_SHARED_ACCESS;
	cc = uv_call(0, (u64)&uvcb);
	report(cc == 1 && uvcb.header.rc == UVC_RC_INV_LEN, "length");
	report(uv_remove_shared(page) == UVC_RC_EXECUTED, "unshare");
	report_prefix_pop();

	report_prefix_pop();
}

static void test_invalid(void)
{
	struct uv_cb_header uvcb = {
		.len = 16,
		.cmd = 0x4242,
	};
	int cc;

	cc = uv_call(0, (u64)&uvcb);
	report(cc == 1 && uvcb.rc == UVC_RC_INV_CMD, "invalid command");
}

int main(void)
{
	bool has_uvc = test_facility(158);

	report_prefix_push("uvc");
	if (!has_uvc) {
		report_skip("Ultravisor call facility is not available");
		goto done;
	}

	page = (unsigned long)alloc_page();
	test_priv();
	test_invalid();
	test_query();
	test_sharing();
	free_page((void *)page);
done:
	report_prefix_pop();
	return report_summary();
}
