#include <linux/compiler.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "renesas-mstp.h"

static const struct renesas_mstp_info *mstp;

static void __iomem *virt2phys_offset;

static void __iomem **MSTPSR;
static void __iomem **SMSTPCR;
static void __iomem **RMSTPCR;
static void __iomem **MMSTPCR;
static void __iomem **SCMSTPCR;
static void __iomem **SAMSTPCR;

static u32 read_mstpsr(unsigned int r)
{
	return ioread32(MSTPSR[r]);
}

static u32 read_smstpcr(unsigned int r)
{
	return ioread32(SMSTPCR[r]);
}

static void write_smstpcr(u32 x, unsigned int r)
{
	iowrite32(x, SMSTPCR[r]);
}

static u32 read_rmstpcr(unsigned int r)
{
	return RMSTPCR ? ioread32(RMSTPCR[r]) : ~0;
}

static void write_rmstpcr(u32 x, unsigned int r)
{
	if (RMSTPCR)
		iowrite32(x, RMSTPCR[r]);
}

static u32 read_mmstpcr(unsigned int r)
{
	return MMSTPCR ? ioread32(MMSTPCR[r]) : ~0;
}

static void write_mmstpcr(u32 x, unsigned int r)
{
	if (MMSTPCR)
		iowrite32(x, MMSTPCR[r]);
}

static u32 read_scmstpcr(unsigned int r)
{
	return SCMSTPCR ? ioread32(SCMSTPCR[r]) : ~0;
}

static void write_scmstpcr(u32 x, unsigned int r)
{
	if (SCMSTPCR)
		iowrite32(x, SCMSTPCR[r]);
}

static u32 read_samstpcr(unsigned int r)
{
	return SAMSTPCR ? ioread32(SAMSTPCR[r]) : ~0;
}

static void write_samstpcr(u32 x, unsigned int r)
{
	if (SAMSTPCR)
		iowrite32(x, SAMSTPCR[r]);
}

static void renesas_show_mstp_clocks(struct seq_file *m)
{
	unsigned int i;

	if (!mstp)
		return;

	for (i = 0; i < mstp->num_clocks; i++) {
		u32 sr, sc, rc = 0, mc = 0, xc = 0, ac = 0, mask;
		sr = read_mstpsr(mstp->clocks[i].idx);
		mask = mstp->clocks[i].mask;
		if (sr & mask)
			continue;
		sc = read_smstpcr(mstp->clocks[i].idx);
		rc = read_rmstpcr(mstp->clocks[i].idx);
		mc = read_mmstpcr(mstp->clocks[i].idx);
		xc = read_scmstpcr(mstp->clocks[i].idx);
		ac = read_samstpcr(mstp->clocks[i].idx);
		if (m)
			seq_printf(m, "%-20s:%s%s%s%s%s\n",
				   mstp->clocks[i].name,
				   sc & mask ? " ." : " S",
				   RMSTPCR ? (rc & mask ? " ." : " R") : "",
				   MMSTPCR ? (mc & mask ? " ." : " M") : "",
				   SCMSTPCR ? (xc & mask ? " ." : " X") : "",
				   SAMSTPCR ? (ac & mask ? " ." : " A") : "");
		else
			pr_info("%-20s:%s%s%s%s%s\n", mstp->clocks[i].name,
				sc & mask ? " ." : " S",
				RMSTPCR ? (rc & mask ? " ." : " R") : "",
				MMSTPCR ? (mc & mask ? " ." : " M") : "",
				SCMSTPCR ? (xc & mask ? " ." : " X") : "",
				SAMSTPCR ? (ac & mask ? " ." : " A") : "");
	}
}

static bool allowed_to_change(unsigned int idx, unsigned int bit)
{
	unsigned int i;
	int n = idx * 100 + bit;
	bool res = false;
	static int last_n = 0;
	static bool last_res;

	/* Avoid printing the message twice in a row */
	if (n == last_n)
		return last_res;

	for (i = 0; i < mstp->num_do_not_touch; i++)
		if (mstp->do_not_touch[i].idx == idx &&
		    mstp->do_not_touch[i].bit == bit) {
			printk("\n    Skipping %s\n   ",
			       mstp->do_not_touch[i].name);
			goto out;
		}

	res = true;

out:
	last_n = n;
	last_res = res;
	return last_res;
}

