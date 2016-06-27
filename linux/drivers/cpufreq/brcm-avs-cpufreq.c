/*
 * CPU frequency scaling for Broadcom SoCs with AVS firmware
 *
 * Copyright (c) 2016 Broadcom
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>

#ifdef CONFIG_ARM_BRCM_AVS_CPUFREQ_DEBUG
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#endif

/* Max number of arguments AVS calls take */
#define AVS_MAX_CMD_ARGS	4
/*
 * This macro is used to generate AVS parameter register offsets. For
 * x >= AVS_MAX_CMD_ARGS, it returns 0 to protect against accidental memory
 * access outside of the parameter range. (Offset 0 is the first parameter.)
 */
#define AVS_PARAM_MULT(x)	((x) < AVS_MAX_CMD_ARGS ? (x) : 0)

/* AVS Mailbox Register offsets */
#define AVS_MBOX_COMMAND	0x00
#define AVS_MBOX_STATUS		0x04
#define AVS_MBOX_VOLTAGE0	0x08
#define AVS_MBOX_TEMP0		0x0c
#define AVS_MBOX_PV0		0x10
#define AVS_MBOX_MV0		0x14
#define AVS_MBOX_PARAM(x)	(0x18 + AVS_PARAM_MULT(x) * sizeof(u32))
#define AVS_MBOX_REVISION	0x28
#define AVS_MBOX_PSTATE		0x2c
#define AVS_MBOX_HEARTBEAT	0x30
#define AVS_MBOX_MAGIC		0x34
#define AVS_MBOX_SIGMA_HVT	0x38
#define AVS_MBOX_SIGMA_SVT	0x3c
#define AVS_MBOX_VOLTAGE1	0x40
#define AVS_MBOX_TEMP1		0x44
#define AVS_MBOX_PV1		0x48
#define AVS_MBOX_MV1		0x4c
#define AVS_MBOX_FREQUENCY	0x50

/* AVS Commands */
#define AVS_CMD_AVAILABLE	0x00
#define AVS_CMD_DISABLE		0x10
#define AVS_CMD_ENABLE		0x11
#define AVS_CMD_S2_ENTER	0x12
#define AVS_CMD_S2_EXIT		0x13
#define AVS_CMD_BBM_ENTER	0x14
#define AVS_CMD_BBM_EXIT	0x15
#define AVS_CMD_S3_ENTER	0x16
#define AVS_CMD_S3_EXIT		0x17
#define AVS_CMD_BALANCE		0x18
/* PMAP and P-STATE commands */
#define AVS_CMD_GET_PMAP	0x30
#define AVS_CMD_SET_PMAP	0x31
#define AVS_CMD_GET_PSTATE	0x40
#define AVS_CMD_SET_PSTATE	0x41

/* Different modes AVS supports (for GET_PMAP/SET_PMAP) */
#define AVS_MODE_AVS		0x0
#define AVS_MODE_DFS		0x1
#define AVS_MODE_DVS		0x2
#define AVS_MODE_DVFS		0x3

/*
 * PMAP parameter p1
 * unused:31-24, mdiv_p0:23-16, unused:15-14, pdiv:13-10 , ndiv_int:9-0
 */
#define NDIV_INT_SHIFT		0
#define NDIV_INT_MASK		0x3ff
#define PDIV_SHIFT		10
#define PDIV_MASK		0xf
#define MDIV_P0_SHIFT		16
#define MDIV_P0_MASK		0xff
/*
 * PMAP parameter p2
 * mdiv_p4:31-24, mdiv_p3:23-16, mdiv_p2:15:8, mdiv_p1:7:0
 */
#define MDIV_P1_SHIFT		0
#define MDIV_P1_MASK		0xff
#define MDIV_P2_SHIFT		8
#define MDIV_P2_MASK		0xff
#define MDIV_P3_SHIFT		16
#define MDIV_P3_MASK		0xff
#define MDIV_P4_SHIFT		24
#define MDIV_P4_MASK		0xff

/* Different P-STATES AVS supports (for GET_PSTATE/SET_PSTATE) */
#define AVS_PSTATE_P0		0x0
#define AVS_PSTATE_P1		0x1
#define AVS_PSTATE_P2		0x2
#define AVS_PSTATE_P3		0x3
#define AVS_PSTATE_P4		0x4
#define AVS_PSTATE_MAX		AVS_PSTATE_P4

