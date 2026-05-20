// SPDX-License-Identifier: GPL-2.0
/*
 * System Control and Management Interface (SCMI) Clock Protocol
 *
 * Copyright (C) 2018-2022 ARM Ltd.
 */

#include <linux/math64.h>
#include <linux/module.h>
#include <linux/limits.h>
#include <linux/sort.h>

#include "protocols.h"
#include "notify.h"
#include "quirks.h"

/* Updated only after ALL the mandatory features for that version are merged */
#define SCMI_PROTOCOL_SUPPORTED_VERSION		0x30000

enum scmi_clock_protocol_cmd {
	CLOCK_ATTRIBUTES = 0x3,
	CLOCK_DESCRIBE_RATES = 0x4,
	CLOCK_RATE_SET = 0x5,
	CLOCK_RATE_GET = 0x6,
	CLOCK_CONFIG_SET = 0x7,
	CLOCK_NAME_GET = 0x8,
	CLOCK_RATE_NOTIFY = 0x9,
	CLOCK_RATE_CHANGE_REQUESTED_NOTIFY = 0xA,
	CLOCK_CONFIG_GET = 0xB,
	CLOCK_POSSIBLE_PARENTS_GET = 0xC,
	CLOCK_PARENT_SET = 0xD,
	CLOCK_PARENT_GET = 0xE,
	CLOCK_GET_PERMISSIONS = 0xF,
};

#define CLOCK_STATE_CONTROL_ALLOWED	BIT(31)
#define CLOCK_PARENT_CONTROL_ALLOWED	BIT(30)
#define CLOCK_RATE_CONTROL_ALLOWED	BIT(29)

enum clk_state {
	CLK_STATE_DISABLE,
	CLK_STATE_ENABLE,
	CLK_STATE_RESERVED,
	CLK_STATE_UNCHANGED,
};

struct scmi_msg_resp_clock_protocol_attributes {
	__le16 num_clocks;
	u8 max_async_req;
	u8 reserved;
};

struct scmi_msg_resp_clock_attributes {
	__le32 attributes;
#define SUPPORTS_RATE_CHANGED_NOTIF(x)		((x) & BIT(31))
#define SUPPORTS_RATE_CHANGE_REQUESTED_NOTIF(x)	((x) & BIT(30))
#define SUPPORTS_EXTENDED_NAMES(x)		((x) & BIT(29))
#define SUPPORTS_PARENT_CLOCK(x)		((x) & BIT(28))
#define SUPPORTS_EXTENDED_CONFIG(x)		((x) & BIT(27))
#define SUPPORTS_GET_PERMISSIONS(x)		((x) & BIT(1))
#define ATTRIBUTES_ENABLED			BIT(0)
	u8 name[SCMI_SHORT_NAME_MAX_SIZE];
	__le32 clock_enable_latency;
};

struct scmi_msg_clock_possible_parents {
	__le32 id;
	__le32 skip_parents;
};

struct scmi_msg_resp_clock_possible_parents {
	__le32 num_parent_flags;
#define NUM_PARENTS_RETURNED(x)		((x) & 0xff)
#define NUM_PARENTS_REMAINING(x)	((x) >> 24)
	__le32 possible_parents[];
};

struct scmi_msg_clock_set_parent {
	__le32 id;
	__le32 parent_id;
};

struct scmi_msg_clock_config_set {
	__le32 id;
	__le32 attributes;
};

/* Valid only from SCMI clock v2.1 */
struct scmi_msg_clock_config_set_v2 {
	__le32 id;
	__le32 attributes;
#define NULL_OEM_TYPE			0
#define REGMASK_OEM_TYPE_SET		GENMASK(23, 16)
#define REGMASK_CLK_STATE		GENMASK(1, 0)
	__le32 oem_config_val;
};

struct scmi_msg_clock_config_get {
	__le32 id;
	__le32 flags;
#define REGMASK_OEM_TYPE_GET		GENMASK(7, 0)
};

struct scmi_msg_resp_clock_config_get {
	__le32 attributes;
	__le32 config;
#define IS_CLK_ENABLED(x)		le32_get_bits((x), BIT(0))
	__le32 oem_config_val;
};

struct scmi_msg_clock_describe_rates {
	__le32 id;
	__le32 rate_index;
};

struct scmi_msg_resp_clock_describe_rates {
	__le32 num_rates_flags;
#define NUM_RETURNED(x)		((x) & 0xfff)
#define RATE_DISCRETE(x)	!((x) & BIT(12))
#define NUM_REMAINING(x)	((x) >> 16)
	struct {
		__le32 value_low;
		__le32 value_high;
	} rate[];
#define RATE_TO_U64(X)		\
({				\
	typeof(X) x = (X);	\
	le32_to_cpu((x).value_low) | (u64)le32_to_cpu((x).value_high) << 32; \
})
};

struct scmi_clock_set_rate {
	__le32 flags;
#define CLOCK_SET_ASYNC		BIT(0)
#define CLOCK_SET_IGNORE_RESP	BIT(1)
#define CLOCK_SET_ROUND_UP	BIT(2)
#define CLOCK_SET_ROUND_AUTO	BIT(3)
	__le32 id;
	__le32 value_low;
	__le32 value_high;
};

struct scmi_msg_resp_set_rate_complete {
	__le32 id;
	__le32 rate_low;
	__le32 rate_high;
};

struct scmi_msg_clock_rate_notify {
	__le32 clk_id;
	__le32 notify_enable;
};

struct scmi_clock_rate_notify_payld {
	__le32 agent_id;
	__le32 clock_id;
	__le32 rate_low;
	__le32 rate_high;
};

struct scmi_clock_desc {
	u32 id;
	unsigned int tot_rates;
	struct scmi_clock_rates r;
#define	RATE_MIN	0
#define	RATE_MAX	1
#define	RATE_STEP	2
	struct scmi_clock_info info;
};

#define to_desc(p)	(container_of(p, struct scmi_clock_desc, info))

struct clock_info {
	int num_clocks;
	int max_async_req;
	bool notify_rate_changed_cmd;
	bool notify_rate_change_requested_cmd;
	atomic_t cur_async_req;
	struct scmi_clock_desc *clkds;
#define CLOCK_INFO(c, i)	(&(((c)->clkds + (i))->info))
	int (*clock_config_set)(const struct scmi_protocol_handle *ph,
				u32 clk_id, enum clk_state state,
				enum scmi_clock_oem_config oem_type,
				u32 oem_val, bool atomic);
	int (*clock_config_get)(const struct scmi_protocol_handle *ph,
				u32 clk_id, enum scmi_clock_oem_config oem_type,
				u32 *attributes, bool *enabled, u32 *oem_val,
				bool atomic);
};

static enum scmi_clock_protocol_cmd evt_2_cmd[] = {
	CLOCK_RATE_NOTIFY,
	CLOCK_RATE_CHANGE_REQUESTED_NOTIFY,
};

static inline struct scmi_clock_info *
scmi_clock_domain_lookup(struct clock_info *ci, u32 clk_id)
{
	if (clk_id >= ci->num_clocks)
		return ERR_PTR(-EINVAL);

	return CLOCK_INFO(ci, clk_id);
}

static int
scmi_clock_protocol_attributes_get(const struct scmi_protocol_handle *ph,
				   struct clock_info *ci)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_resp_clock_protocol_attributes *attr;

	ret = ph->xops->xfer_get_init(ph, PROTOCOL_ATTRIBUTES,
				      0, sizeof(*attr), &t);
	if (ret)
		return ret;

	attr = t->rx.buf;

	ret = ph->xops->do_xfer(ph, t);
	if (!ret) {
		ci->num_clocks = le16_to_cpu(attr->num_clocks);
		ci->max_async_req = attr->max_async_req;
	}

	ph->xops->xfer_put(ph, t);

	if (!ret) {
		if (!ph->hops->protocol_msg_check(ph, CLOCK_RATE_NOTIFY, NULL))
			ci->notify_rate_changed_cmd = true;

		if (!ph->hops->protocol_msg_check(ph,
						  CLOCK_RATE_CHANGE_REQUESTED_NOTIFY,
						  NULL))
			ci->notify_rate_change_requested_cmd = true;
	}

	return ret;
}

struct scmi_clk_ipriv {
	u32 id;		/* Actual ID used instead of clkd->id, for quirks that need an override */
	struct device *dev;
	struct scmi_clock_desc *clkd;
};

static void iter_clk_possible_parents_prepare_message(void *message, unsigned int desc_index,
						      const void *priv)
{
	struct scmi_msg_clock_possible_parents *msg = message;
	const struct scmi_clk_ipriv *p = priv;

	msg->id = cpu_to_le32(p->id);
	/* Set the number of OPPs to be skipped/already read */
	msg->skip_parents = cpu_to_le32(desc_index);
}

static int iter_clk_possible_parents_update_state(struct scmi_iterator_state *st,
						  const void *response, void *priv)
{
	const struct scmi_msg_resp_clock_possible_parents *r = response;
	struct scmi_clk_ipriv *p = priv;
	u32 flags;

	flags = le32_to_cpu(r->num_parent_flags);
	st->num_returned = NUM_PARENTS_RETURNED(flags);
	st->num_remaining = NUM_PARENTS_REMAINING(flags);

	/*
	 * num parents is not declared previously anywhere so we
	 * assume it's returned+remaining on first call.
	 */
	if (!st->max_resources) {
		int num_parents = st->num_returned + st->num_remaining;

		p->clkd->info.parents = devm_kcalloc(p->dev, num_parents,
						     sizeof(*p->clkd->info.parents),
						     GFP_KERNEL);
		if (!p->clkd->info.parents)
			return -ENOMEM;

		/* max_resources is used by the iterators to control bounds */
		st->max_resources = st->num_returned + st->num_remaining;
	}

	return 0;
}

static int iter_clk_possible_parents_process_response(const struct scmi_protocol_handle *ph,
						      const void *response,
						      struct scmi_iterator_state *st,
						      void *priv)
{
	const struct scmi_msg_resp_clock_possible_parents *r = response;
	struct scmi_clk_ipriv *p = priv;

	p->clkd->info.parents[st->desc_index + st->loop_idx] =
		le32_to_cpu(r->possible_parents[st->loop_idx]);

	/* Count only effectively discovered parents */
	p->clkd->info.num_parents++;

	return 0;
}

static int scmi_clock_possible_parents(const struct scmi_protocol_handle *ph,
				       u32 clk_id, struct clock_info *cinfo)
{
	struct scmi_iterator_ops ops = {
		.prepare_message = iter_clk_possible_parents_prepare_message,
		.update_state = iter_clk_possible_parents_update_state,
		.process_response = iter_clk_possible_parents_process_response,
	};
	struct scmi_clock_desc *clkd = &cinfo->clkds[clk_id];
	struct scmi_clk_ipriv ppriv = {
		.id = clk_id,
		.clkd = clkd,
		.dev = ph->dev,
	};
	void *iter;

	iter = ph->hops->iter_response_init(ph, &ops, 0,
					    CLOCK_POSSIBLE_PARENTS_GET,
					    sizeof(struct scmi_msg_clock_possible_parents),
					    &ppriv);
	if (IS_ERR(iter))
		return PTR_ERR(iter);

	return ph->hops->iter_response_run(iter);
}

static int
scmi_clock_get_permissions(const struct scmi_protocol_handle *ph, u32 clk_id,
			   struct scmi_clock_info *clk)
{
	struct scmi_xfer *t;
	u32 perm;
	int ret;

	ret = ph->xops->xfer_get_init(ph, CLOCK_GET_PERMISSIONS,
				      sizeof(clk_id), sizeof(perm), &t);
	if (ret)
		return ret;

	put_unaligned_le32(clk_id, t->tx.buf);

	ret = ph->xops->do_xfer(ph, t);
	if (!ret) {
		perm = get_unaligned_le32(t->rx.buf);

		clk->state_ctrl_forbidden = !(perm & CLOCK_STATE_CONTROL_ALLOWED);
		clk->rate_ctrl_forbidden = !(perm & CLOCK_RATE_CONTROL_ALLOWED);
		clk->parent_ctrl_forbidden = !(perm & CLOCK_PARENT_CONTROL_ALLOWED);
	}

	ph->xops->xfer_put(ph, t);

	return ret;
}

static void quirk_rcar_x5h_crit_clocks_fixup(struct scmi_clock_info *clk)
{
	clk->state_ctrl_forbidden = true;
	clk->rate_ctrl_forbidden = true;
	clk->parent_ctrl_forbidden = true;
}

#define QUIRK_RCAR_X5H_4_28_CRIT_CLOCKS					\
	({								\
		switch (clk_id) {					\
		case 468:		/* MDLC_INTAP0 */		\
		case 498 ... 505:	/* MDLC_APRTMGINT0..7 */	\
		case 838 ... 861:	/* CLK_ZC0/ZC1/ZD_APU0..7 */	\
			quirk_rcar_x5h_crit_clocks_fixup(clk);		\
			break;						\
		}							\
	})

#define QUIRK_RCAR_X5H_4_31_CRIT_CLOCKS					\
	({								\
		switch (clk_id) {					\
		case 464:		/* MDLC_INTAP0 */		\
		case 494 ... 501:	/* MDLC_APRTMGINT0..7 */	\
		case 834 ... 857:	/* CLK_ZC0/ZC1/ZD_APU0..7 */	\
			quirk_rcar_x5h_crit_clocks_fixup(clk);		\
			break;						\
		}							\
	})

struct quirk_rcar_x5h_no_attributes {
	u32 id;
	const char *name;
};