#define CHECK_CONSISTENCY(r, b, sc0, rc0, mc0, xc0, ac0)		\
{									\
	u32 sr = read_mstpsr(r);					\
	if ((sr & BIT(b)) != (sc0 & rc0 & mc0 & xc0 & ac0 & BIT(b))) {	\
		unsigned int n = 0;					\
		char buf[100];						\
		buf[0] = 0;						\
		if (SMSTPCR)						\
			n += sprintf(buf + n, " S 0x%08x", sc0);	\
		if (RMSTPCR)						\
			n += sprintf(buf + n, " R 0x%08x", rc0);	\
		if (MMSTPCR)						\
			n += sprintf(buf + n, " M 0x%08x", mc0);	\
		if (SCMSTPCR)						\
			n += sprintf(buf + n, " X 0x%08x", xc0);	\
		if (SAMSTPCR)						\
			n += sprintf(buf + n, " A 0x%08x", ac0);	\
		printk("\n%u: Inconsistency MSTP%u%02u SR 0x%08x%s\n",	\
		       __LINE__, r, b, sr, buf);			\
	}								\
}

#define FLIP_RMSTP(r, b, sc0, rc0, mc0, xc0, ac0)			     \
if (RMSTPCR) {								     \
	u32 rc1 = rc0 ^ BIT(b);						     \
	write_rmstpcr(rc1, r);						     \
	mdelay(10);							     \
	rc0 = read_rmstpcr(r);						     \
	if (rc0 != rc1)							     \
		printk("\n%u: Couldn't change RMSTP%u%02u: 0x%08x 0x%08x\n", \
		       __LINE__, r, b, rc0, rc1);			     \
	udelay(1000);							     \
	CHECK_CONSISTENCY(r, b, sc0, rc0, mc0, xc0, ac0);		     \
}

#define FLIP_MMSTP(r, b, sc0, rc0, mc0, xc0, ac0)			     \
if (MMSTPCR) {								     \
	u32 mc1 = mc0 ^ BIT(b);						     \
	write_mmstpcr(mc1, r);						     \
	mdelay(10);							     \
	mc0 = read_mmstpcr(r);						     \
	if (mc0 != mc1)							     \
		printk("\n%u: Couldn't change MMSTP%u%02u: 0x%08x 0x%08x\n", \
		       __LINE__, r, b, mc0, mc1);			     \
	udelay(1000);							     \
	CHECK_CONSISTENCY(r, b, sc0, rc0, mc0, xc0, ac0);		     \
}

#define FLIP_SMSTP(r, b, sc0, rc0, mc0, xc0, ac0)			     \
if (allowed_to_change(r, b)) {						     \
	u32 sc1 = sc0 ^ mask;						     \
	write_smstpcr(sc1, r);						     \
	udelay(1000);							     \
	sc0 = read_smstpcr(r);						     \
	if (sc0 != sc1)							     \
		printk("\n%u: Couldn't change SMSTP%u%02u: 0x%08x 0x%08x\n", \
		       __LINE__, r, b, sc0, sc1);			     \
	udelay(1000);							     \
	CHECK_CONSISTENCY(r, b, sc0, rc0, mc0, xc0, ac0);		     \
}

#define FLIP_SCMSTP(r, b, sc0, rc0, mc0, xc0, ac0)			     \
if (SCMSTPCR) {								     \
	u32 xc1 = xc0 ^ BIT(b);						     \
	write_scmstpcr(xc1, r);						     \
	mdelay(10);							     \
	xc0 = read_scmstpcr(r);						     \
	if (xc0 != xc1)							     \
		printk("\n%u: Couldn't change SCMSTP%u%02u: 0x%08x 0x%08x\n",\
		       __LINE__, r, b, xc0, xc1);			     \
	udelay(1000);							     \
	CHECK_CONSISTENCY(r, b, sc0, rc0, mc0, xc0, ac0);		     \
}