/* CPU L2 Interrupt Controller Registers */
#define AVS_CPU_L2_SET0		0x04
#define AVS_CPU_L2_INT_MASK	BIT(31)

/* AVS Command Status Values */
#define AVS_STATUS_CLEAR	0x00
/* Command/notification accepted */
#define AVS_STATUS_SUCCESS	0xf0
/* Command/notification rejected */
#define AVS_STATUS_FAILURE	0xff
/* Invalid command/notification (unknown) */
#define AVS_STATUS_INVALID	0xf1
/* Non-AVS modes are not supported */
#define AVS_STATUS_NO_SUPP	0xf2
/* Cannot set P-State until P-Map supplied */
#define AVS_STATUS_NO_MAP	0xf3
/* Cannot change P-Map after initial P-Map set */
#define AVS_STATUS_MAP_SET	0xf4
/* Max AVS status; higher numbers are used for debugging */
#define AVS_STATUS_MAX		0xff

/* Other AVS related constants */
#define AVS_LOOP_LIMIT		50000
#define AVS_FIRMWARE_MAGIC	0xa11600d1

#define BRCM_AVS_CPUFREQ_NAME	"brcm-avs-cpufreq"
#define BRCM_AVS_CPU_DATA	"brcm,avs-cpu-data-mem"
#define BRCM_AVS_CPU_INTR	"brcm,avs-cpu-l2-intr"
#define BRCM_AVS_HOST_INTR	"sw_intr"

struct pmap {
	unsigned mode;
	unsigned p1;
	unsigned p2;
};

struct private_data {
	void __iomem *base;
	void __iomem *avs_intr_base;
	void __iomem *host_intr_base;
#ifdef CONFIG_ARM_BRCM_AVS_CPUFREQ_DEBUG
	struct dentry *debugfs;
#endif
	spinlock_t lock;
	struct pmap pmap;
};

#ifdef CONFIG_ARM_BRCM_AVS_CPUFREQ_DEBUG

enum debugfs_format {
	DEBUGFS_NORMAL,
	DEBUGFS_FLOAT,
	DEBUGFS_REV,
};

struct debugfs_data {
	struct debugfs_entry *entry;
	struct private_data *priv;
};

struct debugfs_entry {
	char *name;
	u32 offset;
	fmode_t mode;
	enum debugfs_format format;
};

#define DEBUGFS_ENTRY(name, mode, format)	{ \
	#name, AVS_MBOX_##name, mode, format \
}

/*
 * These are used for debugfs only. Otherwise we use AVS_MBOX_PARAM() directly.
 */
#define AVS_MBOX_PARAM1		AVS_MBOX_PARAM(0)
#define AVS_MBOX_PARAM2		AVS_MBOX_PARAM(1)
#define AVS_MBOX_PARAM3		AVS_MBOX_PARAM(2)
#define AVS_MBOX_PARAM4		AVS_MBOX_PARAM(3)

/*
 * This table stores the name, access permissions and offset for each hardware
 * register and is used to generate debugfs entries.
 */
static struct debugfs_entry debugfs_entries[] = {
	DEBUGFS_ENTRY(COMMAND, S_IWUSR, DEBUGFS_NORMAL),
	DEBUGFS_ENTRY(STATUS, S_IWUSR, DEBUGFS_NORMAL),
	DEBUGFS_ENTRY(VOLTAGE0, 0, DEBUGFS_FLOAT),
	DEBUGFS_ENTRY(TEMP0, 0, DEBUGFS_FLOAT),
	DEBUGFS_ENTRY(PV0, 0, DEBUGFS_FLOAT),
	DEBUGFS_ENTRY(MV0, 0, DEBUGFS_FLOAT),
	DEBUGFS_ENTRY(PARAM1, S_IWUSR, DEBUGFS_NORMAL),
	DEBUGFS_ENTRY(PARAM2, S_IWUSR, DEBUGFS_NORMAL),
	DEBUGFS_ENTRY(PARAM3, S_IWUSR, DEBUGFS_NORMAL),
	DEBUGFS_ENTRY(PARAM4, S_IWUSR, DEBUGFS_NORMAL),
	DEBUGFS_ENTRY(REVISION, 0, DEBUGFS_REV),
	DEBUGFS_ENTRY(PSTATE, 0, DEBUGFS_NORMAL),
	DEBUGFS_ENTRY(HEARTBEAT, 0, DEBUGFS_NORMAL),
	DEBUGFS_ENTRY(MAGIC, S_IWUSR, DEBUGFS_NORMAL),
	DEBUGFS_ENTRY(SIGMA_HVT, 0, DEBUGFS_NORMAL),
	DEBUGFS_ENTRY(SIGMA_SVT, 0, DEBUGFS_NORMAL),
	DEBUGFS_ENTRY(VOLTAGE1, 0, DEBUGFS_FLOAT),
	DEBUGFS_ENTRY(TEMP1, 0, DEBUGFS_FLOAT),
	DEBUGFS_ENTRY(PV1, 0, DEBUGFS_FLOAT),
	DEBUGFS_ENTRY(MV1, 0, DEBUGFS_FLOAT),
	DEBUGFS_ENTRY(FREQUENCY, 0, DEBUGFS_NORMAL),
};

