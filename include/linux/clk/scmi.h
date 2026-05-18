/* SPDX-License-Identifier: GPL-2.0+
 */

#ifndef __LINUX_CLK_SCMI_H_
#define __LINUX_CLK_SCMI_H_

#include <linux/types.h>

struct clk;

#if IS_ENABLED(CONFIG_COMMON_CLK_SCMI)
bool scmi_clk_is_pm_clk(struct clk *clk);
#else
static inline bool scmi_clk_is_pm_clk(struct clk *clk) { return false; }
#endif

#endif /* __LINUX_CLK_SCMI_H_ */