#define FLIP_SAMSTP(r, b, sc0, rc0, mc0, xc0, ac0)			     \
if (SAMSTPCR) {								     \
	u32 ac1 = ac0 ^ BIT(b);						     \
	write_samstpcr(ac1, r);						     \
	mdelay(10);							     \
	ac0 = read_samstpcr(r);						     \
	if (ac0 != ac1)							     \
		printk("\n%u: Couldn't change SAMSTP%u%02u: 0x%08x 0x%08x\n",\
		       __LINE__, r, b, ac0, ac1);			     \
	udelay(1000);							     \
	CHECK_CONSISTENCY(r, b, sc0, rc0, mc0, ac0, ac0);		     \
}


void renesas_test_mstp_clocks(void)
{
	unsigned int r, b;

	if (!mstp)
		return;

	printk("STARTING TEST\n");
	for (r = 0; r < mstp->num_regs; r++) {
		u32 sr, save_sc, save_rc, save_mc, save_xc, save_ac;
		unsigned int n = 0;
		char buf[100];
		buf[0] = 0;
		/* Save MSTP registers */
		sr = read_mstpsr(r);
		save_sc = read_smstpcr(r);
		save_rc = read_rmstpcr(r);
		save_mc = read_mmstpcr(r);
		save_xc = read_scmstpcr(r);
		save_ac = read_samstpcr(r);
		if (SMSTPCR)
			n += sprintf(buf + n, " S 0x%08x", save_sc);
		if (RMSTPCR)
			n += sprintf(buf + n, " R 0x%08x", save_rc);
		if (MMSTPCR)
			n += sprintf(buf + n, " M 0x%08x", save_mc);
		if (SCMSTPCR)
			n += sprintf(buf + n, " X 0x%08x", save_xc);
		if (SAMSTPCR)
			n += sprintf(buf + n, " A 0x%08x", save_ac);
		printk("MSTP%u: SR 0x%08x%s\n", r, sr, buf);
		for (b = 31; b < 32; b--) {
			u32 mask, rc0, sc0, mc0, xc0, ac0;
			printk(" %u", b);
			mask = BIT(b);

			/* Read MSTP registers */
			sc0 = read_smstpcr(r);
			rc0 = read_rmstpcr(r);
			mc0 = read_mmstpcr(r);
			xc0 = read_scmstpcr(r);
			ac0 = read_scmstpcr(r);
			udelay(1000);
			CHECK_CONSISTENCY(r, b, sc0, rc0, mc0, xc0, ac0);

			/* Test if MSTP bits can be changed */
			FLIP_RMSTP(r, b, sc0, rc0, mc0, xc0, ac0);
			FLIP_MMSTP(r, b, sc0, rc0, mc0, xc0, ac0);
			FLIP_SMSTP(r, b, sc0, rc0, mc0, xc0, ac0);
			FLIP_SCMSTP(r, b, sc0, rc0, mc0, xc0, ac0);
			FLIP_SAMSTP(r, b, sc0, rc0, mc0, xc0, ac0);

			FLIP_RMSTP(r, b, sc0, rc0, mc0, xc0, ac0);
			FLIP_MMSTP(r, b, sc0, rc0, mc0, xc0, ac0);
			FLIP_SMSTP(r, b, sc0, rc0, mc0, xc0, ac0);
			FLIP_SCMSTP(r, b, sc0, rc0, mc0, xc0, ac0);
			FLIP_SAMSTP(r, b, sc0, rc0, mc0, xc0, ac0);

			/* Restore MSTP registers */
			write_smstpcr(save_sc, r);
			write_rmstpcr(save_rc, r);
			write_mmstpcr(save_mc, r);
			write_scmstpcr(save_xc, r);
			write_scmstpcr(save_ac, r);
			mdelay(10);
			sc0 = read_smstpcr(r);
			if (sc0 != save_sc)
				printk("\n%u: SMSTPCR%u 0x%08x != 0x%08x\n",
				       __LINE__, r, sc0, save_sc);
			rc0 = read_rmstpcr(r);
			if (rc0 != save_rc)
				printk("\n%u: RMSTPCR%u 0x%08x != 0x%08x\n",
				       __LINE__, r, rc0, save_rc);
			mc0 = read_mmstpcr(r);
			if (mc0 != save_mc)
				printk("\n%u: MMSTPCR%u 0x%08x != 0x%08x\n",
				       __LINE__, r, mc0, save_mc);
			xc0 = read_scmstpcr(r);
			if (xc0 != save_xc)
				printk("\n%u: SCMSTPCR%u 0x%08x != 0x%08x\n",
				       __LINE__, r, xc0, save_xc);
			ac0 = read_scmstpcr(r);
			if (ac0 != save_ac)
				printk("\n%u: SAMSTPCR%u 0x%08x != 0x%08x\n",
				       __LINE__, r, ac0, save_ac);
			udelay(1000);
			CHECK_CONSISTENCY(r, b, sc0, rc0, mc0, xc0, ac0);
		}
		printk("\n");
	}
	printk("TEST COMPLETED\n");
}