static int brcm_avs_target_index(struct cpufreq_policy *policy, unsigned index);

static char *__strtolower(char *s)
{
	char *p = s;

	while (*p != '\0') {
		*p = tolower(*p);
		p++;
	}

	return s;
}

#endif /* CONFIG_ARM_BRCM_AVS_CPUFREQ_DEBUG */

static void __iomem *__map_region(const char *name)
{
	struct device_node *np;
	void __iomem *ptr;

	np = of_find_compatible_node(NULL, NULL, name);
	if (!np)
		return NULL;
	ptr = of_iomap(np, 0);
	if (!ptr)
		return NULL;

	return ptr;
}

static int __issue_avs_command(struct private_data *priv, int cmd, bool is_send,
	u32 args[])
{
	void __iomem *base = priv->base;
	unsigned long flags;
	int ret = 0;
	unsigned i;
	u32 val;

	spin_lock_irqsave(&priv->lock, flags);
	/*
	 * Make sure no other command is currently running: cmd is 0 if AVS
	 * co-processor is idle.
	 */
	for (i = 0, val = 1; val != 0 && i < AVS_LOOP_LIMIT; i++)
		val = readl(base + AVS_MBOX_COMMAND);
	/* Give the caller a chance to retry if AVS is busy. */
	if (i >= AVS_LOOP_LIMIT) {
		ret = -EAGAIN;
		goto out;
	}

	/* Clear status before we begin. */
	writel(AVS_STATUS_CLEAR, base + AVS_MBOX_STATUS);

	/* We need to send arguments for this command. */
	if (args && is_send)
		for (i = 0; i < AVS_MAX_CMD_ARGS; i++)
			writel(args[i], base + AVS_MBOX_PARAM(i));

	/* Now issue the command. */
	writel(cmd, base + AVS_MBOX_COMMAND);
	/* Tell firmware to wake-up. */
	writel(AVS_CPU_L2_INT_MASK, priv->avs_intr_base + AVS_CPU_L2_SET0);

	/*
	 * Wait for AVS co-processor to finish processing the command. Status
	 * will be non-0 (and not greater than AVS_STATUS_MAX) once it's ready.
	 */
	for (i = val = 0; (val == 0 || val > AVS_STATUS_MAX) &&
					i < AVS_LOOP_LIMIT; i++)
		val = readl(base + AVS_MBOX_STATUS);
	if (i >= AVS_LOOP_LIMIT) {
		ret = -ETIMEDOUT;
		goto out;
	}

	/* This command returned arguments, so we read them back. */
	if (args && !is_send)
		for (i = 0; i < AVS_MAX_CMD_ARGS; i++)
			args[i] = readl(base + AVS_MBOX_PARAM(i));

	/* Clear status to tell AVS co-processor we are done. */
	writel(AVS_STATUS_CLEAR, base + AVS_MBOX_STATUS);

	/* Convert firmware errors to errno's as much as possible. */
	switch (val) {
	case AVS_STATUS_INVALID:
		ret = -EINVAL;
		break;
	case AVS_STATUS_NO_SUPP:
		ret = -ENOTSUPP;
		break;
	case AVS_STATUS_NO_MAP:
		ret = -ENOENT;
		break;
	case AVS_STATUS_MAP_SET:
		ret = -EEXIST;
		break;
	case AVS_STATUS_FAILURE:
		ret = -EIO;
		break;
	}

out:
	spin_unlock_irqrestore(&priv->lock, flags);

	return ret;
}

