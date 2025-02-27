// SPDX-License-Identifier: GPL-2.0
/*
 * spacemit-k1x watchdog driver
 *
 * Copyright (C) 2023 Spacemit
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/timer.h>
#include <linux/miscdevice.h> /* for MODULE_ALIAS_MISCDEV */
#include <linux/watchdog.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <asm/cacheflush.h>

#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/reboot.h>

/* Watchdog Timer Registers Offset */
#define WDT_WMER	(0x00b8)
#define WDT_WMR		(0x00bc)
#define WDT_WVR		(0x00cc)
#define WDT_WCR		(0x00c8)
#define WDT_WSR		(0x00c0)
#define WDT_WFAR	(0x00b0)
#define WDT_WSAR	(0x00b4)
#define WDT_WICR	(0x00c4)

#define CONFIG_SPACEMIT_WATCHDOG_ATBOOT		(0)
/* default timeout is 60s */
#define CONFIG_SPACEMIT_WATCHDOG_DEFAULT_TIME	(60)
#define SPACEMIT_WATCHDOG_MAX_TIMEOUT		(255)
#define SPACEMIT_WATCHDOG_EXPIRE_TIME		(100)
/* touch watchdog every 30s */
#define SPACEMIT_WATCHDOG_FEED_TIMEOUT	(30)

#ifdef CONFIG_K1X_WDT_TEST
#define K1X_WATCHDOG_IRQ_EXPIRE_TIME	(16)
#define K1X_WATCHDOG_IRQ_TEST_TIME	(64)
#define K1X_WATCHDOG_IRQ_TEST_ID	0
#define K1X_WATCHDOG_RESET_TEST_ID	1
#endif

#define MPMU_APRR		(0x1020)
#define MPMU_APRR_WDTR	(1<<4)
#define DEFAULT_SHIFT (8)
/*
 * MPMU_APSR is a dummy reg which is used to handle reboot
 * cmds. Its layout is:
 *	bit0~7:   untouchable
 *	bit8~11:  set to 0x1 when normal boot with no parameter
 *                set to 0x5 for other valid cmds.
 */
#define MPMU_ARSR		(0x1028)
#define MPMU_ARSR_REBOOT_CMD(x)	((x) << 8)
#define MPMU_ARSR_SWR_MASK	(0xf << 8)
#define REBOOT_CMD_NORMAL	0x1
#define REBOOT_CMD_VALID	0x5

static bool nowayout	= WATCHDOG_NOWAYOUT ? true : false;
static spinlock_t reboot_lock;
static DEFINE_MUTEX(wdt_clk_lock);

phys_addr_t reboot_cmd_mem = 0;
uint32_t reboot_cmd_size = 0;

#ifdef CONFIG_K1X_WDT_TEST
static int wdt_irq_count;
#endif

module_param(nowayout,   bool, 0);

MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
			__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

static struct spa_wdt_info *syscore_info;
struct spa_wdt_info {
	void __iomem	*wdt_base;
	void __iomem	*mpmu_base;
	struct device *dev;
	struct clk *clk;
	struct reset_control *reset;
	struct hrtimer feed_timer;
	ktime_t feed_timeout;
	spinlock_t wdt_lock;
	struct watchdog_device wdt_dev;
	int ctrl;
	bool wdt_clk_open;
	int enable_restart_handler;
	struct notifier_block restart_handler;
};

void spa_wdt_shutdown_reason(char *cmd)
{
	void __iomem *mpmu_arsr;
	u32 reg;

	if (!syscore_info) {
		pr_err("syscore_info not ready\n");
		return;
	}

	if (!reboot_cmd_mem || !reboot_cmd_size) {
		pr_err("No reboot cmd buffer reserved, cmd omitted!\n");
		cmd = NULL;
	}

	if (cmd) {
		if ((strlen(cmd) + 1) > reboot_cmd_size) {
			pr_err("Reboot cmd len(%lu bytes) oversizes reserved mem (%d bytes), \
				cmd omitted!\n", strlen(cmd) + 1, reboot_cmd_size);
			cmd = NULL;
		} else {
			/* save cmd to reserved memory */
			memcpy(phys_to_virt(reboot_cmd_mem), cmd, strlen(cmd) + 1);
			printk("cmd = %s\n", cmd);
		}
	}

	//set reboot flag
	mpmu_arsr = syscore_info->mpmu_base + MPMU_ARSR;
	reg = readl(mpmu_arsr);
	reg &= ~MPMU_ARSR_SWR_MASK;
	if (!cmd)
		reg |= MPMU_ARSR_REBOOT_CMD(REBOOT_CMD_NORMAL);
	else
		reg |= MPMU_ARSR_REBOOT_CMD(REBOOT_CMD_VALID);
	writel(reg, mpmu_arsr);
}
EXPORT_SYMBOL(spa_wdt_shutdown_reason);

