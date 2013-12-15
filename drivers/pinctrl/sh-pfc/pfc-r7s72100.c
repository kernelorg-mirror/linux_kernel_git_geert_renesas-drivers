/*
 * R7S72100 processor support
 *
 * Copyright (C) 2013  Renesas Electronics Corporation
 * Copyright (C) 2013  Magnus Damm
 * Copyright (C) 2012  Renesas Solutions Corp.
 * Copyright (C) 2012  Kuninori Morimoto <kuninori.morimoto.gx@renesas.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 of the
 * License.
 */

#include <linux/kernel.h>

#include "core.h"
#include "sh_pfc.h"

#define PORT_P_1(bank, pin, fn, sfx) fn(bank, pin, P_##bank##_##pin, sfx)

#define PORT_P_16(bank, fn, sfx)					\
	PORT_P_1(bank, 0,  fn, sfx), PORT_P_1(bank, 1,  fn, sfx),	\
	PORT_P_1(bank, 2,  fn, sfx), PORT_P_1(bank, 3,  fn, sfx),	\
	PORT_P_1(bank, 4,  fn, sfx), PORT_P_1(bank, 5,  fn, sfx),	\
	PORT_P_1(bank, 6,  fn, sfx), PORT_P_1(bank, 7,  fn, sfx),	\
	PORT_P_1(bank, 8,  fn, sfx), PORT_P_1(bank, 9,  fn, sfx),	\
	PORT_P_1(bank, 10, fn, sfx), PORT_P_1(bank, 11, fn, sfx),	\
	PORT_P_1(bank, 12, fn, sfx), PORT_P_1(bank, 13, fn, sfx),	\
	PORT_P_1(bank, 14, fn, sfx), PORT_P_1(bank, 15, fn, sfx)

#define CPU_ALL_PORT(fn, sfx)						\
	PORT_P_16(0, fn, sfx), PORT_P_16(1, fn, sfx),			\
	PORT_P_16(2, fn, sfx), PORT_P_16(3, fn, sfx),			\
	PORT_P_16(4, fn, sfx), PORT_P_16(5, fn, sfx),			\
	PORT_P_16(6, fn, sfx), PORT_P_16(7, fn, sfx),			\
	PORT_P_16(8, fn, sfx), PORT_P_16(9, fn, sfx),			\
	PORT_P_16(10, fn, sfx), PORT_P_16(11, fn, sfx),			\
	PORT_P_16(12, fn, sfx)

#define P_ALL(n) GP_ALL(n)

enum {
	PINMUX_RESERVED = 0,

	PINMUX_DATA_BEGIN,
	P_ALL(DATA),
	PINMUX_DATA_END,

	PINMUX_FUNCTION_BEGIN,
	P_ALL(PMC_0), P_ALL(PMC_1),
	P_ALL(PFC_0), P_ALL(PFC_1),
	P_ALL(PFCE_0), P_ALL(PFCE_1),
	P_ALL(PFCAE_0), P_ALL(PFCAE_1),
	P_ALL(PIBC_0), P_ALL(PIBC_1),
	P_ALL(PBDC_0), P_ALL(PBDC_1),
	P_ALL(PIPC_0), P_ALL(PIPC_1),
	PINMUX_FUNCTION_END,

	PINMUX_MARK_BEGIN,
	P_ALL(MARK_FN1), P_ALL(MARK_FN2), P_ALL(MARK_FN3), P_ALL(MARK_FN4),
	P_ALL(MARK_FN5), P_ALL(MARK_FN6), P_ALL(MARK_FN7), P_ALL(MARK_FN8),
	PINMUX_MARK_END,
};

#define _P_ALL(n) CPU_ALL_PORT(n, unused)

#define _P_GPIO(bank, _pin, _name, sfx) _GP_GPIO(16, bank, _pin, _name, sfx)