static irqreturn_t irq_handler(int irq, void *data)
{
	return IRQ_HANDLED;
}

static char *brcm_avs_mode_to_string(unsigned mode)
{
	switch (mode) {
	case AVS_MODE_AVS:
		return "AVS";
	case AVS_MODE_DFS:
		return "DFS";
	case AVS_MODE_DVS:
		return "DVS";
	case AVS_MODE_DVFS:
		return "DVFS";
	}
	return NULL;
}

static void brcm_avs_parse_p1(u32 p1, unsigned *mdiv_p0, unsigned *pdiv,
				unsigned *ndiv)
{
	*mdiv_p0 = (p1 >> MDIV_P0_SHIFT) & MDIV_P0_MASK;
	*pdiv = (p1 >> PDIV_SHIFT) & PDIV_MASK;
	*ndiv = (p1 >> NDIV_INT_SHIFT) & NDIV_INT_MASK;
}

static void brcm_avs_parse_p2(u32 p2, unsigned *mdiv_p1, unsigned *mdiv_p2,
			unsigned *mdiv_p3, unsigned *mdiv_p4)
{
	*mdiv_p4 = (p2 >> MDIV_P4_SHIFT) & MDIV_P4_MASK;
	*mdiv_p3 = (p2 >> MDIV_P3_SHIFT) & MDIV_P3_MASK;
	*mdiv_p2 = (p2 >> MDIV_P2_SHIFT) & MDIV_P2_MASK;
	*mdiv_p1 = (p2 >> MDIV_P1_SHIFT) & MDIV_P1_MASK;
}

static int brcm_avs_get_pmap(struct private_data *priv, struct pmap *pmap)
{
	u32 args[AVS_MAX_CMD_ARGS];
	int ret;

	ret = __issue_avs_command(priv, AVS_CMD_GET_PMAP, false, args);
	if (ret || !pmap)
		return ret;

	pmap->mode = args[0];
	pmap->p1 = args[1];
	pmap->p2 = args[2];

	return 0;
}

static int brcm_avs_set_pmap(struct private_data *priv, struct pmap *pmap)
{
	u32 args[AVS_MAX_CMD_ARGS];

	args[0] = pmap->mode;
	args[1] = pmap->p1;
	args[2] = pmap->p2;

	return __issue_avs_command(priv, AVS_CMD_SET_PMAP, true, args);
}

static int brcm_avs_get_pstate(struct private_data *priv, unsigned *pstate)
{
	u32 args[AVS_MAX_CMD_ARGS];
	int ret;

	ret = __issue_avs_command(priv, AVS_CMD_GET_PSTATE, false, args);
	if (ret)
		return ret;
	*pstate = args[0];

	return 0;
}

static int brcm_avs_set_pstate(struct private_data *priv, unsigned pstate)
{
	u32 args[AVS_MAX_CMD_ARGS];

	args[0] = pstate;
	return __issue_avs_command(priv, AVS_CMD_SET_PSTATE, true, args);
}

/*
 * TODO: This function will become brcm_avs_get_frequency() once the newest
 * AVS firmware is ready.
 */
static unsigned long brcm_avs_get_frequency1(void __iomem *base)
{
	return readl(base + AVS_MBOX_FREQUENCY);
}

static unsigned long brcm_avs_get_frequency(struct private_data *priv,
						unsigned pstate)
{
#define REF_CLK_FREQ	54
	/*
	 * TODO: replace this function with the simple one-liner above.
	 */
	unsigned mdiv_p0, mdiv_p1, mdiv_p2, mdiv_p3, mdiv_p4;
	unsigned pdiv, mdiv, ndiv;
	unsigned vco, freq;
	struct pmap pmap;
	int ret;

	ret = brcm_avs_get_pmap(priv, &pmap);
	if (ret)
		return ret;

	brcm_avs_parse_p1(pmap.p1, &mdiv_p0, &pdiv, &ndiv);
	brcm_avs_parse_p2(pmap.p2, &mdiv_p1, &mdiv_p2, &mdiv_p3, &mdiv_p4);
	switch (pstate) {
	case AVS_PSTATE_P0:
		mdiv = mdiv_p0;
		break;
	case AVS_PSTATE_P1:
		mdiv = mdiv_p1;
		break;
	case AVS_PSTATE_P2:
		mdiv = mdiv_p2;
		break;
	case AVS_PSTATE_P3:
		mdiv = mdiv_p3;
		break;
	case AVS_PSTATE_P4:
		mdiv = mdiv_p4;
		break;
	}

	vco = (REF_CLK_FREQ / pdiv) * ndiv;
	freq = (vco / mdiv) * 1000;	/* in kHz */

	return freq;

#undef REF_CLK_FREQ
}