static inline u32 spa_wdt_read(struct spa_wdt_info *info,
				unsigned reg)
{
	return readl(info->wdt_base + reg);
}

static inline void spa_wdt_write_access(struct spa_wdt_info *info)
{
	writel(0xbaba, info->wdt_base + WDT_WFAR);
	writel(0xeb10, info->wdt_base + WDT_WSAR);
}

static inline void spa_wdt_write(struct spa_wdt_info *info,
				unsigned reg, u32 val)
{
	spa_wdt_write_access(info);
	writel(val, info->wdt_base + reg);
}

static int spa_wdt_set_timeout(struct watchdog_device *wdd, unsigned timeout)
{
	struct spa_wdt_info *info =
		container_of(wdd, struct spa_wdt_info, wdt_dev);
	/*
	 * the wdt timer is 16 bit,
	 * frequence is 256HZ
	 */
	unsigned int tick = timeout << DEFAULT_SHIFT;
	if ((long long)tick > 0xffff) {
		dev_info(info->dev, "use default value!\n");
		timeout = SPACEMIT_WATCHDOG_MAX_TIMEOUT;
		tick = timeout << DEFAULT_SHIFT;
	}

	spa_wdt_write(info, WDT_WMR, tick);

	wdd->timeout = timeout;

	return 0;
}

static void spa_enable_wdt_clk(struct spa_wdt_info *info)
{
	mutex_lock(&wdt_clk_lock);
	if (!info->wdt_clk_open) {
		clk_prepare_enable(info->clk);
		reset_control_deassert(info->reset);
		info->wdt_clk_open = true;
	}
	mutex_unlock(&wdt_clk_lock);
}

static void spa_disable_wdt_clk(struct spa_wdt_info *info)
{
	mutex_lock(&wdt_clk_lock);
	if (info->wdt_clk_open) {
		clk_disable_unprepare(info->clk);
		reset_control_assert(info->reset);
		info->wdt_clk_open = false;
	}
	mutex_unlock(&wdt_clk_lock);
}

static int spa_wdt_stop(struct watchdog_device *wdd)
{
	struct spa_wdt_info *info =
		container_of(wdd, struct spa_wdt_info, wdt_dev);
	spin_lock(&info->wdt_lock);
	dev_dbg(info->dev, "cnt = 0x%x , match = 0x%x\n", spa_wdt_read(info, WDT_WVR), spa_wdt_read(info, WDT_WMR));

	/* reset counter */
	spa_wdt_write(info, WDT_WCR, 0x1);

	/* disable WDT */
	spa_wdt_write(info, WDT_WMER, 0x0);

	spin_unlock(&info->wdt_lock);

	msleep(3);

	spa_disable_wdt_clk(info);

	return 0;
}

static int spa_wdt_start(struct watchdog_device *wdd)
{
	struct spa_wdt_info *info =
		container_of(wdd, struct spa_wdt_info, wdt_dev);
	void __iomem *mpmu_aprr;
	u32 reg;

	spa_enable_wdt_clk(info);

	spin_lock(&info->wdt_lock);

	/* set timeout = 100s */
	spa_wdt_set_timeout(&info->wdt_dev,
		SPACEMIT_WATCHDOG_EXPIRE_TIME);

	/* enable counter and reset/interrupt */
	spa_wdt_write(info, WDT_WMER, 0x3);

	/* negate hardware reset to the WDT after system reset */
	mpmu_aprr = info->mpmu_base + MPMU_APRR;
	reg = readl(mpmu_aprr);
	reg |= MPMU_APRR_WDTR;
	writel(reg, mpmu_aprr);

	/* clear previous WDT status */
	spa_wdt_write(info, WDT_WSR, 0x0);

	spin_unlock(&info->wdt_lock);

	return 0;
}

#ifdef CONFIG_K1X_WDT_TEST
static int spa_wdt_start_irq(struct watchdog_device *wdd)
{
	struct spa_wdt_info *info =container_of(wdd, struct spa_wdt_info, wdt_dev);

	spa_enable_wdt_clk(info);

	spin_lock(&info->wdt_lock);

	/* set timeout = 2/256 s */
	spa_wdt_set_timeout(&info->wdt_dev, K1X_WATCHDOG_IRQ_EXPIRE_TIME);
	/* enable counter and reset/interrupt */
	spa_wdt_write(info, WDT_WMER, 0x1);

	spin_unlock(&info->wdt_lock);

	return 0;
}