static const struct quirk_rcar_x5h_no_attributes quirk_rcar_x5h_4_28_no_attributes[] = {
	// FIXME We don't need all of them (1165!)
	{ 228,	"hscif0" },		// -EOPNOTSUPP
	{ 1649,	"sgd4_perw_main" },	// -EOPNOTSUPP
	{ 1661,	"sgd4_perw_bus" },	// -EOPNOTSUPP
	{ 1663,	"sgd16_perw_bus" },	// -EOPNOTSUPP
	{ 1673,	"sgd4_mp_main" },	// -EOPNOTSUPP
	{ 1680,	"sgd4_mp_bus" },	// -EOPNOTSUPP
	{ 1690,	"s0d4_pere_main" },	// -EOPNOTSUPP
#if 0
	{ 0,	"vipn_fcpcs0" },	// -EREMOTEIO
	{ 1,	"vipn_fcpcs1" },	// -EREMOTEIO
	{ 2,	"vipn_vcp5x0" },	// -EREMOTEIO
	{ 3,	"vipn_vcp5x1" },	// -EREMOTEIO
	{ 4,	"vipn_msync" },		// -EOPNOTSUPP
	{ 5,	"vipn_umfl0" },		// -EREMOTEIO
	{ 6,	"vipn_umfl1" },		// -EREMOTEIO
	{ 7,	"vips_fcpcs0" },	// -EREMOTEIO
	{ 8,	"vips_fcpcs1" },	// -EREMOTEIO
	{ 9,	"vips_vcp5x0" },	// -EREMOTEIO
	{ 10,	"vips_vcp5x1" },	// -EREMOTEIO
	{ 11,	"vips_msync" },		// -EOPNOTSUPP
	{ 12,	"vips_umfl0" },		// -EREMOTEIO
	{ 13,	"vips_umfl1" },		// -EREMOTEIO
	{ 15,	"isp0" },		// -EREMOTEIO
	{ 16,	"isp1" },		// -EREMOTEIO
	{ 17,	"isp2" },		// -EREMOTEIO
	{ 18,	"isp3" },		// -EREMOTEIO
	{ 19,	"ispcs0" },		// -EREMOTEIO
	{ 20,	"ispcs1" },		// -EREMOTEIO
	{ 21,	"ispcs2" },		// -EREMOTEIO
	{ 22,	"ispcs3" },		// -EREMOTEIO
	{ 23,	"csitop0" },		// -EREMOTEIO
	{ 24,	"csitop1" },		// -EREMOTEIO
	{ 25,	"csitop2" },		// -EREMOTEIO
	{ 26,	"csitop3" },		// -EREMOTEIO
	{ 27,	"dptx0" },		// -EREMOTEIO
	{ 28,	"dptx1" },		// -EREMOTEIO
	{ 29,	"dptx2" },		// -EREMOTEIO
	{ 30,	"vspd0" },		// -EREMOTEIO
	{ 31,	"vspd1" },		// -EREMOTEIO
	{ 32,	"vspd2" },		// -EREMOTEIO
	{ 33,	"vspd3" },		// -EREMOTEIO
	{ 34,	"vspd4" },		// -EREMOTEIO
	{ 35,	"vspdb0" },		// -EREMOTEIO
	{ 36,	"vspdb1" },		// -EREMOTEIO
	{ 37,	"vspdb2" },		// -EREMOTEIO
	{ 38,	"vspdb3" },		// -EREMOTEIO
	{ 39,	"vspdb4" },		// -EREMOTEIO
	{ 40,	"vspx0" },		// -EREMOTEIO
	{ 41,	"vspx1" },		// -EREMOTEIO
	{ 42,	"vspx2" },		// -EREMOTEIO
	{ 43,	"vspx3" },		// -EREMOTEIO
	{ 44,	"fcpvd0" },		// -EREMOTEIO
	{ 45,	"fcpvd1" },		// -EREMOTEIO
	{ 46,	"fcpvd2" },		// -EREMOTEIO
	{ 47,	"fcpvd3" },		// -EREMOTEIO
	{ 48,	"fcpvd4" },		// -EREMOTEIO
	{ 49,	"fcpvd5" },		// -EREMOTEIO
	{ 50,	"fcpvd6" },		// -EREMOTEIO
	{ 51,	"fcpvd7" },		// -EREMOTEIO
	{ 52,	"fcpvd8" },		// -EREMOTEIO
	{ 53,	"fcpvd9" },		// -EREMOTEIO
	{ 56,	"fcpvx0" },		// -EREMOTEIO
	{ 57,	"fcpvx1" },		// -EREMOTEIO
	{ 58,	"fcpvx2" },		// -EREMOTEIO
	{ 59,	"fcpvx3" },		// -EREMOTEIO
	{ 60,	"vin000" },		// -EREMOTEIO
	{ 61,	"vin001" },		// -EREMOTEIO
	{ 62,	"vin002" },		// -EREMOTEIO
	{ 63,	"vin003" },		// -EREMOTEIO
	{ 64,	"vin004" },		// -EREMOTEIO
	{ 65,	"vin005" },		// -EREMOTEIO
	{ 66,	"vin006" },		// -EREMOTEIO
	{ 67,	"vin007" },		// -EREMOTEIO
	{ 68,	"vin010" },		// -EREMOTEIO
	{ 69,	"vin011" },		// -EREMOTEIO
	{ 70,	"vin012" },		// -EREMOTEIO
	{ 71,	"vin013" },		// -EREMOTEIO
	{ 72,	"vin014" },		// -EREMOTEIO
	{ 73,	"vin015" },		// -EREMOTEIO
	{ 74,	"vin016" },		// -EREMOTEIO
	{ 75,	"vin017" },		// -EREMOTEIO
	{ 76,	"vin020" },		// -EREMOTEIO
	{ 77,	"vin021" },		// -EREMOTEIO
	{ 78,	"vin022" },		// -EREMOTEIO
	{ 79,	"vin023" },		// -EREMOTEIO
	{ 80,	"vin024" },		// -EREMOTEIO
	{ 81,	"vin025" },		// -EREMOTEIO
	{ 82,	"vin026" },		// -EREMOTEIO
	{ 83,	"vin027" },		// -EREMOTEIO
	{ 84,	"vin030" },		// -EREMOTEIO
	{ 85,	"vin031" },		// -EREMOTEIO
	{ 86,	"vin032" },		// -EREMOTEIO
	{ 87,	"vin033" },		// -EREMOTEIO
	{ 88,	"vin034" },		// -EREMOTEIO
	{ 89,	"vin035" },		// -EREMOTEIO
	{ 90,	"vin036" },		// -EREMOTEIO
	{ 91,	"vin037" },		// -EREMOTEIO
	{ 92,	"vin040" },		// -EREMOTEIO
	{ 93,	"vin041" },		// -EREMOTEIO
	{ 94,	"vin042" },		// -EREMOTEIO
	{ 95,	"vin043" },		// -EREMOTEIO
	{ 96,	"vin044" },		// -EREMOTEIO
	{ 97,	"vin045" },		// -EREMOTEIO
	{ 98,	"vin046" },		// -EREMOTEIO
	{ 99,	"vin047" },		// -EREMOTEIO
	{ 100,	"vin050" },		// -EREMOTEIO
	{ 101,	"vin051" },		// -EREMOTEIO
	{ 102,	"vin052" },		// -EREMOTEIO
	{ 103,	"vin053" },		// -EREMOTEIO
	{ 104,	"vin054" },		// -EREMOTEIO
	{ 105,	"vin055" },		// -EREMOTEIO
	{ 106,	"vin056" },		// -EREMOTEIO
	{ 107,	"vin057" },		// -EREMOTEIO
	{ 108,	"vin060" },		// -EREMOTEIO
	{ 109,	"vin061" },		// -EREMOTEIO
	{ 110,	"vin062" },		// -EREMOTEIO
	{ 111,	"vin063" },		// -EREMOTEIO
	{ 112,	"vin064" },		// -EREMOTEIO
	{ 113,	"vin065" },		// -EREMOTEIO
	{ 114,	"vin066" },		// -EREMOTEIO
	{ 115,	"vin067" },		// -EREMOTEIO
	{ 116,	"vin070" },		// -EREMOTEIO
	{ 117,	"vin071" },		// -EREMOTEIO
	{ 118,	"vin072" },		// -EREMOTEIO
	{ 119,	"vin073" },		// -EREMOTEIO
	{ 120,	"vin074" },		// -EREMOTEIO
	{ 121,	"vin075" },		// -EREMOTEIO
	{ 122,	"vin076" },		// -EREMOTEIO
	{ 123,	"vin077" },		// -EREMOTEIO
	{ 124,	"vin080" },		// -EREMOTEIO
	{ 125,	"vin081" },		// -EREMOTEIO
	{ 126,	"vin082" },		// -EREMOTEIO
	{ 127,	"vin083" },		// -EREMOTEIO
	{ 128,	"vin084" },		// -EREMOTEIO
	{ 129,	"vin085" },		// -EREMOTEIO
	{ 130,	"vin086" },		// -EREMOTEIO
	{ 131,	"vin087" },		// -EREMOTEIO
	{ 132,	"vin090" },		// -EREMOTEIO
	{ 133,	"vin091" },		// -EREMOTEIO
	{ 134,	"vin092" },		// -EREMOTEIO
	{ 135,	"vin093" },		// -EREMOTEIO
	{ 136,	"vin094" },		// -EREMOTEIO
	{ 137,	"vin095" },		// -EREMOTEIO
	{ 138,	"vin096" },		// -EREMOTEIO
	{ 139,	"vin097" },		// -EREMOTEIO
	{ 140,	"vin100" },		// -EREMOTEIO
	{ 141,	"vin101" },		// -EREMOTEIO
	{ 142,	"vin102" },		// -EREMOTEIO
	{ 143,	"vin103" },		// -EREMOTEIO
	{ 144,	"vin104" },		// -EREMOTEIO
	{ 145,	"vin105" },		// -EREMOTEIO
	{ 146,	"vin106" },		// -EREMOTEIO
	{ 147,	"vin107" },		// -EREMOTEIO
	{ 148,	"vin110" },		// -EREMOTEIO
	{ 149,	"vin111" },		// -EREMOTEIO
	{ 150,	"vin112" },		// -EREMOTEIO
	{ 151,	"vin113" },		// -EREMOTEIO
	{ 152,	"vin114" },		// -EREMOTEIO
	{ 153,	"vin115" },		// -EREMOTEIO
	{ 154,	"vin116" },		// -EREMOTEIO
	{ 155,	"vin117" },		// -EREMOTEIO
	{ 157,	"vcon0" },		// -EREMOTEIO
	{ 158,	"vcon1" },		// -EREMOTEIO
	{ 159,	"vcon2" },		// -EREMOTEIO
	{ 160,	"vcon3" },		// -EREMOTEIO
	{ 161,	"vcon4" },		// -EREMOTEIO
	{ 162,	"vcon5" },		// -EREMOTEIO
	{ 163,	"vcon6" },		// -EREMOTEIO
	{ 164,	"vcon7" },		// -EREMOTEIO
	{ 165,	"vcon8" },		// -EREMOTEIO
	{ 166,	"vcon9" },		// -EREMOTEIO
	{ 167,	"vspb0" },		// -EREMOTEIO
	{ 168,	"vspb1" },		// -EREMOTEIO
	{ 169,	"vspb2" },		// -EREMOTEIO
	{ 170,	"vspb3" },		// -EREMOTEIO
	{ 171,	"vspb4" },		// -EREMOTEIO
	{ 172,	"vspi0" },		// -EREMOTEIO
	{ 173,	"vspi1" },		// -EREMOTEIO
	{ 174,	"vspi2" },		// -EREMOTEIO
	{ 175,	"vspi3" },		// -EREMOTEIO
	{ 176,	"fcpvb0" },		// -EREMOTEIO
	{ 177,	"fcpvb1" },		// -EREMOTEIO
	{ 178,	"fcpvb2" },		// -EREMOTEIO
	{ 179,	"fcpvb3" },		// -EREMOTEIO
	{ 180,	"fcpvb4" },		// -EREMOTEIO
	{ 181,	"fcpvi0" },		// -EREMOTEIO
	{ 182,	"fcpvi1" },		// -EREMOTEIO
	{ 183,	"fcpvi2" },		// -EREMOTEIO
	{ 184,	"fcpvi3" },		// -EREMOTEIO
	{ 185,	"vin0" },		// -EREMOTEIO
	{ 186,	"vin1" },		// -EREMOTEIO
	{ 187,	"vin2" },		// -EREMOTEIO
	{ 188,	"vin3" },		// -EREMOTEIO
	{ 189,	"vin4" },		// -EREMOTEIO
	{ 190,	"vin5" },		// -EREMOTEIO
	{ 191,	"vin6" },		// -EREMOTEIO
	{ 192,	"vin7" },		// -EREMOTEIO
	{ 193,	"vin8" },		// -EREMOTEIO
	{ 194,	"vin9" },		// -EREMOTEIO
	{ 195,	"vin10" },		// -EREMOTEIO
	{ 196,	"vin11" },		// -EREMOTEIO
	{ 198,	"pere_gpiodm1" },	// -EOPNOTSUPP
	{ 199,	"pere_gpiodm2" },	// -EOPNOTSUPP
	{ 200,	"pere_gpiodm3" },	// -EOPNOTSUPP
	{ 206,	"perw_gpiodm1" },	// -EOPNOTSUPP
	{ 207,	"perw_gpiodm2" },	// -EOPNOTSUPP
	{ 208,	"perw_gpiodm3" },	// -EOPNOTSUPP
	{ 337,	"hscn_gpiodm1" },	// -EOPNOTSUPP
	{ 338,	"hscn_gpiodm2" },	// -EOPNOTSUPP
	{ 339,	"hscn_gpiodm3" },	// -EOPNOTSUPP
	{ 340,	"us30" },		// -EREMOTEIO
	{ 341,	"us31" },		// -EREMOTEIO
	{ 342,	"us32" },		// -EREMOTEIO
	{ 343,	"us33" },		// -EREMOTEIO
	{ 350,	"pci411" },		// -EREMOTEIO
	{ 351,	"pci402" },		// -EOPNOTSUPP
	{ 352,	"pci412" },		// -EOPNOTSUPP
	{ 353,	"cr52top0" },		// -EOPNOTSUPP
	{ 356,	"cr52core0_po" },	// -EOPNOTSUPP
	{ 358,	"cr52core1_po" },	// -EOPNOTSUPP
	{ 360,	"cr52shadow0_po" },	// -EOPNOTSUPP
	{ 362,	"cr52shadow1_po" },	// -EOPNOTSUPP
	{ 363,	"cr52top1" },		// -EOPNOTSUPP
	{ 365,	"cr52core2" },		// -EOPNOTSUPP
	{ 366,	"cr52core2_po" },	// -EOPNOTSUPP
	{ 368,	"cr52core3_po" },	// -EOPNOTSUPP
	{ 370,	"cr52shadow2_po" },	// -EOPNOTSUPP
	{ 372,	"cr52shadow3_po" },	// -EOPNOTSUPP
	{ 373,	"cr52top2" },		// -EOPNOTSUPP
	{ 376,	"cr52core4_po" },	// -EOPNOTSUPP
	{ 378,	"cr52core5_po" },	// -EOPNOTSUPP
	{ 380,	"cr52shadow4_po" },	// -EOPNOTSUPP
	{ 382,	"cr52shadow5_po" },	// -EOPNOTSUPP
	{ 392,	"wdt1" },		// -EOPNOTSUPP
	{ 393,	"wwdt00" },		// -EOPNOTSUPP
	{ 394,	"wwdt10" },		// -EOPNOTSUPP
	{ 395,	"wwdt20" },		// -EOPNOTSUPP
	{ 396,	"wwdt30" },		// -EOPNOTSUPP
	{ 397,	"wwdt40" },		// -EOPNOTSUPP
	{ 398,	"wwdt50" },		// -EOPNOTSUPP
	{ 399,	"wwdt60" },		// -EOPNOTSUPP
	{ 400,	"wwdt70" },		// -EOPNOTSUPP
	{ 401,	"wwdt80" },		// -EOPNOTSUPP
	{ 402,	"wwdt90" },		// -EOPNOTSUPP
	{ 403,	"wwdt100" },		// -EOPNOTSUPP
	{ 404,	"wwdt110" },		// -EOPNOTSUPP
	{ 405,	"wwdt120" },		// -EOPNOTSUPP
	{ 406,	"wwdt130" },		// -EOPNOTSUPP
	{ 407,	"wwdt01" },		// -EOPNOTSUPP
	{ 408,	"wwdt11" },		// -EOPNOTSUPP
	{ 409,	"wwdt21" },		// -EOPNOTSUPP
	{ 410,	"wwdt31" },		// -EOPNOTSUPP
	{ 411,	"wwdt41" },		// -EOPNOTSUPP
	{ 412,	"wwdt51" },		// -EOPNOTSUPP
	{ 413,	"wwdt61" },		// -EOPNOTSUPP
	{ 414,	"wwdt71" },		// -EOPNOTSUPP
	{ 415,	"wwdt81" },		// -EOPNOTSUPP
	{ 416,	"wwdt91" },		// -EOPNOTSUPP
	{ 417,	"wwdt101" },		// -EOPNOTSUPP
	{ 418,	"wwdt111" },		// -EOPNOTSUPP
	{ 419,	"wwdt121" },		// -EOPNOTSUPP
	{ 420,	"wwdt131" },		// -EOPNOTSUPP
	{ 421,	"wwdt140" },		// -EOPNOTSUPP
	{ 422,	"wwdt141" },		// -EOPNOTSUPP
	{ 423,	"wwdt150" },		// -EOPNOTSUPP
	{ 424,	"wwdt151" },		// -EOPNOTSUPP
	{ 425,	"wwdt160" },		// -EOPNOTSUPP
	{ 426,	"wwdt161" },		// -EOPNOTSUPP
	{ 427,	"wwdt170" },		// -EOPNOTSUPP
	{ 428,	"wwdt171" },		// -EOPNOTSUPP
	{ 429,	"wwdt180" },		// -EOPNOTSUPP
	{ 430,	"wwdt181" },		// -EOPNOTSUPP
	{ 431,	"wwdt190" },		// -EOPNOTSUPP
	{ 432,	"wwdt191" },		// -EOPNOTSUPP
	{ 433,	"swdt0" },		// -EOPNOTSUPP
	{ 434,	"swdt1" },		// -EOPNOTSUPP
	{ 469,	"inttp" },		// -EOPNOTSUPP
	{ 470,	"intap1" },		// -EOPNOTSUPP
	{ 506,	"cxsrtmg" },		// -EOPNOTSUPP
	{ 507,	"cmn_topn_gic" },	// -EOPNOTSUPP
	{ 508,	"cmn_topn_dbg" },	// -EOPNOTSUPP
	{ 509,	"cmn_tope" },		// -EOPNOTSUPP
	{ 510,	"cmn_tops" },		// -EOPNOTSUPP
	{ 511,	"cmn_topw" },		// -EOPNOTSUPP
	{ 517,	"scmt" },		// -EOPNOTSUPP
	{ 526,	"intap_cmn2top0" },	// -EOPNOTSUPP
	{ 527,	"pci60bg0" },		// -EOPNOTSUPP
	{ 528,	"pci60bg1" },		// -EOPNOTSUPP
	{ 542,	"pci601" },		// -EREMOTEIO
	{ 543,	"pci611" },		// -EOPNOTSUPP
	{ 544,	"pci602" },		// -EREMOTEIO
	{ 545,	"pci612" },		// -EOPNOTSUPP
	{ 546,	"imn_imr000" },		// -EREMOTEIO
	{ 547,	"imn_imr001" },		// -EREMOTEIO
	{ 548,	"imn_ims00" },		// -EREMOTEIO
	{ 549,	"imn_ims01" },		// -EREMOTEIO
	{ 550,	"imn_ims02" },		// -EREMOTEIO
	{ 551,	"imn_ims03" },		// -EREMOTEIO
	{ 552,	"ims_imr000" },		// -EREMOTEIO
	{ 553,	"ims_imr001" },		// -EREMOTEIO
	{ 554,	"ims_ims00" },		// -EREMOTEIO
	{ 555,	"ims_ims01" },		// -EREMOTEIO
	{ 556,	"ims_ims02" },		// -EREMOTEIO
	{ 557,	"ims_ims03" },		// -EREMOTEIO
	{ 558,	"rgx_jones0" },		// -EREMOTEIO
	{ 559,	"rgx_mercer0" },	// -EREMOTEIO
	{ 560,	"rgx_mercer1" },	// -EREMOTEIO
	{ 561,	"rgx_mercer2" },	// -EREMOTEIO
	{ 562,	"rgx_mercer3" },	// -EREMOTEIO
	{ 563,	"rgx_texas0" },		// -EREMOTEIO
	{ 564,	"rgx_texas1" },		// -EREMOTEIO
	{ 565,	"rgx_swift0" },		// -EREMOTEIO
	{ 566,	"rgx_swift1" },		// -EREMOTEIO
	{ 567,	"rgx_swift2" },		// -EREMOTEIO
	{ 568,	"rgx_swift3" },		// -EREMOTEIO
	{ 569,	"rgx_jones1" },		// -EREMOTEIO
	{ 570,	"rgx_mercer4" },	// -EREMOTEIO
	{ 571,	"rgx_mercer5" },	// -EREMOTEIO
	{ 572,	"rgx_mercer6" },	// -EREMOTEIO
	{ 573,	"rgx_mercer7" },	// -EREMOTEIO
	{ 574,	"rgx_texas2" },		// -EREMOTEIO
	{ 575,	"rgx_texas3" },		// -EREMOTEIO
	{ 576,	"rgx_swift4" },		// -EREMOTEIO
	{ 577,	"rgx_swift5" },		// -EREMOTEIO
	{ 578,	"rgx_swift6" },		// -EREMOTEIO
	{ 579,	"rgx_swift7" },		// -EREMOTEIO
	{ 582,	"dsparcsyn_axi" },	// -EOPNOTSUPP
	{ 585,	"dsp2_misc_pores" },	// -EOPNOTSUPP
	{ 586,	"dsp2_misc_atres" },	// -EOPNOTSUPP
	{ 588,	"dsp2_core0_csd" },	// -EOPNOTSUPP
	{ 590,	"dsp2_core1_csd" },	// -EOPNOTSUPP
	{ 592,	"dsp2_core2_csd" },	// -EOPNOTSUPP
	{ 594,	"dsp2_core3_csd" },	// -EOPNOTSUPP
	{ 597,	"dsp3_misc_pores" },	// -EOPNOTSUPP
	{ 598,	"dsp3_misc_atres" },	// -EOPNOTSUPP
	{ 600,	"dsp3_core0_csd" },	// -EOPNOTSUPP
	{ 602,	"dsp3_core1_csd" },	// -EOPNOTSUPP
	{ 604,	"dsp3_core2_csd" },	// -EOPNOTSUPP
	{ 606,	"dsp3_core3_csd" },	// -EOPNOTSUPP
	{ 609,	"dsp4_misc_pores" },	// -EOPNOTSUPP
	{ 610,	"dsp4_misc_atres" },	// -EOPNOTSUPP
	{ 612,	"dsp4_core0_csd" },	// -EOPNOTSUPP
	{ 614,	"dsp4_core1_csd" },	// -EOPNOTSUPP
	{ 616,	"dsp4_core2_csd" },	// -EOPNOTSUPP
	{ 618,	"dsp4_core3_csd" },	// -EOPNOTSUPP
	{ 621,	"dsp5_misc_pores" },	// -EOPNOTSUPP
	{ 622,	"dsp5_misc_atres" },	// -EOPNOTSUPP
	{ 624,	"dsp5_core0_csd" },	// -EOPNOTSUPP
	{ 626,	"dsp5_core1_csd" },	// -EOPNOTSUPP
	{ 628,	"dsp5_core2_csd" },	// -EOPNOTSUPP
	{ 630,	"dsp5_core3_csd" },	// -EOPNOTSUPP
	{ 633,	"dsp6_misc_pores" },	// -EOPNOTSUPP
	{ 634,	"dsp6_misc_atres" },	// -EOPNOTSUPP
	{ 636,	"dsp6_core0_csd" },	// -EOPNOTSUPP
	{ 638,	"dsp6_core1_csd" },	// -EOPNOTSUPP
	{ 640,	"dsp6_core2_csd" },	// -EOPNOTSUPP
	{ 642,	"dsp6_core3_csd" },	// -EOPNOTSUPP
	{ 661,	"npu0_atres" },		// -EOPNOTSUPP
	{ 663,	"npu0_axi" },		// -EOPNOTSUPP
	{ 664,	"npu0_nl2" },		// -EOPNOTSUPP
	{ 665,	"npu0_nl2arc0" },	// -EOPNOTSUPP
	{ 666,	"npu0_nl2arc1" },	// -EOPNOTSUPP
	{ 667,	"npu0_nl1grp0" },	// -EOPNOTSUPP
	{ 668,	"npu0_sl0nl1arc" },	// -EOPNOTSUPP
	{ 669,	"npu0_sl1nl1arc" },	// -EOPNOTSUPP
	{ 670,	"npu0_sl2nl1arc" },	// -EOPNOTSUPP
	{ 671,	"npu0_nl1grp1" },	// -EOPNOTSUPP
	{ 672,	"npu0_sl3nl1arc" },	// -EOPNOTSUPP
	{ 673,	"npu0_sl4nl1arc" },	// -EOPNOTSUPP
	{ 674,	"npu0_sl5nl1arc" },	// -EOPNOTSUPP
	{ 675,	"npu0_nl1grp2" },	// -EOPNOTSUPP
	{ 676,	"npu0_sl6nl1arc" },	// -EOPNOTSUPP
	{ 677,	"npu0_sl7nl1arc" },	// -EOPNOTSUPP
	{ 678,	"npu0_sl8nl1arc" },	// -EOPNOTSUPP
	{ 679,	"npu0_nl1grp3" },	// -EOPNOTSUPP
	{ 680,	"npu0_sl9nl1arc" },	// -EOPNOTSUPP
	{ 681,	"npu0_sl10nl1arc" },	// -EOPNOTSUPP
	{ 682,	"npu0_sl11nl1arc" },	// -EOPNOTSUPP
	{ 685,	"npu0_aon_noc" },	// -EOPNOTSUPP
	{ 686,	"npu0_aon_cfg" },	// -EOPNOTSUPP
	{ 687,	"npu0_aon_csd" },	// -EOPNOTSUPP
	{ 688,	"npu0_core0_grp0" },	// -EREMOTEIO
	{ 689,	"npu0_core1_grp0" },	// -EREMOTEIO
	{ 690,	"npu0_core2_grp0" },	// -EREMOTEIO
	{ 691,	"npu0_core0_grp1" },	// -EREMOTEIO
	{ 692,	"npu0_core1_grp1" },	// -EREMOTEIO
	{ 693,	"npu0_core2_grp1" },	// -EREMOTEIO
	{ 694,	"npu0_core0_grp2" },	// -EREMOTEIO
	{ 695,	"npu0_core1_grp2" },	// -EREMOTEIO
	{ 696,	"npu0_core2_grp2" },	// -EREMOTEIO
	{ 697,	"npu0_core0_grp3" },	// -EREMOTEIO
	{ 698,	"npu0_core1_grp3" },	// -EREMOTEIO
	{ 699,	"npu0_core2_grp3" },	// -EREMOTEIO
	{ 700,	"npu0_pres" },		// -EREMOTEIO
	{ 703,	"npu0_misc_pres" },	// -EOPNOTSUPP
	{ 704,	"npu0_misc_atres" },	// -EOPNOTSUPP
	{ 706,	"npu0_c0_dsp_csd" },	// -EOPNOTSUPP
	{ 708,	"npu0_c1_dsp_csd" },	// -EOPNOTSUPP
	{ 710,	"npu0_c2_dsp_csd" },	// -EOPNOTSUPP
	{ 712,	"npu0_c3_dsp_csd" },	// -EOPNOTSUPP
	{ 715,	"npu1_atres" },		// -EOPNOTSUPP
	{ 717,	"npu1_axi" },		// -EOPNOTSUPP
	{ 718,	"npu1_nl2" },		// -EOPNOTSUPP
	{ 719,	"npu1_nl2arc0" },	// -EOPNOTSUPP
	{ 720,	"npu1_nl2arc1" },	// -EOPNOTSUPP
	{ 721,	"npu1_nl1grp0" },	// -EOPNOTSUPP
	{ 722,	"npu1_sl0nl1arc" },	// -EOPNOTSUPP
	{ 723,	"npu1_sl1nl1arc" },	// -EOPNOTSUPP
	{ 724,	"npu1_sl2nl1arc" },	// -EOPNOTSUPP
	{ 725,	"npu1_nl1grp1" },	// -EOPNOTSUPP
	{ 726,	"npu1_sl3nl1arc" },	// -EOPNOTSUPP
	{ 727,	"npu1_sl4nl1arc" },	// -EOPNOTSUPP
	{ 728,	"npu1_sl5nl1arc" },	// -EOPNOTSUPP
	{ 729,	"npu1_nl1grp2" },	// -EOPNOTSUPP
	{ 730,	"npu1_sl6nl1arc" },	// -EOPNOTSUPP
	{ 731,	"npu1_sl7nl1arc" },	// -EOPNOTSUPP
	{ 732,	"npu1_sl8nl1arc" },	// -EOPNOTSUPP
	{ 733,	"npu1_nl1grp3" },	// -EOPNOTSUPP
	{ 734,	"npu1_sl9nl1arc" },	// -EOPNOTSUPP
	{ 735,	"npu1_sl10nl1arc" },	// -EOPNOTSUPP
	{ 736,	"npu1_sl11nl1arc" },	// -EOPNOTSUPP
	{ 739,	"npu1_aon_noc" },	// -EOPNOTSUPP
	{ 740,	"npu1_aon_cfg" },	// -EOPNOTSUPP
	{ 741,	"npu1_aon_csd" },	// -EOPNOTSUPP
	{ 742,	"npu1_core0_grp0" },	// -EREMOTEIO
	{ 743,	"npu1_core1_grp0" },	// -EREMOTEIO
	{ 744,	"npu1_core2_grp0" },	// -EREMOTEIO
	{ 745,	"npu1_core0_grp1" },	// -EREMOTEIO
	{ 746,	"npu1_core1_grp1" },	// -EREMOTEIO
	{ 747,	"npu1_core2_grp1" },	// -EREMOTEIO
	{ 748,	"npu1_core0_grp2" },	// -EREMOTEIO
	{ 749,	"npu1_core1_grp2" },	// -EREMOTEIO
	{ 750,	"npu1_core2_grp2" },	// -EREMOTEIO
	{ 751,	"npu1_core0_grp3" },	// -EREMOTEIO
	{ 752,	"npu1_core1_grp3" },	// -EREMOTEIO
	{ 753,	"npu1_core2_grp3" },	// -EREMOTEIO
	{ 754,	"npu1_pres" },		// -EREMOTEIO
	{ 757,	"npu1_misc_pres" },	// -EOPNOTSUPP
	{ 758,	"npu1_misc_atres" },	// -EOPNOTSUPP
	{ 760,	"npu1_c0_dsp_csd" },	// -EOPNOTSUPP
	{ 762,	"npu1_c1_dsp_csd" },	// -EOPNOTSUPP
	{ 764,	"npu1_c2_dsp_csd" },	// -EOPNOTSUPP
	{ 766,	"npu1_c3_dsp_csd" },	// -EOPNOTSUPP
	{ 768,	"cmn_core0_pores" },	// -EOPNOTSUPP
	{ 769,	"cmn_core0_syres" },	// -EOPNOTSUPP
	{ 770,	"cmn_core1_pores" },	// -EOPNOTSUPP
	{ 771,	"cmn_core1_syres" },	// -EOPNOTSUPP
	{ 772,	"cmn_core2_pores" },	// -EOPNOTSUPP
	{ 773,	"cmn_core2_syres" },	// -EOPNOTSUPP
	{ 774,	"cmn_core3_pores" },	// -EOPNOTSUPP
	{ 775,	"cmn_core3_syres" },	// -EOPNOTSUPP
	{ 776,	"scp" },		// -EOPNOTSUPP
	{ 779,	"tauj1" },		// -EOPNOTSUPP
	{ 806,	"fray01" },		// -EOPNOTSUPP
	{ 808,	"wwdt200" },		// -EOPNOTSUPP
	{ 809,	"wwdt201" },		// -EOPNOTSUPP
	{ 812,	"intscp" },		// -EOPNOTSUPP
	{ 813,	"tauj3" },		// -EOPNOTSUPP
	{ 814,	"rtca" },		// -EOPNOTSUPP
	{ 816,	"gpiodm1" },		// -EOPNOTSUPP
	{ 817,	"gpiodm2" },		// -EOPNOTSUPP
	{ 818,	"gpiodm3" },		// -EOPNOTSUPP
	{ 819,	"s0d1" },		// -EOPNOTSUPP
	{ 820,	"s0d2" },		// -EOPNOTSUPP
	{ 821,	"s0d4" },		// -EOPNOTSUPP
	{ 822,	"s0d8" },		// -EOPNOTSUPP
	{ 823,	"cl" },			// -EOPNOTSUPP
	{ 824,	"sgd1" },		// -EOPNOTSUPP
	{ 825,	"sgd2" },		// -EOPNOTSUPP
	{ 828,	"cl16m" },		// -EOPNOTSUPP
	{ 837,	"zx" },			// -EOPNOTSUPP
	{ 862,	"s0d1_cmn_busn" },	// -EOPNOTSUPP
	{ 863,	"s0d2_cmn_busn" },	// -EOPNOTSUPP
	{ 864,	"s0d4_cmn_busn" },	// -EOPNOTSUPP
	{ 865,	"zx_cmn_busn" },	// -EOPNOTSUPP
	{ 866,	"s0d1_cmn_buss" },	// -EOPNOTSUPP
	{ 867,	"s0d2_cmn_buss" },	// -EOPNOTSUPP
	{ 868,	"s0d4_cmn_buss" },	// -EOPNOTSUPP
	{ 869,	"zx_cmn_buss" },	// -EOPNOTSUPP
	{ 870,	"zx_cmn_main0" },	// -EOPNOTSUPP
	{ 871,	"zx_cmn_main1" },	// -EOPNOTSUPP
	{ 872,	"cmn_core_grp2" },	// -EOPNOTSUPP
	{ 873,	"cmn_core_grp3" },	// -EOPNOTSUPP
	{ 874,	"cmn_ccg2_grp0" },	// -EOPNOTSUPP
	{ 875,	"cmn_ccg2_grp1" },	// -EOPNOTSUPP
	{ 876,	"cmn_ccg2_grp2" },	// -EOPNOTSUPP
	{ 877,	"cmn_ccg2_grp3" },	// -EOPNOTSUPP
	{ 878,	"cmn_ccg3_grp0" },	// -EOPNOTSUPP
	{ 879,	"cmn_ccg3_grp1" },	// -EOPNOTSUPP
	{ 880,	"cmn_ccg3_grp2" },	// -EOPNOTSUPP
	{ 881,	"cmn_ccg3_grp3" },	// -EOPNOTSUPP
	{ 882,	"sgd1_vio_other" },	// -EOPNOTSUPP
	{ 883,	"sgd2_vio_other" },	// -EOPNOTSUPP
	{ 884,	"sgd4_vio_other" },	// -EOPNOTSUPP
	{ 885,	"sgd8_vio_other" },	// -EOPNOTSUPP
	{ 886,	"sgd16_vio_other" },	// -EOPNOTSUPP
	{ 888,	"sgd1_vio_bus" },	// -EOPNOTSUPP
	{ 889,	"sgd2_vio_bus" },	// -EOPNOTSUPP
	{ 890,	"sgd4_vio_bus" },	// -EOPNOTSUPP
	{ 891,	"sgd8_vio_bus" },	// -EOPNOTSUPP
	{ 892,	"sgd16_vio_bus" },	// -EOPNOTSUPP
	{ 893,	"sgd1_vio_csi0" },	// -EOPNOTSUPP
	{ 894,	"sgd2_vio_csi0" },	// -EOPNOTSUPP
	{ 895,	"sgd4_vio_csi0" },	// -EOPNOTSUPP
	{ 896,	"sgd8_vio_csi0" },	// -EOPNOTSUPP
	{ 897,	"sgd16_vio_csi0" },	// -EOPNOTSUPP
	{ 899,	"sgd1_vio_csi1" },	// -EOPNOTSUPP
	{ 900,	"sgd2_vio_csi1" },	// -EOPNOTSUPP
	{ 901,	"sgd4_vio_csi1" },	// -EOPNOTSUPP
	{ 902,	"sgd8_vio_csi1" },	// -EOPNOTSUPP
	{ 903,	"sgd16_vio_csi1" },	// -EOPNOTSUPP
	{ 905,	"sgd1_vio_csi2" },	// -EOPNOTSUPP
	{ 906,	"sgd2_vio_csi2" },	// -EOPNOTSUPP
	{ 907,	"sgd4_vio_csi2" },	// -EOPNOTSUPP
	{ 908,	"sgd8_vio_csi2" },	// -EOPNOTSUPP
	{ 909,	"sgd16_vio_csi2" },	// -EOPNOTSUPP
	{ 911,	"sgd1_vio_isp0" },	// -EOPNOTSUPP
	{ 912,	"sgd2_vio_isp0" },	// -EOPNOTSUPP
	{ 913,	"sgd4_vio_isp0" },	// -EOPNOTSUPP
	{ 914,	"sgd8_vio_isp0" },	// -EOPNOTSUPP
	{ 915,	"sgd16_vio_isp0" },	// -EOPNOTSUPP
	{ 916,	"sgd1_vio_isp1" },	// -EOPNOTSUPP
	{ 917,	"sgd2_vio_isp1" },	// -EOPNOTSUPP
	{ 918,	"sgd4_vio_isp1" },	// -EOPNOTSUPP
	{ 919,	"sgd8_vio_isp1" },	// -EOPNOTSUPP
	{ 920,	"sgd16_vio_isp1" },	// -EOPNOTSUPP
	{ 921,	"sgd1_vio_isp2" },	// -EOPNOTSUPP
	{ 922,	"sgd2_vio_isp2" },	// -EOPNOTSUPP
	{ 923,	"sgd4_vio_isp2" },	// -EOPNOTSUPP
	{ 924,	"sgd8_vio_isp2" },	// -EOPNOTSUPP
	{ 925,	"sgd16_vio_isp2" },	// -EOPNOTSUPP
	{ 926,	"sgd1_vio_isp3" },	// -EOPNOTSUPP
	{ 927,	"sgd2_vio_isp3" },	// -EOPNOTSUPP
	{ 928,	"sgd4_vio_isp3" },	// -EOPNOTSUPP
	{ 929,	"sgd8_vio_isp3" },	// -EOPNOTSUPP
	{ 930,	"sgd16_vio_isp3" },	// -EOPNOTSUPP
	{ 931,	"s0d1_vio_im0" },	// -EOPNOTSUPP
	{ 932,	"s0d2_vio_im0" },	// -EOPNOTSUPP
	{ 933,	"s0d4_vio_im0" },	// -EOPNOTSUPP
	{ 934,	"s0d1_vio_im1" },	// -EOPNOTSUPP
	{ 935,	"s0d2_vio_im1" },	// -EOPNOTSUPP
	{ 936,	"s0d4_vio_im1" },	// -EOPNOTSUPP
	{ 937,	"s0d1_vio_im2" },	// -EOPNOTSUPP
	{ 938,	"s0d2_vio_im2" },	// -EOPNOTSUPP
	{ 939,	"s0d4_vio_im2" },	// -EOPNOTSUPP
	{ 940,	"sgd1_other0" },	// -EOPNOTSUPP
	{ 941,	"sgd2_other0" },	// -EOPNOTSUPP
	{ 942,	"sgd4_other0" },	// -EOPNOTSUPP
	{ 943,	"sgd8_other0" },	// -EOPNOTSUPP
	{ 944,	"sgd16_other0" },	// -EOPNOTSUPP
	{ 947,	"sgd1_other1" },	// -EOPNOTSUPP
	{ 948,	"sgd2_other1" },	// -EOPNOTSUPP
	{ 949,	"sgd4_other1" },	// -EOPNOTSUPP
	{ 950,	"sgd8_other1" },	// -EOPNOTSUPP
	{ 951,	"sgd16_other1" },	// -EOPNOTSUPP
	{ 954,	"sgd1_other2" },	// -EOPNOTSUPP
	{ 955,	"sgd2_other2" },	// -EOPNOTSUPP
	{ 956,	"sgd4_other2" },	// -EOPNOTSUPP
	{ 957,	"sgd8_other2" },	// -EOPNOTSUPP
	{ 958,	"sgd16_other2" },	// -EOPNOTSUPP
	{ 961,	"sgd1_vio_dp_tx" },	// -EOPNOTSUPP
	{ 962,	"sgd2_vio_dp_tx" },	// -EOPNOTSUPP
	{ 963,	"sgd4_vio_dp_tx" },	// -EOPNOTSUPP
	{ 964,	"sgd8_vio_dp_tx" },	// -EOPNOTSUPP
	{ 965,	"sgd16_vio_dp_tx" },	// -EOPNOTSUPP
	{ 968,	"s0d1_vipn_other" },	// -EOPNOTSUPP
	{ 969,	"s0d2_vipn_other" },	// -EOPNOTSUPP
	{ 970,	"s0d4_vipn_other" },	// -EOPNOTSUPP
	{ 971,	"s0d1_vips_other" },	// -EOPNOTSUPP
	{ 972,	"s0d2_vips_other" },	// -EOPNOTSUPP
	{ 973,	"s0d4_vips_other" },	// -EOPNOTSUPP
	{ 974,	"s0d1_imn_other" },	// -EOPNOTSUPP
	{ 975,	"s0d2_imn_other" },	// -EOPNOTSUPP
	{ 976,	"s0d4_imn_other" },	// -EOPNOTSUPP
	{ 977,	"s0d1_imn_core0" },	// -EOPNOTSUPP
	{ 978,	"s0d2_imn_core0" },	// -EOPNOTSUPP
	{ 979,	"s0d4_imn_core0" },	// -EOPNOTSUPP
	{ 980,	"s0d1_imn_core1" },	// -EOPNOTSUPP
	{ 981,	"s0d2_imn_core1" },	// -EOPNOTSUPP
	{ 982,	"s0d4_imn_core1" },	// -EOPNOTSUPP
	{ 983,	"s0d1_ims_other" },	// -EOPNOTSUPP
	{ 984,	"s0d2_ims_other" },	// -EOPNOTSUPP
	{ 985,	"s0d4_ims_other" },	// -EOPNOTSUPP
	{ 986,	"s0d1_ims_core0" },	// -EOPNOTSUPP
	{ 987,	"s0d2_ims_core0" },	// -EOPNOTSUPP
	{ 988,	"s0d4_ims_core0" },	// -EOPNOTSUPP
	{ 989,	"s0d1_ims_core1" },	// -EOPNOTSUPP
	{ 990,	"s0d2_ims_core1" },	// -EOPNOTSUPP
	{ 991,	"s0d4_ims_core1" },	// -EOPNOTSUPP
	{ 992,	"zgd1_gpc_other" },	// -EOPNOTSUPP
	{ 993,	"zgd2_gpc_other" },	// -EOPNOTSUPP
	{ 994,	"zgd4_gpc_other" },	// -EOPNOTSUPP
	{ 995,	"zgd1_gpc_jones0" },	// -EOPNOTSUPP
	{ 996,	"zgd2_gpc_jones0" },	// -EOPNOTSUPP
	{ 997,	"zgd4_gpc_jones0" },	// -EOPNOTSUPP
	{ 998,	"zgd1_gpc_jones1" },	// -EOPNOTSUPP
	{ 999,	"zgd2_gpc_jones1" },	// -EOPNOTSUPP
	{ 1000,	"zgd4_gpc_jones1" },	// -EOPNOTSUPP
	{ 1001,	"zgd1_gpc_mer0" },	// -EOPNOTSUPP
	{ 1002,	"zgd2_gpc_mer0" },	// -EOPNOTSUPP
	{ 1003,	"zgd4_gpc_mer0" },	// -EOPNOTSUPP
	{ 1004,	"zgd1_gpc_mer1" },	// -EOPNOTSUPP
	{ 1005,	"zgd2_gpc_mer1" },	// -EOPNOTSUPP
	{ 1006,	"zgd4_gpc_mer1" },	// -EOPNOTSUPP
	{ 1007,	"zgd1_gpc_mer2" },	// -EOPNOTSUPP
	{ 1008,	"zgd2_gpc_mer2" },	// -EOPNOTSUPP
	{ 1009,	"zgd4_gpc_mer2" },	// -EOPNOTSUPP
	{ 1010,	"zgd1_gpc_mer3" },	// -EOPNOTSUPP
	{ 1011,	"zgd2_gpc_mer3" },	// -EOPNOTSUPP
	{ 1012,	"zgd4_gpc_mer3" },	// -EOPNOTSUPP
	{ 1013,	"zgd1_gpc_mer4" },	// -EOPNOTSUPP
	{ 1014,	"zgd2_gpc_mer4" },	// -EOPNOTSUPP
	{ 1015,	"zgd4_gpc_mer4" },	// -EOPNOTSUPP
	{ 1016,	"zgd1_gpc_mer5" },	// -EOPNOTSUPP
	{ 1017,	"zgd2_gpc_mer5" },	// -EOPNOTSUPP
	{ 1018,	"zgd4_gpc_mer5" },	// -EOPNOTSUPP
	{ 1019,	"zgd1_gpc_mer6" },	// -EOPNOTSUPP
	{ 1020,	"zgd2_gpc_mer6" },	// -EOPNOTSUPP
	{ 1021,	"zgd4_gpc_mer6" },	// -EOPNOTSUPP
	{ 1022,	"zgd1_gpc_mer7" },	// -EOPNOTSUPP
	{ 1023,	"zgd2_gpc_mer7" },	// -EOPNOTSUPP
	{ 1024,	"zgd4_gpc_mer7" },	// -EOPNOTSUPP
	{ 1025,	"zgd1_gpc_texas0" },	// -EOPNOTSUPP
	{ 1026,	"zgd2_gpc_texas0" },	// -EOPNOTSUPP
	{ 1027,	"zgd4_gpc_texas0" },	// -EOPNOTSUPP
	{ 1028,	"zgd1_gpc_texas1" },	// -EOPNOTSUPP
	{ 1029,	"zgd2_gpc_texas1" },	// -EOPNOTSUPP
	{ 1030,	"zgd4_gpc_texas1" },	// -EOPNOTSUPP
	{ 1031,	"zgd1_gpc_texas2" },	// -EOPNOTSUPP
	{ 1032,	"zgd2_gpc_texas2" },	// -EOPNOTSUPP
	{ 1033,	"zgd4_gpc_texas2" },	// -EOPNOTSUPP
	{ 1034,	"zgd1_gpc_texas3" },	// -EOPNOTSUPP
	{ 1035,	"zgd2_gpc_texas3" },	// -EOPNOTSUPP
	{ 1036,	"zgd4_gpc_texas3" },	// -EOPNOTSUPP
	{ 1037,	"zgd1_gpc_swift0" },	// -EOPNOTSUPP
	{ 1038,	"zgd2_gpc_swift0" },	// -EOPNOTSUPP
	{ 1039,	"zgd4_gpc_swift0" },	// -EOPNOTSUPP
	{ 1040,	"zgd1_gpc_swift1" },	// -EOPNOTSUPP
	{ 1041,	"zgd2_gpc_swift1" },	// -EOPNOTSUPP
	{ 1042,	"zgd4_gpc_swift1" },	// -EOPNOTSUPP
	{ 1043,	"zgd1_gpc_swift2" },	// -EOPNOTSUPP
	{ 1044,	"zgd2_gpc_swift2" },	// -EOPNOTSUPP
	{ 1045,	"zgd4_gpc_swift2" },	// -EOPNOTSUPP
	{ 1046,	"zgd1_gpc_swift3" },	// -EOPNOTSUPP
	{ 1047,	"zgd2_gpc_swift3" },	// -EOPNOTSUPP
	{ 1048,	"zgd4_gpc_swift3" },	// -EOPNOTSUPP
	{ 1049,	"zgd1_gpc_swift4" },	// -EOPNOTSUPP
	{ 1050,	"zgd2_gpc_swift4" },	// -EOPNOTSUPP
	{ 1051,	"zgd4_gpc_swift4" },	// -EOPNOTSUPP
	{ 1052,	"zgd1_gpc_swift5" },	// -EOPNOTSUPP
	{ 1053,	"zgd2_gpc_swift5" },	// -EOPNOTSUPP
	{ 1054,	"zgd4_gpc_swift5" },	// -EOPNOTSUPP
	{ 1055,	"zgd1_gpc_swift6" },	// -EOPNOTSUPP
	{ 1056,	"zgd2_gpc_swift6" },	// -EOPNOTSUPP
	{ 1057,	"zgd4_gpc_swift6" },	// -EOPNOTSUPP
	{ 1058,	"zgd1_gpc_swift7" },	// -EOPNOTSUPP
	{ 1059,	"zgd2_gpc_swift7" },	// -EOPNOTSUPP
	{ 1060,	"zgd4_gpc_swift7" },	// -EOPNOTSUPP
	{ 1061,	"sgd1_dsp_other" },	// -EOPNOTSUPP
	{ 1062,	"sgd2_dsp_other" },	// -EOPNOTSUPP
	{ 1063,	"sgd4_dsp_other" },	// -EOPNOTSUPP
	{ 1065,	"sgd1_dsp_bus0" },	// -EOPNOTSUPP
	{ 1066,	"sgd2_dsp_bus0" },	// -EOPNOTSUPP
	{ 1067,	"sgd4_dsp_bus0" },	// -EOPNOTSUPP
	{ 1069,	"sgd1_dsp_bus1" },	// -EOPNOTSUPP
	{ 1070,	"sgd2_dsp_bus1" },	// -EOPNOTSUPP
	{ 1071,	"sgd4_dsp_bus1" },	// -EOPNOTSUPP
	{ 1073,	"sgd1_dsp2_vpx0" },	// -EOPNOTSUPP
	{ 1074,	"sgd2_dsp2_vpx0" },	// -EOPNOTSUPP
	{ 1075,	"sgd4_dsp2_vpx0" },	// -EOPNOTSUPP
	{ 1077,	"wdt_dsp2_vpx0" },	// -EOPNOTSUPP
	{ 1078,	"sgd1_dsp2_vpx1" },	// -EOPNOTSUPP
	{ 1079,	"sgd2_dsp2_vpx1" },	// -EOPNOTSUPP
	{ 1080,	"sgd4_dsp2_vpx1" },	// -EOPNOTSUPP
	{ 1082,	"wdt_dsp2_vpx1" },	// -EOPNOTSUPP
	{ 1083,	"sgd1_dsp2_vpx2" },	// -EOPNOTSUPP
	{ 1084,	"sgd2_dsp2_vpx2" },	// -EOPNOTSUPP
	{ 1085,	"sgd4_dsp2_vpx2" },	// -EOPNOTSUPP
	{ 1087,	"wdt_dsp2_vpx2" },	// -EOPNOTSUPP
	{ 1088,	"sgd1_dsp2_vpx3" },	// -EOPNOTSUPP
	{ 1089,	"sgd2_dsp2_vpx3" },	// -EOPNOTSUPP
	{ 1090,	"sgd4_dsp2_vpx3" },	// -EOPNOTSUPP
	{ 1092,	"wdt_dsp2_vpx3" },	// -EOPNOTSUPP
	{ 1093,	"sgd1_dsp3_vpx0" },	// -EOPNOTSUPP
	{ 1094,	"sgd2_dsp3_vpx0" },	// -EOPNOTSUPP
	{ 1095,	"sgd4_dsp3_vpx0" },	// -EOPNOTSUPP
	{ 1097,	"wdt_dsp3_vpx0" },	// -EOPNOTSUPP
	{ 1098,	"sgd1_dsp3_vpx1" },	// -EOPNOTSUPP
	{ 1099,	"sgd2_dsp3_vpx1" },	// -EOPNOTSUPP
	{ 1100,	"sgd4_dsp3_vpx1" },	// -EOPNOTSUPP
	{ 1102,	"wdt_dsp3_vpx1" },	// -EOPNOTSUPP
	{ 1103,	"sgd1_dsp3_vpx2" },	// -EOPNOTSUPP
	{ 1104,	"sgd2_dsp3_vpx2" },	// -EOPNOTSUPP
	{ 1105,	"sgd4_dsp3_vpx2" },	// -EOPNOTSUPP
	{ 1107,	"wdt_dsp3_vpx2" },	// -EOPNOTSUPP
	{ 1108,	"sgd1_dsp3_vpx3" },	// -EOPNOTSUPP
	{ 1109,	"sgd2_dsp3_vpx3" },	// -EOPNOTSUPP
	{ 1110,	"sgd4_dsp3_vpx3" },	// -EOPNOTSUPP
	{ 1112,	"wdt_dsp3_vpx3" },	// -EOPNOTSUPP
	{ 1113,	"sgd1_dsp4_vpx0" },	// -EOPNOTSUPP
	{ 1114,	"sgd2_dsp4_vpx0" },	// -EOPNOTSUPP
	{ 1115,	"sgd4_dsp4_vpx0" },	// -EOPNOTSUPP
	{ 1117,	"wdt_dsp4_vpx0" },	// -EOPNOTSUPP
	{ 1118,	"sgd1_dsp4_vpx1" },	// -EOPNOTSUPP
	{ 1119,	"sgd2_dsp4_vpx1" },	// -EOPNOTSUPP
	{ 1120,	"sgd4_dsp4_vpx1" },	// -EOPNOTSUPP
	{ 1122,	"wdt_dsp4_vpx1" },	// -EOPNOTSUPP
	{ 1123,	"sgd1_dsp4_vpx2" },	// -EOPNOTSUPP
	{ 1124,	"sgd2_dsp4_vpx2" },	// -EOPNOTSUPP
	{ 1125,	"sgd4_dsp4_vpx2" },	// -EOPNOTSUPP
	{ 1127,	"wdt_dsp4_vpx2" },	// -EOPNOTSUPP
	{ 1128,	"sgd1_dsp4_vpx3" },	// -EOPNOTSUPP
	{ 1129,	"sgd2_dsp4_vpx3" },	// -EOPNOTSUPP
	{ 1130,	"sgd4_dsp4_vpx3" },	// -EOPNOTSUPP
	{ 1132,	"wdt_dsp4_vpx3" },	// -EOPNOTSUPP
	{ 1133,	"sgd1_dsp5_vpx0" },	// -EOPNOTSUPP
	{ 1134,	"sgd2_dsp5_vpx0" },	// -EOPNOTSUPP
	{ 1135,	"sgd4_dsp5_vpx0" },	// -EOPNOTSUPP
	{ 1137,	"wdt_dsp5_vpx0" },	// -EOPNOTSUPP
	{ 1138,	"sgd1_dsp5_vpx1" },	// -EOPNOTSUPP
	{ 1139,	"sgd2_dsp5_vpx1" },	// -EOPNOTSUPP
	{ 1140,	"sgd4_dsp5_vpx1" },	// -EOPNOTSUPP
	{ 1142,	"wdt_dsp5_vpx1" },	// -EOPNOTSUPP
	{ 1143,	"sgd1_dsp5_vpx2" },	// -EOPNOTSUPP
	{ 1144,	"sgd2_dsp5_vpx2" },	// -EOPNOTSUPP
	{ 1145,	"sgd4_dsp5_vpx2" },	// -EOPNOTSUPP
	{ 1147,	"wdt_dsp5_vpx2" },	// -EOPNOTSUPP
	{ 1148,	"sgd1_dsp5_vpx3" },	// -EOPNOTSUPP
	{ 1149,	"sgd2_dsp5_vpx3" },	// -EOPNOTSUPP
	{ 1150,	"sgd4_dsp5_vpx3" },	// -EOPNOTSUPP
	{ 1152,	"wdt_dsp5_vpx3" },	// -EOPNOTSUPP
	{ 1153,	"sgd1_dsp6_vpx0" },	// -EOPNOTSUPP
	{ 1154,	"sgd2_dsp6_vpx0" },	// -EOPNOTSUPP
	{ 1155,	"sgd4_dsp6_vpx0" },	// -EOPNOTSUPP
	{ 1157,	"wdt_dsp6_vpx0" },	// -EOPNOTSUPP
	{ 1158,	"sgd1_dsp6_vpx1" },	// -EOPNOTSUPP
	{ 1159,	"sgd2_dsp6_vpx1" },	// -EOPNOTSUPP
	{ 1160,	"sgd4_dsp6_vpx1" },	// -EOPNOTSUPP
	{ 1162,	"wdt_dsp6_vpx1" },	// -EOPNOTSUPP
	{ 1163,	"sgd1_dsp6_vpx2" },	// -EOPNOTSUPP
	{ 1164,	"sgd2_dsp6_vpx2" },	// -EOPNOTSUPP
	{ 1165,	"sgd4_dsp6_vpx2" },	// -EOPNOTSUPP
	{ 1167,	"wdt_dsp6_vpx2" },	// -EOPNOTSUPP
	{ 1168,	"sgd1_dsp6_vpx3" },	// -EOPNOTSUPP
	{ 1169,	"sgd2_dsp6_vpx3" },	// -EOPNOTSUPP
	{ 1170,	"sgd4_dsp6_vpx3" },	// -EOPNOTSUPP
	{ 1172,	"wdt_dsp6_vpx3" },	// -EOPNOTSUPP
	{ 1173,	"sgd1_dsp2_vpxl2" },	// -EOPNOTSUPP
	{ 1174,	"sgd2_dsp2_vpxl2" },	// -EOPNOTSUPP
	{ 1175,	"sgd4_dsp2_vpxl2" },	// -EOPNOTSUPP
	{ 1177,	"wdt_dsp_vpxl2" },	// -EOPNOTSUPP
	{ 1178,	"sgd1_dsp3_vpxl2" },	// -EOPNOTSUPP
	{ 1179,	"sgd2_dsp3_vpxl2" },	// -EOPNOTSUPP
	{ 1180,	"sgd4_dsp3_vpxl2" },	// -EOPNOTSUPP
	{ 1182,	"wdt_dsp3_vpxl2" },	// -EOPNOTSUPP
	{ 1183,	"sgd1_dsp4_vpxl2" },	// -EOPNOTSUPP
	{ 1184,	"sgd2_dsp4_vpxl2" },	// -EOPNOTSUPP
	{ 1185,	"sgd4_dsp4_vpxl2" },	// -EOPNOTSUPP
	{ 1187,	"wdt_dsp4_vpxl2" },	// -EOPNOTSUPP
	{ 1188,	"sgd1_dsp5_vpxl2" },	// -EOPNOTSUPP
	{ 1189,	"sgd2_dsp5_vpxl2" },	// -EOPNOTSUPP
	{ 1190,	"sgd4_dsp5_vpxl2" },	// -EOPNOTSUPP
	{ 1192,	"wdt_dsp5_vpxl2" },	// -EOPNOTSUPP
	{ 1193,	"sgd1_dsp6_vpxl2" },	// -EOPNOTSUPP
	{ 1194,	"sgd2_dsp6_vpxl2" },	// -EOPNOTSUPP
	{ 1195,	"sgd4_dsp6_vpxl2" },	// -EOPNOTSUPP
	{ 1197,	"wdt_dsp6_vpxl2" },	// -EOPNOTSUPP
	{ 1198,	"sgd1_npu0_other" },	// -EOPNOTSUPP
	{ 1199,	"sgd2_npu0_other" },	// -EOPNOTSUPP
	{ 1200,	"sgd4_npu0_other" },	// -EOPNOTSUPP
	{ 1202,	"sgd1_npu0_npul2" },	// -EOPNOTSUPP
	{ 1203,	"sgd2_npu0_npul2" },	// -EOPNOTSUPP
	{ 1204,	"sgd4_npu0_npul2" },	// -EOPNOTSUPP
	{ 1206,	"wdt_npu0_npul2" },	// -EOPNOTSUPP
	{ 1207,	"sgd1_npu0_vpxl2" },	// -EOPNOTSUPP
	{ 1208,	"sgd2_npu0_vpxl2" },	// -EOPNOTSUPP
	{ 1209,	"sgd4_npu0_vpxl2" },	// -EOPNOTSUPP
	{ 1211,	"wdt_npu0_vpx_l2" },	// -EOPNOTSUPP
	{ 1212,	"sgd1_n0_grp0_c0" },	// -EOPNOTSUPP
	{ 1213,	"sgd2_n0_grp0_c0" },	// -EOPNOTSUPP
	{ 1214,	"sgd4_n0_grp0_c0" },	// -EOPNOTSUPP
	{ 1216,	"wdt_n0_grp0_c0" },	// -EOPNOTSUPP
	{ 1217,	"sgd1_n0_grp0_c1" },	// -EOPNOTSUPP
	{ 1218,	"sgd2_n0_grp0_c1" },	// -EOPNOTSUPP
	{ 1219,	"sgd4_n0_grp0_c1" },	// -EOPNOTSUPP
	{ 1221,	"wdt_n0_grp0_c1" },	// -EOPNOTSUPP
	{ 1222,	"sgd1_n0_grp0_c2" },	// -EOPNOTSUPP
	{ 1223,	"sgd2_n0_grp0_c2" },	// -EOPNOTSUPP
	{ 1224,	"sgd4_n0_grp0_c2" },	// -EOPNOTSUPP
	{ 1226,	"wdt_n0_c02" },		// -EOPNOTSUPP
	{ 1227,	"sgd1_n0_grp1_c0" },	// -EOPNOTSUPP
	{ 1228,	"sgd2_n0_grp1_c0" },	// -EOPNOTSUPP
	{ 1229,	"sgd4_n0_grp1_c0" },	// -EOPNOTSUPP
	{ 1231,	"wdt_n0_grp1_c0" },	// -EOPNOTSUPP
	{ 1232,	"sgd1_n0_grp1_c1" },	// -EOPNOTSUPP
	{ 1233,	"sgd2_n0_grp1_c1" },	// -EOPNOTSUPP
	{ 1234,	"sgd4_n0_grp1_c1" },	// -EOPNOTSUPP
	{ 1236,	"wdt_n0_grp1_c1" },	// -EOPNOTSUPP
	{ 1237,	"sgd1_n0_grp1_c2" },	// -EOPNOTSUPP
	{ 1238,	"sgd2_n0_grp1_c2" },	// -EOPNOTSUPP
	{ 1239,	"sgd4_n0_grp1_c2" },	// -EOPNOTSUPP
	{ 1241,	"wdt_n0_grp1_c2" },	// -EOPNOTSUPP
	{ 1242,	"sgd1_n0_grp2_c0" },	// -EOPNOTSUPP
	{ 1243,	"sgd2_n0_grp2_c0" },	// -EOPNOTSUPP
	{ 1244,	"sgd4_n0_grp2_c0" },	// -EOPNOTSUPP
	{ 1246,	"wdt_n0_grp2_c0" },	// -EOPNOTSUPP
	{ 1247,	"sgd1_n0_grp2_c1" },	// -EOPNOTSUPP
	{ 1248,	"sgd2_n0_grp2_c1" },	// -EOPNOTSUPP
	{ 1249,	"sgd4_n0_grp2_c1" },	// -EOPNOTSUPP
	{ 1251,	"wdt_n0_grp2_c1" },	// -EOPNOTSUPP
	{ 1252,	"sgd1_n0_grp2_c2" },	// -EOPNOTSUPP
	{ 1253,	"sgd2_n0_grp2_c2" },	// -EOPNOTSUPP
	{ 1254,	"sgd4_n0_grp2_c2" },	// -EOPNOTSUPP
	{ 1256,	"wdt_n0_grp2_c2" },	// -EOPNOTSUPP
	{ 1257,	"sgd1_n0_grp3_c0" },	// -EOPNOTSUPP
	{ 1258,	"sgd2_n0_grp3_c0" },	// -EOPNOTSUPP
	{ 1259,	"sgd4_n0_grp3_c0" },	// -EOPNOTSUPP
	{ 1261,	"wdt_n0_grp3_c0" },	// -EOPNOTSUPP
	{ 1262,	"sgd1_n0_grp3_c1" },	// -EOPNOTSUPP
	{ 1263,	"sgd2_n0_grp3_c1" },	// -EOPNOTSUPP
	{ 1264,	"sgd4_n0_grp3_c1" },	// -EOPNOTSUPP
	{ 1266,	"wdt_n0_grp3_c1" },	// -EOPNOTSUPP
	{ 1267,	"sgd1_n0_grp3_c2" },	// -EOPNOTSUPP
	{ 1268,	"sgd2_n0_grp3_c2" },	// -EOPNOTSUPP
	{ 1269,	"sgd4_n0_grp3_c2" },	// -EOPNOTSUPP
	{ 1271,	"wdt_n0_grp3_c2" },	// -EOPNOTSUPP
	{ 1272,	"sgd1_n0_vpx_c0" },	// -EOPNOTSUPP
	{ 1273,	"sgd2_n0_vpx_c0" },	// -EOPNOTSUPP
	{ 1274,	"sgd4_n0_vpx_c0" },	// -EOPNOTSUPP
	{ 1276,	"wdt_n0_vpx_c0" },	// -EOPNOTSUPP
	{ 1277,	"sgd1_n0_vpx_c1" },	// -EOPNOTSUPP
	{ 1278,	"sgd2_n0_vpx_c1" },	// -EOPNOTSUPP
	{ 1279,	"sgd4_n0_vpx_c1" },	// -EOPNOTSUPP
	{ 1281,	"wdt_n0_vpx_c1" },	// -EOPNOTSUPP
	{ 1282,	"sgd1_n0_vpx_c2" },	// -EOPNOTSUPP
	{ 1283,	"sgd2_n0_vpx_c2" },	// -EOPNOTSUPP
	{ 1284,	"sgd4_n0_vpx_c2" },	// -EOPNOTSUPP
	{ 1286,	"wdt_n0_vpx_c2" },	// -EOPNOTSUPP
	{ 1287,	"sgd1_n0_vpx_c3" },	// -EOPNOTSUPP
	{ 1288,	"sgd2_n0_vpx_c3" },	// -EOPNOTSUPP
	{ 1289,	"sgd4_n0_vpx_c3" },	// -EOPNOTSUPP
	{ 1291,	"wdt_n0_vpx_c3" },	// -EOPNOTSUPP
	{ 1292,	"sgd1_npu1_other" },	// -EOPNOTSUPP
	{ 1293,	"sgd2_npu1_other" },	// -EOPNOTSUPP
	{ 1294,	"sgd4_npu1_other" },	// -EOPNOTSUPP
	{ 1296,	"sgd1_npu1_npul2" },	// -EOPNOTSUPP
	{ 1297,	"sgd2_npu1_npul2" },	// -EOPNOTSUPP
	{ 1298,	"sgd4_npu1_npul2" },	// -EOPNOTSUPP
	{ 1300,	"wdt_npu1_npul2" },	// -EOPNOTSUPP
	{ 1301,	"sgd1_npu1_vpxl2" },	// -EOPNOTSUPP
	{ 1302,	"sgd2_npu1_vpxl2" },	// -EOPNOTSUPP
	{ 1303,	"sgd4_npu1_vpxl2" },	// -EOPNOTSUPP
	{ 1305,	"wdt_n1_vpx_l2" },	// -EOPNOTSUPP
	{ 1306,	"sgd1_n1_grp0_c0" },	// -EOPNOTSUPP
	{ 1307,	"sgd2_n1_grp0_c0" },	// -EOPNOTSUPP
	{ 1308,	"sgd4_n1_grp0_c0" },	// -EOPNOTSUPP
	{ 1310,	"wdt_n1_grp0_c0" },	// -EOPNOTSUPP
	{ 1311,	"sgd1_n1_grp0_c1" },	// -EOPNOTSUPP
	{ 1312,	"sgd2_n1_grp0_c1" },	// -EOPNOTSUPP
	{ 1313,	"sgd4_n1_grp0_c1" },	// -EOPNOTSUPP
	{ 1315,	"wdt_n1_grp0_c1" },	// -EOPNOTSUPP
	{ 1316,	"sgd1_n1_grp0_c2" },	// -EOPNOTSUPP
	{ 1317,	"sgd2_n1_grp0_c2" },	// -EOPNOTSUPP
	{ 1318,	"sgd4_n1_grp0_c2" },	// -EOPNOTSUPP
	{ 1320,	"wdt_n1_c02" },		// -EOPNOTSUPP
	{ 1321,	"sgd1_n1_grp1_c0" },	// -EOPNOTSUPP
	{ 1322,	"sgd2_n1_grp1_c0" },	// -EOPNOTSUPP
	{ 1323,	"sgd4_n1_grp1_c0" },	// -EOPNOTSUPP
	{ 1325,	"wdt_n1_grp1_c0" },	// -EOPNOTSUPP
	{ 1326,	"sgd1_n1_grp1_c1" },	// -EOPNOTSUPP
	{ 1327,	"sgd2_n1_grp1_c1" },	// -EOPNOTSUPP
	{ 1328,	"sgd4_n1_grp1_c1" },	// -EOPNOTSUPP
	{ 1330,	"wdt_n1_grp1_c1" },	// -EOPNOTSUPP
	{ 1331,	"sgd1_n1_grp1_c2" },	// -EOPNOTSUPP
	{ 1332,	"sgd2_n1_grp1_c2" },	// -EOPNOTSUPP
	{ 1333,	"sgd4_n1_grp1_c2" },	// -EOPNOTSUPP
	{ 1335,	"wdt_n1_grp1_c2" },	// -EOPNOTSUPP
	{ 1336,	"sgd1_n1_grp2_c0" },	// -EOPNOTSUPP
	{ 1337,	"sgd2_n1_grp2_c0" },	// -EOPNOTSUPP
	{ 1338,	"sgd4_n1_grp2_c0" },	// -EOPNOTSUPP
	{ 1340,	"wdt_n1_grp2_c0" },	// -EOPNOTSUPP
	{ 1341,	"sgd1_n1_grp2_c1" },	// -EOPNOTSUPP
	{ 1342,	"sgd2_n1_grp2_c1" },	// -EOPNOTSUPP
	{ 1343,	"sgd4_n1_grp2_c1" },	// -EOPNOTSUPP
	{ 1345,	"wdt_n1_grp2_c1" },	// -EOPNOTSUPP
	{ 1346,	"sgd1_n1_grp2_c2" },	// -EOPNOTSUPP
	{ 1347,	"sgd2_n1_grp2_c2" },	// -EOPNOTSUPP
	{ 1348,	"sgd4_n1_grp2_c2" },	// -EOPNOTSUPP
	{ 1350,	"wdt_n1_grp2_c2" },	// -EOPNOTSUPP
	{ 1351,	"sgd1_n1_grp3_c0" },	// -EOPNOTSUPP
	{ 1352,	"sgd2_n1_grp3_c0" },	// -EOPNOTSUPP
	{ 1353,	"sgd4_n1_grp3_c0" },	// -EOPNOTSUPP
	{ 1355,	"wdt_n1_grp3_c0" },	// -EOPNOTSUPP
	{ 1356,	"sgd1_n1_grp3_c1" },	// -EOPNOTSUPP
	{ 1357,	"sgd2_n1_grp3_c1" },	// -EOPNOTSUPP
	{ 1358,	"sgd4_n1_grp3_c1" },	// -EOPNOTSUPP
	{ 1360,	"wdt_n1_grp3_c1" },	// -EOPNOTSUPP
	{ 1361,	"sgd1_n1_grp3_c2" },	// -EOPNOTSUPP
	{ 1362,	"sgd2_n1_grp3_c2" },	// -EOPNOTSUPP
	{ 1363,	"sgd4_n1_grp3_c2" },	// -EOPNOTSUPP
	{ 1365,	"wdt_n1_grp3_c2" },	// -EOPNOTSUPP
	{ 1366,	"sgd1_n1_vpx_c0" },	// -EOPNOTSUPP
	{ 1367,	"sgd2_n1_vpx_c0" },	// -EOPNOTSUPP
	{ 1368,	"sgd4_n1_vpx_c0" },	// -EOPNOTSUPP
	{ 1370,	"wdt_n1_vpx_c0" },	// -EOPNOTSUPP
	{ 1371,	"sgd1_n1_vpx_c1" },	// -EOPNOTSUPP
	{ 1372,	"sgd2_n1_vpx_c1" },	// -EOPNOTSUPP
	{ 1373,	"sgd4_n1_vpx_c1" },	// -EOPNOTSUPP
	{ 1375,	"wdt_n1_vpx_c1" },	// -EOPNOTSUPP
	{ 1376,	"sgd1_n1_vpx_c2" },	// -EOPNOTSUPP
	{ 1377,	"sgd2_n1_vpx_c2" },	// -EOPNOTSUPP
	{ 1378,	"sgd4_n1_vpx_c2" },	// -EOPNOTSUPP
	{ 1380,	"wdt_n1_vpx_c2" },	// -EOPNOTSUPP
	{ 1381,	"sgd1_n1_vpx_c3" },	// -EOPNOTSUPP
	{ 1382,	"sgd2_n1_vpx_c3" },	// -EOPNOTSUPP
	{ 1383,	"sgd4_n1_vpx_c3" },	// -EOPNOTSUPP
	{ 1385,	"wdt_n1_vpx_c3" },	// -EOPNOTSUPP
	{ 1386,	"zrd6_rt_main" },	// -EOPNOTSUPP
	{ 1387,	"zrd12_rt_main" },	// -EOPNOTSUPP
	{ 1388,	"zrd24_rt_main" },	// -EOPNOTSUPP
	{ 1389,	"zrd48_rt_main" },	// -EOPNOTSUPP
	{ 1390,	"zrd96_rt_main" },	// -EOPNOTSUPP
	{ 1393,	"cl16m_rt_main" },	// -EOPNOTSUPP
	{ 1394,	"sad1_rt_main" },	// -EOPNOTSUPP
	{ 1395,	"sad2_rt_main" },	// -EOPNOTSUPP
	{ 1396,	"sad4_rt_main" },	// -EOPNOTSUPP
	{ 1397,	"zrd6_rt_dmac" },	// -EOPNOTSUPP
	{ 1398,	"zrd12_rt_dmac" },	// -EOPNOTSUPP
	{ 1399,	"zrd24_rt_dmac" },	// -EOPNOTSUPP
	{ 1400,	"zrd48_rt_dmac" },	// -EOPNOTSUPP
	{ 1401,	"zrd96_rt_dmac" },	// -EOPNOTSUPP
	{ 1404,	"cl16m_rt_dmac" },	// -EOPNOTSUPP
	{ 1405,	"zrd6_rt_cr52ss0" },	// -EOPNOTSUPP
	{ 1406,	"zrd12_rt_ss0" },	// -EOPNOTSUPP
	{ 1407,	"zrd24_rt_ss0" },	// -EOPNOTSUPP
	{ 1408,	"zrd48_rt_ss0" },	// -EOPNOTSUPP
	{ 1409,	"zrd96_rt_ss0" },	// -EOPNOTSUPP
	{ 1412,	"zr_rt_cr52ss0" },	// -EOPNOTSUPP
	{ 1413,	"zrd6_rt_cr52ss1" },	// -EOPNOTSUPP
	{ 1414,	"zrd12_rt_ss1" },	// -EOPNOTSUPP
	{ 1415,	"zrd24_rt_ss1" },	// -EOPNOTSUPP
	{ 1416,	"zrd48_rt_ss1" },	// -EOPNOTSUPP
	{ 1417,	"zrd96_rt_ss1" },	// -EOPNOTSUPP
	{ 1420,	"zr_rt_cr52ss1" },	// -EOPNOTSUPP
	{ 1421,	"zrd6_rt_cr52ss2" },	// -EOPNOTSUPP
	{ 1422,	"zrd12_rt_ss2" },	// -EOPNOTSUPP
	{ 1423,	"zrd24_rt_ss2" },	// -EOPNOTSUPP
	{ 1424,	"zrd48_rt_ss2" },	// -EOPNOTSUPP
	{ 1425,	"zrd96_rt_ss2" },	// -EOPNOTSUPP
	{ 1428,	"zr_rt_cr52ss2" },	// -EOPNOTSUPP
	{ 1429,	"zr_rt_c00" },		// -EOPNOTSUPP
	{ 1430,	"zr_rt_c01" },		// -EOPNOTSUPP
	{ 1431,	"zr_rt_c10" },		// -EOPNOTSUPP
	{ 1432,	"zr_rt_c11" },		// -EOPNOTSUPP
	{ 1433,	"zr_rt_c20" },		// -EOPNOTSUPP
	{ 1434,	"zr_rt_c21" },		// -EOPNOTSUPP
	{ 1435,	"zr_rt_shadow00" },	// -EOPNOTSUPP
	{ 1436,	"zr_rt_shadow01" },	// -EOPNOTSUPP
	{ 1437,	"zr_rt_shadow10" },	// -EOPNOTSUPP
	{ 1438,	"zr_rt_shadow11" },	// -EOPNOTSUPP
	{ 1439,	"zr_rt_shadow20" },	// -EOPNOTSUPP
	{ 1440,	"zr_rt_shadow21" },	// -EOPNOTSUPP
	{ 1441,	"zrd6_rt_sec" },	// -EOPNOTSUPP
	{ 1442,	"zrd12_rt_sec" },	// -EOPNOTSUPP
	{ 1443,	"zrd24_rt_sec" },	// -EOPNOTSUPP
	{ 1444,	"zrd48_rt_sec" },	// -EOPNOTSUPP
	{ 1445,	"zrd96_rt_sec" },	// -EOPNOTSUPP
	{ 1446,	"zrd6_rt_mem" },	// -EOPNOTSUPP
	{ 1447,	"zrd12_rt_mem" },	// -EOPNOTSUPP
	{ 1448,	"zrd24_rt_mem" },	// -EOPNOTSUPP
	{ 1449,	"zrd48_rt_mem" },	// -EOPNOTSUPP
	{ 1450,	"zrd96_rt_mem" },	// -EOPNOTSUPP
	{ 1453,	"busd1_scp_main" },	// -EOPNOTSUPP
	{ 1454,	"busd2_scp_main" },	// -EOPNOTSUPP
	{ 1455,	"busd4_scp_main" },	// -EOPNOTSUPP
	{ 1456,	"busd6_scp_main" },	// -EOPNOTSUPP
	{ 1457,	"busd8_scp_main" },	// -EOPNOTSUPP
	{ 1458,	"busd16_scp_main" },	// -EOPNOTSUPP
	{ 1459,	"busd32_scp_main" },	// -EOPNOTSUPP
	{ 1461,	"fray_scp_main" },	// -EOPNOTSUPP
	{ 1464,	"canxl_scp_main" },	// -EOPNOTSUPP
	{ 1465,	"cl16m_scp_main" },	// -EOPNOTSUPP
	{ 1467,	"sgd1_hscs_other" },	// -EOPNOTSUPP
	{ 1468,	"sgd2_hscs_other" },	// -EOPNOTSUPP
	{ 1469,	"sgd4_hscs_other" },	// -EOPNOTSUPP
	{ 1470,	"sgd8_hscs_other" },	// -EOPNOTSUPP
	{ 1471,	"sgd16_hscs_oth" },	// -EOPNOTSUPP
	{ 1472,	"pcick_hscs_oth" },	// -EOPNOTSUPP
	{ 1473,	"sgd1_hscs_pci" },	// -EOPNOTSUPP
	{ 1474,	"sgd2_hscs_pci" },	// -EOPNOTSUPP
	{ 1475,	"sgd4_hscs_pci" },	// -EOPNOTSUPP
	{ 1476,	"sgd8_hscs_pci" },	// -EOPNOTSUPP
	{ 1477,	"sgd16_hscs_pci" },	// -EOPNOTSUPP
	{ 1478,	"pcick_hscs_pc" },	// -EOPNOTSUPP
	{ 1479,	"sgd1_hscs_uci0" },	// -EOPNOTSUPP
	{ 1480,	"sgd12_hscs_uci0" },	// -EOPNOTSUPP
	{ 1481,	"sgd24_hscs_uci0" },	// -EOPNOTSUPP
	{ 1482,	"sgd48_hscs_uci0" },	// -EOPNOTSUPP
	{ 1483,	"sgd96_hscs_uci0" },	// -EOPNOTSUPP
	{ 1484,	"sb_hscs_uci0" },	// -EOPNOTSUPP
	{ 1487,	"ref_hscs_uci0" },	// -EOPNOTSUPP
	{ 1488,	"sgd1_hscs_uci1" },	// -EOPNOTSUPP
	{ 1489,	"sgd12_hscs_uci1" },	// -EOPNOTSUPP
	{ 1490,	"sgd24_hscs_uci1" },	// -EOPNOTSUPP
	{ 1491,	"sgd48_hscs_uci1" },	// -EOPNOTSUPP
	{ 1492,	"sgd96_hscs_uci1" },	// -EOPNOTSUPP
	{ 1493,	"sb_hscs_uci1" },	// -EOPNOTSUPP
	{ 1496,	"ref_hscs_uci1" },	// -EOPNOTSUPP
	{ 1497,	"s0d1_hscn_other" },	// -EOPNOTSUPP
	{ 1498,	"s0d2_hscn_other" },	// -EOPNOTSUPP
	{ 1499,	"s0d4_hscn_other" },	// -EOPNOTSUPP
	{ 1500,	"s0d8_hscn_other" },	// -EOPNOTSUPP
	{ 1501,	"s0d12_hscn_oth" },	// -EOPNOTSUPP
	{ 1502,	"s0d16_hscn_oth" },	// -EOPNOTSUPP
	{ 1503,	"s0d24_hscn_oth" },	// -EOPNOTSUPP
	{ 1506,	"cl16m_hscn_oth" },	// -EOPNOTSUPP
	{ 1507,	"s0d1_hscn_pci4" },	// -EOPNOTSUPP
	{ 1508,	"s0d2_hscn_pci4" },	// -EOPNOTSUPP
	{ 1509,	"s0d4_hscn_pci4" },	// -EOPNOTSUPP
	{ 1510,	"s0d8_hscn_pci4" },	// -EOPNOTSUPP
	{ 1511,	"s0d12_hscn_pci4" },	// -EOPNOTSUPP
	{ 1512,	"s0d16_hscn_pci4" },	// -EOPNOTSUPP
	{ 1513,	"s0d24_hscn_pci4" },	// -EOPNOTSUPP
	{ 1516,	"cl16m_hscn_pci4" },	// -EOPNOTSUPP
	{ 1517,	"s0d1_hscn_usb" },	// -EOPNOTSUPP
	{ 1518,	"s0d2_hscn_usb" },	// -EOPNOTSUPP
	{ 1519,	"s0d4_hscn_usb" },	// -EOPNOTSUPP
	{ 1520,	"s0d8_hscn_usb" },	// -EOPNOTSUPP
	{ 1521,	"s0d12_hscn_usb" },	// -EOPNOTSUPP
	{ 1522,	"s0d16_hscn_usb" },	// -EOPNOTSUPP
	{ 1523,	"s0d24_hscn_usb" },	// -EOPNOTSUPP
	{ 1526,	"cl16m_hscn_usb" },	// -EOPNOTSUPP
	{ 1527,	"s0d1_rsw3_mfwd" },	// -EOPNOTSUPP
	{ 1528,	"s0d2_rsw3_mfwd" },	// -EOPNOTSUPP
	{ 1529,	"s0d4_rsw3_mfwd" },	// -EOPNOTSUPP
	{ 1530,	"s0d8_rsw3_mfwd" },	// -EOPNOTSUPP
	{ 1531,	"s0d12_rsw3_mfwd" },	// -EOPNOTSUPP
	{ 1532,	"s0d16_rsw3_mfwd" },	// -EOPNOTSUPP
	{ 1533,	"s0d24_rsw3_mfwd" },	// -EOPNOTSUPP
	{ 1537,	"s0d1_rsw3_main" },	// -EOPNOTSUPP
	{ 1538,	"s0d2_rsw3_main" },	// -EOPNOTSUPP
	{ 1539,	"s0d4_rsw3_main" },	// -EOPNOTSUPP
	{ 1540,	"s0d8_rsw3_main" },	// -EOPNOTSUPP
	{ 1541,	"s0d12_rsw3_main" },	// -EOPNOTSUPP
	{ 1542,	"s0d16_rsw3_main" },	// -EOPNOTSUPP
	{ 1543,	"s0d24_rsw3_main" },	// -EOPNOTSUPP
	{ 1547,	"s0d1_rsw3_aes" },	// -EOPNOTSUPP
	{ 1548,	"s0d2_rsw3_aes" },	// -EOPNOTSUPP
	{ 1549,	"s0d4_rsw3_aes" },	// -EOPNOTSUPP
	{ 1550,	"s0d8_rsw3_aes" },	// -EOPNOTSUPP
	{ 1551,	"s0d12_rsw3_aes" },	// -EOPNOTSUPP
	{ 1552,	"s0d16_rsw3_aes" },	// -EOPNOTSUPP
	{ 1553,	"s0d24_rsw3_aes" },	// -EOPNOTSUPP
	{ 1557,	"s0d1_rsw3_tsn" },	// -EOPNOTSUPP
	{ 1558,	"s0d2_rsw3_tsn" },	// -EOPNOTSUPP
	{ 1559,	"s0d4_rsw3_tsn" },	// -EOPNOTSUPP
	{ 1560,	"s0d8_rsw3_tsn" },	// -EOPNOTSUPP
	{ 1561,	"s0d12_rsw3_tsn" },	// -EOPNOTSUPP
	{ 1562,	"s0d16_rsw3_tsn" },	// -EOPNOTSUPP
	{ 1563,	"s0d24_rsw3_tsn" },	// -EOPNOTSUPP
	{ 1567,	"s0d1_mm_bus" },	// -EOPNOTSUPP
	{ 1568,	"s0d2_mm_bus" },	// -EOPNOTSUPP
	{ 1569,	"s0d4_mm_bus" },	// -EOPNOTSUPP
	{ 1570,	"s0d1_mm_iniu" },	// -EOPNOTSUPP
	{ 1571,	"s0d2_mm_iniu" },	// -EOPNOTSUPP
	{ 1572,	"s0d4_mm_iniu" },	// -EOPNOTSUPP
	{ 1573,	"s0d1_mm_axcidb" },	// -EOPNOTSUPP
	{ 1574,	"s0d2_mm_axcidb" },	// -EOPNOTSUPP
	{ 1575,	"s0d4_mm_axcidb" },	// -EOPNOTSUPP
	{ 1576,	"s0d1_mm_tniu0" },	// -EOPNOTSUPP
	{ 1577,	"s0d2_mm_tniu0" },	// -EOPNOTSUPP
	{ 1578,	"s0d4_mm_tniu0" },	// -EOPNOTSUPP
	{ 1579,	"s0d1_mm_tniu1" },	// -EOPNOTSUPP
	{ 1580,	"s0d2_mm_tniu1" },	// -EOPNOTSUPP
	{ 1581,	"s0d4_mm_tniu1" },	// -EOPNOTSUPP
	{ 1582,	"s0d1_mm_other" },	// -EOPNOTSUPP
	{ 1583,	"s0d2_mm_other" },	// -EOPNOTSUPP
	{ 1584,	"s0d4_mm_other" },	// -EOPNOTSUPP
	{ 1585,	"s0d1_mm_dbsc0" },	// -EOPNOTSUPP
	{ 1586,	"s0d2_mm_dbsc0" },	// -EOPNOTSUPP
	{ 1587,	"s0d4_mm_dbsc0" },	// -EOPNOTSUPP
	{ 1589,	"s0d1_mm_dbsc1" },	// -EOPNOTSUPP
	{ 1590,	"s0d2_mm_dbsc1" },	// -EOPNOTSUPP
	{ 1591,	"s0d4_mm_dbsc1" },	// -EOPNOTSUPP
	{ 1593,	"s0d1_mm_dbsc2" },	// -EOPNOTSUPP
	{ 1594,	"s0d2_mm_dbsc2" },	// -EOPNOTSUPP
	{ 1595,	"s0d4_mm_dbsc2" },	// -EOPNOTSUPP
	{ 1597,	"s0d1_mm_dbsc3" },	// -EOPNOTSUPP
	{ 1598,	"s0d2_mm_dbsc3" },	// -EOPNOTSUPP
	{ 1599,	"s0d4_mm_dbsc3" },	// -EOPNOTSUPP
	{ 1601,	"s0d1_mm_dbsc4" },	// -EOPNOTSUPP
	{ 1602,	"s0d2_mm_dbsc4" },	// -EOPNOTSUPP
	{ 1603,	"s0d4_mm_dbsc4" },	// -EOPNOTSUPP
	{ 1605,	"s0d1_mm_dbsc5" },	// -EOPNOTSUPP
	{ 1606,	"s0d2_mm_dbsc5" },	// -EOPNOTSUPP
	{ 1607,	"s0d4_mm_dbsc5" },	// -EOPNOTSUPP
	{ 1609,	"s0d1_mm_dbsc6" },	// -EOPNOTSUPP
	{ 1610,	"s0d2_mm_dbsc6" },	// -EOPNOTSUPP
	{ 1611,	"s0d4_mm_dbsc6" },	// -EOPNOTSUPP
	{ 1613,	"s0d1_mm_dbsc7" },	// -EOPNOTSUPP
	{ 1614,	"s0d2_mm_dbsc7" },	// -EOPNOTSUPP
	{ 1615,	"s0d4_mm_dbsc7" },	// -EOPNOTSUPP
	{ 1617,	"s0d1_ddr0_main" },	// -EOPNOTSUPP
	{ 1618,	"s0d2_ddr0_main" },	// -EOPNOTSUPP
	{ 1619,	"s0d4_ddr0_main" },	// -EOPNOTSUPP
	{ 1621,	"s0d1_ddr1_main" },	// -EOPNOTSUPP
	{ 1622,	"s0d2_ddr1_main" },	// -EOPNOTSUPP
	{ 1623,	"s0d4_ddr1_main" },	// -EOPNOTSUPP
	{ 1625,	"s0d1_ddr2_main" },	// -EOPNOTSUPP
	{ 1626,	"s0d2_ddr2_main" },	// -EOPNOTSUPP
	{ 1627,	"s0d4_ddr2_main" },	// -EOPNOTSUPP
	{ 1629,	"s0d1_ddr3_main" },	// -EOPNOTSUPP
	{ 1630,	"s0d2_ddr3_main" },	// -EOPNOTSUPP
	{ 1631,	"s0d4_ddr3_main" },	// -EOPNOTSUPP
	{ 1633,	"s0d1_ddr4_main" },	// -EOPNOTSUPP
	{ 1634,	"s0d2_ddr4_main" },	// -EOPNOTSUPP
	{ 1635,	"s0d4_ddr4_main" },	// -EOPNOTSUPP
	{ 1637,	"s0d1_ddr5_main" },	// -EOPNOTSUPP
	{ 1638,	"s0d2_ddr5_main" },	// -EOPNOTSUPP
	{ 1639,	"s0d4_ddr5_main" },	// -EOPNOTSUPP
	{ 1641,	"s0d1_ddr6_main" },	// -EOPNOTSUPP
	{ 1642,	"s0d2_ddr6_main" },	// -EOPNOTSUPP
	{ 1643,	"s0d4_ddr6_main" },	// -EOPNOTSUPP
	{ 1645,	"s0d1_ddr7_main" },	// -EOPNOTSUPP
	{ 1646,	"s0d2_ddr7_main" },	// -EOPNOTSUPP
	{ 1647,	"s0d4_ddr7_main" },	// -EOPNOTSUPP
	{ 1650,	"sgd8_perw_main" },	// -EOPNOTSUPP
	{ 1651,	"sgd16_perw_main" },	// -EOPNOTSUPP
	{ 1652,	"sgd32_perw_main" },	// -EOPNOTSUPP
	{ 1653,	"sgad4_perw_m" },	// -EOPNOTSUPP
	{ 1654,	"sgad8_perw_m" },	// -EOPNOTSUPP
	{ 1655,	"sgad16_perw_m" },	// -EOPNOTSUPP
	{ 1656,	"sgad32_perw_m" },	// -EOPNOTSUPP
	{ 1657,	"s0ad8_perw_m" },	// -EOPNOTSUPP
	{ 1658,	"cl16m_perw_main" },	// -EOPNOTSUPP
	{ 1660,	"sa_i3c_perw_m" },	// -EOPNOTSUPP
	{ 1662,	"sgd8_perw_bus" },	// -EOPNOTSUPP
	{ 1664,	"sgd32_perw_bus" },	// -EOPNOTSUPP
	{ 1665,	"sgad4_perw_bus" },	// -EOPNOTSUPP
	{ 1666,	"sgad8_perw_bus" },	// -EOPNOTSUPP
	{ 1667,	"sgad16_perw_bus" },	// -EOPNOTSUPP
	{ 1668,	"sgad32_perw_bus" },	// -EOPNOTSUPP
	{ 1669,	"s0ad8_perw_bus" },	// -EOPNOTSUPP
	{ 1670,	"cl16m_perw_bus" },	// -EOPNOTSUPP
	{ 1672,	"sa_i3c_perw_bus" },	// -EOPNOTSUPP
	{ 1674,	"sgd8_mp_main" },	// -EOPNOTSUPP
	{ 1675,	"sgd16_mp_main" },	// -EOPNOTSUPP
	{ 1676,	"sgd32_mp_main" },	// -EOPNOTSUPP
	{ 1677,	"cl16m_mp_main" },	// -EOPNOTSUPP
	{ 1678,	"adghd1_mp_main" },	// -EOPNOTSUPP
	{ 1679,	"adghd4_mp_main" },	// -EOPNOTSUPP
	{ 1681,	"sgd8_mp_bus" },	// -EOPNOTSUPP
	{ 1682,	"sgd16_mp_bus" },	// -EOPNOTSUPP
	{ 1683,	"sgd32_mp_bus" },	// -EOPNOTSUPP
	{ 1684,	"cl16m_mp_bus" },	// -EOPNOTSUPP
	{ 1685,	"adghd1ck_mp_bus" },	// -EOPNOTSUPP
	{ 1686,	"adghd4ck_mp_bus" },	// -EOPNOTSUPP
	{ 1687,	"s0d1_pere_main" },	// -EOPNOTSUPP
	{ 1688,	"s0d2_pere_main" },	// -EOPNOTSUPP
	{ 1689,	"s0d3_pere_main" },	// -EOPNOTSUPP
	{ 1691,	"s0d6_pere_main" },	// -EOPNOTSUPP
	{ 1692,	"s0d8_pere_main" },	// -EOPNOTSUPP
	{ 1693,	"s0d12_pere_main" },	// -EOPNOTSUPP
	{ 1694,	"s0d24_pere_main" },	// -EOPNOTSUPP
	{ 1699,	"cl16m_pere_main" },	// -EOPNOTSUPP
	{ 1700,	"ufs_pere_main" },	// -EOPNOTSUPP
#endif
};