/*
 * We determine which frequencies are supported by cycling through all P-states
 * and reading back what frequency we are running at for each P-state.
 */
static struct cpufreq_frequency_table *
brcm_avs_get_freq_table(struct device *dev, struct private_data *priv)
{
	struct cpufreq_frequency_table *table;
	unsigned pstate;
	int i, ret;

	/* Remember P-state for later */
	ret = brcm_avs_get_pstate(priv, &pstate);
	if (ret)
		return ERR_PTR(ret);

	table = devm_kzalloc(dev, (AVS_PSTATE_MAX + 1) * sizeof(*table),
			GFP_KERNEL);
	if (!table)
		return ERR_PTR(-ENOMEM);

	for (i = AVS_PSTATE_P0; i <= AVS_PSTATE_MAX; i++) {
		ret = brcm_avs_set_pstate(priv, i);
		if (ret)
			return ERR_PTR(ret);
		table[i].frequency = brcm_avs_get_frequency(priv, i);
		table[i].driver_data = i;
	}
	table[i].frequency = CPUFREQ_TABLE_END;
	table[i].driver_data = i;

	/* Restore P-state */
	ret = brcm_avs_set_pstate(priv, pstate);
	if (ret)
		return ERR_PTR(ret);

	return table;
}

#ifdef CONFIG_ARM_BRCM_AVS_CPUFREQ_DEBUG

#define MANT(x)	(unsigned)(abs((x)) / 1000)
#define FRAC(x)	(unsigned)(abs((x)) - abs((x)) / 1000 * 1000)

static int brcm_avs_debug_show(struct seq_file *s, void *data)
{
	struct debugfs_data *dbgfs = s->private;
	void __iomem *base;
	u32 val, offset;

	if (!dbgfs) {
		seq_puts(s, "No device pointer\n");
		return 0;
	}

	base = dbgfs->priv->base;
	offset = dbgfs->entry->offset;
	val = readl(base + offset);
	switch (dbgfs->entry->format) {
	case DEBUGFS_NORMAL:
		break;
	case DEBUGFS_FLOAT:
		seq_printf(s, "%d.%03d\n", MANT(val), FRAC(val));
		break;
	case DEBUGFS_REV:
		seq_printf(s, "%c.%c.%c.%c\n", (val >> 24 & 0xff),
			(val >> 16 & 0xff), (val >> 8 & 0xff),
			val & 0xff);
		break;
	}
	seq_printf(s, "0x%08x\n", val);

	return 0;
}

#undef MANT
#undef FRAC

static ssize_t brcm_avs_seq_write(struct file *file, const char __user *buf,
	size_t size, loff_t *ppos)
{
	struct seq_file *s = file->private_data;
	struct debugfs_data *dbgfs = s->private;
	void __iomem *base, *avs_intr_base;
	bool use_issue_command = false;
	unsigned long val, offset;
	char str[128];
	int ret;
	char *str_ptr = str;

	if (size >= sizeof(str))
		return -E2BIG;

	memset(str, 0, sizeof(str));
	ret = copy_from_user(str, buf, size);
	if (ret)
		return ret;

	base = dbgfs->priv->base;
	avs_intr_base = dbgfs->priv->avs_intr_base;
	offset = dbgfs->entry->offset;
	/*
	 * Special case writing to "command" entry only: if the string starts
	 * with a 'c', we use the driver's __issue_avs_command() function.
	 * Otherwise, we perform a raw write. This should allow testing of raw
	 * access as well as using the higher level function. (Raw access
	 * doesn't clear the firmware return status after issuing the command.)
	 */
	if (str_ptr[0] == 'c' && offset == AVS_MBOX_COMMAND) {
		use_issue_command = true;
		str_ptr++;
	}
	if (kstrtoul(str_ptr, 0, &val) != 0)
		return -EINVAL;

	if (use_issue_command) {
		/*
		 * Setting the P-state is a special case. We need to update the
		 * CPU frequency we report.
		 */
		if (val == AVS_CMD_SET_PSTATE) {
			struct cpufreq_policy *policy;
			unsigned pstate;

			policy = cpufreq_cpu_get(smp_processor_id());
			/* Read back the P-state we are about to set */
			pstate = readl(base + AVS_MBOX_PARAM(0));
			ret = brcm_avs_target_index(policy, pstate);
		} else {
			ret = __issue_avs_command(dbgfs->priv, val, false,
						NULL);
		}
	} else {
		/*
		 * BEWARE: using this "raw access" code path to set the P-state
		 * will *NOT* update the frequency reported by the system. We
		 * don't perform any error checking regarding the AVS return
		 * code, nor do we interpret and "smartly" handle commands. We
		 * simply process each instruction individually as provided by
		 * userland and without context.
		 */
		writel(val, base + offset);
		/* We have to wake up the firmware to process a command. */
		if (offset == AVS_MBOX_COMMAND)
			writel(AVS_CPU_L2_INT_MASK,
				avs_intr_base + AVS_CPU_L2_SET0);
	}

	return size;
}