static void spa_wdt_stop_irq(struct watchdog_device *wdd)
{
	struct spa_wdt_info *info =container_of(wdd, struct spa_wdt_info, wdt_dev);

	spin_lock(&info->wdt_lock);

	/* reset counter */
	spa_wdt_write(info, WDT_WCR, 0x1);

	/* disable WDT */
	spa_wdt_write(info, WDT_WMER, 0x0);

	spin_unlock(&info->wdt_lock);

	msleep(3);

	spa_disable_wdt_clk(info);
}
#endif

static int spa_wdt_ping(struct watchdog_device *wdd)
{
	int ret = 0;

	struct spa_wdt_info *info =
		container_of(wdd, struct spa_wdt_info, wdt_dev);

	spin_lock(&reboot_lock);
	spin_lock(&info->wdt_lock);

	/* reset counter */
	if (wdd->timeout > 0) {
		spa_wdt_write(info, WDT_WCR, 0x1);
	} else
		ret = -EINVAL;

	spin_unlock(&info->wdt_lock);
	spin_unlock(&reboot_lock);

	return ret;
}

#define OPTIONS (WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING)

static const struct watchdog_info spa_wdt_ident = {
	.options          =	OPTIONS,
	.firmware_version =	0,
	.identity         =	"K1X Watchdog",
};

static struct watchdog_ops spa_wdt_ops = {
	.owner = THIS_MODULE,
	.start = spa_wdt_start,
	.stop = spa_wdt_stop,
	.ping = spa_wdt_ping,
	.set_timeout = spa_wdt_set_timeout,
};

static struct watchdog_device spa_wdt = {
	.info = &spa_wdt_ident,
	.ops = &spa_wdt_ops,
};

static void spa_init_wdt(struct spa_wdt_info *info)
{

	if (info->ctrl) {
		spa_wdt_start(&info->wdt_dev);
		hrtimer_start(&info->feed_timer, info->feed_timeout, HRTIMER_MODE_REL);
	} else
		spa_wdt_stop(&info->wdt_dev);

	if (test_bit(WDOG_ACTIVE, &((info->wdt_dev).status)))
		spa_wdt_ping(&info->wdt_dev);
}

static const struct of_device_id spa_wdt_match[] = {
	{ .compatible = "spacemit,soc-wdt", .data = NULL},
	{},
};
MODULE_DEVICE_TABLE(of, spa_wdt_match);

static ssize_t wdt_ctrl_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct spa_wdt_info *info = dev_get_drvdata(dev);
	int s = 0;

	s += sprintf(buf, "wdt control: %d\n", info->ctrl);
	return s;
}

static ssize_t wdt_ctrl_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t size)
{
	struct spa_wdt_info *info = dev_get_drvdata(dev);
	ssize_t ret = 0;
	int ctrl;

	if (info == NULL) {
		pr_err("device info is empty!\n");
		return 0;
	}
	ret = sscanf(buf, "%d", &ctrl);
	if (ret == 0) {
		pr_err("sscanf() error, try again\n");
		ret = -EINVAL;
	}
	dev_info(dev, "%s: wdt control %s\n",
		 __func__, (ctrl ? "enabled" : "disabled"));
	if (ret < 0)
		return ret;

	if (ctrl && (info->ctrl == 0)) {
		spa_wdt_start(&info->wdt_dev);
		hrtimer_start(&info->feed_timer, info->feed_timeout, HRTIMER_MODE_REL);
	} else if ((ctrl == 0) && info->ctrl) {
		hrtimer_cancel(&info->feed_timer);
		spa_wdt_stop(&info->wdt_dev);
	}

	info->ctrl = ctrl;
	return size;
}
static DEVICE_ATTR(wdt_ctrl, S_IRUGO | S_IWUSR, wdt_ctrl_show, wdt_ctrl_store);