static int renesas_mstp_setup(const struct renesas_mstp_info *info)
{
	static void __iomem *base;
	unsigned int i;

	if (mstp && mstp != info) {
		pr_warn("MSTP already set up for different CPU\n");
		return -EINVAL;
	}

	if (mstp)
		return 0;

	base = ioremap(info->mstp_base, PAGE_SIZE);
	if (!base) {
		pr_err("Cannot ioremap MSTP regs\n");
		return -ENOMEM;
	}

	virt2phys_offset = base - info->mstp_base;

	MSTPSR = kzalloc(info->num_regs * sizeof(*MSTPSR), GFP_KERNEL);
	SMSTPCR = kzalloc(info->num_regs * sizeof(*SMSTPCR), GFP_KERNEL);
	if (!MSTPSR || !SMSTPCR)
		return -ENOMEM;

	if (info->regs[0].rmstpcr) {
		RMSTPCR = kzalloc(info->num_regs * sizeof(*RMSTPCR),
				  GFP_KERNEL);
		if (!RMSTPCR)
			return -ENOMEM;
	}
	if (info->regs[0].mmstpcr) {
		MMSTPCR = kzalloc(info->num_regs * sizeof(*MMSTPCR),
				  GFP_KERNEL);
		if (!MMSTPCR)
			return -ENOMEM;
	}
	if (info->regs[0].scmstpcr) {
		SCMSTPCR = kzalloc(info->num_regs * sizeof(*SCMSTPCR),
				   GFP_KERNEL);
		if (!SCMSTPCR)
			return -ENOMEM;
	}
	if (info->regs[0].samstpcr) {
		SAMSTPCR = kzalloc(info->num_regs * sizeof(*SAMSTPCR),
				   GFP_KERNEL);
		if (!SAMSTPCR)
			return -ENOMEM;
	}

	for (i = 0; i < info->num_regs; i++) {
		MSTPSR[i] = info->regs[i].mstpsr + virt2phys_offset;
		SMSTPCR[i] = info->regs[i].smstpcr + virt2phys_offset;
		if (RMSTPCR)
			RMSTPCR[i] = info->regs[i].rmstpcr + virt2phys_offset;
		if (MMSTPCR)
			MMSTPCR[i] = info->regs[i].mmstpcr + virt2phys_offset;
		if (SCMSTPCR)
			SCMSTPCR[i] = info->regs[i].scmstpcr + virt2phys_offset;
		if (SAMSTPCR)
			SAMSTPCR[i] = info->regs[i].samstpcr + virt2phys_offset;
	};

	mstp = info;
	return 0;
}