static const struct quirk_rcar_x5h_no_attributes quirk_rcar_x5h_4_31_no_attributes[] = {
	// FIXME We don't need all of them (1160!)
	{ 1645,	"sgd4_perw_main" },	// -EOPNOTSUPP
	{ 1657,	"sgd4_perw_bus" },	// -EOPNOTSUPP
	{ 1659,	"sgd16_perw_bus" },	// -EOPNOTSUPP
	{ 1669,	"sgd4_mp_main" },	// -EOPNOTSUPP
	{ 1676,	"sgd4_mp_bus" },	// -EOPNOTSUPP
	{ 1686,	"s0d4_pere_main" },	// -EOPNOTSUPP
#if 0
	{ 0,	"vipn_fcpcs0" },	// -EREMOTEIO
	{ 1,	"vipn_fcpcs1" },	// -EREMOTEIO
	{ 2,	"vipn_vcp5x0" },	// -EREMOTEIO
	{ 3,	"vipn_vcp5x1" },	// -EREMOTEIO
	{ 4,	"vipn_msync" },		// -EOPNOTSUPP
	{ 5,	"vips_fcpcs0" },	// -EREMOTEIO
	{ 6,	"vips_fcpcs1" },	// -EREMOTEIO
	{ 7,	"vips_vcp5x0" },	// -EREMOTEIO
	{ 8,	"vips_vcp5x1" },	// -EREMOTEIO
	{ 9,	"vips_msync" },		// -EOPNOTSUPP
	{ 11,	"isp0" },		// -EREMOTEIO
	{ 12,	"isp1" },		// -EREMOTEIO
	{ 13,	"isp2" },		// -EREMOTEIO
	{ 14,	"isp3" },		// -EREMOTEIO
	{ 15,	"ispcs0" },		// -EREMOTEIO
	{ 16,	"ispcs1" },		// -EREMOTEIO
	{ 17,	"ispcs2" },		// -EREMOTEIO
	{ 18,	"ispcs3" },		// -EREMOTEIO
	{ 19,	"csitop0" },		// -EREMOTEIO
	{ 20,	"csitop1" },		// -EREMOTEIO
	{ 21,	"csitop2" },		// -EREMOTEIO
	{ 22,	"csitop3" },		// -EREMOTEIO
	{ 23,	"dptx0" },		// -EREMOTEIO
	{ 24,	"dptx1" },		// -EREMOTEIO
	{ 25,	"dptx2" },		// -EREMOTEIO
	{ 26,	"vspd0" },		// -EREMOTEIO
	{ 27,	"vspd1" },		// -EREMOTEIO
	{ 28,	"vspd2" },		// -EREMOTEIO
	{ 29,	"vspd3" },		// -EREMOTEIO
	{ 30,	"vspd4" },		// -EREMOTEIO
	{ 31,	"vspdb0" },		// -EREMOTEIO
	{ 32,	"vspdb1" },		// -EREMOTEIO
	{ 33,	"vspdb2" },		// -EREMOTEIO
	{ 34,	"vspdb3" },		// -EREMOTEIO
	{ 35,	"vspdb4" },		// -EREMOTEIO
	{ 36,	"vspx0" },		// -EREMOTEIO
	{ 37,	"vspx1" },		// -EREMOTEIO
	{ 38,	"vspx2" },		// -EREMOTEIO
	{ 39,	"vspx3" },		// -EREMOTEIO
	{ 40,	"fcpvd0" },		// -EREMOTEIO
	{ 41,	"fcpvd1" },		// -EREMOTEIO
	{ 42,	"fcpvd2" },		// -EREMOTEIO
	{ 43,	"fcpvd3" },		// -EREMOTEIO
	{ 44,	"fcpvd4" },		// -EREMOTEIO
	{ 45,	"fcpvd5" },		// -EREMOTEIO
	{ 46,	"fcpvd6" },		// -EREMOTEIO
	{ 47,	"fcpvd7" },		// -EREMOTEIO
	{ 48,	"fcpvd8" },		// -EREMOTEIO
	{ 49,	"fcpvd9" },		// -EREMOTEIO
	{ 52,	"fcpvx0" },		// -EREMOTEIO
	{ 53,	"fcpvx1" },		// -EREMOTEIO
	{ 54,	"fcpvx2" },		// -EREMOTEIO
	{ 55,	"fcpvx3" },		// -EREMOTEIO
	{ 56,	"vin000" },		// -EREMOTEIO
	{ 57,	"vin001" },		// -EREMOTEIO
	{ 58,	"vin002" },		// -EREMOTEIO
	{ 59,	"vin003" },		// -EREMOTEIO
	{ 60,	"vin004" },		// -EREMOTEIO
	{ 61,	"vin005" },		// -EREMOTEIO
	{ 62,	"vin006" },		// -EREMOTEIO
	{ 63,	"vin007" },		// -EREMOTEIO
	{ 64,	"vin010" },		// -EREMOTEIO
	{ 65,	"vin011" },		// -EREMOTEIO
	{ 66,	"vin012" },		// -EREMOTEIO
	{ 67,	"vin013" },		// -EREMOTEIO
	{ 68,	"vin014" },		// -EREMOTEIO
	{ 69,	"vin015" },		// -EREMOTEIO
	{ 70,	"vin016" },		// -EREMOTEIO
	{ 71,	"vin017" },		// -EREMOTEIO
	{ 72,	"vin020" },		// -EREMOTEIO
	{ 73,	"vin021" },		// -EREMOTEIO
	{ 74,	"vin022" },		// -EREMOTEIO
	{ 75,	"vin023" },		// -EREMOTEIO
	{ 76,	"vin024" },		// -EREMOTEIO
	{ 77,	"vin025" },		// -EREMOTEIO
	{ 78,	"vin026" },		// -EREMOTEIO
	{ 79,	"vin027" },		// -EREMOTEIO
	{ 80,	"vin030" },		// -EREMOTEIO
	{ 81,	"vin031" },		// -EREMOTEIO
	{ 82,	"vin032" },		// -EREMOTEIO
	{ 83,	"vin033" },		// -EREMOTEIO
	{ 84,	"vin034" },		// -EREMOTEIO
	{ 85,	"vin035" },		// -EREMOTEIO
	{ 86,	"vin036" },		// -EREMOTEIO
	{ 87,	"vin037" },		// -EREMOTEIO
	{ 88,	"vin040" },		// -EREMOTEIO
	{ 89,	"vin041" },		// -EREMOTEIO
	{ 90,	"vin042" },		// -EREMOTEIO
	{ 91,	"vin043" },		// -EREMOTEIO
	{ 92,	"vin044" },		// -EREMOTEIO
	{ 93,	"vin045" },		// -EREMOTEIO
	{ 94,	"vin046" },		// -EREMOTEIO
	{ 95,	"vin047" },		// -EREMOTEIO
	{ 96,	"vin050" },		// -EREMOTEIO
	{ 97,	"vin051" },		// -EREMOTEIO
	{ 98,	"vin052" },		// -EREMOTEIO
	{ 99,	"vin053" },		// -EREMOTEIO
	{ 100,	"vin054" },		// -EREMOTEIO
	{ 101,	"vin055" },		// -EREMOTEIO
	{ 102,	"vin056" },		// -EREMOTEIO
	{ 103,	"vin057" },		// -EREMOTEIO
	{ 104,	"vin060" },		// -EREMOTEIO
	{ 105,	"vin061" },		// -EREMOTEIO
	{ 106,	"vin062" },		// -EREMOTEIO
	{ 107,	"vin063" },		// -EREMOTEIO
	{ 108,	"vin064" },		// -EREMOTEIO
	{ 109,	"vin065" },		// -EREMOTEIO
	{ 110,	"vin066" },		// -EREMOTEIO
	{ 111,	"vin067" },		// -EREMOTEIO
	{ 112,	"vin070" },		// -EREMOTEIO
	{ 113,	"vin071" },		// -EREMOTEIO
	{ 114,	"vin072" },		// -EREMOTEIO
	{ 115,	"vin073" },		// -EREMOTEIO
	{ 116,	"vin074" },		// -EREMOTEIO
	{ 117,	"vin075" },		// -EREMOTEIO
	{ 118,	"vin076" },		// -EREMOTEIO
	{ 119,	"vin077" },		// -EREMOTEIO
	{ 120,	"vin080" },		// -EREMOTEIO
	{ 121,	"vin081" },		// -EREMOTEIO
	{ 122,	"vin082" },		// -EREMOTEIO
	{ 123,	"vin083" },		// -EREMOTEIO
	{ 124,	"vin084" },		// -EREMOTEIO
	{ 125,	"vin085" },		// -EREMOTEIO
	{ 126,	"vin086" },		// -EREMOTEIO
	{ 127,	"vin087" },		// -EREMOTEIO
	{ 128,	"vin090" },		// -EREMOTEIO
	{ 129,	"vin091" },		// -EREMOTEIO
	{ 130,	"vin092" },		// -EREMOTEIO
	{ 131,	"vin093" },		// -EREMOTEIO
	{ 132,	"vin094" },		// -EREMOTEIO
	{ 133,	"vin095" },		// -EREMOTEIO
	{ 134,	"vin096" },		// -EREMOTEIO
	{ 135,	"vin097" },		// -EREMOTEIO
	{ 136,	"vin100" },		// -EREMOTEIO
	{ 137,	"vin101" },		// -EREMOTEIO
	{ 138,	"vin102" },		// -EREMOTEIO
	{ 139,	"vin103" },		// -EREMOTEIO
	{ 140,	"vin104" },		// -EREMOTEIO
	{ 141,	"vin105" },		// -EREMOTEIO
	{ 142,	"vin106" },		// -EREMOTEIO
	{ 143,	"vin107" },		// -EREMOTEIO
	{ 144,	"vin110" },		// -EREMOTEIO
	{ 145,	"vin111" },		// -EREMOTEIO
	{ 146,	"vin112" },		// -EREMOTEIO
	{ 147,	"vin113" },		// -EREMOTEIO
	{ 148,	"vin114" },		// -EREMOTEIO
	{ 149,	"vin115" },		// -EREMOTEIO
	{ 150,	"vin116" },		// -EREMOTEIO
	{ 151,	"vin117" },		// -EREMOTEIO
	{ 153,	"vcon0" },		// -EREMOTEIO
	{ 154,	"vcon1" },		// -EREMOTEIO
	{ 155,	"vcon2" },		// -EREMOTEIO
	{ 156,	"vcon3" },		// -EREMOTEIO
	{ 157,	"vcon4" },		// -EREMOTEIO
	{ 158,	"vcon5" },		// -EREMOTEIO
	{ 159,	"vcon6" },		// -EREMOTEIO
	{ 160,	"vcon7" },		// -EREMOTEIO
	{ 161,	"vcon8" },		// -EREMOTEIO
	{ 162,	"vcon9" },		// -EREMOTEIO
	{ 163,	"vspb0" },		// -EREMOTEIO
	{ 164,	"vspb1" },		// -EREMOTEIO
	{ 165,	"vspb2" },		// -EREMOTEIO
	{ 166,	"vspb3" },		// -EREMOTEIO
	{ 167,	"vspb4" },		// -EREMOTEIO
	{ 168,	"vspi0" },		// -EREMOTEIO
	{ 169,	"vspi1" },		// -EREMOTEIO
	{ 170,	"vspi2" },		// -EREMOTEIO
	{ 171,	"vspi3" },		// -EREMOTEIO
	{ 172,	"fcpvb0" },		// -EREMOTEIO
	{ 173,	"fcpvb1" },		// -EREMOTEIO
	{ 174,	"fcpvb2" },		// -EREMOTEIO
	{ 175,	"fcpvb3" },		// -EREMOTEIO
	{ 176,	"fcpvb4" },		// -EREMOTEIO
	{ 177,	"fcpvi0" },		// -EREMOTEIO
	{ 178,	"fcpvi1" },		// -EREMOTEIO
	{ 179,	"fcpvi2" },		// -EREMOTEIO
	{ 180,	"fcpvi3" },		// -EREMOTEIO
	{ 181,	"vin0" },		// -EREMOTEIO
	{ 182,	"vin1" },		// -EREMOTEIO
	{ 183,	"vin2" },		// -EREMOTEIO
	{ 184,	"vin3" },		// -EREMOTEIO
	{ 185,	"vin4" },		// -EREMOTEIO
	{ 186,	"vin5" },		// -EREMOTEIO
	{ 187,	"vin6" },		// -EREMOTEIO
	{ 188,	"vin7" },		// -EREMOTEIO
	{ 189,	"vin8" },		// -EREMOTEIO
	{ 190,	"vin9" },		// -EREMOTEIO
	{ 191,	"vin10" },		// -EREMOTEIO
	{ 192,	"vin11" },		// -EREMOTEIO
	{ 194,	"pere_gpiodm1" },	// -EOPNOTSUPP
	{ 195,	"pere_gpiodm2" },	// -EOPNOTSUPP
	{ 196,	"pere_gpiodm3" },	// -EOPNOTSUPP
	{ 202,	"perw_gpiodm1" },	// -EOPNOTSUPP
	{ 203,	"perw_gpiodm2" },	// -EOPNOTSUPP
	{ 204,	"perw_gpiodm3" },	// -EOPNOTSUPP
	{ 333,	"hscn_gpiodm1" },	// -EOPNOTSUPP
	{ 334,	"hscn_gpiodm2" },	// -EOPNOTSUPP
	{ 335,	"hscn_gpiodm3" },	// -EOPNOTSUPP
	{ 336,	"us30" },		// -EREMOTEIO
	{ 337,	"us31" },		// -EREMOTEIO
	{ 338,	"us32" },		// -EREMOTEIO
	{ 339,	"us33" },		// -EREMOTEIO
	{ 346,	"pci411" },		// -EREMOTEIO
	{ 347,	"pci402" },		// -EOPNOTSUPP
	{ 348,	"pci412" },		// -EOPNOTSUPP
	{ 349,	"cr52top0" },		// -EOPNOTSUPP
	{ 352,	"cr52core0_po" },	// -EOPNOTSUPP
	{ 354,	"cr52core1_po" },	// -EOPNOTSUPP
	{ 356,	"cr52shadow0_po" },	// -EOPNOTSUPP
	{ 358,	"cr52shadow1_po" },	// -EOPNOTSUPP
	{ 359,	"cr52top1" },		// -EOPNOTSUPP
	{ 361,	"cr52core2" },		// -EOPNOTSUPP
	{ 362,	"cr52core2_po" },	// -EOPNOTSUPP
	{ 364,	"cr52core3_po" },	// -EOPNOTSUPP
	{ 366,	"cr52shadow2_po" },	// -EOPNOTSUPP
	{ 368,	"cr52shadow3_po" },	// -EOPNOTSUPP
	{ 369,	"cr52top2" },		// -EOPNOTSUPP
	{ 372,	"cr52core4_po" },	// -EOPNOTSUPP
	{ 374,	"cr52core5_po" },	// -EOPNOTSUPP
	{ 376,	"cr52shadow4_po" },	// -EOPNOTSUPP
	{ 378,	"cr52shadow5_po" },	// -EOPNOTSUPP
	{ 388,	"wdt1" },		// -EOPNOTSUPP
	{ 389,	"wwdt00" },		// -EOPNOTSUPP
	{ 390,	"wwdt10" },		// -EOPNOTSUPP
	{ 391,	"wwdt20" },		// -EOPNOTSUPP
	{ 392,	"wwdt30" },		// -EOPNOTSUPP
	{ 393,	"wwdt40" },		// -EOPNOTSUPP
	{ 394,	"wwdt50" },		// -EOPNOTSUPP
	{ 395,	"wwdt60" },		// -EOPNOTSUPP
	{ 396,	"wwdt70" },		// -EOPNOTSUPP
	{ 397,	"wwdt80" },		// -EOPNOTSUPP
	{ 398,	"wwdt90" },		// -EOPNOTSUPP
	{ 399,	"wwdt100" },		// -EOPNOTSUPP
	{ 400,	"wwdt110" },		// -EOPNOTSUPP
	{ 401,	"wwdt120" },		// -EOPNOTSUPP
	{ 402,	"wwdt130" },		// -EOPNOTSUPP
	{ 403,	"wwdt01" },		// -EOPNOTSUPP
	{ 404,	"wwdt11" },		// -EOPNOTSUPP
	{ 405,	"wwdt21" },		// -EOPNOTSUPP
	{ 406,	"wwdt31" },		// -EOPNOTSUPP
	{ 407,	"wwdt41" },		// -EOPNOTSUPP
	{ 408,	"wwdt51" },		// -EOPNOTSUPP
	{ 409,	"wwdt61" },		// -EOPNOTSUPP
	{ 410,	"wwdt71" },		// -EOPNOTSUPP
	{ 411,	"wwdt81" },		// -EOPNOTSUPP
	{ 412,	"wwdt91" },		// -EOPNOTSUPP
	{ 413,	"wwdt101" },		// -EOPNOTSUPP
	{ 414,	"wwdt111" },		// -EOPNOTSUPP
	{ 415,	"wwdt121" },		// -EOPNOTSUPP
	{ 416,	"wwdt131" },		// -EOPNOTSUPP
	{ 417,	"wwdt140" },		// -EOPNOTSUPP
	{ 418,	"wwdt141" },		// -EOPNOTSUPP
	{ 419,	"wwdt150" },		// -EOPNOTSUPP
	{ 420,	"wwdt151" },		// -EOPNOTSUPP
	{ 421,	"wwdt160" },		// -EOPNOTSUPP
	{ 422,	"wwdt161" },		// -EOPNOTSUPP
	{ 423,	"wwdt170" },		// -EOPNOTSUPP
	{ 424,	"wwdt171" },		// -EOPNOTSUPP
	{ 425,	"wwdt180" },		// -EOPNOTSUPP
	{ 426,	"wwdt181" },		// -EOPNOTSUPP
	{ 427,	"wwdt190" },		// -EOPNOTSUPP
	{ 428,	"wwdt191" },		// -EOPNOTSUPP
	{ 429,	"swdt0" },		// -EOPNOTSUPP
	{ 430,	"swdt1" },		// -EOPNOTSUPP
	{ 465,	"inttp" },		// -EOPNOTSUPP
	{ 466,	"intap1" },		// -EOPNOTSUPP
	{ 502,	"c0rtmg" },		// -EOPNOTSUPP
	{ 503,	"cmn_topn_gic" },	// -EOPNOTSUPP
	{ 504,	"cmn_topn_dbg" },	// -EOPNOTSUPP
	{ 505,	"cmn_tope" },		// -EOPNOTSUPP
	{ 506,	"cmn_tops" },		// -EOPNOTSUPP
	{ 507,	"cmn_topw" },		// -EOPNOTSUPP
	{ 513,	"scmt" },		// -EOPNOTSUPP
	{ 522,	"intap_cmn2top0" },	// -EOPNOTSUPP
	{ 523,	"pci60bg0" },		// -EOPNOTSUPP
	{ 524,	"pci60bg1" },		// -EOPNOTSUPP
	{ 538,	"pci601" },		// -EREMOTEIO
	{ 539,	"pci611" },		// -EOPNOTSUPP
	{ 540,	"pci602" },		// -EREMOTEIO
	{ 541,	"pci612" },		// -EOPNOTSUPP
	{ 542,	"imn_imr000" },		// -EREMOTEIO
	{ 543,	"imn_imr001" },		// -EREMOTEIO
	{ 544,	"imn_ims00" },		// -EREMOTEIO
	{ 545,	"imn_ims01" },		// -EREMOTEIO
	{ 546,	"imn_ims02" },		// -EREMOTEIO
	{ 547,	"imn_ims03" },		// -EREMOTEIO
	{ 548,	"ims_imr000" },		// -EREMOTEIO
	{ 549,	"ims_imr001" },		// -EREMOTEIO
	{ 550,	"ims_ims00" },		// -EREMOTEIO
	{ 551,	"ims_ims01" },		// -EREMOTEIO
	{ 552,	"ims_ims02" },		// -EREMOTEIO
	{ 553,	"ims_ims03" },		// -EREMOTEIO
	{ 554,	"rgx_jones0" },		// -EREMOTEIO
	{ 555,	"rgx_mercer0" },	// -EREMOTEIO
	{ 556,	"rgx_mercer1" },	// -EREMOTEIO
	{ 557,	"rgx_mercer2" },	// -EREMOTEIO
	{ 558,	"rgx_mercer3" },	// -EREMOTEIO
	{ 559,	"rgx_texas0" },		// -EREMOTEIO
	{ 560,	"rgx_texas1" },		// -EREMOTEIO
	{ 561,	"rgx_swift0" },		// -EREMOTEIO
	{ 562,	"rgx_swift1" },		// -EREMOTEIO
	{ 563,	"rgx_swift2" },		// -EREMOTEIO
	{ 564,	"rgx_swift3" },		// -EREMOTEIO
	{ 565,	"rgx_jones1" },		// -EREMOTEIO
	{ 566,	"rgx_mercer4" },	// -EREMOTEIO
	{ 567,	"rgx_mercer5" },	// -EREMOTEIO
	{ 568,	"rgx_mercer6" },	// -EREMOTEIO
	{ 569,	"rgx_mercer7" },	// -EREMOTEIO
	{ 570,	"rgx_texas2" },		// -EREMOTEIO
	{ 571,	"rgx_texas3" },		// -EREMOTEIO
	{ 572,	"rgx_swift4" },		// -EREMOTEIO
	{ 573,	"rgx_swift5" },		// -EREMOTEIO
	{ 574,	"rgx_swift6" },		// -EREMOTEIO
	{ 575,	"rgx_swift7" },		// -EREMOTEIO
	{ 578,	"dsparcsyn_axi" },	// -EOPNOTSUPP
	{ 581,	"dsp2_misc_pores" },	// -EOPNOTSUPP
	{ 582,	"dsp2_misc_atres" },	// -EOPNOTSUPP
	{ 584,	"dsp2_core0_csd" },	// -EOPNOTSUPP
	{ 586,	"dsp2_core1_csd" },	// -EOPNOTSUPP
	{ 588,	"dsp2_core2_csd" },	// -EOPNOTSUPP
	{ 590,	"dsp2_core3_csd" },	// -EOPNOTSUPP
	{ 593,	"dsp3_misc_pores" },	// -EOPNOTSUPP
	{ 594,	"dsp3_misc_atres" },	// -EOPNOTSUPP
	{ 596,	"dsp3_core0_csd" },	// -EOPNOTSUPP
	{ 598,	"dsp3_core1_csd" },	// -EOPNOTSUPP
	{ 600,	"dsp3_core2_csd" },	// -EOPNOTSUPP
	{ 602,	"dsp3_core3_csd" },	// -EOPNOTSUPP
	{ 605,	"dsp4_misc_pores" },	// -EOPNOTSUPP
	{ 606,	"dsp4_misc_atres" },	// -EOPNOTSUPP
	{ 608,	"dsp4_core0_csd" },	// -EOPNOTSUPP
	{ 610,	"dsp4_core1_csd" },	// -EOPNOTSUPP
	{ 612,	"dsp4_core2_csd" },	// -EOPNOTSUPP
	{ 614,	"dsp4_core3_csd" },	// -EOPNOTSUPP
	{ 617,	"dsp5_misc_pores" },	// -EOPNOTSUPP
	{ 618,	"dsp5_misc_atres" },	// -EOPNOTSUPP
	{ 620,	"dsp5_core0_csd" },	// -EOPNOTSUPP
	{ 622,	"dsp5_core1_csd" },	// -EOPNOTSUPP
	{ 624,	"dsp5_core2_csd" },	// -EOPNOTSUPP
	{ 626,	"dsp5_core3_csd" },	// -EOPNOTSUPP
	{ 629,	"dsp6_misc_pores" },	// -EOPNOTSUPP
	{ 630,	"dsp6_misc_atres" },	// -EOPNOTSUPP
	{ 632,	"dsp6_core0_csd" },	// -EOPNOTSUPP
	{ 634,	"dsp6_core1_csd" },	// -EOPNOTSUPP
	{ 636,	"dsp6_core2_csd" },	// -EOPNOTSUPP
	{ 638,	"dsp6_core3_csd" },	// -EOPNOTSUPP
	{ 657,	"npu0_atres" },		// -EOPNOTSUPP
	{ 659,	"npu0_axi" },		// -EOPNOTSUPP
	{ 660,	"npu0_nl2" },		// -EOPNOTSUPP
	{ 661,	"npu0_nl2arc0" },	// -EOPNOTSUPP
	{ 662,	"npu0_nl2arc1" },	// -EOPNOTSUPP
	{ 663,	"npu0_nl1grp0" },	// -EOPNOTSUPP
	{ 664,	"npu0_sl0nl1arc" },	// -EOPNOTSUPP
	{ 665,	"npu0_sl1nl1arc" },	// -EOPNOTSUPP
	{ 666,	"npu0_sl2nl1arc" },	// -EOPNOTSUPP
	{ 667,	"npu0_nl1grp1" },	// -EOPNOTSUPP
	{ 668,	"npu0_sl3nl1arc" },	// -EOPNOTSUPP
	{ 669,	"npu0_sl4nl1arc" },	// -EOPNOTSUPP
	{ 670,	"npu0_sl5nl1arc" },	// -EOPNOTSUPP
	{ 671,	"npu0_nl1grp2" },	// -EOPNOTSUPP
	{ 672,	"npu0_sl6nl1arc" },	// -EOPNOTSUPP
	{ 673,	"npu0_sl7nl1arc" },	// -EOPNOTSUPP
	{ 674,	"npu0_sl8nl1arc" },	// -EOPNOTSUPP
	{ 675,	"npu0_nl1grp3" },	// -EOPNOTSUPP
	{ 676,	"npu0_sl9nl1arc" },	// -EOPNOTSUPP
	{ 677,	"npu0_sl10nl1arc" },	// -EOPNOTSUPP
	{ 678,	"npu0_sl11nl1arc" },	// -EOPNOTSUPP
	{ 681,	"npu0_aon_noc" },	// -EOPNOTSUPP
	{ 682,	"npu0_aon_cfg" },	// -EOPNOTSUPP
	{ 683,	"npu0_aon_csd" },	// -EOPNOTSUPP
	{ 684,	"npu0_core0_grp0" },	// -EREMOTEIO
	{ 685,	"npu0_core1_grp0" },	// -EREMOTEIO
	{ 686,	"npu0_core2_grp0" },	// -EREMOTEIO
	{ 687,	"npu0_core0_grp1" },	// -EREMOTEIO
	{ 688,	"npu0_core1_grp1" },	// -EREMOTEIO
	{ 689,	"npu0_core2_grp1" },	// -EREMOTEIO
	{ 690,	"npu0_core0_grp2" },	// -EREMOTEIO
	{ 691,	"npu0_core1_grp2" },	// -EREMOTEIO
	{ 692,	"npu0_core2_grp2" },	// -EREMOTEIO
	{ 693,	"npu0_core0_grp3" },	// -EREMOTEIO
	{ 694,	"npu0_core1_grp3" },	// -EREMOTEIO
	{ 695,	"npu0_core2_grp3" },	// -EREMOTEIO
	{ 696,	"npu0_pres" },		// -EREMOTEIO
	{ 699,	"npu0_misc_pres" },	// -EOPNOTSUPP
	{ 700,	"npu0_misc_atres" },	// -EOPNOTSUPP
	{ 702,	"npu0_c0_dsp_csd" },	// -EOPNOTSUPP
	{ 704,	"npu0_c1_dsp_csd" },	// -EOPNOTSUPP
	{ 706,	"npu0_c2_dsp_csd" },	// -EOPNOTSUPP
	{ 708,	"npu0_c3_dsp_csd" },	// -EOPNOTSUPP
	{ 711,	"npu1_atres" },		// -EOPNOTSUPP
	{ 713,	"npu1_axi" },		// -EOPNOTSUPP
	{ 714,	"npu1_nl2" },		// -EOPNOTSUPP
	{ 715,	"npu1_nl2arc0" },	// -EOPNOTSUPP
	{ 716,	"npu1_nl2arc1" },	// -EOPNOTSUPP
	{ 717,	"npu1_nl1grp0" },	// -EOPNOTSUPP
	{ 718,	"npu1_sl0nl1arc" },	// -EOPNOTSUPP
	{ 719,	"npu1_sl1nl1arc" },	// -EOPNOTSUPP
	{ 720,	"npu1_sl2nl1arc" },	// -EOPNOTSUPP
	{ 721,	"npu1_nl1grp1" },	// -EOPNOTSUPP
	{ 722,	"npu1_sl3nl1arc" },	// -EOPNOTSUPP
	{ 723,	"npu1_sl4nl1arc" },	// -EOPNOTSUPP
	{ 724,	"npu1_sl5nl1arc" },	// -EOPNOTSUPP
	{ 725,	"npu1_nl1grp2" },	// -EOPNOTSUPP
	{ 726,	"npu1_sl6nl1arc" },	// -EOPNOTSUPP
	{ 727,	"npu1_sl7nl1arc" },	// -EOPNOTSUPP
	{ 728,	"npu1_sl8nl1arc" },	// -EOPNOTSUPP
	{ 729,	"npu1_nl1grp3" },	// -EOPNOTSUPP
	{ 730,	"npu1_sl9nl1arc" },	// -EOPNOTSUPP
	{ 731,	"npu1_sl10nl1arc" },	// -EOPNOTSUPP
	{ 732,	"npu1_sl11nl1arc" },	// -EOPNOTSUPP
	{ 735,	"npu1_aon_noc" },	// -EOPNOTSUPP
	{ 736,	"npu1_aon_cfg" },	// -EOPNOTSUPP
	{ 737,	"npu1_aon_csd" },	// -EOPNOTSUPP
	{ 738,	"npu1_core0_grp0" },	// -EREMOTEIO
	{ 739,	"npu1_core1_grp0" },	// -EREMOTEIO
	{ 740,	"npu1_core2_grp0" },	// -EREMOTEIO
	{ 741,	"npu1_core0_grp1" },	// -EREMOTEIO
	{ 742,	"npu1_core1_grp1" },	// -EREMOTEIO
	{ 743,	"npu1_core2_grp1" },	// -EREMOTEIO
	{ 744,	"npu1_core0_grp2" },	// -EREMOTEIO
	{ 745,	"npu1_core1_grp2" },	// -EREMOTEIO
	{ 746,	"npu1_core2_grp2" },	// -EREMOTEIO
	{ 747,	"npu1_core0_grp3" },	// -EREMOTEIO
	{ 748,	"npu1_core1_grp3" },	// -EREMOTEIO
	{ 749,	"npu1_core2_grp3" },	// -EREMOTEIO
	{ 750,	"npu1_pres" },		// -EREMOTEIO
	{ 753,	"npu1_misc_pres" },	// -EOPNOTSUPP
	{ 754,	"npu1_misc_atres" },	// -EOPNOTSUPP
	{ 756,	"npu1_c0_dsp_csd" },	// -EOPNOTSUPP
	{ 758,	"npu1_c1_dsp_csd" },	// -EOPNOTSUPP
	{ 760,	"npu1_c2_dsp_csd" },	// -EOPNOTSUPP
	{ 762,	"npu1_c3_dsp_csd" },	// -EOPNOTSUPP
	{ 764,	"cmn_core0_pores" },	// -EOPNOTSUPP
	{ 765,	"cmn_core0_syres" },	// -EOPNOTSUPP
	{ 766,	"cmn_core1_pores" },	// -EOPNOTSUPP
	{ 767,	"cmn_core1_syres" },	// -EOPNOTSUPP
	{ 768,	"cmn_core2_pores" },	// -EOPNOTSUPP
	{ 769,	"cmn_core2_syres" },	// -EOPNOTSUPP
	{ 770,	"cmn_core3_pores" },	// -EOPNOTSUPP
	{ 771,	"cmn_core3_syres" },	// -EOPNOTSUPP
	{ 772,	"scp" },		// -EOPNOTSUPP
	{ 775,	"tauj1" },		// -EOPNOTSUPP
	{ 802,	"fray01" },		// -EOPNOTSUPP
	{ 804,	"wwdt200" },		// -EOPNOTSUPP
	{ 805,	"wwdt201" },		// -EOPNOTSUPP
	{ 808,	"intscp" },		// -EOPNOTSUPP
	{ 809,	"tauj3" },		// -EOPNOTSUPP
	{ 810,	"rtca" },		// -EOPNOTSUPP
	{ 812,	"gpiodm1" },		// -EOPNOTSUPP
	{ 813,	"gpiodm2" },		// -EOPNOTSUPP
	{ 814,	"gpiodm3" },		// -EOPNOTSUPP
	{ 815,	"s0d1" },		// -EOPNOTSUPP
	{ 816,	"s0d2" },		// -EOPNOTSUPP
	{ 817,	"s0d4" },		// -EOPNOTSUPP
	{ 818,	"s0d8" },		// -EOPNOTSUPP
	{ 819,	"cl" },			// -EOPNOTSUPP
	{ 820,	"sgd1" },		// -EOPNOTSUPP
	{ 821,	"sgd2" },		// -EOPNOTSUPP
	{ 824,	"cl16m" },		// -EOPNOTSUPP
	{ 833,	"zx" },			// -EOPNOTSUPP
	{ 858,	"s0d1_cmn_busn" },	// -EOPNOTSUPP
	{ 859,	"s0d2_cmn_busn" },	// -EOPNOTSUPP
	{ 860,	"s0d4_cmn_busn" },	// -EOPNOTSUPP
	{ 861,	"zx_cmn_busn" },	// -EOPNOTSUPP
	{ 862,	"s0d1_cmn_buss" },	// -EOPNOTSUPP
	{ 863,	"s0d2_cmn_buss" },	// -EOPNOTSUPP
	{ 864,	"s0d4_cmn_buss" },	// -EOPNOTSUPP
	{ 865,	"zx_cmn_buss" },	// -EOPNOTSUPP
	{ 866,	"zx_cmn_main0" },	// -EOPNOTSUPP
	{ 867,	"zx_cmn_main1" },	// -EOPNOTSUPP
	{ 868,	"cmn_core_grp2" },	// -EOPNOTSUPP
	{ 869,	"cmn_core_grp3" },	// -EOPNOTSUPP
	{ 870,	"cmn_ccg2_grp0" },	// -EOPNOTSUPP
	{ 871,	"cmn_ccg2_grp1" },	// -EOPNOTSUPP
	{ 872,	"cmn_ccg2_grp2" },	// -EOPNOTSUPP
	{ 873,	"cmn_ccg2_grp3" },	// -EOPNOTSUPP
	{ 874,	"cmn_ccg3_grp0" },	// -EOPNOTSUPP
	{ 875,	"cmn_ccg3_grp1" },	// -EOPNOTSUPP
	{ 876,	"cmn_ccg3_grp2" },	// -EOPNOTSUPP
	{ 877,	"cmn_ccg3_grp3" },	// -EOPNOTSUPP
	{ 878,	"sgd1_vio_other" },	// -EOPNOTSUPP
	{ 879,	"sgd2_vio_other" },	// -EOPNOTSUPP
	{ 880,	"sgd4_vio_other" },	// -EOPNOTSUPP
	{ 881,	"sgd8_vio_other" },	// -EOPNOTSUPP
	{ 882,	"sgd16_vio_other" },	// -EOPNOTSUPP
	{ 884,	"sgd1_vio_bus" },	// -EOPNOTSUPP
	{ 885,	"sgd2_vio_bus" },	// -EOPNOTSUPP
	{ 886,	"sgd4_vio_bus" },	// -EOPNOTSUPP
	{ 887,	"sgd8_vio_bus" },	// -EOPNOTSUPP
	{ 888,	"sgd16_vio_bus" },	// -EOPNOTSUPP
	{ 889,	"sgd1_vio_csi0" },	// -EOPNOTSUPP
	{ 890,	"sgd2_vio_csi0" },	// -EOPNOTSUPP
	{ 891,	"sgd4_vio_csi0" },	// -EOPNOTSUPP
	{ 892,	"sgd8_vio_csi0" },	// -EOPNOTSUPP
	{ 893,	"sgd16_vio_csi0" },	// -EOPNOTSUPP
	{ 895,	"sgd1_vio_csi1" },	// -EOPNOTSUPP
	{ 896,	"sgd2_vio_csi1" },	// -EOPNOTSUPP
	{ 897,	"sgd4_vio_csi1" },	// -EOPNOTSUPP
	{ 898,	"sgd8_vio_csi1" },	// -EOPNOTSUPP
	{ 899,	"sgd16_vio_csi1" },	// -EOPNOTSUPP
	{ 901,	"sgd1_vio_csi2" },	// -EOPNOTSUPP
	{ 902,	"sgd2_vio_csi2" },	// -EOPNOTSUPP
	{ 903,	"sgd4_vio_csi2" },	// -EOPNOTSUPP
	{ 904,	"sgd8_vio_csi2" },	// -EOPNOTSUPP
	{ 905,	"sgd16_vio_csi2" },	// -EOPNOTSUPP
	{ 907,	"sgd1_vio_isp0" },	// -EOPNOTSUPP
	{ 908,	"sgd2_vio_isp0" },	// -EOPNOTSUPP
	{ 909,	"sgd4_vio_isp0" },	// -EOPNOTSUPP
	{ 910,	"sgd8_vio_isp0" },	// -EOPNOTSUPP
	{ 911,	"sgd16_vio_isp0" },	// -EOPNOTSUPP
	{ 912,	"sgd1_vio_isp1" },	// -EOPNOTSUPP
	{ 913,	"sgd2_vio_isp1" },	// -EOPNOTSUPP
	{ 914,	"sgd4_vio_isp1" },	// -EOPNOTSUPP
	{ 915,	"sgd8_vio_isp1" },	// -EOPNOTSUPP
	{ 916,	"sgd16_vio_isp1" },	// -EOPNOTSUPP
	{ 917,	"sgd1_vio_isp2" },	// -EOPNOTSUPP
	{ 918,	"sgd2_vio_isp2" },	// -EOPNOTSUPP
	{ 919,	"sgd4_vio_isp2" },	// -EOPNOTSUPP
	{ 920,	"sgd8_vio_isp2" },	// -EOPNOTSUPP
	{ 921,	"sgd16_vio_isp2" },	// -EOPNOTSUPP
	{ 922,	"sgd1_vio_isp3" },	// -EOPNOTSUPP
	{ 923,	"sgd2_vio_isp3" },	// -EOPNOTSUPP
	{ 924,	"sgd4_vio_isp3" },	// -EOPNOTSUPP
	{ 925,	"sgd8_vio_isp3" },	// -EOPNOTSUPP
	{ 926,	"sgd16_vio_isp3" },	// -EOPNOTSUPP
	{ 927,	"s0d1_vio_im0" },	// -EOPNOTSUPP
	{ 928,	"s0d2_vio_im0" },	// -EOPNOTSUPP
	{ 929,	"s0d4_vio_im0" },	// -EOPNOTSUPP
	{ 930,	"s0d1_vio_im1" },	// -EOPNOTSUPP
	{ 931,	"s0d2_vio_im1" },	// -EOPNOTSUPP
	{ 932,	"s0d4_vio_im1" },	// -EOPNOTSUPP
	{ 933,	"s0d1_vio_im2" },	// -EOPNOTSUPP
	{ 934,	"s0d2_vio_im2" },	// -EOPNOTSUPP
	{ 935,	"s0d4_vio_im2" },	// -EOPNOTSUPP
	{ 936,	"sgd1_other0" },	// -EOPNOTSUPP
	{ 937,	"sgd2_other0" },	// -EOPNOTSUPP
	{ 938,	"sgd4_other0" },	// -EOPNOTSUPP
	{ 939,	"sgd8_other0" },	// -EOPNOTSUPP
	{ 940,	"sgd16_other0" },	// -EOPNOTSUPP
	{ 943,	"sgd1_other1" },	// -EOPNOTSUPP
	{ 944,	"sgd2_other1" },	// -EOPNOTSUPP
	{ 945,	"sgd4_other1" },	// -EOPNOTSUPP
	{ 946,	"sgd8_other1" },	// -EOPNOTSUPP
	{ 947,	"sgd16_other1" },	// -EOPNOTSUPP
	{ 950,	"sgd1_other2" },	// -EOPNOTSUPP
	{ 951,	"sgd2_other2" },	// -EOPNOTSUPP
	{ 952,	"sgd4_other2" },	// -EOPNOTSUPP
	{ 953,	"sgd8_other2" },	// -EOPNOTSUPP
	{ 954,	"sgd16_other2" },	// -EOPNOTSUPP
	{ 957,	"sgd1_vio_dp_tx" },	// -EOPNOTSUPP
	{ 958,	"sgd2_vio_dp_tx" },	// -EOPNOTSUPP
	{ 959,	"sgd4_vio_dp_tx" },	// -EOPNOTSUPP
	{ 960,	"sgd8_vio_dp_tx" },	// -EOPNOTSUPP
	{ 961,	"sgd16_vio_dp_tx" },	// -EOPNOTSUPP
	{ 964,	"s0d1_vipn_other" },	// -EOPNOTSUPP
	{ 965,	"s0d2_vipn_other" },	// -EOPNOTSUPP
	{ 966,	"s0d4_vipn_other" },	// -EOPNOTSUPP
	{ 967,	"s0d1_vips_other" },	// -EOPNOTSUPP
	{ 968,	"s0d2_vips_other" },	// -EOPNOTSUPP
	{ 969,	"s0d4_vips_other" },	// -EOPNOTSUPP
	{ 970,	"s0d1_imn_other" },	// -EOPNOTSUPP
	{ 971,	"s0d2_imn_other" },	// -EOPNOTSUPP
	{ 972,	"s0d4_imn_other" },	// -EOPNOTSUPP
	{ 973,	"s0d1_imn_core0" },	// -EOPNOTSUPP
	{ 974,	"s0d2_imn_core0" },	// -EOPNOTSUPP
	{ 975,	"s0d4_imn_core0" },	// -EOPNOTSUPP
	{ 976,	"s0d1_imn_core1" },	// -EOPNOTSUPP
	{ 977,	"s0d2_imn_core1" },	// -EOPNOTSUPP
	{ 978,	"s0d4_imn_core1" },	// -EOPNOTSUPP
	{ 979,	"s0d1_ims_other" },	// -EOPNOTSUPP
	{ 980,	"s0d2_ims_other" },	// -EOPNOTSUPP
	{ 981,	"s0d4_ims_other" },	// -EOPNOTSUPP
	{ 982,	"s0d1_ims_core0" },	// -EOPNOTSUPP
	{ 983,	"s0d2_ims_core0" },	// -EOPNOTSUPP
	{ 984,	"s0d4_ims_core0" },	// -EOPNOTSUPP
	{ 985,	"s0d1_ims_core1" },	// -EOPNOTSUPP
	{ 986,	"s0d2_ims_core1" },	// -EOPNOTSUPP
	{ 987,	"s0d4_ims_core1" },	// -EOPNOTSUPP
	{ 988,	"zgd1_gpc_other" },	// -EOPNOTSUPP
	{ 989,	"zgd2_gpc_other" },	// -EOPNOTSUPP
	{ 990,	"zgd4_gpc_other" },	// -EOPNOTSUPP
	{ 991,	"zgd1_gpc_jones0" },	// -EOPNOTSUPP
	{ 992,	"zgd2_gpc_jones0" },	// -EOPNOTSUPP
	{ 993,	"zgd4_gpc_jones0" },	// -EOPNOTSUPP
	{ 994,	"zgd1_gpc_jones1" },	// -EOPNOTSUPP
	{ 995,	"zgd2_gpc_jones1" },	// -EOPNOTSUPP
	{ 996,	"zgd4_gpc_jones1" },	// -EOPNOTSUPP
	{ 997,	"zgd1_gpc_mer0" },	// -EOPNOTSUPP
	{ 998,	"zgd2_gpc_mer0" },	// -EOPNOTSUPP
	{ 999,	"zgd4_gpc_mer0" },	// -EOPNOTSUPP
	{ 1000,	"zgd1_gpc_mer1" },	// -EOPNOTSUPP
	{ 1001,	"zgd2_gpc_mer1" },	// -EOPNOTSUPP
	{ 1002,	"zgd4_gpc_mer1" },	// -EOPNOTSUPP
	{ 1003,	"zgd1_gpc_mer2" },	// -EOPNOTSUPP
	{ 1004,	"zgd2_gpc_mer2" },	// -EOPNOTSUPP
	{ 1005,	"zgd4_gpc_mer2" },	// -EOPNOTSUPP
	{ 1006,	"zgd1_gpc_mer3" },	// -EOPNOTSUPP
	{ 1007,	"zgd2_gpc_mer3" },	// -EOPNOTSUPP
	{ 1008,	"zgd4_gpc_mer3" },	// -EOPNOTSUPP
	{ 1009,	"zgd1_gpc_mer4" },	// -EOPNOTSUPP
	{ 1010,	"zgd2_gpc_mer4" },	// -EOPNOTSUPP
	{ 1011,	"zgd4_gpc_mer4" },	// -EOPNOTSUPP
	{ 1012,	"zgd1_gpc_mer5" },	// -EOPNOTSUPP
	{ 1013,	"zgd2_gpc_mer5" },	// -EOPNOTSUPP
	{ 1014,	"zgd4_gpc_mer5" },	// -EOPNOTSUPP
	{ 1015,	"zgd1_gpc_mer6" },	// -EOPNOTSUPP
	{ 1016,	"zgd2_gpc_mer6" },	// -EOPNOTSUPP
	{ 1017,	"zgd4_gpc_mer6" },	// -EOPNOTSUPP
	{ 1018,	"zgd1_gpc_mer7" },	// -EOPNOTSUPP
	{ 1019,	"zgd2_gpc_mer7" },	// -EOPNOTSUPP
	{ 1020,	"zgd4_gpc_mer7" },	// -EOPNOTSUPP
	{ 1021,	"zgd1_gpc_texas0" },	// -EOPNOTSUPP
	{ 1022,	"zgd2_gpc_texas0" },	// -EOPNOTSUPP
	{ 1023,	"zgd4_gpc_texas0" },	// -EOPNOTSUPP
	{ 1024,	"zgd1_gpc_texas1" },	// -EOPNOTSUPP
	{ 1025,	"zgd2_gpc_texas1" },	// -EOPNOTSUPP
	{ 1026,	"zgd4_gpc_texas1" },	// -EOPNOTSUPP
	{ 1027,	"zgd1_gpc_texas2" },	// -EOPNOTSUPP
	{ 1028,	"zgd2_gpc_texas2" },	// -EOPNOTSUPP
	{ 1029,	"zgd4_gpc_texas2" },	// -EOPNOTSUPP
	{ 1030,	"zgd1_gpc_texas3" },	// -EOPNOTSUPP
	{ 1031,	"zgd2_gpc_texas3" },	// -EOPNOTSUPP
	{ 1032,	"zgd4_gpc_texas3" },	// -EOPNOTSUPP
	{ 1033,	"zgd1_gpc_swift0" },	// -EOPNOTSUPP
	{ 1034,	"zgd2_gpc_swift0" },	// -EOPNOTSUPP
	{ 1035,	"zgd4_gpc_swift0" },	// -EOPNOTSUPP
	{ 1036,	"zgd1_gpc_swift1" },	// -EOPNOTSUPP
	{ 1037,	"zgd2_gpc_swift1" },	// -EOPNOTSUPP
	{ 1038,	"zgd4_gpc_swift1" },	// -EOPNOTSUPP
	{ 1039,	"zgd1_gpc_swift2" },	// -EOPNOTSUPP
	{ 1040,	"zgd2_gpc_swift2" },	// -EOPNOTSUPP
	{ 1041,	"zgd4_gpc_swift2" },	// -EOPNOTSUPP
	{ 1042,	"zgd1_gpc_swift3" },	// -EOPNOTSUPP
	{ 1043,	"zgd2_gpc_swift3" },	// -EOPNOTSUPP
	{ 1044,	"zgd4_gpc_swift3" },	// -EOPNOTSUPP
	{ 1045,	"zgd1_gpc_swift4" },	// -EOPNOTSUPP
	{ 1046,	"zgd2_gpc_swift4" },	// -EOPNOTSUPP
	{ 1047,	"zgd4_gpc_swift4" },	// -EOPNOTSUPP
	{ 1048,	"zgd1_gpc_swift5" },	// -EOPNOTSUPP
	{ 1049,	"zgd2_gpc_swift5" },	// -EOPNOTSUPP
	{ 1050,	"zgd4_gpc_swift5" },	// -EOPNOTSUPP
	{ 1051,	"zgd1_gpc_swift6" },	// -EOPNOTSUPP
	{ 1052,	"zgd2_gpc_swift6" },	// -EOPNOTSUPP
	{ 1053,	"zgd4_gpc_swift6" },	// -EOPNOTSUPP
	{ 1054,	"zgd1_gpc_swift7" },	// -EOPNOTSUPP
	{ 1055,	"zgd2_gpc_swift7" },	// -EOPNOTSUPP
	{ 1056,	"zgd4_gpc_swift7" },	// -EOPNOTSUPP
	{ 1057,	"sgd1_dsp_other" },	// -EOPNOTSUPP
	{ 1058,	"sgd2_dsp_other" },	// -EOPNOTSUPP
	{ 1059,	"sgd4_dsp_other" },	// -EOPNOTSUPP
	{ 1061,	"sgd1_dsp_bus0" },	// -EOPNOTSUPP
	{ 1062,	"sgd2_dsp_bus0" },	// -EOPNOTSUPP
	{ 1063,	"sgd4_dsp_bus0" },	// -EOPNOTSUPP
	{ 1065,	"sgd1_dsp_bus1" },	// -EOPNOTSUPP
	{ 1066,	"sgd2_dsp_bus1" },	// -EOPNOTSUPP
	{ 1067,	"sgd4_dsp_bus1" },	// -EOPNOTSUPP
	{ 1069,	"sgd1_dsp2_vpx0" },	// -EOPNOTSUPP
	{ 1070,	"sgd2_dsp2_vpx0" },	// -EOPNOTSUPP
	{ 1071,	"sgd4_dsp2_vpx0" },	// -EOPNOTSUPP
	{ 1073,	"wdt_dsp2_vpx0" },	// -EOPNOTSUPP
	{ 1074,	"sgd1_dsp2_vpx1" },	// -EOPNOTSUPP
	{ 1075,	"sgd2_dsp2_vpx1" },	// -EOPNOTSUPP
	{ 1076,	"sgd4_dsp2_vpx1" },	// -EOPNOTSUPP
	{ 1078,	"wdt_dsp2_vpx1" },	// -EOPNOTSUPP
	{ 1079,	"sgd1_dsp2_vpx2" },	// -EOPNOTSUPP
	{ 1080,	"sgd2_dsp2_vpx2" },	// -EOPNOTSUPP
	{ 1081,	"sgd4_dsp2_vpx2" },	// -EOPNOTSUPP
	{ 1083,	"wdt_dsp2_vpx2" },	// -EOPNOTSUPP
	{ 1084,	"sgd1_dsp2_vpx3" },	// -EOPNOTSUPP
	{ 1085,	"sgd2_dsp2_vpx3" },	// -EOPNOTSUPP
	{ 1086,	"sgd4_dsp2_vpx3" },	// -EOPNOTSUPP
	{ 1088,	"wdt_dsp2_vpx3" },	// -EOPNOTSUPP
	{ 1089,	"sgd1_dsp3_vpx0" },	// -EOPNOTSUPP
	{ 1090,	"sgd2_dsp3_vpx0" },	// -EOPNOTSUPP
	{ 1091,	"sgd4_dsp3_vpx0" },	// -EOPNOTSUPP
	{ 1093,	"wdt_dsp3_vpx0" },	// -EOPNOTSUPP
	{ 1094,	"sgd1_dsp3_vpx1" },	// -EOPNOTSUPP
	{ 1095,	"sgd2_dsp3_vpx1" },	// -EOPNOTSUPP
	{ 1096,	"sgd4_dsp3_vpx1" },	// -EOPNOTSUPP
	{ 1098,	"wdt_dsp3_vpx1" },	// -EOPNOTSUPP
	{ 1099,	"sgd1_dsp3_vpx2" },	// -EOPNOTSUPP
	{ 1100,	"sgd2_dsp3_vpx2" },	// -EOPNOTSUPP
	{ 1101,	"sgd4_dsp3_vpx2" },	// -EOPNOTSUPP
	{ 1103,	"wdt_dsp3_vpx2" },	// -EOPNOTSUPP
	{ 1104,	"sgd1_dsp3_vpx3" },	// -EOPNOTSUPP
	{ 1105,	"sgd2_dsp3_vpx3" },	// -EOPNOTSUPP
	{ 1106,	"sgd4_dsp3_vpx3" },	// -EOPNOTSUPP
	{ 1108,	"wdt_dsp3_vpx3" },	// -EOPNOTSUPP
	{ 1109,	"sgd1_dsp4_vpx0" },	// -EOPNOTSUPP
	{ 1110,	"sgd2_dsp4_vpx0" },	// -EOPNOTSUPP
	{ 1111,	"sgd4_dsp4_vpx0" },	// -EOPNOTSUPP
	{ 1113,	"wdt_dsp4_vpx0" },	// -EOPNOTSUPP
	{ 1114,	"sgd1_dsp4_vpx1" },	// -EOPNOTSUPP
	{ 1115,	"sgd2_dsp4_vpx1" },	// -EOPNOTSUPP
	{ 1116,	"sgd4_dsp4_vpx1" },	// -EOPNOTSUPP
	{ 1118,	"wdt_dsp4_vpx1" },	// -EOPNOTSUPP
	{ 1119,	"sgd1_dsp4_vpx2" },	// -EOPNOTSUPP
	{ 1120,	"sgd2_dsp4_vpx2" },	// -EOPNOTSUPP
	{ 1121,	"sgd4_dsp4_vpx2" },	// -EOPNOTSUPP
	{ 1123,	"wdt_dsp4_vpx2" },	// -EOPNOTSUPP
	{ 1124,	"sgd1_dsp4_vpx3" },	// -EOPNOTSUPP
	{ 1125,	"sgd2_dsp4_vpx3" },	// -EOPNOTSUPP
	{ 1126,	"sgd4_dsp4_vpx3" },	// -EOPNOTSUPP
	{ 1128,	"wdt_dsp4_vpx3" },	// -EOPNOTSUPP
	{ 1129,	"sgd1_dsp5_vpx0" },	// -EOPNOTSUPP
	{ 1130,	"sgd2_dsp5_vpx0" },	// -EOPNOTSUPP
	{ 1131,	"sgd4_dsp5_vpx0" },	// -EOPNOTSUPP
	{ 1133,	"wdt_dsp5_vpx0" },	// -EOPNOTSUPP
	{ 1134,	"sgd1_dsp5_vpx1" },	// -EOPNOTSUPP
	{ 1135,	"sgd2_dsp5_vpx1" },	// -EOPNOTSUPP
	{ 1136,	"sgd4_dsp5_vpx1" },	// -EOPNOTSUPP
	{ 1138,	"wdt_dsp5_vpx1" },	// -EOPNOTSUPP
	{ 1139,	"sgd1_dsp5_vpx2" },	// -EOPNOTSUPP
	{ 1140,	"sgd2_dsp5_vpx2" },	// -EOPNOTSUPP
	{ 1141,	"sgd4_dsp5_vpx2" },	// -EOPNOTSUPP
	{ 1143,	"wdt_dsp5_vpx2" },	// -EOPNOTSUPP
	{ 1144,	"sgd1_dsp5_vpx3" },	// -EOPNOTSUPP
	{ 1145,	"sgd2_dsp5_vpx3" },	// -EOPNOTSUPP
	{ 1146,	"sgd4_dsp5_vpx3" },	// -EOPNOTSUPP
	{ 1148,	"wdt_dsp5_vpx3" },	// -EOPNOTSUPP
	{ 1149,	"sgd1_dsp6_vpx0" },	// -EOPNOTSUPP
	{ 1150,	"sgd2_dsp6_vpx0" },	// -EOPNOTSUPP
	{ 1151,	"sgd4_dsp6_vpx0" },	// -EOPNOTSUPP
	{ 1153,	"wdt_dsp6_vpx0" },	// -EOPNOTSUPP
	{ 1154,	"sgd1_dsp6_vpx1" },	// -EOPNOTSUPP
	{ 1155,	"sgd2_dsp6_vpx1" },	// -EOPNOTSUPP
	{ 1156,	"sgd4_dsp6_vpx1" },	// -EOPNOTSUPP
	{ 1158,	"wdt_dsp6_vpx1" },	// -EOPNOTSUPP
	{ 1159,	"sgd1_dsp6_vpx2" },	// -EOPNOTSUPP
	{ 1160,	"sgd2_dsp6_vpx2" },	// -EOPNOTSUPP
	{ 1161,	"sgd4_dsp6_vpx2" },	// -EOPNOTSUPP
	{ 1163,	"wdt_dsp6_vpx2" },	// -EOPNOTSUPP
	{ 1164,	"sgd1_dsp6_vpx3" },	// -EOPNOTSUPP
	{ 1165,	"sgd2_dsp6_vpx3" },	// -EOPNOTSUPP
	{ 1166,	"sgd4_dsp6_vpx3" },	// -EOPNOTSUPP
	{ 1168,	"wdt_dsp6_vpx3" },	// -EOPNOTSUPP
	{ 1169,	"sgd1_dsp2_vpxl2" },	// -EOPNOTSUPP
	{ 1170,	"sgd2_dsp2_vpxl2" },	// -EOPNOTSUPP
	{ 1171,	"sgd4_dsp2_vpxl2" },	// -EOPNOTSUPP
	{ 1173,	"wdt_dsp_vpxl2" },	// -EOPNOTSUPP
	{ 1174,	"sgd1_dsp3_vpxl2" },	// -EOPNOTSUPP
	{ 1175,	"sgd2_dsp3_vpxl2" },	// -EOPNOTSUPP
	{ 1176,	"sgd4_dsp3_vpxl2" },	// -EOPNOTSUPP
	{ 1178,	"wdt_dsp3_vpxl2" },	// -EOPNOTSUPP
	{ 1179,	"sgd1_dsp4_vpxl2" },	// -EOPNOTSUPP
	{ 1180,	"sgd2_dsp4_vpxl2" },	// -EOPNOTSUPP
	{ 1181,	"sgd4_dsp4_vpxl2" },	// -EOPNOTSUPP
	{ 1183,	"wdt_dsp4_vpxl2" },	// -EOPNOTSUPP
	{ 1184,	"sgd1_dsp5_vpxl2" },	// -EOPNOTSUPP
	{ 1185,	"sgd2_dsp5_vpxl2" },	// -EOPNOTSUPP
	{ 1186,	"sgd4_dsp5_vpxl2" },	// -EOPNOTSUPP
	{ 1188,	"wdt_dsp5_vpxl2" },	// -EOPNOTSUPP
	{ 1189,	"sgd1_dsp6_vpxl2" },	// -EOPNOTSUPP
	{ 1190,	"sgd2_dsp6_vpxl2" },	// -EOPNOTSUPP
	{ 1191,	"sgd4_dsp6_vpxl2" },	// -EOPNOTSUPP
	{ 1193,	"wdt_dsp6_vpxl2" },	// -EOPNOTSUPP
	{ 1194,	"sgd1_npu0_other" },	// -EOPNOTSUPP
	{ 1195,	"sgd2_npu0_other" },	// -EOPNOTSUPP
	{ 1196,	"sgd4_npu0_other" },	// -EOPNOTSUPP
	{ 1198,	"sgd1_npu0_npul2" },	// -EOPNOTSUPP
	{ 1199,	"sgd2_npu0_npul2" },	// -EOPNOTSUPP
	{ 1200,	"sgd4_npu0_npul2" },	// -EOPNOTSUPP
	{ 1202,	"wdt_npu0_npul2" },	// -EOPNOTSUPP
	{ 1203,	"sgd1_npu0_vpxl2" },	// -EOPNOTSUPP
	{ 1204,	"sgd2_npu0_vpxl2" },	// -EOPNOTSUPP
	{ 1205,	"sgd4_npu0_vpxl2" },	// -EOPNOTSUPP
	{ 1207,	"wdt_npu0_vpx_l2" },	// -EOPNOTSUPP
	{ 1208,	"sgd1_n0_grp0_c0" },	// -EOPNOTSUPP
	{ 1209,	"sgd2_n0_grp0_c0" },	// -EOPNOTSUPP
	{ 1210,	"sgd4_n0_grp0_c0" },	// -EOPNOTSUPP
	{ 1212,	"wdt_n0_grp0_c0" },	// -EOPNOTSUPP
	{ 1213,	"sgd1_n0_grp0_c1" },	// -EOPNOTSUPP
	{ 1214,	"sgd2_n0_grp0_c1" },	// -EOPNOTSUPP
	{ 1215,	"sgd4_n0_grp0_c1" },	// -EOPNOTSUPP
	{ 1217,	"wdt_n0_grp0_c1" },	// -EOPNOTSUPP
	{ 1218,	"sgd1_n0_grp0_c2" },	// -EOPNOTSUPP
	{ 1219,	"sgd2_n0_grp0_c2" },	// -EOPNOTSUPP
	{ 1220,	"sgd4_n0_grp0_c2" },	// -EOPNOTSUPP
	{ 1222,	"wdt_n0_c02" },		// -EOPNOTSUPP
	{ 1223,	"sgd1_n0_grp1_c0" },	// -EOPNOTSUPP
	{ 1224,	"sgd2_n0_grp1_c0" },	// -EOPNOTSUPP
	{ 1225,	"sgd4_n0_grp1_c0" },	// -EOPNOTSUPP
	{ 1227,	"wdt_n0_grp1_c0" },	// -EOPNOTSUPP
	{ 1228,	"sgd1_n0_grp1_c1" },	// -EOPNOTSUPP
	{ 1229,	"sgd2_n0_grp1_c1" },	// -EOPNOTSUPP
	{ 1230,	"sgd4_n0_grp1_c1" },	// -EOPNOTSUPP
	{ 1232,	"wdt_n0_grp1_c1" },	// -EOPNOTSUPP
	{ 1233,	"sgd1_n0_grp1_c2" },	// -EOPNOTSUPP
	{ 1234,	"sgd2_n0_grp1_c2" },	// -EOPNOTSUPP
	{ 1235,	"sgd4_n0_grp1_c2" },	// -EOPNOTSUPP
	{ 1237,	"wdt_n0_grp1_c2" },	// -EOPNOTSUPP
	{ 1238,	"sgd1_n0_grp2_c0" },	// -EOPNOTSUPP
	{ 1239,	"sgd2_n0_grp2_c0" },	// -EOPNOTSUPP
	{ 1240,	"sgd4_n0_grp2_c0" },	// -EOPNOTSUPP
	{ 1242,	"wdt_n0_grp2_c0" },	// -EOPNOTSUPP
	{ 1243,	"sgd1_n0_grp2_c1" },	// -EOPNOTSUPP
	{ 1244,	"sgd2_n0_grp2_c1" },	// -EOPNOTSUPP
	{ 1245,	"sgd4_n0_grp2_c1" },	// -EOPNOTSUPP
	{ 1247,	"wdt_n0_grp2_c1" },	// -EOPNOTSUPP
	{ 1248,	"sgd1_n0_grp2_c2" },	// -EOPNOTSUPP
	{ 1249,	"sgd2_n0_grp2_c2" },	// -EOPNOTSUPP
	{ 1250,	"sgd4_n0_grp2_c2" },	// -EOPNOTSUPP
	{ 1252,	"wdt_n0_grp2_c2" },	// -EOPNOTSUPP
	{ 1253,	"sgd1_n0_grp3_c0" },	// -EOPNOTSUPP
	{ 1254,	"sgd2_n0_grp3_c0" },	// -EOPNOTSUPP
	{ 1255,	"sgd4_n0_grp3_c0" },	// -EOPNOTSUPP
	{ 1257,	"wdt_n0_grp3_c0" },	// -EOPNOTSUPP
	{ 1258,	"sgd1_n0_grp3_c1" },	// -EOPNOTSUPP
	{ 1259,	"sgd2_n0_grp3_c1" },	// -EOPNOTSUPP
	{ 1260,	"sgd4_n0_grp3_c1" },	// -EOPNOTSUPP
	{ 1262,	"wdt_n0_grp3_c1" },	// -EOPNOTSUPP
	{ 1263,	"sgd1_n0_grp3_c2" },	// -EOPNOTSUPP
	{ 1264,	"sgd2_n0_grp3_c2" },	// -EOPNOTSUPP
	{ 1265,	"sgd4_n0_grp3_c2" },	// -EOPNOTSUPP
	{ 1267,	"wdt_n0_grp3_c2" },	// -EOPNOTSUPP
	{ 1268,	"sgd1_n0_vpx_c0" },	// -EOPNOTSUPP
	{ 1269,	"sgd2_n0_vpx_c0" },	// -EOPNOTSUPP
	{ 1270,	"sgd4_n0_vpx_c0" },	// -EOPNOTSUPP
	{ 1272,	"wdt_n0_vpx_c0" },	// -EOPNOTSUPP
	{ 1273,	"sgd1_n0_vpx_c1" },	// -EOPNOTSUPP
	{ 1274,	"sgd2_n0_vpx_c1" },	// -EOPNOTSUPP
	{ 1275,	"sgd4_n0_vpx_c1" },	// -EOPNOTSUPP
	{ 1277,	"wdt_n0_vpx_c1" },	// -EOPNOTSUPP
	{ 1278,	"sgd1_n0_vpx_c2" },	// -EOPNOTSUPP
	{ 1279,	"sgd2_n0_vpx_c2" },	// -EOPNOTSUPP
	{ 1280,	"sgd4_n0_vpx_c2" },	// -EOPNOTSUPP
	{ 1282,	"wdt_n0_vpx_c2" },	// -EOPNOTSUPP
	{ 1283,	"sgd1_n0_vpx_c3" },	// -EOPNOTSUPP
	{ 1284,	"sgd2_n0_vpx_c3" },	// -EOPNOTSUPP
	{ 1285,	"sgd4_n0_vpx_c3" },	// -EOPNOTSUPP
	{ 1287,	"wdt_n0_vpx_c3" },	// -EOPNOTSUPP
	{ 1288,	"sgd1_npu1_other" },	// -EOPNOTSUPP
	{ 1289,	"sgd2_npu1_other" },	// -EOPNOTSUPP
	{ 1290,	"sgd4_npu1_other" },	// -EOPNOTSUPP
	{ 1292,	"sgd1_npu1_npul2" },	// -EOPNOTSUPP
	{ 1293,	"sgd2_npu1_npul2" },	// -EOPNOTSUPP
	{ 1294,	"sgd4_npu1_npul2" },	// -EOPNOTSUPP
	{ 1296,	"wdt_npu1_npul2" },	// -EOPNOTSUPP
	{ 1297,	"sgd1_npu1_vpxl2" },	// -EOPNOTSUPP
	{ 1298,	"sgd2_npu1_vpxl2" },	// -EOPNOTSUPP
	{ 1299,	"sgd4_npu1_vpxl2" },	// -EOPNOTSUPP
	{ 1301,	"wdt_n1_vpx_l2" },	// -EOPNOTSUPP
	{ 1302,	"sgd1_n1_grp0_c0" },	// -EOPNOTSUPP
	{ 1303,	"sgd2_n1_grp0_c0" },	// -EOPNOTSUPP
	{ 1304,	"sgd4_n1_grp0_c0" },	// -EOPNOTSUPP
	{ 1306,	"wdt_n1_grp0_c0" },	// -EOPNOTSUPP
	{ 1307,	"sgd1_n1_grp0_c1" },	// -EOPNOTSUPP
	{ 1308,	"sgd2_n1_grp0_c1" },	// -EOPNOTSUPP
	{ 1309,	"sgd4_n1_grp0_c1" },	// -EOPNOTSUPP
	{ 1311,	"wdt_n1_grp0_c1" },	// -EOPNOTSUPP
	{ 1312,	"sgd1_n1_grp0_c2" },	// -EOPNOTSUPP
	{ 1313,	"sgd2_n1_grp0_c2" },	// -EOPNOTSUPP
	{ 1314,	"sgd4_n1_grp0_c2" },	// -EOPNOTSUPP
	{ 1316,	"wdt_n1_c02" },		// -EOPNOTSUPP
	{ 1317,	"sgd1_n1_grp1_c0" },	// -EOPNOTSUPP
	{ 1318,	"sgd2_n1_grp1_c0" },	// -EOPNOTSUPP
	{ 1319,	"sgd4_n1_grp1_c0" },	// -EOPNOTSUPP
	{ 1321,	"wdt_n1_grp1_c0" },	// -EOPNOTSUPP
	{ 1322,	"sgd1_n1_grp1_c1" },	// -EOPNOTSUPP
	{ 1323,	"sgd2_n1_grp1_c1" },	// -EOPNOTSUPP
	{ 1324,	"sgd4_n1_grp1_c1" },	// -EOPNOTSUPP
	{ 1326,	"wdt_n1_grp1_c1" },	// -EOPNOTSUPP
	{ 1327,	"sgd1_n1_grp1_c2" },	// -EOPNOTSUPP
	{ 1328,	"sgd2_n1_grp1_c2" },	// -EOPNOTSUPP
	{ 1329,	"sgd4_n1_grp1_c2" },	// -EOPNOTSUPP
	{ 1331,	"wdt_n1_grp1_c2" },	// -EOPNOTSUPP
	{ 1332,	"sgd1_n1_grp2_c0" },	// -EOPNOTSUPP
	{ 1333,	"sgd2_n1_grp2_c0" },	// -EOPNOTSUPP
	{ 1334,	"sgd4_n1_grp2_c0" },	// -EOPNOTSUPP
	{ 1336,	"wdt_n1_grp2_c0" },	// -EOPNOTSUPP
	{ 1337,	"sgd1_n1_grp2_c1" },	// -EOPNOTSUPP
	{ 1338,	"sgd2_n1_grp2_c1" },	// -EOPNOTSUPP
	{ 1339,	"sgd4_n1_grp2_c1" },	// -EOPNOTSUPP
	{ 1341,	"wdt_n1_grp2_c1" },	// -EOPNOTSUPP
	{ 1342,	"sgd1_n1_grp2_c2" },	// -EOPNOTSUPP
	{ 1343,	"sgd2_n1_grp2_c2" },	// -EOPNOTSUPP
	{ 1344,	"sgd4_n1_grp2_c2" },	// -EOPNOTSUPP
	{ 1346,	"wdt_n1_grp2_c2" },	// -EOPNOTSUPP
	{ 1347,	"sgd1_n1_grp3_c0" },	// -EOPNOTSUPP
	{ 1348,	"sgd2_n1_grp3_c0" },	// -EOPNOTSUPP
	{ 1349,	"sgd4_n1_grp3_c0" },	// -EOPNOTSUPP
	{ 1351,	"wdt_n1_grp3_c0" },	// -EOPNOTSUPP
	{ 1352,	"sgd1_n1_grp3_c1" },	// -EOPNOTSUPP
	{ 1353,	"sgd2_n1_grp3_c1" },	// -EOPNOTSUPP
	{ 1354,	"sgd4_n1_grp3_c1" },	// -EOPNOTSUPP
	{ 1356,	"wdt_n1_grp3_c1" },	// -EOPNOTSUPP
	{ 1357,	"sgd1_n1_grp3_c2" },	// -EOPNOTSUPP
	{ 1358,	"sgd2_n1_grp3_c2" },	// -EOPNOTSUPP
	{ 1359,	"sgd4_n1_grp3_c2" },	// -EOPNOTSUPP
	{ 1361,	"wdt_n1_grp3_c2" },	// -EOPNOTSUPP
	{ 1362,	"sgd1_n1_vpx_c0" },	// -EOPNOTSUPP
	{ 1363,	"sgd2_n1_vpx_c0" },	// -EOPNOTSUPP
	{ 1364,	"sgd4_n1_vpx_c0" },	// -EOPNOTSUPP
	{ 1366,	"wdt_n1_vpx_c0" },	// -EOPNOTSUPP
	{ 1367,	"sgd1_n1_vpx_c1" },	// -EOPNOTSUPP
	{ 1368,	"sgd2_n1_vpx_c1" },	// -EOPNOTSUPP
	{ 1369,	"sgd4_n1_vpx_c1" },	// -EOPNOTSUPP
	{ 1371,	"wdt_n1_vpx_c1" },	// -EOPNOTSUPP
	{ 1372,	"sgd1_n1_vpx_c2" },	// -EOPNOTSUPP
	{ 1373,	"sgd2_n1_vpx_c2" },	// -EOPNOTSUPP
	{ 1374,	"sgd4_n1_vpx_c2" },	// -EOPNOTSUPP
	{ 1376,	"wdt_n1_vpx_c2" },	// -EOPNOTSUPP
	{ 1377,	"sgd1_n1_vpx_c3" },	// -EOPNOTSUPP
	{ 1378,	"sgd2_n1_vpx_c3" },	// -EOPNOTSUPP
	{ 1379,	"sgd4_n1_vpx_c3" },	// -EOPNOTSUPP
	{ 1381,	"wdt_n1_vpx_c3" },	// -EOPNOTSUPP
	{ 1382,	"zrd6_rt_main" },	// -EOPNOTSUPP
	{ 1383,	"zrd12_rt_main" },	// -EOPNOTSUPP
	{ 1384,	"zrd24_rt_main" },	// -EOPNOTSUPP
	{ 1385,	"zrd48_rt_main" },	// -EOPNOTSUPP
	{ 1386,	"zrd96_rt_main" },	// -EOPNOTSUPP
	{ 1389,	"cl16m_rt_main" },	// -EOPNOTSUPP
	{ 1390,	"sad1_rt_main" },	// -EOPNOTSUPP
	{ 1391,	"sad2_rt_main" },	// -EOPNOTSUPP
	{ 1392,	"sad4_rt_main" },	// -EOPNOTSUPP
	{ 1393,	"zrd6_rt_dmac" },	// -EOPNOTSUPP
	{ 1394,	"zrd12_rt_dmac" },	// -EOPNOTSUPP
	{ 1395,	"zrd24_rt_dmac" },	// -EOPNOTSUPP
	{ 1396,	"zrd48_rt_dmac" },	// -EOPNOTSUPP
	{ 1397,	"zrd96_rt_dmac" },	// -EOPNOTSUPP
	{ 1400,	"cl16m_rt_dmac" },	// -EOPNOTSUPP
	{ 1401,	"zrd6_rt_cr52ss0" },	// -EOPNOTSUPP
	{ 1402,	"zrd12_rt_ss0" },	// -EOPNOTSUPP
	{ 1403,	"zrd24_rt_ss0" },	// -EOPNOTSUPP
	{ 1404,	"zrd48_rt_ss0" },	// -EOPNOTSUPP
	{ 1405,	"zrd96_rt_ss0" },	// -EOPNOTSUPP
	{ 1408,	"zr_rt_cr52ss0" },	// -EOPNOTSUPP
	{ 1409,	"zrd6_rt_cr52ss1" },	// -EOPNOTSUPP
	{ 1410,	"zrd12_rt_ss1" },	// -EOPNOTSUPP
	{ 1411,	"zrd24_rt_ss1" },	// -EOPNOTSUPP
	{ 1412,	"zrd48_rt_ss1" },	// -EOPNOTSUPP
	{ 1413,	"zrd96_rt_ss1" },	// -EOPNOTSUPP
	{ 1416,	"zr_rt_cr52ss1" },	// -EOPNOTSUPP
	{ 1417,	"zrd6_rt_cr52ss2" },	// -EOPNOTSUPP
	{ 1418,	"zrd12_rt_ss2" },	// -EOPNOTSUPP
	{ 1419,	"zrd24_rt_ss2" },	// -EOPNOTSUPP
	{ 1420,	"zrd48_rt_ss2" },	// -EOPNOTSUPP
	{ 1421,	"zrd96_rt_ss2" },	// -EOPNOTSUPP
	{ 1424,	"zr_rt_cr52ss2" },	// -EOPNOTSUPP
	{ 1425,	"zr_rt_c00" },		// -EOPNOTSUPP
	{ 1426,	"zr_rt_c01" },		// -EOPNOTSUPP
	{ 1427,	"zr_rt_c10" },		// -EOPNOTSUPP
	{ 1428,	"zr_rt_c11" },		// -EOPNOTSUPP
	{ 1429,	"zr_rt_c20" },		// -EOPNOTSUPP
	{ 1430,	"zr_rt_c21" },		// -EOPNOTSUPP
	{ 1431,	"zr_rt_shadow00" },	// -EOPNOTSUPP
	{ 1432,	"zr_rt_shadow01" },	// -EOPNOTSUPP
	{ 1433,	"zr_rt_shadow10" },	// -EOPNOTSUPP
	{ 1434,	"zr_rt_shadow11" },	// -EOPNOTSUPP
	{ 1435,	"zr_rt_shadow20" },	// -EOPNOTSUPP
	{ 1436,	"zr_rt_shadow21" },	// -EOPNOTSUPP
	{ 1437,	"zrd6_rt_sec" },	// -EOPNOTSUPP
	{ 1438,	"zrd12_rt_sec" },	// -EOPNOTSUPP
	{ 1439,	"zrd24_rt_sec" },	// -EOPNOTSUPP
	{ 1440,	"zrd48_rt_sec" },	// -EOPNOTSUPP
	{ 1441,	"zrd96_rt_sec" },	// -EOPNOTSUPP
	{ 1442,	"zrd6_rt_mem" },	// -EOPNOTSUPP
	{ 1443,	"zrd12_rt_mem" },	// -EOPNOTSUPP
	{ 1444,	"zrd24_rt_mem" },	// -EOPNOTSUPP
	{ 1445,	"zrd48_rt_mem" },	// -EOPNOTSUPP
	{ 1446,	"zrd96_rt_mem" },	// -EOPNOTSUPP
	{ 1449,	"busd1_scp_main" },	// -EOPNOTSUPP
	{ 1450,	"busd2_scp_main" },	// -EOPNOTSUPP
	{ 1451,	"busd4_scp_main" },	// -EOPNOTSUPP
	{ 1452,	"busd6_scp_main" },	// -EOPNOTSUPP
	{ 1453,	"busd8_scp_main" },	// -EOPNOTSUPP
	{ 1454,	"busd16_scp_main" },	// -EOPNOTSUPP
	{ 1455,	"busd32_scp_main" },	// -EOPNOTSUPP
	{ 1457,	"fray_scp_main" },	// -EOPNOTSUPP
	{ 1460,	"canxl_scp_main" },	// -EOPNOTSUPP
	{ 1461,	"cl16m_scp_main" },	// -EOPNOTSUPP
	{ 1463,	"sgd1_hscs_other" },	// -EOPNOTSUPP
	{ 1464,	"sgd2_hscs_other" },	// -EOPNOTSUPP
	{ 1465,	"sgd4_hscs_other" },	// -EOPNOTSUPP
	{ 1466,	"sgd8_hscs_other" },	// -EOPNOTSUPP
	{ 1467,	"sgd16_hscs_oth" },	// -EOPNOTSUPP
	{ 1468,	"pcick_hscs_oth" },	// -EOPNOTSUPP
	{ 1469,	"sgd1_hscs_pci" },	// -EOPNOTSUPP
	{ 1470,	"sgd2_hscs_pci" },	// -EOPNOTSUPP
	{ 1471,	"sgd4_hscs_pci" },	// -EOPNOTSUPP
	{ 1472,	"sgd8_hscs_pci" },	// -EOPNOTSUPP
	{ 1473,	"sgd16_hscs_pci" },	// -EOPNOTSUPP
	{ 1474,	"pcick_hscs_pc" },	// -EOPNOTSUPP
	{ 1475,	"sgd1_hscs_uci0" },	// -EOPNOTSUPP
	{ 1476,	"sgd12_hscs_uci0" },	// -EOPNOTSUPP
	{ 1477,	"sgd24_hscs_uci0" },	// -EOPNOTSUPP
	{ 1478,	"sgd48_hscs_uci0" },	// -EOPNOTSUPP
	{ 1479,	"sgd96_hscs_uci0" },	// -EOPNOTSUPP
	{ 1480,	"sb_hscs_uci0" },	// -EOPNOTSUPP
	{ 1483,	"ref_hscs_uci0" },	// -EOPNOTSUPP
	{ 1484,	"sgd1_hscs_uci1" },	// -EOPNOTSUPP
	{ 1485,	"sgd12_hscs_uci1" },	// -EOPNOTSUPP
	{ 1486,	"sgd24_hscs_uci1" },	// -EOPNOTSUPP
	{ 1487,	"sgd48_hscs_uci1" },	// -EOPNOTSUPP
	{ 1488,	"sgd96_hscs_uci1" },	// -EOPNOTSUPP
	{ 1489,	"sb_hscs_uci1" },	// -EOPNOTSUPP
	{ 1492,	"ref_hscs_uci1" },	// -EOPNOTSUPP
	{ 1493,	"s0d1_hscn_other" },	// -EOPNOTSUPP
	{ 1494,	"s0d2_hscn_other" },	// -EOPNOTSUPP
	{ 1495,	"s0d4_hscn_other" },	// -EOPNOTSUPP
	{ 1496,	"s0d8_hscn_other" },	// -EOPNOTSUPP
	{ 1497,	"s0d12_hscn_oth" },	// -EOPNOTSUPP
	{ 1498,	"s0d16_hscn_oth" },	// -EOPNOTSUPP
	{ 1499,	"s0d24_hscn_oth" },	// -EOPNOTSUPP
	{ 1502,	"cl16m_hscn_oth" },	// -EOPNOTSUPP
	{ 1503,	"s0d1_hscn_pci4" },	// -EOPNOTSUPP
	{ 1504,	"s0d2_hscn_pci4" },	// -EOPNOTSUPP
	{ 1505,	"s0d4_hscn_pci4" },	// -EOPNOTSUPP
	{ 1506,	"s0d8_hscn_pci4" },	// -EOPNOTSUPP
	{ 1507,	"s0d12_hscn_pci4" },	// -EOPNOTSUPP
	{ 1508,	"s0d16_hscn_pci4" },	// -EOPNOTSUPP
	{ 1509,	"s0d24_hscn_pci4" },	// -EOPNOTSUPP
	{ 1512,	"cl16m_hscn_pci4" },	// -EOPNOTSUPP
	{ 1513,	"s0d1_hscn_usb" },	// -EOPNOTSUPP
	{ 1514,	"s0d2_hscn_usb" },	// -EOPNOTSUPP
	{ 1515,	"s0d4_hscn_usb" },	// -EOPNOTSUPP
	{ 1516,	"s0d8_hscn_usb" },	// -EOPNOTSUPP
	{ 1517,	"s0d12_hscn_usb" },	// -EOPNOTSUPP
	{ 1518,	"s0d16_hscn_usb" },	// -EOPNOTSUPP
	{ 1519,	"s0d24_hscn_usb" },	// -EOPNOTSUPP
	{ 1522,	"cl16m_hscn_usb" },	// -EOPNOTSUPP
	{ 1523,	"s0d1_rsw3_mfwd" },	// -EOPNOTSUPP
	{ 1524,	"s0d2_rsw3_mfwd" },	// -EOPNOTSUPP
	{ 1525,	"s0d4_rsw3_mfwd" },	// -EOPNOTSUPP
	{ 1526,	"s0d8_rsw3_mfwd" },	// -EOPNOTSUPP
	{ 1527,	"s0d12_rsw3_mfwd" },	// -EOPNOTSUPP
	{ 1528,	"s0d16_rsw3_mfwd" },	// -EOPNOTSUPP
	{ 1529,	"s0d24_rsw3_mfwd" },	// -EOPNOTSUPP
	{ 1533,	"s0d1_rsw3_main" },	// -EOPNOTSUPP
	{ 1534,	"s0d2_rsw3_main" },	// -EOPNOTSUPP
	{ 1535,	"s0d4_rsw3_main" },	// -EOPNOTSUPP
	{ 1536,	"s0d8_rsw3_main" },	// -EOPNOTSUPP
	{ 1537,	"s0d12_rsw3_main" },	// -EOPNOTSUPP
	{ 1538,	"s0d16_rsw3_main" },	// -EOPNOTSUPP
	{ 1539,	"s0d24_rsw3_main" },	// -EOPNOTSUPP
	{ 1543,	"s0d1_rsw3_aes" },	// -EOPNOTSUPP
	{ 1544,	"s0d2_rsw3_aes" },	// -EOPNOTSUPP
	{ 1545,	"s0d4_rsw3_aes" },	// -EOPNOTSUPP
	{ 1546,	"s0d8_rsw3_aes" },	// -EOPNOTSUPP
	{ 1547,	"s0d12_rsw3_aes" },	// -EOPNOTSUPP
	{ 1548,	"s0d16_rsw3_aes" },	// -EOPNOTSUPP
	{ 1549,	"s0d24_rsw3_aes" },	// -EOPNOTSUPP
	{ 1553,	"s0d1_rsw3_tsn" },	// -EOPNOTSUPP
	{ 1554,	"s0d2_rsw3_tsn" },	// -EOPNOTSUPP
	{ 1555,	"s0d4_rsw3_tsn" },	// -EOPNOTSUPP
	{ 1556,	"s0d8_rsw3_tsn" },	// -EOPNOTSUPP
	{ 1557,	"s0d12_rsw3_tsn" },	// -EOPNOTSUPP
	{ 1558,	"s0d16_rsw3_tsn" },	// -EOPNOTSUPP
	{ 1559,	"s0d24_rsw3_tsn" },	// -EOPNOTSUPP
	{ 1563,	"s0d1_mm_bus" },	// -EOPNOTSUPP
	{ 1564,	"s0d2_mm_bus" },	// -EOPNOTSUPP
	{ 1565,	"s0d4_mm_bus" },	// -EOPNOTSUPP
	{ 1566,	"s0d1_mm_iniu" },	// -EOPNOTSUPP
	{ 1567,	"s0d2_mm_iniu" },	// -EOPNOTSUPP
	{ 1568,	"s0d4_mm_iniu" },	// -EOPNOTSUPP
	{ 1569,	"s0d1_mm_axcidb" },	// -EOPNOTSUPP
	{ 1570,	"s0d2_mm_axcidb" },	// -EOPNOTSUPP
	{ 1571,	"s0d4_mm_axcidb" },	// -EOPNOTSUPP
	{ 1572,	"s0d1_mm_tniu0" },	// -EOPNOTSUPP
	{ 1573,	"s0d2_mm_tniu0" },	// -EOPNOTSUPP
	{ 1574,	"s0d4_mm_tniu0" },	// -EOPNOTSUPP
	{ 1575,	"s0d1_mm_tniu1" },	// -EOPNOTSUPP
	{ 1576,	"s0d2_mm_tniu1" },	// -EOPNOTSUPP
	{ 1577,	"s0d4_mm_tniu1" },	// -EOPNOTSUPP
	{ 1578,	"s0d1_mm_other" },	// -EOPNOTSUPP
	{ 1579,	"s0d2_mm_other" },	// -EOPNOTSUPP
	{ 1580,	"s0d4_mm_other" },	// -EOPNOTSUPP
	{ 1581,	"s0d1_mm_dbsc0" },	// -EOPNOTSUPP
	{ 1582,	"s0d2_mm_dbsc0" },	// -EOPNOTSUPP
	{ 1583,	"s0d4_mm_dbsc0" },	// -EOPNOTSUPP
	{ 1585,	"s0d1_mm_dbsc1" },	// -EOPNOTSUPP
	{ 1586,	"s0d2_mm_dbsc1" },	// -EOPNOTSUPP
	{ 1587,	"s0d4_mm_dbsc1" },	// -EOPNOTSUPP
	{ 1589,	"s0d1_mm_dbsc2" },	// -EOPNOTSUPP
	{ 1590,	"s0d2_mm_dbsc2" },	// -EOPNOTSUPP
	{ 1591,	"s0d4_mm_dbsc2" },	// -EOPNOTSUPP
	{ 1593,	"s0d1_mm_dbsc3" },	// -EOPNOTSUPP
	{ 1594,	"s0d2_mm_dbsc3" },	// -EOPNOTSUPP
	{ 1595,	"s0d4_mm_dbsc3" },	// -EOPNOTSUPP
	{ 1597,	"s0d1_mm_dbsc4" },	// -EOPNOTSUPP
	{ 1598,	"s0d2_mm_dbsc4" },	// -EOPNOTSUPP
	{ 1599,	"s0d4_mm_dbsc4" },	// -EOPNOTSUPP
	{ 1601,	"s0d1_mm_dbsc5" },	// -EOPNOTSUPP
	{ 1602,	"s0d2_mm_dbsc5" },	// -EOPNOTSUPP
	{ 1603,	"s0d4_mm_dbsc5" },	// -EOPNOTSUPP
	{ 1605,	"s0d1_mm_dbsc6" },	// -EOPNOTSUPP
	{ 1606,	"s0d2_mm_dbsc6" },	// -EOPNOTSUPP
	{ 1607,	"s0d4_mm_dbsc6" },	// -EOPNOTSUPP
	{ 1609,	"s0d1_mm_dbsc7" },	// -EOPNOTSUPP
	{ 1610,	"s0d2_mm_dbsc7" },	// -EOPNOTSUPP
	{ 1611,	"s0d4_mm_dbsc7" },	// -EOPNOTSUPP
	{ 1613,	"s0d1_ddr0_main" },	// -EOPNOTSUPP
	{ 1614,	"s0d2_ddr0_main" },	// -EOPNOTSUPP
	{ 1615,	"s0d4_ddr0_main" },	// -EOPNOTSUPP
	{ 1617,	"s0d1_ddr1_main" },	// -EOPNOTSUPP
	{ 1618,	"s0d2_ddr1_main" },	// -EOPNOTSUPP
	{ 1619,	"s0d4_ddr1_main" },	// -EOPNOTSUPP
	{ 1621,	"s0d1_ddr2_main" },	// -EOPNOTSUPP
	{ 1622,	"s0d2_ddr2_main" },	// -EOPNOTSUPP
	{ 1623,	"s0d4_ddr2_main" },	// -EOPNOTSUPP
	{ 1625,	"s0d1_ddr3_main" },	// -EOPNOTSUPP
	{ 1626,	"s0d2_ddr3_main" },	// -EOPNOTSUPP
	{ 1627,	"s0d4_ddr3_main" },	// -EOPNOTSUPP
	{ 1629,	"s0d1_ddr4_main" },	// -EOPNOTSUPP
	{ 1630,	"s0d2_ddr4_main" },	// -EOPNOTSUPP
	{ 1631,	"s0d4_ddr4_main" },	// -EOPNOTSUPP
	{ 1633,	"s0d1_ddr5_main" },	// -EOPNOTSUPP
	{ 1634,	"s0d2_ddr5_main" },	// -EOPNOTSUPP
	{ 1635,	"s0d4_ddr5_main" },	// -EOPNOTSUPP
	{ 1637,	"s0d1_ddr6_main" },	// -EOPNOTSUPP
	{ 1638,	"s0d2_ddr6_main" },	// -EOPNOTSUPP
	{ 1639,	"s0d4_ddr6_main" },	// -EOPNOTSUPP
	{ 1641,	"s0d1_ddr7_main" },	// -EOPNOTSUPP
	{ 1642,	"s0d2_ddr7_main" },	// -EOPNOTSUPP
	{ 1643,	"s0d4_ddr7_main" },	// -EOPNOTSUPP
	{ 1646,	"sgd8_perw_main" },	// -EOPNOTSUPP
	{ 1647,	"sgd16_perw_main" },	// -EOPNOTSUPP
	{ 1648,	"sgd32_perw_main" },	// -EOPNOTSUPP
	{ 1649,	"sgad4_perw_m" },	// -EOPNOTSUPP
	{ 1650,	"sgad8_perw_m" },	// -EOPNOTSUPP
	{ 1651,	"sgad16_perw_m" },	// -EOPNOTSUPP
	{ 1652,	"sgad32_perw_m" },	// -EOPNOTSUPP
	{ 1653,	"s0ad8_perw_m" },	// -EOPNOTSUPP
	{ 1654,	"cl16m_perw_main" },	// -EOPNOTSUPP
	{ 1656,	"sa_i3c_perw_m" },	// -EOPNOTSUPP
	{ 1658,	"sgd8_perw_bus" },	// -EOPNOTSUPP
	{ 1660,	"sgd32_perw_bus" },	// -EOPNOTSUPP
	{ 1661,	"sgad4_perw_bus" },	// -EOPNOTSUPP
	{ 1662,	"sgad8_perw_bus" },	// -EOPNOTSUPP
	{ 1663,	"sgad16_perw_bus" },	// -EOPNOTSUPP
	{ 1664,	"sgad32_perw_bus" },	// -EOPNOTSUPP
	{ 1665,	"s0ad8_perw_bus" },	// -EOPNOTSUPP
	{ 1666,	"cl16m_perw_bus" },	// -EOPNOTSUPP
	{ 1668,	"sa_i3c_perw_bus" },	// -EOPNOTSUPP
	{ 1670,	"sgd8_mp_main" },	// -EOPNOTSUPP
	{ 1671,	"sgd16_mp_main" },	// -EOPNOTSUPP
	{ 1672,	"sgd32_mp_main" },	// -EOPNOTSUPP
	{ 1673,	"cl16m_mp_main" },	// -EOPNOTSUPP
	{ 1674,	"adghd1_mp_main" },	// -EOPNOTSUPP
	{ 1675,	"adghd4_mp_main" },	// -EOPNOTSUPP
	{ 1677,	"sgd8_mp_bus" },	// -EOPNOTSUPP
	{ 1678,	"sgd16_mp_bus" },	// -EOPNOTSUPP
	{ 1679,	"sgd32_mp_bus" },	// -EOPNOTSUPP
	{ 1680,	"cl16m_mp_bus" },	// -EOPNOTSUPP
	{ 1681,	"adghd1ck_mp_bus" },	// -EOPNOTSUPP
	{ 1682,	"adghd4ck_mp_bus" },	// -EOPNOTSUPP
	{ 1683,	"s0d1_pere_main" },	// -EOPNOTSUPP
	{ 1684,	"s0d2_pere_main" },	// -EOPNOTSUPP
	{ 1685,	"s0d3_pere_main" },	// -EOPNOTSUPP
	{ 1687,	"s0d6_pere_main" },	// -EOPNOTSUPP
	{ 1688,	"s0d8_pere_main" },	// -EOPNOTSUPP
	{ 1689,	"s0d12_pere_main" },	// -EOPNOTSUPP
	{ 1690,	"s0d24_pere_main" },	// -EOPNOTSUPP
	{ 1695,	"cl16m_pere_main" },	// -EOPNOTSUPP
	{ 1696,	"ufs_pere_main" },	// -EOPNOTSUPP
#endif
};