#ifdef CONFIG_K1X_WDT_TEST
static void spa_wdt_reset_test(struct spa_wdt_info *info)
{
	void __iomem *mpmu_aprr;
	unsigned long flags = 0;
	u32 reg;

	mpmu_aprr = info->mpmu_base + MPMU_APRR;

	spin_lock_irqsave(&info->wdt_lock, flags);
	spa_wdt_shutdown_reason(NULL);

	spa_wdt_write(info, WDT_WSR, 0x0);
	spa_wdt_set_timeout(&info->wdt_dev, 0);
	spa_wdt_write(info, WDT_WMER, 0x3);
	spa_wdt_write(info, WDT_WCR, 0x1);

	reg = readl(mpmu_aprr);
	reg |= MPMU_APRR_WDTR;
	writel(reg, mpmu_aprr);
	spin_unlock_irqrestore(&info->wdt_lock, flags);

	mdelay(5000);
	panic("reboot system failed");

	pr_err("reboot system failed: this line shouldn't appear.\n");
}

static void spa_wdt_irq_test(struct spa_wdt_info *info)
{
	int expected_irq_count;

	wdt_irq_count = 0;
	expected_irq_count = K1X_WATCHDOG_IRQ_TEST_TIME / K1X_WATCHDOG_IRQ_EXPIRE_TIME;

	/* avoid suspend within 15 seconds */
	pm_wakeup_event(info->dev, 15000);
	if (info->ctrl) {
		hrtimer_cancel(&info->feed_timer);
		spa_wdt_stop(&info->wdt_dev);
		info->ctrl = 0;
	}
	/* start watchdog timer with irq mode */
	spa_wdt_start_irq(&info->wdt_dev);

	mdelay((K1X_WATCHDOG_IRQ_TEST_TIME) * 1000 / 256 + 50);
	/* stop watchdog timer with irq mode */
	spa_wdt_stop_irq(&info->wdt_dev);

	if (!info->ctrl) {
		spa_wdt_start(&info->wdt_dev);
		hrtimer_start(&info->feed_timer, info->feed_timeout, HRTIMER_MODE_REL);
		info->ctrl = 1;
	}
	pr_err("irq count: expected(%d), actual(%d) %s\n", expected_irq_count, wdt_irq_count,
		(expected_irq_count == wdt_irq_count) ? "PASS" : "FAIL");
}

static ssize_t wdt_debug_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	int s = 0;

	s += sprintf(buf, "wdt irq count: %d\n", wdt_irq_count);
	return s;
}

static ssize_t wdt_debug_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t size)
{
	struct spa_wdt_info *info = dev_get_drvdata(dev);
	ssize_t ret = 0;
	int test_id;

	if (info == NULL) {
		pr_err("device info is empty!\n");
		return 0;
	}
	ret = sscanf(buf, "%d", &test_id);
	if (ret == 0) {
		pr_err("sscanf() error, try again\n");
		ret = -EINVAL;
	}

	if (test_id == K1X_WATCHDOG_IRQ_TEST_ID)
		spa_wdt_irq_test(info);
	else
		spa_wdt_reset_test(info);

	return size;
}
static DEVICE_ATTR(wdt_debug, S_IRUGO | S_IWUSR, wdt_debug_show, wdt_debug_store);

static irqreturn_t wdt_irq_handler(int irq, void *data)
{
	struct spa_wdt_info *info = data;

	wdt_irq_count++;
	/* clear irq flag */
	spin_lock(&info->wdt_lock);
	spa_wdt_write(info, WDT_WICR, 0x1);
	spa_wdt_write(info, WDT_WCR, 0x1);
	spin_unlock(&info->wdt_lock);

	return IRQ_HANDLED;
}
#endif

static int spa_wdt_restart_handler(struct notifier_block *this, unsigned long mode,
		void *cmd)
{
	struct spa_wdt_info *info = container_of(this, struct spa_wdt_info,
			restart_handler);
	void __iomem *mpmu_aprr;
	u32 reg;

	spin_lock(&reboot_lock);
	spa_wdt_shutdown_reason(cmd);

	spa_enable_wdt_clk(info);

	spa_wdt_write(info, WDT_WSR, 0x0);
	spa_wdt_set_timeout(&info->wdt_dev, 10);
	spa_wdt_write(info, WDT_WMER, 0x3);
	spa_wdt_write(info, WDT_WCR, 0x1);

	mpmu_aprr = info->mpmu_base + MPMU_APRR;
	reg = readl(mpmu_aprr);
	reg |= MPMU_APRR_WDTR;
	writel(reg, mpmu_aprr);

	mdelay(5000);
	panic("reboot system failed");
	spin_unlock(&reboot_lock);

	pr_err("reboot system failed: this line shouldn't appear.\n");
	return NOTIFY_DONE;
}