static struct debugfs_entry *__find_debugfs_entry(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(debugfs_entries); i++)
		if (strcasecmp(debugfs_entries[i].name, name) == 0)
			return &debugfs_entries[i];

	return NULL;
}

static int brcm_avs_debug_open(struct inode *inode, struct file *file)
{
	struct debugfs_data *data;
	fmode_t fmode;
	int ret;

	/*
	 * seq_open(), which is called by single_open(), clears "write" access.
	 * We need write access to some files, so we preserve our access mode
	 * and restore it.
	 */
	fmode = file->f_mode;
	/*
	 * Check access permissions even for root. We don't want to be writing
	 * to read-only registers. Access for regular users has already been
	 * checked by the VFS layer.
	 */
	if ((fmode & FMODE_WRITER) && !(inode->i_mode & S_IWUSR))
		return -EACCES;

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	/*
	 * We use the same file system operations for all our debug files. To
	 * produce specific output, we look up the file name upon opening a
	 * debugfs entry and map it to a memory offset. This offset is then used
	 * in the generic "show" function to read a specific register.
	 */
	data->entry = __find_debugfs_entry(file->f_path.dentry->d_iname);
	data->priv = inode->i_private;

	ret = single_open(file, brcm_avs_debug_show, data);
	if (ret)
		kfree(data);
	file->f_mode = fmode;

	return ret;
}

static int brcm_avs_debug_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq_priv = file->private_data;
	struct debugfs_data *data = seq_priv->private;

	kfree(data);
	return single_release(inode, file);
}

static const struct file_operations brcm_avs_debug_ops = {
	.open		= brcm_avs_debug_open,
	.read		= seq_read,
	.write		= brcm_avs_seq_write,
	.llseek		= seq_lseek,
	.release	= brcm_avs_debug_release,
};

static void brcm_avs_cpufreq_debug_init(struct platform_device *pdev)
{
	struct private_data *priv = platform_get_drvdata(pdev);
	struct dentry *dir;
	int i;

	if (!priv)
		return;

	dir = debugfs_create_dir(BRCM_AVS_CPUFREQ_NAME, NULL);
	if (IS_ERR_OR_NULL(dir))
		return;
	priv->debugfs = dir;

	for (i = 0; i < ARRAY_SIZE(debugfs_entries); i++) {
		/*
		 * The DEBUGFS_ENTRY macro generates uppercase strings. We
		 * convert them to lowercase before creating the debugfs
		 * entries.
		 */
		char *entry = __strtolower(debugfs_entries[i].name);
		fmode_t mode = debugfs_entries[i].mode;

		if (!debugfs_create_file(entry, S_IFREG | S_IRUGO | mode,
				dir, priv, &brcm_avs_debug_ops)) {
			priv->debugfs = NULL;
			debugfs_remove_recursive(dir);
			break;
		}
	}
}

static void brcm_avs_cpufreq_debug_exit(struct platform_device *pdev)
{
	struct private_data *priv = platform_get_drvdata(pdev);

	if (priv && priv->debugfs) {
		debugfs_remove_recursive(priv->debugfs);
		priv->debugfs = NULL;
	}
}

#else

static void brcm_avs_cpufreq_debug_init(struct platform_device *pdev) {}
static void brcm_avs_cpufreq_debug_exit(struct platform_device *pdev) {}