void __init renesas_disable_mstp_clocks(const struct renesas_mstp_info *info)
{
	const struct mstp_disable *disable;
	void __iomem *reg;
	unsigned int i;

	if (renesas_mstp_setup(info))
		return;

	renesas_show_mstp_clocks(NULL);

	pr_info("Disabling MSTP clocks for the System Core\n");
	disable = mstp->smstp_disable;
	for (i = 0; i < mstp->num_smstp_disable; i++) {
		pr_info("  SMSTPCR%u: *0x%08lx |= 0x%08x\n", i,
			disable[i].reg, disable[i].bits);
		reg = disable[i].reg + virt2phys_offset;
		iowrite32(ioread32(reg) | disable[i].bits, reg);
	}

	if (RMSTPCR) {
		disable = mstp->rmstp_disable;
		pr_info("Disabling MSTP clocks for the Realtime Core\n");
		for (i = 0; i < mstp->num_rmstp_disable; i++) {
			pr_info("  RMSTPCR%u: *0x%08lx |= 0x%08x\n", i,
				disable[i].reg, disable[i].bits);
			reg = disable[i].reg + virt2phys_offset;
			iowrite32(ioread32(reg) | disable[i].bits, reg);
		}
	}

	if (MMSTPCR) {
		disable = mstp->mmstp_disable;
		pr_info("Disabling MSTP clocks for the Modem Core\n");
		for (i = 0; i < mstp->num_mmstp_disable; i++) {
			pr_info("  MMSTPCR%u: *0x%08lx |= 0x%08x\n", i,
				disable[i].reg,
				disable[i].bits);
			reg = disable[i].reg + virt2phys_offset;
			iowrite32(ioread32(reg) | disable[i].bits, reg);
		}
	}

	if (SCMSTPCR) {
		disable = mstp->scmstp_disable;
		pr_info("Disabling MSTP clocks for the Modem Core\n");
		for (i = 0; i < mstp->num_scmstp_disable; i++) {
			pr_info("  SCMSTPCR%u: *0x%08lx |= 0x%08x\n", i,
				disable[i].reg,
				disable[i].bits);
			reg = disable[i].reg + virt2phys_offset;
			iowrite32(ioread32(reg) | disable[i].bits, reg);
		}
	}

	if (SAMSTPCR) {
		disable = mstp->samstp_disable;
		pr_info("Disabling MSTP clocks for the Modem Core\n");
		for (i = 0; i < mstp->num_samstp_disable; i++) {
			pr_info("  SAMSTPCR%u: *0x%08lx |= 0x%08x\n", i,
				disable[i].reg,
				disable[i].bits);
			reg = disable[i].reg + virt2phys_offset;
			iowrite32(ioread32(reg) | disable[i].bits, reg);
		}
	}

	renesas_show_mstp_clocks(NULL);
}

static int mstp_proc_show(struct seq_file *m, void *v)
{
	renesas_show_mstp_clocks(m);
	return 0;
}

static int mstp_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, mstp_proc_show, NULL);
}

static ssize_t mstp_write(struct file *file, const char __user *user,
			  size_t size, loff_t *ppos)
{
	unsigned int len, i, nr, idx, bit;
	char buf[32], *p;
	const char *key, *val;
	int error;
	u32 mask;

	len = min(size, sizeof(buf) - 1);
	if (copy_from_user(buf, user, len))
		return -EFAULT;

	buf[len] = '\0';

	p = strchr(buf, '=');
	if (!p)
		return -EINVAL;
	*p = 0;

	key = buf;
	val = p + 1;

	for (i = 0; i < mstp->num_clocks; i++) {
		if (!strcasecmp(key, mstp->clocks[i].name)) {
			idx = mstp->clocks[i].idx;
			mask = mstp->clocks[i].mask;
			goto found;
		}
	}

	error = kstrtouint(key, 10, &nr);
	if (error)
		return error;

	idx = nr / 100;
	if (idx >= mstp->num_regs)
		return -EINVAL;

	bit = nr % 100;
	if (bit > 31)
		return -EINVAL;

	mask = BIT(bit);

found:
	p = strpbrk(val, " \t\n");
	if (p)
		*p = 0;

	if (!strcasecmp(val, "on") || !strcmp(val, "1"))
		write_smstpcr(read_smstpcr(idx) & ~mask, idx);
	else if (!strcasecmp(val, "off") || !strcmp(val, "0")) {
		write_smstpcr(read_smstpcr(idx) | mask, idx);
		write_rmstpcr(read_rmstpcr(idx) | mask, idx);
		write_mmstpcr(read_mmstpcr(idx) | mask, idx);
	} else
		return -EINVAL;

	return size;
}

static const struct file_operations mstp_proc_fops = {
	.open		= mstp_proc_open,
	.read		= seq_read,
	.write		= mstp_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int test_mstp_proc_show(struct seq_file *m, void *v)
{
	renesas_test_mstp_clocks();
	return 0;
}

static int test_mstp_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, test_mstp_proc_show, NULL);
}

static const struct file_operations test_mstp_proc_fops = {
	.open		= test_mstp_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init proc_mstp_init(void)
{
	int error = -ENODEV;

	if (error)
		return error;

	proc_create("mstp", 0, NULL, &mstp_proc_fops);
	proc_create("mstp_test", 0, NULL, &test_mstp_proc_fops);

	return 0;
}
fs_initcall(proc_mstp_init);