static int spa_wdt_dt_init(struct device_node *np, struct device *dev,
			    struct spa_wdt_info *info)
{
	if (info == NULL) {
		pr_err("watchdog dt is empty!\n");
		return -EINVAL;
	}
	if (of_get_property(np, "spa,wdt-disabled", NULL))
		info->ctrl = 0;
	else
		info->ctrl = 1;

	if (of_get_property(np, "spa,wdt-enable-restart-handler", NULL))
		info->enable_restart_handler = 1;
	else
		info->enable_restart_handler = 0;
	return 0;
}

static enum hrtimer_restart spa_wdt_feed(struct hrtimer *timer)
{
	struct spa_wdt_info *info = container_of(timer, struct spa_wdt_info, feed_timer);

	/* reset counter value */
	if (likely(info->ctrl)) {
		spa_wdt_ping(&info->wdt_dev);
		hrtimer_forward_now(timer, info->feed_timeout);
	} else
		return HRTIMER_NORESTART;

	return HRTIMER_RESTART;
}

static int spa_wdt_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct resource wdt_mem;
	struct resource mpmu_mem;
	void __iomem *mpmu_arsr;
	u32 reg;

	int ret;
#ifdef CONFIG_K1X_WDT_TEST
	int irq;
#endif
	static int is_wdt_reset;
	struct spa_wdt_info *info;
	info = devm_kzalloc(&pdev->dev, sizeof(struct spa_wdt_info),
			GFP_KERNEL);
	if (info == NULL) {
		dev_err(&pdev->dev, "Cannot allocate memory.\n");
		return -ENOMEM;
	}

	info->dev = &pdev->dev;

#ifdef CONFIG_K1X_WDT_TEST
	irq = platform_get_irq(pdev, 0);
	ret = devm_request_irq(&pdev->dev, irq, wdt_irq_handler,
			       IRQF_TIMER | IRQF_IRQPOLL,
			       "watchdog",
			       info);
	if (ret) {
		dev_err(&pdev->dev, "%s: Failed to request irq!\n", __func__);
		return ret;
	}
#endif

	ret = of_address_to_resource(np, 0, &wdt_mem);
	if (ret < 0) {
		dev_err(info->dev, "no memory resource specified for WDT\n");
		return -ENOENT;
	}

	info->wdt_base = devm_ioremap(&pdev->dev, wdt_mem.start,
						  resource_size(&wdt_mem));
	if (IS_ERR(info->wdt_base))
		return PTR_ERR(info->wdt_base);

	ret = of_address_to_resource(np, 1, &mpmu_mem);
	if (ret < 0) {
		dev_err(info->dev, "no memory resource specified for MPMU\n");
		return -ENOENT;
	}

	info->mpmu_base = devm_ioremap(&pdev->dev,
		mpmu_mem.start, resource_size(&mpmu_mem));
	if (IS_ERR(info->mpmu_base))
		return PTR_ERR(info->mpmu_base);

	mpmu_arsr = info->mpmu_base + MPMU_ARSR;
	reg = readl(mpmu_arsr);
	reg &= ~MPMU_ARSR_SWR_MASK;
	writel(reg, mpmu_arsr);

	/* get WDT clock */
	info->clk = devm_clk_get(info->dev, NULL);
	if (IS_ERR(info->clk)) {
		dev_err(info->dev, "failed to get WDT clock\n");
		return PTR_ERR(info->clk);
	}

	info->reset = devm_reset_control_get_optional(info->dev,NULL);
	if(IS_ERR(info->reset)) {
		dev_err(info->dev, "watchdog get reset failed\n");
		return PTR_ERR(info->reset);
	}
	/*
	 * the writing of some WDT registers must be
	 * under the condition of that WDT clock is on
	 */
	spa_enable_wdt_clk(info);

	/* check before the watchdog is initialized */
	is_wdt_reset = spa_wdt_read(info, WDT_WSR);
	if (is_wdt_reset)
		pr_info("System boots up because of SoC watchdog reset.\n");
	else
		pr_info("System boots up not because of SoC watchdog reset.\n");

	spin_lock_init(&info->wdt_lock);

	watchdog_set_nowayout(&spa_wdt, nowayout);

	info->wdt_dev = spa_wdt;
	ret = watchdog_register_device(&info->wdt_dev);
	if (ret) {
		dev_err(info->dev, "cannot register watchdog (%d)\n", ret);
		goto err_register_fail;
	}

	info->feed_timeout = ktime_set(SPACEMIT_WATCHDOG_FEED_TIMEOUT, 0);
	hrtimer_init(&info->feed_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	info->feed_timer.function = spa_wdt_feed;

	platform_set_drvdata(pdev, info);

	spa_wdt_dt_init(np, info->dev, info);

	spa_init_wdt(info);

	ret = device_create_file(info->dev, &dev_attr_wdt_ctrl);
	if (ret < 0) {
		dev_err(&pdev->dev, "device attr create fail: %d\n", ret);
		goto err_alloc;
	}

#ifdef CONFIG_K1X_WDT_TEST
	ret = device_create_file(info->dev, &dev_attr_wdt_debug);
	if (ret < 0) {
		dev_err(&pdev->dev, "device attr create fail: %d\n", ret);
		goto err_alloc;
	}
#endif

	if (info->enable_restart_handler) {
		info->restart_handler.notifier_call = spa_wdt_restart_handler;
		info->restart_handler.priority = 0;
		ret = register_restart_handler(&info->restart_handler);
		if (ret)
			dev_warn(&pdev->dev,
					"cannot register restart handler (err=%d)\n", ret);
	}

	syscore_info = info;
	return 0;

err_alloc:
	if (info->ctrl) {
		hrtimer_cancel(&info->feed_timer);
		spa_wdt_stop(&info->wdt_dev);
	}

	watchdog_unregister_device(&info->wdt_dev);
err_register_fail:
	spa_disable_wdt_clk(info);
	clk_put(info->clk);

	return ret;
}