#endif /* CONFIG_ARM_BRCM_AVS_CPUFREQ_DEBUG */

/*
 * To ensure the right firmware is running we need to
 *    - check the MAGIC matches what we expect
 *    - brcm_avs_get_pmap() doesn't return -ENOTSUPP
 */
static bool brcm_avs_is_firmware_loaded(struct private_data *priv)
{
	u32 magic;
	int rc;

	rc = brcm_avs_get_pmap(priv, NULL);
	magic = readl(priv->base + AVS_MBOX_MAGIC);

	return (magic == AVS_FIRMWARE_MAGIC) && (rc != -ENOTSUPP);
}

static unsigned brcm_avs_cpufreq_get(unsigned cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

	return policy->cur;
}

static int brcm_avs_target_index(struct cpufreq_policy *policy, unsigned index)
{
	int ret;

	ret = brcm_avs_set_pstate(policy->driver_data,
		policy->freq_table[index].driver_data);
	if (ret)
		return ret;

	policy->cur = policy->freq_table[index].frequency;
	return 0;
}

static int brcm_avs_suspend(struct cpufreq_policy *policy)
{
	struct private_data *priv = policy->driver_data;

	return brcm_avs_get_pmap(priv, &priv->pmap);
}

static int brcm_avs_resume(struct cpufreq_policy *policy)
{
	struct private_data *priv = policy->driver_data;
	int ret;

	ret = brcm_avs_set_pmap(priv, &priv->pmap);
	if (ret == -EEXIST) {
		struct platform_device *pdev  = cpufreq_get_driver_data();
		struct device *dev = &pdev->dev;

		dev_warn(dev, "PMAP was already set\n");
		ret = 0;
	}

	return ret;
}

static int brcm_avs_cpu_init(struct cpufreq_policy *policy)
{
	struct cpufreq_frequency_table *freq_table;
	struct platform_device *pdev;
	struct private_data *priv;
	struct device *dev;
	int host_irq;
	int ret;

	pdev = cpufreq_get_driver_data();
	dev = &pdev->dev;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = __map_region(BRCM_AVS_CPU_DATA);
	if (!priv->base) {
		dev_err(dev, "Couldn't find property %s in device tree.\n",
			BRCM_AVS_CPU_DATA);
		return -ENOENT;
	}

	priv->avs_intr_base = __map_region(BRCM_AVS_CPU_INTR);
	if (!priv->avs_intr_base) {
		dev_err(dev, "Couldn't find property %s in device tree.\n",
			BRCM_AVS_CPU_INTR);
		return -ENOENT;
	}

	/* It is not an error if this property isn't present. */
	host_irq = platform_get_irq_byname(pdev, BRCM_AVS_HOST_INTR);
	if (host_irq >= 0) {
		ret = devm_request_irq(dev, host_irq, irq_handler,
				IRQF_TRIGGER_RISING, BRCM_AVS_HOST_INTR, NULL);
		if (ret) {
			dev_err(dev, "IRQ request failed: %s (%d) -- %d\n",
				BRCM_AVS_HOST_INTR, host_irq, ret);
			host_irq = -1;
		}
	}

	if (!brcm_avs_is_firmware_loaded(priv)) {
		dev_err(dev,
			"AVS firmware is not loaded or doesn't support DVFS\n");
		return -ENODEV;
	}

	freq_table = brcm_avs_get_freq_table(dev, priv);
	if (IS_ERR(freq_table)) {
		dev_err(dev, "Couldn't determine frequency table (%ld).\n",
			PTR_ERR(freq_table));
		return PTR_ERR(freq_table);
	}

	ret = cpufreq_table_validate_and_show(policy, freq_table);
	if (ret) {
		dev_err(dev, "invalid frequency table: %d\n", ret);
		return ret;
	}

	policy->driver_data = priv;
	spin_lock_init(&priv->lock);
	platform_set_drvdata(pdev, priv);

	/* All cores share the same clock and thus the same policy. */
	cpumask_setall(policy->cpus);

	ret = __issue_avs_command(priv, AVS_CMD_ENABLE, false, NULL);
	if (!ret) {
		unsigned pstate;

		ret = brcm_avs_get_pstate(priv, &pstate);
		if (!ret) {
			policy->cur = freq_table[pstate].frequency;
			dev_info(dev, "registered\n");
		}
	}
	if (ret)
		dev_err(dev, "couldn't initialize driver (%d)\n", ret);

	return ret;
}

