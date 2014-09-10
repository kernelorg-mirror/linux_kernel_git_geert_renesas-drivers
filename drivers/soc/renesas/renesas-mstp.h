#include <linux/types.h>

#define MSTP_BIT(name, reg)	{ #name, reg, MSTP ## reg ## _ ## name	}

/* Physical MSTP addresses */
struct mstp_regs {
	/* Module Stop Status Register (MSTPSRx) */
	unsigned long mstpsr;
	/* System Module Stop Control Register (SMSTPSRx) */
	unsigned long smstpcr;
	/* Optional Realtime Module Stop Control Register (RMSTPSRx) */
	unsigned long rmstpcr;
	/* Optional Modem Module Stop Control Register (MMSTPSRx) */
	unsigned long mmstpcr;
	/* Optional Secure Module Stop Control Register (SCMSTPSRx) */
	unsigned long scmstpcr;
	/* Optional Safety Module Stop Control Register (SAMSTPSRx) */
	unsigned long samstpcr;
};

struct mstp_clock {
	const char *name;
	unsigned int idx;
	u32 mask;
};

struct mstp_disable {
	unsigned long reg;	/* Physical xMSTPCRy address */
	u32 bits;
};

struct mstp_do_not_touch {
	unsigned int idx;
	unsigned int bit;
	const char *name;
};

struct renesas_mstp_info {
	unsigned long mstp_base;

	const struct mstp_regs *regs;
	const struct mstp_clock *clocks;
	const struct mstp_disable *smstp_disable;	/* optional */
	const struct mstp_disable *rmstp_disable;	/* optional */
	const struct mstp_disable *mmstp_disable;	/* optional */
	const struct mstp_disable *scmstp_disable;	/* optional */
	const struct mstp_disable *samstp_disable;	/* optional */
	const struct mstp_do_not_touch *do_not_touch;

	unsigned int num_regs;
	unsigned int num_clocks;
	unsigned int num_smstp_disable;
	unsigned int num_rmstp_disable;
	unsigned int num_mmstp_disable;
	unsigned int num_scmstp_disable;
	unsigned int num_samstp_disable;
	unsigned int num_do_not_touch;
};


struct seq_file;

extern void renesas_disable_mstp_clocks(const struct renesas_mstp_info *info);