static void quirk_rcar_x5h_no_attributes_fixup(const struct quirk_rcar_x5h_no_attributes *table,
		unsigned int len, u32 id, struct scmi_clock_info *clk, u32 *attributes, int *ret)
{
	for (unsigned int i = 0; i < len; i++)
		if (id == table[i].id) {
			strscpy(clk->name, table[i].name);
			*attributes = ATTRIBUTES_ENABLED;
			clk->state_ctrl_forbidden = true;
			clk->rate_ctrl_forbidden = true;
			clk->parent_ctrl_forbidden = true;
			*ret = 0;
			break;
		}
}

#define QUIRK_RCAR_X5H_4_28_NO_ATTRIBUTES						\
	({										\
		quirk_rcar_x5h_no_attributes_fixup(quirk_rcar_x5h_4_28_no_attributes,	\
			ARRAY_SIZE(quirk_rcar_x5h_4_28_no_attributes), clk_id, clk,	\
			&attributes, &ret);						\
	})

#define QUIRK_RCAR_X5H_4_31_NO_ATTRIBUTES						\
	({										\
		quirk_rcar_x5h_no_attributes_fixup(quirk_rcar_x5h_4_31_no_attributes,	\
			ARRAY_SIZE(quirk_rcar_x5h_4_31_no_attributes), clk_id, clk,	\
			&attributes, &ret);						\
	})