#define _P_DATA(bank, pin, name, sfx)					\
	PINMUX_DATA(name##_DATA, name##_PMC_0,		\
		    name##_PIBC_1, name##_PBDC_1)

#define _P_FN(n, fn, pfcae, pfce, pfc)					\
	PINMUX_DATA(n##_MARK_FN##fn, n##_PMC_1,		\
		    n##_PFCAE_##pfcae, n##_PFCE_##pfce, n##_PFC_##pfc)

#define _P_MARK_FN1(bank, pin, name, sfx) _P_FN(name, 1, 0, 0, 0)
#define _P_MARK_FN2(bank, pin, name, sfx) _P_FN(name, 2, 0, 0, 1)
#define _P_MARK_FN3(bank, pin, name, sfx) _P_FN(name, 3, 0, 1, 0)
#define _P_MARK_FN4(bank, pin, name, sfx) _P_FN(name, 4, 0, 1, 1)
#define _P_MARK_FN5(bank, pin, name, sfx) _P_FN(name, 5, 1, 0, 0)
#define _P_MARK_FN6(bank, pin, name, sfx) _P_FN(name, 6, 1, 0, 1)
#define _P_MARK_FN7(bank, pin, name, sfx) _P_FN(name, 7, 1, 1, 0)
#define _P_MARK_FN8(bank, pin, name, sfx) _P_FN(name, 8, 1, 1, 1)

static const u16 pinmux_data[] = {
	_P_ALL(_P_DATA), /* PINMUX_DATA(P_M_N_DATA, P_M_N_PMC_0)... */
	_P_ALL(_P_MARK_FN1), _P_ALL(_P_MARK_FN2),
	_P_ALL(_P_MARK_FN3), _P_ALL(_P_MARK_FN4),
	_P_ALL(_P_MARK_FN5), _P_ALL(_P_MARK_FN6),
	_P_ALL(_P_MARK_FN7), _P_ALL(_P_MARK_FN8),
};

static struct sh_pfc_pin pinmux_pins[] = {
	_P_ALL(_P_GPIO),
};

#define RZ_PORT_PIN(bank, pin) (((bank) * 16) + (pin))

#define __RZ_STR(pfx, hw, bank, pin, sfx)		\
	pfx##_##hw##_p##bank##_##pin####sfx

#define RZ_PIN_AND_MUX(pfx, hw, bank, pin, fn)				\
static const unsigned int __RZ_STR(pfx, hw, bank, pin, _pins)[] = {	\
	RZ_PORT_PIN(bank, pin),						\
};									\
static const unsigned int __RZ_STR(pfx, hw, bank, pin, _mux)[] = {	\
	P_##bank##_##pin##_MARK_FN##fn,					\
};

#define RZ_PMX_GROUP(pfx, hw, bank, pin, fn) \
	SH_PFC_PIN_GROUP(pfx##_##hw##_p##bank##_##pin),

#define __RZ_GROUPS(x) #x

#define RZ_GROUPS(pfx, hw, bank, pin, fn) \
	__RZ_GROUPS(pfx##_##hw##_p##bank##_##pin),

#define RIIC0(fn)			\
	fn(riic0, scl, 1, 0, 1)		\
	fn(riic0, sda, 1, 1, 1)

#define RIIC1(fn)			\
	fn(riic1, scl, 1, 2, 1)		\
	fn(riic1, sda, 1, 3, 1)

#define RIIC2(fn)			\
	fn(riic2, scl, 1, 4, 1)		\
	fn(riic2, sda, 1, 5, 1)

#define RIIC3(fn)			\
	fn(riic3, scl, 1, 6, 1)		\
	fn(riic3, sda, 1, 7, 1)

RIIC0(RZ_PIN_AND_MUX)
RIIC1(RZ_PIN_AND_MUX)
RIIC2(RZ_PIN_AND_MUX)
RIIC3(RZ_PIN_AND_MUX)

#define SCIF0(fn)			\
	fn(scif0, clk, 2, 13, 6)	\
	fn(scif0, txd, 2, 14, 6)	\
	fn(scif0, rxd, 2, 15, 6)	\
	fn(scif0, clk, 4, 8, 7)		\
	fn(scif0, txd, 4, 9, 7)		\
	fn(scif0, rxd, 4, 10, 7)	\
	fn(scif0, clk, 6, 8, 5)		\
	fn(scif0, txd, 6, 9, 5)		\
	fn(scif0, rxd, 6, 10, 5)

#define SCIF1(fn)			\
	fn(scif1, cts, 2, 3, 6)		\
	fn(scif1, clk, 2, 4, 6)		\
	fn(scif1, txd, 2, 5, 6)		\
	fn(scif1, rxd, 2, 6, 6)		\
	fn(scif1, rts, 2, 7, 6)		\
	fn(scif1, clk, 4, 11, 7)	\
	fn(scif1, txd, 4, 12, 7)	\
	fn(scif1, rxd, 4, 13, 7)	\
	fn(scif1, clk, 6, 11, 5)	\
	fn(scif1, txd, 6, 12, 5)	\
	fn(scif1, rxd, 6, 13, 5)	\
	fn(scif1, clk, 9, 2, 4)		\
	fn(scif1, txd, 9, 3, 4)		\
	fn(scif1, rxd, 9, 4, 4)		\
	fn(scif1, cts, 9, 5, 4)		\
	fn(scif1, rts, 9, 6, 4)

#define SCIF2(fn)			\
	fn(scif2, clk, 3, 0, 4)		\
	fn(scif2, txd, 3, 1, 4)		\
	fn(scif2, rxd, 3, 2, 4)		\
	fn(scif2, txd, 3, 0, 6)		\
	fn(scif2, clk, 4, 1, 5)		\
	fn(scif2, txd, 4, 2, 5)		\
	fn(scif2, rxd, 4, 3, 5)		\
	fn(scif2, txd, 4, 14, 7)	\
	fn(scif2, rxd, 4, 15, 7)	\
	fn(scif2, txd, 6, 2, 7)		\
	fn(scif2, rxd, 6, 3, 7)		\
	fn(scif2, clk, 8, 3, 7)		\
	fn(scif2, rxd, 8, 4, 7)		\
	fn(scif2, txd, 8, 6, 7)

#define SCIF3(fn)			\
	fn(scif3, clk, 3, 4, 7)		\
	fn(scif3, txd, 3, 5, 7)		\
	fn(scif3, rxd, 3, 6, 7)		\
	fn(scif3, clk, 5, 2, 5)		\
	fn(scif3, txd, 5, 3, 5)		\
	fn(scif3, rxd, 5, 4, 5)		\
	fn(scif3, rxd, 6, 0, 7)		\
	fn(scif3, txd, 6, 1, 7)		\
	fn(scif3, txd, 8, 8, 7)		\
	fn(scif3, rxd, 8, 9, 7)

#define SCIF4(fn)			\
	fn(scif4, txd, 5, 0, 5)		\
	fn(scif4, rxd, 5, 1, 5)		\
	fn(scif4, clk, 7, 0, 4)		\
	fn(scif4, txd, 7, 1, 4)		\
	fn(scif4, rxd, 7, 2, 4)		\
	fn(scif4, txd, 8, 14, 7)	\
	fn(scif4, rxd, 8, 15, 7)

#define SCIF5(fn)			\
	fn(scif5, cts, 6, 3, 5)		\
	fn(scif5, rts, 6, 4, 5)		\
	fn(scif5, clk, 6, 5, 5)		\
	fn(scif5, txd, 6, 6, 5)		\
	fn(scif5, rxd, 6, 7, 5)		\
	fn(scif5, cts, 7, 15, 4)	\
	fn(scif5, clk, 8, 0, 4)		\
	fn(scif5, txd, 8, 1, 4)		\
	fn(scif5, rxd, 8, 2, 4)		\
	fn(scif5, rts, 8, 3, 4)		\
	fn(scif5, rxd, 8, 11, 5)	\
	fn(scif5, clk, 8, 12, 5)	\
	fn(scif5, txd, 8, 13, 5)	\
	fn(scif5, cts, 11, 7, 3)	\
	fn(scif5, rts, 11, 8, 3)	\
	fn(scif5, clk, 11, 9, 3)	\
	fn(scif5, txd, 11, 10, 3)	\
	fn(scif5, rxd, 11, 11, 3)

#define SCIF6(fn)			\
	fn(scif6, txd, 5, 6, 5)		\
	fn(scif6, rxd, 5, 7, 5)		\
	fn(scif6, clk, 6, 13, 4)	\
	fn(scif6, txd, 6, 14, 4)	\
	fn(scif6, rxd, 6, 15, 4)	\
	fn(scif6, clk, 11, 0, 4)	\
	fn(scif6, txd, 11, 1, 4)	\
	fn(scif6, rxd, 11, 2, 4)

#define SCIF7(fn)			\
	fn(scif7, clk, 7, 3, 4)		\
	fn(scif7, txd, 7, 4, 4)		\
	fn(scif7, rxd, 7, 5, 4)		\
	fn(scif7, cts, 7, 6, 4)		\
	fn(scif7, rts, 7, 7, 4)

SCIF0(RZ_PIN_AND_MUX)
SCIF1(RZ_PIN_AND_MUX)
SCIF2(RZ_PIN_AND_MUX)
SCIF3(RZ_PIN_AND_MUX)
SCIF4(RZ_PIN_AND_MUX)
SCIF5(RZ_PIN_AND_MUX)
SCIF6(RZ_PIN_AND_MUX)
SCIF7(RZ_PIN_AND_MUX)

#define ETHERNET(fn)			\
	fn(ethernet, col,    1,  3, 3)		\
	fn(ethernet, col,    1, 14, 4)		\
	fn(ethernet, int,    1, 15, 1)		\
	fn(ethernet, txclk,  2,  0, 2)		\
	fn(ethernet, txer,   2,  1, 2)		\
	fn(ethernet, txen,   2,  2, 2)		\
	fn(ethernet, txcrs,  2,  3, 2)		\
	fn(ethernet, txd,    2,  4, 2)		\
	fn(ethernet, txd,    2,  5, 2)		\
	fn(ethernet, txd,    2,  6, 2)		\
	fn(ethernet, txd,    2,  7, 2)		\
	fn(ethernet, rxd,    2,  8, 2)		\
	fn(ethernet, rxd,    2,  9, 2)		\
	fn(ethernet, rxd,    2, 10, 2)		\
	fn(ethernet, rxd,    2, 11, 2)		\
	fn(ethernet, txclk,  3,  0, 2)		\
	fn(ethernet, txer,   3,  1, 2)		\
	fn(ethernet, txen,   3,  2, 2)		\
	fn(ethernet, mdio,   3,  3, 2)		\
	fn(ethernet, rxclk,  3,  4, 2)		\
	fn(ethernet, rxer,   3,  5, 2)		\
	fn(ethernet, rxdv,   3,  6, 2)		\
	fn(ethernet, mdc,    5,  9, 2)		\
	fn(ethernet, mdc,    7,  0, 3)		\
	fn(ethernet, txclk,  7,  1, 3)		\
	fn(ethernet, txer,   7,  2, 3)		\
	fn(ethernet, txen,   7,  3, 3)		\
	fn(ethernet, txd,    7,  4, 3)		\
	fn(ethernet, txd,    7,  5, 3)		\
	fn(ethernet, txd,    7,  6, 3)		\
	fn(ethernet, txd,    7,  7, 3)		\
	fn(ethernet, rxd,    7,  9, 3)		\
	fn(ethernet, rxd,    7, 10, 3)		\
	fn(ethernet, rxd,    7, 11, 2)		\
	fn(ethernet, rxd,    7, 12, 3)		\
	fn(ethernet, mdio,   7, 13, 3)		\
	fn(ethernet, crs,    7, 14, 3)		\
	fn(ethernet, rxclk,  7, 15, 3)		\
	fn(ethernet, rxer,   8,  0, 3)		\
	fn(ethernet, rxd,    8,  1, 3)		\
	fn(ethernet, col,    8,  7, 5)		\
	fn(ethernet, txclk, 10,  0, 4)		\
	fn(ethernet, txer,  10,  1, 4)		\
	fn(ethernet, txen,  10,  2, 4)		\
	fn(ethernet, crs,   10,  3, 4)		\
	fn(ethernet, txd,   10,  4, 4)		\
	fn(ethernet, txd,   10,  5, 4)		\
	fn(ethernet, txd,   10,  6, 4)		\
	fn(ethernet, txd,   10,  7, 4)		\
	fn(ethernet, txd,   10,  8, 4)		\
	fn(ethernet, txd,   10,  9, 4)		\
	fn(ethernet, txd,   10, 10, 4)		\
	fn(ethernet, txd,   10, 11, 4)		\

ETHERNET(RZ_PIN_AND_MUX)

static const struct sh_pfc_pin_group pinmux_groups[] = {
	RIIC0(RZ_PMX_GROUP)
	RIIC1(RZ_PMX_GROUP)
	RIIC2(RZ_PMX_GROUP)
	RIIC3(RZ_PMX_GROUP)
	SCIF0(RZ_PMX_GROUP)
	SCIF1(RZ_PMX_GROUP)
	SCIF2(RZ_PMX_GROUP)
	SCIF3(RZ_PMX_GROUP)
	SCIF4(RZ_PMX_GROUP)
	SCIF5(RZ_PMX_GROUP)
	SCIF6(RZ_PMX_GROUP)
	SCIF7(RZ_PMX_GROUP)
	ETHERNET(RZ_PMX_GROUP)
};

static const char * const riic0_groups[] = {
	RIIC0(RZ_GROUPS)
};

static const char * const riic1_groups[] = {
	RIIC1(RZ_GROUPS)
};

static const char * const riic2_groups[] = {
	RIIC2(RZ_GROUPS)
};

static const char * const riic3_groups[] = {
	RIIC3(RZ_GROUPS)
};

static const char * const scif0_groups[] = {
	SCIF0(RZ_GROUPS)
};

static const char * const scif1_groups[] = {
	SCIF1(RZ_GROUPS)
};

static const char * const scif2_groups[] = {
	SCIF2(RZ_GROUPS)
};

static const char * const scif3_groups[] = {
	SCIF3(RZ_GROUPS)
};

static const char * const scif4_groups[] = {
	SCIF4(RZ_GROUPS)
};

static const char * const scif5_groups[] = {
	SCIF5(RZ_GROUPS)
};

static const char * const scif6_groups[] = {
	SCIF6(RZ_GROUPS)
};

static const char * const scif7_groups[] = {
	SCIF7(RZ_GROUPS)
};

static const char * const ethernet_groups[] = {
	ETHERNET(RZ_GROUPS)
};

static const struct sh_pfc_function pinmux_functions[] = {
	SH_PFC_FUNCTION(riic0),
	SH_PFC_FUNCTION(riic1),
	SH_PFC_FUNCTION(riic2),
	SH_PFC_FUNCTION(riic3),
	SH_PFC_FUNCTION(scif0),
	SH_PFC_FUNCTION(scif1),
	SH_PFC_FUNCTION(scif2),
	SH_PFC_FUNCTION(scif3),
	SH_PFC_FUNCTION(scif4),
	SH_PFC_FUNCTION(scif5),
	SH_PFC_FUNCTION(scif6),
	SH_PFC_FUNCTION(scif7),
	SH_PFC_FUNCTION(ethernet),
};

#define PFC_REG(idx, name, reg)						\
	{ PINMUX_CFG_REG(__stringify(name), reg, 16, 1) {		\
		P_##idx##_15_##name##_0, P_##idx##_15_##name##_1,	\
		P_##idx##_14_##name##_0, P_##idx##_14_##name##_1,	\
		P_##idx##_13_##name##_0, P_##idx##_13_##name##_1,	\
		P_##idx##_12_##name##_0, P_##idx##_12_##name##_1,	\
		P_##idx##_11_##name##_0, P_##idx##_11_##name##_1,	\
		P_##idx##_10_##name##_0, P_##idx##_10_##name##_1,	\
		P_##idx##_9_##name##_0, P_##idx##_9_##name##_1,		\
		P_##idx##_8_##name##_0, P_##idx##_8_##name##_1,		\
		P_##idx##_7_##name##_0, P_##idx##_7_##name##_1,		\
		P_##idx##_6_##name##_0, P_##idx##_6_##name##_1,		\
		P_##idx##_5_##name##_0, P_##idx##_5_##name##_1,		\
		P_##idx##_4_##name##_0, P_##idx##_4_##name##_1,		\
		P_##idx##_3_##name##_0, P_##idx##_3_##name##_1,		\
		P_##idx##_2_##name##_0, P_##idx##_2_##name##_1,		\
		P_##idx##_1_##name##_0, P_##idx##_1_##name##_1,		\
		P_##idx##_0_##name##_0, P_##idx##_0_##name##_1 }	\
	}

#define PFC_REGS(idx)						\
	PFC_REG(idx, PMC, (0xfcfe3400 + (idx * 4))),		\
	PFC_REG(idx, PFC, (0xfcfe3500 + (idx * 4))),		\
	PFC_REG(idx, PFCE, (0xfcfe3600 + (idx * 4))),		\
	PFC_REG(idx, PFCAE, (0xfcfe3a00 + (idx * 4))),		\
	PFC_REG(idx, PIBC, (0xfcfe7000 + (idx * 4))),		\
	PFC_REG(idx, PBDC, (0xfcfe7100 + (idx * 4))),		\
	PFC_REG(idx, PIPC, (0xfcfe7200 + (idx * 4)))

static struct pinmux_cfg_reg pinmux_config_regs[] = {
	PFC_REGS(0), PFC_REGS(1), PFC_REGS(2), PFC_REGS(3),
	PFC_REGS(4), PFC_REGS(5), PFC_REGS(6), PFC_REGS(7),
	PFC_REGS(8), PFC_REGS(9), PFC_REGS(10), PFC_REGS(11),
	PFC_REG(12, PMC, 0xfcfe7b40),
	PFC_REG(12, PIBC, 0xfcfe7f00),
	{ },
};

const struct sh_pfc_soc_info r7s72100_pinmux_info = {
	.name = "r7s72100_pfc",

	.function = { PINMUX_FUNCTION_BEGIN, PINMUX_FUNCTION_END },

	.pins = pinmux_pins,
	.nr_pins = ARRAY_SIZE(pinmux_pins),
	.groups = pinmux_groups,
	.nr_groups = ARRAY_SIZE(pinmux_groups),
	.functions = pinmux_functions,
	.nr_functions = ARRAY_SIZE(pinmux_functions),

	.cfg_regs = pinmux_config_regs,

	.gpio_data = pinmux_data,
	.gpio_data_size = ARRAY_SIZE(pinmux_data),
};
