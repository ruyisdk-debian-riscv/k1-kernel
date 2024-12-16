// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cpufreq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpumask.h>
#include <linux/clk/clk-conf.h>
#include <linux/pm_qos.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/mutex.h>
#include <linux/pm_opp.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include "../opp/opp.h"
#include "cpufreq-dt.h"

#define TURBO_FREQUENCY		(1600000000)
#define STABLE_FREQUENCY	(1200000000)

#define PRODUCT_ID_M1		(0x36070000)

static int spacemit_policy_notifier(struct notifier_block *nb,
                                  unsigned long event, void *data)
{
	int cpu;
	u64 rates;
	static int cci_init;
	struct clk *cci_clk;
	struct device *cpu_dev;
	struct cpufreq_policy *policy = data;
	struct opp_table *opp_table;

	cpu = cpumask_first(policy->related_cpus);
	cpu_dev = get_cpu_device(cpu);
	opp_table = _find_opp_table(cpu_dev);

	if (cci_init == 0) {
		cci_clk = of_clk_get_by_name(opp_table->np, "cci");
		if (IS_ERR(cci_clk))
			return 0;

		of_property_read_u64_array(opp_table->np, "cci-hz", &rates, 1);
		clk_set_rate(cci_clk, rates);
		clk_put(cci_clk);
		cci_init = 1;
	}

	return 0;
}

static int spacemit_processor_notifier(struct notifier_block *nb,
                                  unsigned long event, void *data)
{
	int cpu;
	struct device *cpu_dev;
	struct cpufreq_freqs *freqs = (struct cpufreq_freqs *)data;
	struct cpufreq_policy *policy = ( struct cpufreq_policy *)freqs->policy;
	struct opp_table *opp_table;
	struct device_node *np;
	struct clk *tcm_clk, *ace0_clk, *ace1_clk;
	u64 rates;

	cpu = cpumask_first(policy->related_cpus);
	cpu_dev = get_cpu_device(cpu);
	opp_table = _find_opp_table(cpu_dev);

	/* get the tcm/ace clk handler */
	tcm_clk = of_clk_get_by_name(opp_table->np, "tcm");
	ace0_clk = of_clk_get_by_name(opp_table->np, "ace0");
	ace1_clk = of_clk_get_by_name(opp_table->np, "ace1");

	if (event == CPUFREQ_PRECHANGE) {
		/**
		 * change the tcm/ace's frequency first.
		 * binary division is safe
		 */
		if (!IS_ERR(ace0_clk)) {
			clk_set_rate(ace0_clk, clk_get_rate(clk_get_parent(ace0_clk)) / 2);
			clk_put(ace0_clk);
		}

		if (!IS_ERR(ace1_clk)) {
			clk_set_rate(ace1_clk, clk_get_rate(clk_get_parent(ace1_clk)) / 2);
			clk_put(ace1_clk);
		}

		if (!IS_ERR(tcm_clk)) {
			clk_set_rate(tcm_clk, clk_get_rate(clk_get_parent(tcm_clk)) / 2);
			clk_put(tcm_clk);
		}

		if (freqs->new * 1000 >= TURBO_FREQUENCY) {
			if (freqs->old * 1000 >= TURBO_FREQUENCY)
				clk_set_rate(opp_table->clk, STABLE_FREQUENCY);
		}

	}

	if (event == CPUFREQ_POSTCHANGE) {
		if (!IS_ERR(tcm_clk)) {
			clk_get_rate(clk_get_parent(tcm_clk));
			/* get the tcm-hz */
			of_property_read_u64_array(np, "tcm-hz", &rates, 1);
			/* then set rate */
			clk_set_rate(tcm_clk, rates);
			clk_put(tcm_clk);
		}

		if (!IS_ERR(ace0_clk)) {
			clk_get_rate(clk_get_parent(ace0_clk));
			/* get the ace-hz */
			of_property_read_u64_array(np, "ace0-hz", &rates, 1);
			/* then set rate */
			clk_set_rate(ace0_clk, rates);
			clk_put(ace0_clk);
		}

		if (!IS_ERR(ace1_clk)) {
			clk_get_rate(clk_get_parent(ace1_clk));
			/* get the ace-hz */
			of_property_read_u64_array(np, "ace1-hz", &rates, 1);
			/* then set rate */
			clk_set_rate(ace1_clk, rates);
			clk_put(ace1_clk);
		}
	}

	dev_pm_opp_put_opp_table(opp_table);

	return 0;
}

static struct notifier_block spacemit_processor_notifier_block = {
       .notifier_call = spacemit_processor_notifier,
};

static struct notifier_block spacemit_policy_notifier_block = {
       .notifier_call = spacemit_policy_notifier,
};

static int __init spacemit_processor_driver_init(void)
{
       int ret;

       ret = cpufreq_register_notifier(&spacemit_processor_notifier_block, CPUFREQ_TRANSITION_NOTIFIER);
       if (ret) {
               pr_err("register cpufreq notifier failed\n");
               return -EINVAL;
       }

       ret = cpufreq_register_notifier(&spacemit_policy_notifier_block, CPUFREQ_POLICY_NOTIFIER);
       if (ret) {
               pr_err("register cpufreq notifier failed\n");
               return -EINVAL;
       }

       return 0;
}

arch_initcall(spacemit_processor_driver_init);