#define QUIRK_RCAR_X5H_4_28_PM_CLK					\
	({								\
		if (clk_id <= 818 /* Last MDLC clock MDLC_GPIODM3 */)	\
			clk->pm_clk = true;				\
	})

#define QUIRK_RCAR_X5H_4_31_PM_CLK					\
	({								\
		if (clk_id <= 814 /* Last MDLC clock MDLC_GPIODM3 */)	\
			clk->pm_clk = true;				\
	})

static int scmi_clock_attributes_get(const struct scmi_protocol_handle *ph,
				     u32 clk_id, struct clock_info *cinfo)
{
	int ret;
	u32 attributes;
	struct scmi_xfer *t;
	struct scmi_msg_resp_clock_attributes *attr;
	struct scmi_clock_info *clk = CLOCK_INFO(cinfo, clk_id);

	ret = ph->xops->xfer_get_init(ph, CLOCK_ATTRIBUTES,
				      sizeof(clk_id), sizeof(*attr), &t);
	if (ret)
		return ret;

	put_unaligned_le32(clk_id, t->tx.buf);
	attr = t->rx.buf;

	ret = ph->xops->do_xfer(ph, t);
	if (!ret) {
		u32 latency = 0;

		attributes = le32_to_cpu(attr->attributes);
		strscpy(clk->name, attr->name, SCMI_SHORT_NAME_MAX_SIZE);
		/* clock_enable_latency field is present only since SCMI v3.1 */
		if (PROTOCOL_REV_MAJOR(ph->version) >= 0x2)
			latency = le32_to_cpu(attr->clock_enable_latency);
		clk->enable_latency = latency ? : U32_MAX;
	} else {
		SCMI_QUIRK(clock_rcar_x5h_4_28, QUIRK_RCAR_X5H_4_28_NO_ATTRIBUTES);
		SCMI_QUIRK(clock_rcar_x5h_4_31, QUIRK_RCAR_X5H_4_31_NO_ATTRIBUTES);
	}

	ph->xops->xfer_put(ph, t);

	/*
	 * If supported overwrite short name with the extended one;
	 * on error just carry on and use already provided short name.
	 */
	if (!ret && PROTOCOL_REV_MAJOR(ph->version) >= 0x2) {
		if (SUPPORTS_EXTENDED_NAMES(attributes))
			ph->hops->extended_name_get(ph, CLOCK_NAME_GET, clk_id,
						    NULL, clk->name,
						    SCMI_MAX_STR_SIZE);

		if (cinfo->notify_rate_changed_cmd &&
		    SUPPORTS_RATE_CHANGED_NOTIF(attributes))
			clk->rate_changed_notifications = true;
		if (cinfo->notify_rate_change_requested_cmd &&
		    SUPPORTS_RATE_CHANGE_REQUESTED_NOTIF(attributes))
			clk->rate_change_requested_notifications = true;
		if (PROTOCOL_REV_MAJOR(ph->version) >= 0x3) {
			if (SUPPORTS_PARENT_CLOCK(attributes))
				scmi_clock_possible_parents(ph, clk_id, cinfo);
			if (SUPPORTS_GET_PERMISSIONS(attributes))
				scmi_clock_get_permissions(ph, clk_id, clk);
			SCMI_QUIRK(clock_rcar_x5h_4_28, QUIRK_RCAR_X5H_4_28_CRIT_CLOCKS);
			SCMI_QUIRK(clock_rcar_x5h_4_31, QUIRK_RCAR_X5H_4_31_CRIT_CLOCKS);
			if (SUPPORTS_EXTENDED_CONFIG(attributes))
				clk->extended_config = true;
		}

		SCMI_QUIRK(clock_rcar_x5h_4_28, QUIRK_RCAR_X5H_4_28_PM_CLK);
		SCMI_QUIRK(clock_rcar_x5h_4_31, QUIRK_RCAR_X5H_4_31_PM_CLK);
	}

	return ret;
}

