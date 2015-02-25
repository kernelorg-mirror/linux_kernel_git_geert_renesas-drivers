// SPDX-License-Identifier: GPL-2.0
/*
 * r8a73a4 processor support
 *
 * Copyright (C) 2013  Renesas Solutions Corp.
 * Copyright (C) 2013  Magnus Damm
 */

#include <linux/init.h>
#include <linux/of_platform.h>

#include <asm/mach/arch.h>

#include "common.h"
#include "r8a73a4.h"

static void __init r8a73a4_init_machine(void)
{
	r8a73a4_disable_mstp_clocks();
	of_platform_default_populate(NULL, NULL, NULL);
}

static const char *const r8a73a4_boards_compat_dt[] __initconst = {
	"renesas,r8a73a4",
	NULL,
};

DT_MACHINE_START(R8A73A4_DT, "Generic R8A73A4 (Flattened Device Tree)")
	.init_late	= shmobile_init_late,
	.init_machine	= r8a73a4_init_machine,
	.dt_compat	= r8a73a4_boards_compat_dt,
MACHINE_END
