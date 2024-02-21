/*
 * Driver for the Renesas RZ/V2MA thermal sensor driver
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/thermal.h>
#include <linux/units.h>

#include "thermal_core.h"
#include "thermal_hwmon.h"


#define TSU_MAX_NUM     2

/* Register offsets */
#define SCR1    0x00
#define SCR2    0x04
#define SRR1    0x0C

/* Sensor Control Register1(SCR1) */
#define SCR1_EN_TS              BIT(0)
#define SCR1_ADC_PD_TS          BIT(1)

/* Sensor Control Register2(SCR2) */
#define SCR2_START              BIT(0)

#define TEMP_MASK       0xFFF

/* SYS Register */
#define SYS_BASE_ADDR                   0xA3F03000
/* SYS-TSU TS0/TS1_LT_DAT Signal Monitor Register */
#define SYS_TSUn_LT_DAT_MON             0x330
#define SYS_TSUn_HT_DAT_MON             0x338
/* SYS-TSU0/1 TS_LT_TEMP Signal Monitor Register */
#define SYS_TSLT_TEMP_MON               0x350
/* SYS-TSU0/1 TS_HT_TEMP Signal Monitor Register */
#define SYS_TSHT_TEMP_MON               0x358
#define SYS_TS_MAX_SIZE                 (SYS_TSHT_TEMP_MON+8)

#define TS_CODE_CAP_TIMES       8       /* Capture  times */

#define TSU_SCALE(x)                    ((u64)(x) * (u64)1000000)
#define COEF_A                  1230000
#define COEF_B                  37

#define TSU_CORRECT_CALC(_val)  \
                ((u64)COEF_A * (u64)_val ) / (TSU_SCALE(1)+ ((u64)COEF_B * (u64)_val))

struct rzv2ma_thermal_tsu {
        struct device *dev;
        void __iomem *tsu0_base;
        void __iomem *tsu1_base;
        struct thermal_zone_device *zone;
        struct reset_control *rstc;
};

static inline u32 rzv2ma_thermal_read(struct rzv2ma_thermal_tsu *tsu,
                                int ch, u32 reg)
{
        if (!ch)
                return ioread32(tsu->tsu0_base + reg);
        else
                return ioread32(tsu->tsu1_base + reg);
}

static inline void rzv2ma_thermal_write(struct rzv2ma_thermal_tsu *tsu,
                                int ch, u32 reg, u32 data)
{
        if (!ch)
                iowrite32(data, tsu->tsu0_base + reg);
        else
                iowrite32(data, tsu->tsu1_base + reg);
}


static int rzv2ma_thermal_calc_temp(void *devdata, int *temp, int ch)
{
        struct rzv2ma_thermal_tsu *tsu = devdata;
        u32 result = 0, srr1_ave;
        u32 lt_dat, ht_dat, lt_temp, ht_temp;
        u64 val;
        u32 ch_offset = 0;
        void __iomem *sys_base;
        int i;

        for (i = 0; i < TS_CODE_CAP_TIMES ; i++) {
                /* TSU repeats measurement at 70 microseconds intervals and
                 * automatically updates the results of measurement. As per
                 * the HW manual for measuring temperature we need to read 8
                 * values consecutively and then take the average.
                 * srr1_ave = (tsu_srr1[0] + ⋯ + tsu_srr1[7]) / 8
                 */
                result += rzv2ma_thermal_read(tsu, ch, SRR1) & TEMP_MASK;
                usleep_range(70, 80);
        }

        srr1_ave = result / TS_CODE_CAP_TIMES;

        sys_base = ioremap(SYS_BASE_ADDR, SYS_TS_MAX_SIZE);
        if (!sys_base) {
                return -ENOMEM;
        }

        if( ch == 1 )
                ch_offset = 0x10;

        lt_dat = ioread32(sys_base+SYS_TSUn_LT_DAT_MON+ch_offset) & TEMP_MASK;
        ht_dat = ioread32(sys_base+SYS_TSUn_HT_DAT_MON+ch_offset) & TEMP_MASK;
        lt_temp = ioread32(sys_base+SYS_TSLT_TEMP_MON) & TEMP_MASK;
        ht_temp = ioread32(sys_base+SYS_TSHT_TEMP_MON) & TEMP_MASK;

        iounmap(sys_base);

        dev_dbg(tsu->dev, " srr1_ave:%d lt_dat:%d  ht_dat:%d\n", srr1_ave, lt_dat, ht_dat);
        dev_dbg(tsu->dev, "             lt_temp:%d ht_temp:%d\n", lt_temp, ht_temp);
        val = TSU_CORRECT_CALC(srr1_ave) - TSU_CORRECT_CALC(lt_dat);
        val *= ((ht_temp - lt_temp) * 1000 / 16 ) / (TSU_CORRECT_CALC(ht_dat) - TSU_CORRECT_CALC(lt_dat));

        *temp = (int)(val + ((lt_temp/16 - 100) * 1000));

        return 0;
}