static int rate_cmp_func(const void *_r1, const void *_r2)
{
	const u64 *r1 = _r1, *r2 = _r2;

	if (*r1 < *r2)
		return -1;
	else if (*r1 == *r2)
		return 0;
	else
		return 1;
}

static void iter_clk_describe_prepare_message(void *message,
					      const unsigned int desc_index,
					      const void *priv)
{
	struct scmi_msg_clock_describe_rates *msg = message;
	const struct scmi_clk_ipriv *p = priv;

	msg->id = cpu_to_le32(p->id);
	/* Set the number of rates to be skipped/already read */
	msg->rate_index = cpu_to_le32(desc_index);
}

#define QUIRK_OUT_OF_SPEC_TRIPLET					       \
	({								       \
		/*							       \
		 * A known quirk: a triplet is returned but num_returned != 3  \
		 * Check for a safe payload size and fix.		       \
		 */							       \
		if (st->num_returned != 3 && st->num_remaining == 0 &&	       \
		    st->rx_len == sizeof(*r) + sizeof(__le32) * 2 * 3) {       \
			st->num_returned = 3;				       \
			st->num_remaining = 0;				       \
		} else {						       \
			dev_err(p->dev,					       \
				"Cannot fix out-of-spec reply !\n");	       \
			return -EPROTO;					       \
		}							       \
	})