static int brcm_avs_cpu_exit(struct cpufreq_policy *policy)
{
	/*
	 * All our allocations are "managed", so we don't need to do
	 * anything.
	 */
	return 0;
}

static ssize_t show_brcm_avs_pstate(struct cpufreq_policy *policy,
					char *buf)
{
	struct private_data *priv = policy->driver_data;
	unsigned pstate;

	if (brcm_avs_get_pstate(priv, &pstate))
		return sprintf(buf, "<unknown>\n");

	return sprintf(buf, "%u\n", pstate);
}

static ssize_t show_brcm_avs_mode(struct cpufreq_policy *policy,
					char *buf)
{
	struct private_data *priv = policy->driver_data;
	struct pmap pmap;

	if (brcm_avs_get_pmap(priv, &pmap))
		return sprintf(buf, "<unknown>\n");

	return sprintf(buf, "%s %u\n", brcm_avs_mode_to_string(pmap.mode),
		pmap.mode);
}

static ssize_t show_brcm_avs_pmap(struct cpufreq_policy *policy,
					char *buf)
{
	unsigned mdiv_p0, mdiv_p1, mdiv_p2, mdiv_p3, mdiv_p4;
	struct private_data *priv = policy->driver_data;
	unsigned ndiv, pdiv;
	struct pmap pmap;

	if (brcm_avs_get_pmap(priv, &pmap))
		return sprintf(buf, "<unknown>\n");

	brcm_avs_parse_p1(pmap.p1, &mdiv_p0, &pdiv, &ndiv);
	brcm_avs_parse_p2(pmap.p2, &mdiv_p1, &mdiv_p2, &mdiv_p3, &mdiv_p4);

	return sprintf(buf, "0x%08x 0x%08x %u %u %u %u %u %u %u\n",
		pmap.p1, pmap.p2, ndiv, pdiv, mdiv_p0, mdiv_p1, mdiv_p2,
		mdiv_p3, mdiv_p4);
}

cpufreq_freq_attr_ro(brcm_avs_pstate);
cpufreq_freq_attr_ro(brcm_avs_mode);
cpufreq_freq_attr_ro(brcm_avs_pmap);

struct freq_attr *brcm_avs_cpufreq_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	&brcm_avs_pstate,
	&brcm_avs_mode,
	&brcm_avs_pmap,
	NULL
};

static struct cpufreq_driver brcm_avs_driver = {
	.flags		= CPUFREQ_NEED_INITIAL_FREQ_CHECK,
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= brcm_avs_target_index,
	.get		= brcm_avs_cpufreq_get,
	.suspend	= brcm_avs_suspend,
	.resume		= brcm_avs_resume,
	.init		= brcm_avs_cpu_init,
	.exit		= brcm_avs_cpu_exit,
	.name		= BRCM_AVS_CPUFREQ_NAME,
	.attr		= brcm_avs_cpufreq_attr,
};

static int brcm_avs_cpufreq_probe(struct platform_device *pdev)
{
	int ret;

	brcm_avs_driver.driver_data = pdev;
	ret = cpufreq_register_driver(&brcm_avs_driver);
	if (!ret)
		brcm_avs_cpufreq_debug_init(pdev);

	return ret;
}

static int brcm_avs_cpufreq_remove(struct platform_device *pdev)
{
	brcm_avs_cpufreq_debug_exit(pdev);
	return cpufreq_unregister_driver(&brcm_avs_driver);
}

static const struct of_device_id brcm_avs_cpufreq_match[] = {
	{ .compatible = BRCM_AVS_CPU_DATA },
	{ }
};
MODULE_DEVICE_TABLE(of, brcm_avs_cpufreq_match);

static struct platform_driver brcm_avs_cpufreq_platdrv = {
	.driver = {
		.name	= BRCM_AVS_CPUFREQ_NAME,
		.of_match_table = brcm_avs_cpufreq_match,
	},
	.probe		= brcm_avs_cpufreq_probe,
	.remove		= brcm_avs_cpufreq_remove,
};
module_platform_driver(brcm_avs_cpufreq_platdrv);

MODULE_AUTHOR("Markus Mayer <mmayer@broadcom.com>");
MODULE_DESCRIPTION("CPUfreq driver for Broadcom AVS");
MODULE_LICENSE("GPL");