static int rzv2ma_thermal_get_temp(void *devdata, int *temp)
{
        struct rzv2ma_thermal_tsu *tsu = devdata;
        int ret, ch0_tmp, ch1_tmp;

        ch0_tmp = 0; ch1_tmp = 0;

        ret =   rzv2ma_thermal_calc_temp( devdata, &ch0_tmp, 0);
        if(ret)
                return ret;
        ret =   rzv2ma_thermal_calc_temp( devdata, &ch1_tmp, 1 );
        if(ret)
                return ret;

        dev_dbg(tsu->dev, "Calc temperture ch0:ch1 [%d:%d]\n",
                ch0_tmp, ch1_tmp);

        if (ch0_tmp > ch1_tmp)
                *temp = ch0_tmp;
        else
                *temp = ch1_tmp;

        return 0;
}

 const struct thermal_zone_of_device_ops rzv2ma_tz_of_ops = {
        .get_temp = rzv2ma_thermal_get_temp,
};

static void rzv2ma_thermal_init(struct rzv2ma_thermal_tsu *tsu, int ch)
{
        rzv2ma_thermal_write(tsu, ch, SCR1, SCR1_EN_TS);
        usleep_range(60, 80);
        rzv2ma_thermal_write(tsu, ch, SCR2, SCR2_START);
        usleep_range(70, 90);
}

static int rzv2ma_thermal_remove(struct platform_device *pdev)
{
        struct device *dev = &pdev->dev;

        pm_runtime_put(dev);
        pm_runtime_disable(dev);

        return 0;
}

static void rzv2ma_hwmon_action(void *data)
{
        struct thermal_zone_device *zone = data;

        thermal_remove_hwmon_sysfs(zone);
}

static int rzv2ma_thermal_probe(struct platform_device *pdev)
{
        struct thermal_zone_device *zone;
        struct rzv2ma_thermal_tsu *tsu;
        struct device *dev = &pdev->dev;
        struct resource *res;
        int ret;

        tsu = devm_kzalloc(dev, sizeof(*tsu), GFP_KERNEL);
        if (!tsu)
                return -ENOMEM;

        platform_set_drvdata(pdev, tsu);

        pm_runtime_enable(dev);
        pm_runtime_get_sync(dev);

        tsu->dev = dev;

        /* Initialize TSU each channel */
        res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
        tsu->tsu0_base = devm_ioremap_resource(dev, res);
        if (IS_ERR(tsu->tsu0_base)) {
                ret = PTR_ERR(tsu->tsu0_base);
                rzv2ma_thermal_remove(pdev);
                return ret;
        }

        res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
        tsu->tsu1_base = devm_ioremap_resource(dev, res);
        if (IS_ERR(tsu->tsu1_base)) {
                ret = PTR_ERR(tsu->tsu1_base);
                rzv2ma_thermal_remove(pdev);
                return ret;
        }

        rzv2ma_thermal_init(tsu, 0);
        rzv2ma_thermal_init(tsu, 1);

        zone = devm_thermal_zone_of_sensor_register(dev, 0, tsu,
                                            &rzv2ma_tz_of_ops);
        if (IS_ERR(zone)) {
                dev_err(dev, "Can't register thermal zone");
                ret = PTR_ERR(zone);
                rzv2ma_thermal_remove(pdev);
                return ret;
        }

        tsu->zone = zone;
        tsu->zone->tzp->no_hwmon = false;
        ret = thermal_add_hwmon_sysfs(tsu->zone);
        if (ret) {
                rzv2ma_thermal_remove(pdev);
                return ret;
        }

        ret = devm_add_action_or_reset(dev, rzv2ma_hwmon_action, zone);
        if (ret) {
                rzv2ma_thermal_remove(pdev);
                return ret;
        }

        ret = of_thermal_get_ntrips(tsu->zone);
        if (ret < 0) {
                rzv2ma_thermal_remove(pdev);
                return ret;
        }
        dev_info(dev, "TSU: Loaded %d trip points\n", ret);

        return 0;
}

static const struct of_device_id rzv2ma_thermal_dt_ids[] = {
        {
                .compatible = "renesas,rzv2ma-thermal",
        },
        { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rzv2ma_thermal_dt_ids);

static int __maybe_unused rzv2ma_thermal_suspend(struct device *dev)
{
        return 0;
}

static int __maybe_unused rzv2ma_thermal_resume(struct device *dev)
{
        struct rzv2ma_thermal_tsu *tsu = dev_get_drvdata(dev);

        rzv2ma_thermal_init(tsu,0);
        rzv2ma_thermal_init(tsu,1);
        return 0;
}

static SIMPLE_DEV_PM_OPS(rzv2ma_thermal_pm_ops, rzv2ma_thermal_suspend,
                         rzv2ma_thermal_resume);


static struct platform_driver rzv2ma_thermal_driver = {
        .driver = {
                .name = "rzv2ma_thermal",
                .pm = &rzv2ma_thermal_pm_ops,
                .of_match_table = rzv2ma_thermal_dt_ids,
        },
        .probe = rzv2ma_thermal_probe,
        .remove = rzv2ma_thermal_remove,
};
module_platform_driver(rzv2ma_thermal_driver);

MODULE_DESCRIPTION("Renesas RZ/V2MA TSU Thermal Sensor Driver");
MODULE_AUTHOR("Shuichi Sueki <shuichi.sueki.zc@bp.renesas.com>");
MODULE_LICENSE("GPL v2");