static int
iter_clk_describe_update_state(struct scmi_iterator_state *st,
			       const void *response, void *priv)
{
	u32 flags;
	struct scmi_clk_ipriv *p = priv;
	const struct scmi_msg_resp_clock_describe_rates *r = response;

	flags = le32_to_cpu(r->num_rates_flags);
	st->num_remaining = NUM_REMAINING(flags);
	st->num_returned = NUM_RETURNED(flags);
	p->clkd->r.rate_discrete = RATE_DISCRETE(flags);

	/* Warn about out of spec replies ... */
	if (!p->clkd->r.rate_discrete &&
	    (st->num_returned != 3 || st->num_remaining != 0)) {
		dev_warn(p->dev,
			 "Out-of-spec CLOCK_DESCRIBE_RATES reply for %s - returned:%d remaining:%d rx_len:%zd\n",
			 p->clkd->info.name, st->num_returned, st->num_remaining,
			 st->rx_len);

		SCMI_QUIRK(clock_rates_triplet_out_of_spec,
			   QUIRK_OUT_OF_SPEC_TRIPLET);
	}

	if (!st->max_resources) {
		unsigned int tot_rates = st->num_returned + st->num_remaining;

		p->clkd->r.rates = devm_kcalloc(p->dev, tot_rates,
						sizeof(*p->clkd->r.rates), GFP_KERNEL);
		if (!p->clkd->r.rates)
			return -ENOMEM;

		/* max_resources is used by the iterators to control bounds */
		p->clkd->tot_rates = tot_rates;
		st->max_resources = tot_rates;
	}

	return 0;
}

static int
iter_clk_describe_process_response(const struct scmi_protocol_handle *ph,
				   const void *response,
				   struct scmi_iterator_state *st, void *priv)
{
	struct scmi_clk_ipriv *p = priv;
	const struct scmi_msg_resp_clock_describe_rates *r = response;

	p->clkd->r.rates[p->clkd->r.num_rates] = RATE_TO_U64(r->rate[st->loop_idx]);

	/* Count only effectively discovered rates */
	p->clkd->r.num_rates++;

	return 0;
}

static int
scmi_clock_describe_rates_get_full(const struct scmi_protocol_handle *ph, u32 clk_id,
				   struct scmi_clock_desc *clkd)
{
	int ret;
	void *iter;
	struct scmi_iterator_ops ops = {
		.prepare_message = iter_clk_describe_prepare_message,
		.update_state = iter_clk_describe_update_state,
		.process_response = iter_clk_describe_process_response,
	};
	struct scmi_clk_ipriv cpriv = {
		.id = clk_id,
		.clkd = clkd,
		.dev = ph->dev,
	};

	/*
	 * Using tot_rates as max_resources parameter here so as to trigger
	 * the dynamic allocation only when strictly needed: when trying a
	 * full enumeration after a lazy one tot_rates will be non-zero.
	 */
	iter = ph->hops->iter_response_init(ph, &ops, clkd->tot_rates,
					    CLOCK_DESCRIBE_RATES,
					    sizeof(struct scmi_msg_clock_describe_rates),
					    &cpriv);
	if (IS_ERR(iter))
		return PTR_ERR(iter);

	ret = ph->hops->iter_response_run(iter);
	if (ret)
		return ret;

	/* empty set ? */
	if (!clkd->r.num_rates)
		return 0;

	if (clkd->r.rate_discrete && PROTOCOL_REV_MAJOR(ph->version) == 0x1)
		sort(clkd->r.rates, clkd->r.num_rates,
		     sizeof(clkd->r.rates[0]), rate_cmp_func, NULL);

	return 0;
}