static void spa_wdt_remove(struct platform_device *pdev)
{
	struct spa_wdt_info *info = platform_get_drvdata(pdev);

	watchdog_unregister_device(&info->wdt_dev);

	if (info->ctrl) {
		hrtimer_cancel(&info->feed_timer);
		spa_wdt_stop(&info->wdt_dev);
	}

	spa_disable_wdt_clk(info);
	clk_put(info->clk);
}

static void spa_wdt_shutdown(struct platform_device *pdev)
{
	struct spa_wdt_info *info = platform_get_drvdata(pdev);

	if (info->ctrl)
		hrtimer_cancel(&info->feed_timer);

	spa_wdt_stop(&info->wdt_dev);

	/* no need to disable clk if enable restart_handler */
	if (info->enable_restart_handler)
		spa_enable_wdt_clk(info);
}

#ifdef CONFIG_PM
static int spa_wdt_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct spa_wdt_info *info = platform_get_drvdata(pdev);

	if (info->ctrl) {
		/* turn watchdog off */
		hrtimer_cancel(&info->feed_timer);
		spa_wdt_stop(&info->wdt_dev);
	}

	return 0;
}

static int spa_wdt_resume(struct platform_device *pdev)
{
	struct spa_wdt_info *info = platform_get_drvdata(pdev);

	if (info->ctrl) {
		spa_wdt_start(&info->wdt_dev);
		hrtimer_start(&info->feed_timer, info->feed_timeout, HRTIMER_MODE_REL);
	}

	return 0;
}

#else
#define spa_wdt_suspend NULL
#define spa_wdt_resume  NULL
#endif /* CONFIG_PM */

#ifdef CONFIG_OF_RESERVED_MEM
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>

static int __init rmem_reboot_setup(struct reserved_mem *rmem)
{
	phys_addr_t mask = PAGE_SIZE - 1;

	if ((rmem->base & mask) || (rmem->size & mask)) {
		pr_err("Reserved memory: incorrect alignment of reboot region\n");
		return -EINVAL;
	}

	pr_info("Reserved memory: detected reboot memory at %pa, size %ld KiB\n",
		&rmem->base, (unsigned long)rmem->size / SZ_1K);

	reboot_cmd_mem = rmem->base;
	reboot_cmd_size = rmem->size;

	return 0;
}
RESERVEDMEM_OF_DECLARE(reboot, "reboot_page", rmem_reboot_setup);
#endif


static struct platform_driver spa_wdt_driver = {
	.probe		= spa_wdt_probe,
	.remove		= spa_wdt_remove,
	.shutdown	= spa_wdt_shutdown,
	.suspend	= spa_wdt_suspend,
	.resume		= spa_wdt_resume,
	.driver		= {
		.name	= "spa-wdt",
		.of_match_table	= of_match_ptr(spa_wdt_match),
	},
};


module_platform_driver(spa_wdt_driver);

MODULE_DESCRIPTION("Spacemit k1x-plat Watchdog Device Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS_MISCDEV(WATCHDOG_MINOR);
MODULE_ALIAS("platform:soc-wdt");