static int
scmi_clock_describe_rates_get_lazy(const struct scmi_protocol_handle *ph, u32 clk_id,
				   struct scmi_clock_desc *clkd)
{
	struct scmi_iterator_ops ops = {
		.prepare_message = iter_clk_describe_prepare_message,
		.update_state = iter_clk_describe_update_state,
		.process_response = iter_clk_describe_process_response,
	};
	struct scmi_clk_ipriv cpriv = {
		.id = clk_id,
		.clkd = clkd,
		.dev = ph->dev,
	};
	unsigned int first, last;
	void *iter;
	int ret;

	iter = ph->hops->iter_response_init(ph, &ops, 0, CLOCK_DESCRIBE_RATES,
					    sizeof(struct scmi_msg_clock_describe_rates),
					    &cpriv);
	if (IS_ERR(iter))
		return PTR_ERR(iter);

	/* Try to grab a triplet, so that in case is NON-discrete we are done */
	first = 0;
	last = 2;
	ret = ph->hops->iter_response_run_bound(iter, &first, &last);
	if (ret)
		goto out;

	/*
	 * If discrete and we don't already have it, grab the last value, which
	 * should be the max
	 */
	if (clkd->r.rate_discrete && clkd->tot_rates > clkd->r.num_rates) {
		first = clkd->tot_rates - 1;
		last = clkd->tot_rates - 1;
		ret = ph->hops->iter_response_run_bound(iter, &first, &last);
	}

out:
	ph->hops->iter_response_bound_cleanup(iter);

	return ret;
}

struct quirk_rcar_x5h_parent_rate {
	u32 id;
	u32 parent;
};

static const struct quirk_rcar_x5h_parent_rate quirk_rcar_x5h_4_28_parent_rates[] = {
	// FIXME Add more entries
	{ 202 /* MDLC_UFS0 */,		1690 /*	CLK_S0D4_PERE_MAIN */ },
	{ 203 /* MDLC_UFS1 */,		1690 /*	CLK_S0D4_PERE_MAIN */ },
	{ 209 /* MDLC_SCIF0 */,		1663 /* CLK_SGD16_PERW_BUS */ },
	{ 210 /* MDLC_SCIF1 */,		1663 /* CLK_SGD16_PERW_BUS */ },
	{ 211 /* MDLC_SCIF2 */,		1663 /* CLK_SGD16_PERW_BUS */ },
	{ 212 /* MDLC_SCIF3 */,		1663 /* CLK_SGD16_PERW_BUS */ },
	{ 228 /* MDLC_HSCIF0 */,	1661 /* CLK_SGD4_PERW_BUS */ },
	{ 229 /* MDLC_HSCIF1 */,	1661 /* CLK_SGD4_PERW_BUS */ },
	{ 230 /* MDLC_HSCIF2 */,	1661 /* CLK_SGD4_PERW_BUS */ },
	{ 231 /* MDLC_HSCIF3 */,	1661 /* CLK_SGD4_PERW_BUS */ },
};

static const struct quirk_rcar_x5h_parent_rate quirk_rcar_x5h_4_31_parent_rates[] = {
	// FIXME Add more entries
	{ 198 /* MDLC_UFS0 */,		1686 /*	CLK_S0D4_PERE_MAIN */ },
	{ 199 /* MDLC_UFS1 */,		1686 /*	CLK_S0D4_PERE_MAIN */ },
	{ 205 /* MDLC_SCIF0 */,		1659 /* CLK_SGD16_PERW_BUS */ },
	{ 206 /* MDLC_SCIF1 */,		1659 /* CLK_SGD16_PERW_BUS */ },
	{ 207 /* MDLC_SCIF2 */,		1659 /* CLK_SGD16_PERW_BUS */ },
	{ 208 /* MDLC_SCIF3 */,		1659 /* CLK_SGD16_PERW_BUS */ },
	{ 224 /* MDLC_HSCIF0 */,	1657 /* CLK_SGD4_PERW_BUS */ },
	{ 225 /* MDLC_HSCIF1 */,	1657 /* CLK_SGD4_PERW_BUS */ },
	{ 226 /* MDLC_HSCIF2 */,	1657 /* CLK_SGD4_PERW_BUS */ },
	{ 227 /* MDLC_HSCIF3 */,	1657 /* CLK_SGD4_PERW_BUS */ },
};

static void quirk_rcar_x5h_parent_rate_fixup(const struct quirk_rcar_x5h_parent_rate *table,
					     unsigned int len, u32 *id)
{
	for (unsigned int i = 0; i < len; i++)
		if (*id == table[i].id) {
			*id = table[i].parent;
			break;
		}
}

#define QUIRK_RCAR_X5H_4_28_PARENT_RATES					   \
	({									   \
		quirk_rcar_x5h_parent_rate_fixup(quirk_rcar_x5h_4_28_parent_rates, \
			ARRAY_SIZE(quirk_rcar_x5h_4_28_parent_rates), &clk_id);	   \
	})

#define QUIRK_RCAR_X5H_4_31_PARENT_RATES					   \
	({									   \
		quirk_rcar_x5h_parent_rate_fixup(quirk_rcar_x5h_4_31_parent_rates, \
			ARRAY_SIZE(quirk_rcar_x5h_4_31_parent_rates), &clk_id);	   \
	})

#define QUIRK_RCAR_X5H_4_28_WRONG_RATES(rate)				\
	({								\
		switch (clk_id) {					\
		case 1649: /* CLK_SGD4_PERW_MAIN */			\
		case 1661: /* CLK_SGD4_PERW_BUS */			\
		case 1673: /* CLK_SGD4_MP_MAIN */			\
		case 1680: /* CLK_SGD4_MP_BUS */			\
			rate /= 2;					\
			break;						\
		}							\
	})

#define QUIRK_RCAR_X5H_4_31_WRONG_RATES(rate)				\
	({								\
		switch (clk_id) {					\
		case 1645: /* CLK_SGD4_PERW_MAIN */			\
		case 1657: /* CLK_SGD4_PERW_BUS */			\
		case 1669: /* CLK_SGD4_MP_MAIN */			\
		case 1676: /* CLK_SGD4_MP_BUS */			\
			rate /= 2;					\
			break;						\
		}							\
	})

static int
scmi_clock_describe_rates_get(const struct scmi_protocol_handle *ph,
			      u32 clk_id, struct clock_info *cinfo)
{
	struct scmi_clock_desc *clkd = &cinfo->clkds[clk_id];
	int ret;

	SCMI_QUIRK(clock_rcar_x5h_4_28, QUIRK_RCAR_X5H_4_28_PARENT_RATES);
	SCMI_QUIRK(clock_rcar_x5h_4_31, QUIRK_RCAR_X5H_4_31_PARENT_RATES);

	/*
	 * Since only after SCMI Clock v1.0 the returned rates are guaranteed to
	 * be discovered in ascending order, lazy enumeration cannot be use for
	 * SCMI Clock v1.0 protocol.
	 */
	if (PROTOCOL_REV_MAJOR(ph->version) > 0x1)
		ret = scmi_clock_describe_rates_get_lazy(ph, clk_id, clkd);
	else
		ret = scmi_clock_describe_rates_get_full(ph, clk_id, clkd);

	if (ret)
		return ret;

	SCMI_QUIRK(clock_rcar_x5h_4_28, QUIRK_RCAR_X5H_4_28_WRONG_RATES(clkd->r.rates[0]));
	SCMI_QUIRK(clock_rcar_x5h_4_31, QUIRK_RCAR_X5H_4_31_WRONG_RATES(clkd->r.rates[0]));

	clkd->info.min_rate = clkd->r.rates[RATE_MIN];
	if (!clkd->r.rate_discrete) {
		clkd->info.max_rate = clkd->r.rates[RATE_MAX];
		dev_dbg(ph->dev, "Min %llu Max %llu Step %llu Hz\n",
			clkd->r.rates[RATE_MIN], clkd->r.rates[RATE_MAX],
			clkd->r.rates[RATE_STEP]);
	} else {
		clkd->info.max_rate = clkd->r.rates[clkd->r.num_rates - 1];
		dev_dbg(ph->dev, "Clock:%s Num_Rates:%u -> Min %llu Max %llu\n",
			clkd->info.name, clkd->tot_rates,
			clkd->info.min_rate, clkd->info.max_rate);
	}

	return 0;
}

static int
scmi_clock_rate_get(const struct scmi_protocol_handle *ph,
		    u32 clk_id, u64 *value)
{
	int ret;
	struct scmi_xfer *t;

	ret = ph->xops->xfer_get_init(ph, CLOCK_RATE_GET,
				      sizeof(__le32), sizeof(u64), &t);
	if (ret)
		return ret;

	SCMI_QUIRK(clock_rcar_x5h_4_28, QUIRK_RCAR_X5H_4_28_PARENT_RATES);
	SCMI_QUIRK(clock_rcar_x5h_4_31, QUIRK_RCAR_X5H_4_31_PARENT_RATES);

	put_unaligned_le32(clk_id, t->tx.buf);

	ret = ph->xops->do_xfer(ph, t);
	if (!ret) {
		*value = get_unaligned_le64(t->rx.buf);

		SCMI_QUIRK(clock_rcar_x5h_4_28, QUIRK_RCAR_X5H_4_28_WRONG_RATES(*value));
		SCMI_QUIRK(clock_rcar_x5h_4_31, QUIRK_RCAR_X5H_4_31_WRONG_RATES(*value));
	}

	ph->xops->xfer_put(ph, t);
	return ret;
}

static int scmi_clock_rate_set(const struct scmi_protocol_handle *ph,
			       u32 clk_id, u64 rate)
{
	int ret;
	u32 flags = 0;
	struct scmi_xfer *t;
	struct scmi_clock_set_rate *cfg;
	struct clock_info *ci = ph->get_priv(ph);
	struct scmi_clock_info *clk;

	clk = scmi_clock_domain_lookup(ci, clk_id);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	if (clk->rate_ctrl_forbidden)
		return -EACCES;

	ret = ph->xops->xfer_get_init(ph, CLOCK_RATE_SET, sizeof(*cfg), 0, &t);
	if (ret)
		return ret;

	if (ci->max_async_req &&
	    atomic_inc_return(&ci->cur_async_req) < ci->max_async_req)
		flags |= CLOCK_SET_ASYNC;

	cfg = t->tx.buf;
	cfg->flags = cpu_to_le32(flags);
	cfg->id = cpu_to_le32(clk_id);
	cfg->value_low = cpu_to_le32(rate & 0xffffffff);
	cfg->value_high = cpu_to_le32(rate >> 32);

	if (flags & CLOCK_SET_ASYNC) {
		ret = ph->xops->do_xfer_with_response(ph, t);
		if (!ret) {
			struct scmi_msg_resp_set_rate_complete *resp;

			resp = t->rx.buf;
			if (le32_to_cpu(resp->id) == clk_id)
				dev_dbg(ph->dev,
					"Clk ID %d set async to %llu\n", clk_id,
					get_unaligned_le64(&resp->rate_low));
			else
				ret = -EPROTO;
		}
	} else {
		ret = ph->xops->do_xfer(ph, t);
	}

	if (ci->max_async_req)
		atomic_dec(&ci->cur_async_req);

	ph->xops->xfer_put(ph, t);
	return ret;
}

static int scmi_clock_determine_rate(const struct scmi_protocol_handle *ph,
				     u32 clk_id, unsigned long *rate)
{
	u64 fmin, fmax, ftmp;
	struct scmi_clock_info *clk;
	struct scmi_clock_desc *clkd;
	struct clock_info *ci = ph->get_priv(ph);

	if (!rate)
		return -EINVAL;

	clk = scmi_clock_domain_lookup(ci, clk_id);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	clkd = to_desc(clk);

	/*
	 * If we can't figure out what rate it will be, so just return the
	 * rate back to the caller.
	 */
	if (clkd->r.rate_discrete)
		return 0;

	fmin = clk->min_rate;
	fmax = clk->max_rate;
	if (*rate <= fmin) {
		*rate = fmin;
		return 0;
	} else if (*rate >= fmax) {
		*rate = fmax;
		return 0;
	}

	ftmp = *rate - fmin;
	ftmp += clkd->r.rates[RATE_STEP] - 1; /* to round up */
	ftmp = div64_ul(ftmp, clkd->r.rates[RATE_STEP]);

	*rate = ftmp * clkd->r.rates[RATE_STEP] + fmin;

	return 0;
}

static const struct scmi_clock_rates *
scmi_clock_all_rates_get(const struct scmi_protocol_handle *ph, u32 clk_id)
{
	struct clock_info *ci = ph->get_priv(ph);
	struct scmi_clock_desc *clkd;
	struct scmi_clock_info *clk;

	clk = scmi_clock_domain_lookup(ci, clk_id);
	if (IS_ERR(clk) || !clk->name[0])
		return NULL;

	clkd = to_desc(clk);
	/* Needs full enumeration ? */
	if (clkd->r.rate_discrete && clkd->tot_rates != clkd->r.num_rates) {
		int ret;

		SCMI_QUIRK(clock_rcar_x5h_4_28, QUIRK_RCAR_X5H_4_28_PARENT_RATES);
		SCMI_QUIRK(clock_rcar_x5h_4_31, QUIRK_RCAR_X5H_4_31_PARENT_RATES);

		/* rates[] is already allocated BUT we need to re-enumerate */
		clkd->r.num_rates = 0;
		ret = scmi_clock_describe_rates_get_full(ph, clk_id, clkd);
		if (ret)
			return NULL;
	}

	return &clkd->r;
}

static int
scmi_clock_config_set(const struct scmi_protocol_handle *ph, u32 clk_id,
		      enum clk_state state,
		      enum scmi_clock_oem_config __unused0, u32 __unused1,
		      bool atomic)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_clock_config_set *cfg;

	if (state >= CLK_STATE_RESERVED)
		return -EINVAL;

	ret = ph->xops->xfer_get_init(ph, CLOCK_CONFIG_SET,
				      sizeof(*cfg), 0, &t);
	if (ret)
		return ret;

	t->hdr.poll_completion = atomic;

	cfg = t->tx.buf;
	cfg->id = cpu_to_le32(clk_id);
	cfg->attributes = cpu_to_le32(state);

	ret = ph->xops->do_xfer(ph, t);

	ph->xops->xfer_put(ph, t);
	return ret;
}

static int
scmi_clock_set_parent(const struct scmi_protocol_handle *ph, u32 clk_id,
		      u32 parent_id)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_clock_set_parent *cfg;
	struct clock_info *ci = ph->get_priv(ph);
	struct scmi_clock_info *clk;

	clk = scmi_clock_domain_lookup(ci, clk_id);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	if (parent_id >= clk->num_parents)
		return -EINVAL;

	if (clk->parent_ctrl_forbidden)
		return -EACCES;

	ret = ph->xops->xfer_get_init(ph, CLOCK_PARENT_SET,
				      sizeof(*cfg), 0, &t);
	if (ret)
		return ret;

	t->hdr.poll_completion = false;

	cfg = t->tx.buf;
	cfg->id = cpu_to_le32(clk_id);
	cfg->parent_id = cpu_to_le32(clk->parents[parent_id]);

	ret = ph->xops->do_xfer(ph, t);

	ph->xops->xfer_put(ph, t);

	return ret;
}

static int
scmi_clock_get_parent(const struct scmi_protocol_handle *ph, u32 clk_id,
		      u32 *parent_id)
{
	int ret;
	struct scmi_xfer *t;

	ret = ph->xops->xfer_get_init(ph, CLOCK_PARENT_GET,
				      sizeof(__le32), sizeof(u32), &t);
	if (ret)
		return ret;

	put_unaligned_le32(clk_id, t->tx.buf);

	ret = ph->xops->do_xfer(ph, t);
	if (!ret)
		*parent_id = get_unaligned_le32(t->rx.buf);

	ph->xops->xfer_put(ph, t);
	return ret;
}

/* For SCMI clock v3.0 and onwards */
static int
scmi_clock_config_set_v2(const struct scmi_protocol_handle *ph, u32 clk_id,
			 enum clk_state state,
			 enum scmi_clock_oem_config oem_type, u32 oem_val,
			 bool atomic)
{
	int ret;
	u32 attrs;
	struct scmi_xfer *t;
	struct scmi_msg_clock_config_set_v2 *cfg;

	if (state == CLK_STATE_RESERVED ||
	    (!oem_type && state == CLK_STATE_UNCHANGED))
		return -EINVAL;

	ret = ph->xops->xfer_get_init(ph, CLOCK_CONFIG_SET,
				      sizeof(*cfg), 0, &t);
	if (ret)
		return ret;

	t->hdr.poll_completion = atomic;

	attrs = FIELD_PREP(REGMASK_OEM_TYPE_SET, oem_type) |
		 FIELD_PREP(REGMASK_CLK_STATE, state);

	cfg = t->tx.buf;
	cfg->id = cpu_to_le32(clk_id);
	cfg->attributes = cpu_to_le32(attrs);
	/* Clear in any case */
	cfg->oem_config_val = cpu_to_le32(0);
	if (oem_type)
		cfg->oem_config_val = cpu_to_le32(oem_val);

	ret = ph->xops->do_xfer(ph, t);

	ph->xops->xfer_put(ph, t);
	return ret;
}

static int scmi_clock_enable(const struct scmi_protocol_handle *ph, u32 clk_id,
			     bool atomic)
{
	struct clock_info *ci = ph->get_priv(ph);
	struct scmi_clock_info *clk;

	clk = scmi_clock_domain_lookup(ci, clk_id);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	if (clk->state_ctrl_forbidden)
		return -EACCES;

	return ci->clock_config_set(ph, clk_id, CLK_STATE_ENABLE,
				    NULL_OEM_TYPE, 0, atomic);
}

static int scmi_clock_disable(const struct scmi_protocol_handle *ph, u32 clk_id,
			      bool atomic)
{
	struct clock_info *ci = ph->get_priv(ph);
	struct scmi_clock_info *clk;

	clk = scmi_clock_domain_lookup(ci, clk_id);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	if (clk->state_ctrl_forbidden)
		return -EACCES;

	return ci->clock_config_set(ph, clk_id, CLK_STATE_DISABLE,
				    NULL_OEM_TYPE, 0, atomic);
}

static void quirk_rcar_x5h_no_config_get_fixup(const struct quirk_rcar_x5h_no_attributes *table,
				unsigned int len, u32 id, enum scmi_clock_oem_config oem_type,
				u32 *attributes, bool *enabled, u32 *oem_val, int *ret)
{
	for (unsigned int i = 0; i < len; i++)
		if (id == table[i].id) {
			if (attributes)
				*attributes = 0;
			if (enabled)
				*enabled = true;
			if (oem_val && oem_type)
				*oem_val = 0;
			*ret = 0;
			break;
		}
}

#define QUIRK_RCAR_X5H_4_28_NO_CONFIG_GET						\
	({										\
		quirk_rcar_x5h_no_config_get_fixup(quirk_rcar_x5h_4_28_no_attributes,	\
			ARRAY_SIZE(quirk_rcar_x5h_4_28_no_attributes), clk_id,		\
			oem_type, attributes, enabled, oem_val, &ret);			\
	})

#define QUIRK_RCAR_X5H_4_31_NO_CONFIG_GET						\
	({										\
		quirk_rcar_x5h_no_config_get_fixup(quirk_rcar_x5h_4_31_no_attributes,	\
			ARRAY_SIZE(quirk_rcar_x5h_4_31_no_attributes), clk_id,		\
			oem_type, attributes, enabled, oem_val, &ret);			\
	})

/* For SCMI clock v3.0 and onwards */
static int
scmi_clock_config_get_v2(const struct scmi_protocol_handle *ph, u32 clk_id,
			 enum scmi_clock_oem_config oem_type, u32 *attributes,
			 bool *enabled, u32 *oem_val, bool atomic)
{
	int ret;
	u32 flags;
	struct scmi_xfer *t;
	struct scmi_msg_clock_config_get *cfg;

	ret = ph->xops->xfer_get_init(ph, CLOCK_CONFIG_GET,
				      sizeof(*cfg), 0, &t);
	if (ret)
		return ret;

	t->hdr.poll_completion = atomic;

	flags = FIELD_PREP(REGMASK_OEM_TYPE_GET, oem_type);

	cfg = t->tx.buf;
	cfg->id = cpu_to_le32(clk_id);
	cfg->flags = cpu_to_le32(flags);

	ret = ph->xops->do_xfer(ph, t);
	if (!ret) {
		struct scmi_msg_resp_clock_config_get *resp = t->rx.buf;

		if (attributes)
			*attributes = le32_to_cpu(resp->attributes);

		if (enabled)
			*enabled = IS_CLK_ENABLED(resp->config);

		if (oem_val && oem_type)
			*oem_val = le32_to_cpu(resp->oem_config_val);
	} else {
		SCMI_QUIRK(clock_rcar_x5h_4_28, QUIRK_RCAR_X5H_4_28_NO_CONFIG_GET);
		SCMI_QUIRK(clock_rcar_x5h_4_31, QUIRK_RCAR_X5H_4_31_NO_CONFIG_GET);
	}

	ph->xops->xfer_put(ph, t);

	return ret;
}

static int
scmi_clock_config_get(const struct scmi_protocol_handle *ph, u32 clk_id,
		      enum scmi_clock_oem_config oem_type, u32 *attributes,
		      bool *enabled, u32 *oem_val, bool atomic)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_resp_clock_attributes *resp;

	if (!enabled)
		return -EINVAL;

	ret = ph->xops->xfer_get_init(ph, CLOCK_ATTRIBUTES,
				      sizeof(clk_id), sizeof(*resp), &t);
	if (ret)
		return ret;

	t->hdr.poll_completion = atomic;
	put_unaligned_le32(clk_id, t->tx.buf);
	resp = t->rx.buf;

	ret = ph->xops->do_xfer(ph, t);
	if (!ret)
		*enabled = IS_CLK_ENABLED(resp->attributes);

	ph->xops->xfer_put(ph, t);

	return ret;
}

static int scmi_clock_state_get(const struct scmi_protocol_handle *ph,
				u32 clk_id, bool *enabled, bool atomic)
{
	struct clock_info *ci = ph->get_priv(ph);

	return ci->clock_config_get(ph, clk_id, NULL_OEM_TYPE, NULL,
				    enabled, NULL, atomic);
}

static int scmi_clock_config_oem_set(const struct scmi_protocol_handle *ph,
				     u32 clk_id,
				     enum scmi_clock_oem_config oem_type,
				     u32 oem_val, bool atomic)
{
	struct clock_info *ci = ph->get_priv(ph);
	struct scmi_clock_info *clk;

	clk = scmi_clock_domain_lookup(ci, clk_id);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	if (!clk->extended_config)
		return -EOPNOTSUPP;

	return ci->clock_config_set(ph, clk_id, CLK_STATE_UNCHANGED,
				    oem_type, oem_val, atomic);
}

static int scmi_clock_config_oem_get(const struct scmi_protocol_handle *ph,
				     u32 clk_id,
				     enum scmi_clock_oem_config oem_type,
				     u32 *oem_val, u32 *attributes, bool atomic)
{
	struct clock_info *ci = ph->get_priv(ph);
	struct scmi_clock_info *clk;

	clk = scmi_clock_domain_lookup(ci, clk_id);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	if (!clk->extended_config)
		return -EOPNOTSUPP;

	return ci->clock_config_get(ph, clk_id, oem_type, attributes,
				    NULL, oem_val, atomic);
}

static int scmi_clock_count_get(const struct scmi_protocol_handle *ph)
{
	struct clock_info *ci = ph->get_priv(ph);

	return ci->num_clocks;
}

static const struct scmi_clock_info *
scmi_clock_info_get(const struct scmi_protocol_handle *ph, u32 clk_id)
{
	struct scmi_clock_info *clk;
	struct clock_info *ci = ph->get_priv(ph);

	clk = scmi_clock_domain_lookup(ci, clk_id);
	if (IS_ERR(clk))
		return NULL;

	if (!clk->name[0])
		return NULL;

	return clk;
}

static const struct scmi_clk_proto_ops clk_proto_ops = {
	.count_get = scmi_clock_count_get,
	.info_get = scmi_clock_info_get,
	.rate_get = scmi_clock_rate_get,
	.rate_set = scmi_clock_rate_set,
	.determine_rate = scmi_clock_determine_rate,
	.all_rates_get = scmi_clock_all_rates_get,
	.enable = scmi_clock_enable,
	.disable = scmi_clock_disable,
	.state_get = scmi_clock_state_get,
	.config_oem_get = scmi_clock_config_oem_get,
	.config_oem_set = scmi_clock_config_oem_set,
	.parent_set = scmi_clock_set_parent,
	.parent_get = scmi_clock_get_parent,
};

static bool scmi_clk_notify_supported(const struct scmi_protocol_handle *ph,
				      u8 evt_id, u32 src_id)
{
	bool supported;
	struct scmi_clock_info *clk;
	struct clock_info *ci = ph->get_priv(ph);

	if (evt_id >= ARRAY_SIZE(evt_2_cmd))
		return false;

	clk = scmi_clock_domain_lookup(ci, src_id);
	if (IS_ERR(clk))
		return false;

	if (evt_id == SCMI_EVENT_CLOCK_RATE_CHANGED)
		supported = clk->rate_changed_notifications;
	else
		supported = clk->rate_change_requested_notifications;

	return supported;
}

static int scmi_clk_rate_notify(const struct scmi_protocol_handle *ph,
				u32 clk_id, int message_id, bool enable)
{
	int ret;
	struct scmi_xfer *t;
	struct scmi_msg_clock_rate_notify *notify;

	ret = ph->xops->xfer_get_init(ph, message_id, sizeof(*notify), 0, &t);
	if (ret)
		return ret;

	notify = t->tx.buf;
	notify->clk_id = cpu_to_le32(clk_id);
	notify->notify_enable = enable ? cpu_to_le32(BIT(0)) : 0;

	ret = ph->xops->do_xfer(ph, t);

	ph->xops->xfer_put(ph, t);
	return ret;
}

static int scmi_clk_set_notify_enabled(const struct scmi_protocol_handle *ph,
				       u8 evt_id, u32 src_id, bool enable)
{
	int ret, cmd_id;

	if (evt_id >= ARRAY_SIZE(evt_2_cmd))
		return -EINVAL;

	cmd_id = evt_2_cmd[evt_id];
	ret = scmi_clk_rate_notify(ph, src_id, cmd_id, enable);
	if (ret)
		pr_debug("FAIL_ENABLED - evt[%X] dom[%d] - ret:%d\n",
			 evt_id, src_id, ret);

	return ret;
}

static void *scmi_clk_fill_custom_report(const struct scmi_protocol_handle *ph,
					 u8 evt_id, ktime_t timestamp,
					 const void *payld, size_t payld_sz,
					 void *report, u32 *src_id)
{
	const struct scmi_clock_rate_notify_payld *p = payld;
	struct scmi_clock_rate_notif_report *r = report;

	if (sizeof(*p) != payld_sz ||
	    (evt_id != SCMI_EVENT_CLOCK_RATE_CHANGED &&
	     evt_id != SCMI_EVENT_CLOCK_RATE_CHANGE_REQUESTED))
		return NULL;

	r->timestamp = timestamp;
	r->agent_id = le32_to_cpu(p->agent_id);
	r->clock_id = le32_to_cpu(p->clock_id);
	r->rate = get_unaligned_le64(&p->rate_low);
	*src_id = r->clock_id;

	return r;
}

static int scmi_clk_get_num_sources(const struct scmi_protocol_handle *ph)
{
	struct clock_info *ci = ph->get_priv(ph);

	if (!ci)
		return -EINVAL;

	return ci->num_clocks;
}

static const struct scmi_event clk_events[] = {
	{
		.id = SCMI_EVENT_CLOCK_RATE_CHANGED,
		.max_payld_sz = sizeof(struct scmi_clock_rate_notify_payld),
		.max_report_sz = sizeof(struct scmi_clock_rate_notif_report),
	},
	{
		.id = SCMI_EVENT_CLOCK_RATE_CHANGE_REQUESTED,
		.max_payld_sz = sizeof(struct scmi_clock_rate_notify_payld),
		.max_report_sz = sizeof(struct scmi_clock_rate_notif_report),
	},
};

static const struct scmi_event_ops clk_event_ops = {
	.is_notify_supported = scmi_clk_notify_supported,
	.get_num_sources = scmi_clk_get_num_sources,
	.set_notify_enabled = scmi_clk_set_notify_enabled,
	.fill_custom_report = scmi_clk_fill_custom_report,
};

static const struct scmi_protocol_events clk_protocol_events = {
	.queue_sz = SCMI_PROTO_QUEUE_SZ,
	.ops = &clk_event_ops,
	.evts = clk_events,
	.num_events = ARRAY_SIZE(clk_events),
};

static int scmi_clock_protocol_init(const struct scmi_protocol_handle *ph)
{
	int clkid, ret;
	struct clock_info *cinfo;

	dev_dbg(ph->dev, "Clock Version %d.%d\n",
		PROTOCOL_REV_MAJOR(ph->version), PROTOCOL_REV_MINOR(ph->version));

	cinfo = devm_kzalloc(ph->dev, sizeof(*cinfo), GFP_KERNEL);
	if (!cinfo)
		return -ENOMEM;

	ret = scmi_clock_protocol_attributes_get(ph, cinfo);
	if (ret)
		return ret;

	cinfo->clkds = devm_kcalloc(ph->dev, cinfo->num_clocks,
				    sizeof(*cinfo->clkds), GFP_KERNEL);
	if (!cinfo->clkds)
		return -ENOMEM;

	for (clkid = 0; clkid < cinfo->num_clocks; clkid++) {
		cinfo->clkds[clkid].id = clkid;
		ret = scmi_clock_attributes_get(ph, clkid, cinfo);
		if (!ret)
			scmi_clock_describe_rates_get(ph, clkid, cinfo);
	}

	if (PROTOCOL_REV_MAJOR(ph->version) >= 0x3) {
		cinfo->clock_config_set = scmi_clock_config_set_v2;
		cinfo->clock_config_get = scmi_clock_config_get_v2;
	} else {
		cinfo->clock_config_set = scmi_clock_config_set;
		cinfo->clock_config_get = scmi_clock_config_get;
	}

	return ph->set_priv(ph, cinfo);
}

static const struct scmi_protocol scmi_clock = {
	.id = SCMI_PROTOCOL_CLOCK,
	.owner = THIS_MODULE,
	.instance_init = &scmi_clock_protocol_init,
	.ops = &clk_proto_ops,
	.events = &clk_protocol_events,
	.supported_version = SCMI_PROTOCOL_SUPPORTED_VERSION,
};

DEFINE_SCMI_PROTOCOL_REGISTER_UNREGISTER(clock, scmi_clock)
