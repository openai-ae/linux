// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Asus Armoury (WMI) attributes driver. This driver uses the fw_attributes
 * class to expose the various WMI functions that many gaming and some
 * non-gaming ASUS laptops have available.
 * These typically don't fit anywhere else in the sysfs such as under LED class,
 * hwmon or other, and are set in Windows using the ASUS Armoury Crate tool.
 *
 * Copyright(C) 2024 Luke Jones <luke@ljones.dev>
 */

#include "linux/cleanup.h"
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kmod.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_data/x86/asus-wmi.h>
#include <linux/types.h>
#include <linux/acpi.h>

#include "asus-armoury.h"
#include "firmware_attributes_class.h"

#define DEBUG

#define ASUS_NB_WMI_EVENT_GUID "0B3CBB35-E3C2-45ED-91C2-4C5A6D195D1C"

#define ASUS_MINI_LED_MODE_MASK   0x03
/* Standard modes for devices with only on/off */
#define ASUS_MINI_LED_OFF         0x00
#define ASUS_MINI_LED_ON          0x01
/* Like "on" but the effect is more vibrant or brighter */
#define ASUS_MINI_LED_STRONG_MODE 0x02
/* New modes for devices with 3 mini-led mode types */
#define ASUS_MINI_LED_2024_WEAK   0x00
#define ASUS_MINI_LED_2024_STRONG 0x01
#define ASUS_MINI_LED_2024_OFF    0x02

#define ASUS_POWER_CORE_MASK GENMASK(15, 8)
#define ASUS_PERF_CORE_MASK GENMASK(7, 0)

enum cpu_core_type {
	CPU_CORE_PERF = 0,
	CPU_CORE_POWER,
};

enum cpu_core_value {
	CPU_CORE_DEFAULT = 0,
	CPU_CORE_MIN,
	CPU_CORE_MAX,
	CPU_CORE_CURRENT,
};

#define CPU_PERF_CORE_COUNT_MIN 4
#define CPU_POWR_CORE_COUNT_MIN 0

/* Default limits for tunables available on ASUS ROG laptops */
#define NVIDIA_BOOST_MIN      5
#define NVIDIA_BOOST_MAX      25
#define NVIDIA_TEMP_MIN       75
#define NVIDIA_TEMP_MAX       87
#define NVIDIA_POWER_MIN      0
#define NVIDIA_POWER_MAX      70
#define NVIDIA_POWER_DEFAULT  70
#define PPT_CPU_LIMIT_MIN     5
#define PPT_CPU_LIMIT_MAX     150
#define PPT_CPU_LIMIT_DEFAULT 80
#define PPT_PLATFORM_MIN      5
#define PPT_PLATFORM_MAX      100
#define PPT_PLATFORM_DEFAULT  80

/* Tunables provided by ASUS for gaming laptops */
struct rog_tunables {
	u32 cpu_default;
	u32 cpu_min;
	u32 cpu_max;

	u32 platform_default;
	u32 platform_min;
	u32 platform_max;

	u32 ppt_pl1_spl; // cpu
	u32 ppt_pl2_sppt; // cpu
	u32 ppt_pl3_fppt; // cpu
	u32 ppt_apu_sppt; // plat
	u32 ppt_platform_sppt; // plat

	u32 nv_boost_default;
	u32 nv_boost_min;
	u32 nv_boost_max;
	u32 nv_dynamic_boost;

	u32 nv_temp_default;
	u32 nv_temp_min;
	u32 nv_temp_max;
	u32 nv_temp_target;

	u32 dgpu_tgp_default;
	u32 dgpu_tgp_min;
	u32 dgpu_tgp_max;
	u32 dgpu_tgp;

	u32 cur_perf_cores;
	u32 min_perf_cores;
	u32 max_perf_cores;
	u32 cur_power_cores;
	u32 min_power_cores;
	u32 max_power_cores;
};

static const struct class *fw_attr_class;

struct asus_armoury_priv {
	struct device *fw_attr_dev;
	struct kset *fw_attr_kset;

	struct rog_tunables *rog_tunables;
	u32 mini_led_dev_id;
	u32 gpu_mux_dev_id;

	struct mutex mutex;
};

static struct asus_armoury_priv asus_armoury = {
	.mutex = __MUTEX_INITIALIZER(asus_armoury.mutex)
};

struct fw_attrs_group {
	bool pending_reboot;
};

static struct fw_attrs_group fw_attrs = {
	.pending_reboot = false,
};

struct asus_attr_group {
	const struct attribute_group *attr_group;
	u32 wmi_devid;
};

static bool asus_wmi_is_present(u32 dev_id)
{
	u32 retval;
	int status;

	status = asus_wmi_evaluate_method(ASUS_WMI_METHODID_DSTS, dev_id, 0, &retval);
	pr_debug("%s called (0x%08x), retval: 0x%08x\n", __func__, dev_id, retval);

	return status == 0 && (retval & ASUS_WMI_DSTS_PRESENCE_BIT);
}

static void asus_set_reboot_and_signal_event(void)
{
	fw_attrs.pending_reboot = true;
	kobject_uevent(&asus_armoury.fw_attr_dev->kobj, KOBJ_CHANGE);
}

static ssize_t pending_reboot_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", fw_attrs.pending_reboot);
}

static struct kobj_attribute pending_reboot = __ATTR_RO(pending_reboot);

static bool asus_bios_requires_reboot(struct kobj_attribute *attr)
{
	return !strcmp(attr->attr.name, "gpu_mux_mode") ||
	       !strcmp(attr->attr.name, "cores_performance") ||
	       !strcmp(attr->attr.name, "cores_efficiency") ||
	       !strcmp(attr->attr.name, "panel_hd_mode");
}

static int armoury_wmi_set_devstate(struct kobj_attribute *attr, u32 value, u32 wmi_dev)
{
	u32 result;
	int err;

	guard(mutex)(&asus_armoury.mutex);
	err = asus_wmi_set_devstate(wmi_dev, value, &result);
	if (err) {
		pr_err("Failed to set %s: %d\n", attr->attr.name, err);
		return err;
	}
	/*
	 * !1 is usually considered a fail by ASUS, but some WMI methods do use > 1
	 * to return a status code or similar.
	 */
	if (result < 1) {
		pr_err("Failed to set %s: (result): 0x%x\n", attr->attr.name, result);
		return -EIO;
	}

	return 0;
}

/**
 * attr_int_store() - Send an int to wmi method, checks if within min/max exclusive.
 * @kobj: Pointer to the driver object.
 * @attr: Pointer to the attribute calling this function.
 * @buf: The buffer to read from, this is parsed to `int` type.
 * @count: Required by sysfs attribute macros, pass in from the callee attr.
 * @min: Minimum accepted value. Below this returns -EINVAL.
 * @max: Maximum accepted value. Above this returns -EINVAL.
 * @store_value: Pointer to where the parsed value should be stored.
 * @wmi_dev: The WMI function ID to use.
 *
 * This function is intended to be generic so it can be called from any "_store"
 * attribute which works only with integers. The integer to be sent to the WMI method
 * is range checked and an error returned if out of range.
 *
 * If the value is valid and WMI is success, then the sysfs attribute is notified
 * and if asus_bios_requires_reboot() is true then reboot attribute is also notified.
 *
 * Returns: Either count, or an error.
 */
static ssize_t attr_uint_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf,
			      size_t count, u32 min, u32 max, u32 *store_value, u32 wmi_dev)
{
	u32 value;
	int err;

	err = kstrtouint(buf, 10, &value);
	if (err)
		return err;

	if (value < min || value > max)
		return -EINVAL;

	err = armoury_wmi_set_devstate(attr, value, wmi_dev);
	if (err)
		return err;

	if (store_value != NULL)
		*store_value = value;
	sysfs_notify(kobj, NULL, attr->attr.name);

	if (asus_bios_requires_reboot(attr))
		asus_set_reboot_and_signal_event();

	return count;
}

/* Mini-LED mode **************************************************************/
static ssize_t mini_led_mode_current_value_show(struct kobject *kobj,
						struct kobj_attribute *attr, char *buf)
{
	u32 value;
	int err;

	err = asus_wmi_get_devstate_dsts(asus_armoury.mini_led_dev_id, &value);
	if (err)
		return err;

	value &= ASUS_MINI_LED_MODE_MASK;

	/*
	 * Remap the mode values to match previous generation mini-LED. The last gen
	 * WMI 0 == off, while on this version WMI 2 == off (flipped).
	 */
	if (asus_armoury.mini_led_dev_id == ASUS_WMI_DEVID_MINI_LED_MODE2) {
		switch (value) {
		case ASUS_MINI_LED_2024_WEAK:
			value = ASUS_MINI_LED_ON;
			break;
		case ASUS_MINI_LED_2024_STRONG:
			value = ASUS_MINI_LED_STRONG_MODE;
			break;
		case ASUS_MINI_LED_2024_OFF:
			value = ASUS_MINI_LED_OFF;
			break;
		}
	}

	return sysfs_emit(buf, "%u\n", value);
}

static ssize_t mini_led_mode_current_value_store(struct kobject *kobj,
						 struct kobj_attribute *attr,
						const char *buf, size_t count)
{
	u32 mode;
	int err;

	err = kstrtou32(buf, 10, &mode);
	if (err)
		return err;

	if (asus_armoury.mini_led_dev_id == ASUS_WMI_DEVID_MINI_LED_MODE &&
	    mode > ASUS_MINI_LED_ON)
		return -EINVAL;
	if (asus_armoury.mini_led_dev_id == ASUS_WMI_DEVID_MINI_LED_MODE2 &&
	    mode > ASUS_MINI_LED_STRONG_MODE)
		return -EINVAL;

	/*
	 * Remap the mode values so expected behaviour is the same as the last
	 * generation of mini-LED with 0 == off, 1 == on.
	 */
	if (asus_armoury.mini_led_dev_id == ASUS_WMI_DEVID_MINI_LED_MODE2) {
		switch (mode) {
		case ASUS_MINI_LED_OFF:
			mode = ASUS_MINI_LED_2024_OFF;
			break;
		case ASUS_MINI_LED_ON:
			mode = ASUS_MINI_LED_2024_WEAK;
			break;
		case ASUS_MINI_LED_STRONG_MODE:
			mode = ASUS_MINI_LED_2024_STRONG;
			break;
		}
	}

	err = armoury_wmi_set_devstate(attr, mode, asus_armoury.mini_led_dev_id);
	if (err)
		return err;

	sysfs_notify(kobj, NULL, attr->attr.name);

	return count;
}

static ssize_t mini_led_mode_possible_values_show(struct kobject *kobj,
						  struct kobj_attribute *attr, char *buf)
{
	switch (asus_armoury.mini_led_dev_id) {
	case ASUS_WMI_DEVID_MINI_LED_MODE:
		return sysfs_emit(buf, "0;1\n");
	case ASUS_WMI_DEVID_MINI_LED_MODE2:
		return sysfs_emit(buf, "0;1;2\n");
	}

	return sysfs_emit(buf, "0\n");
}

ATTR_GROUP_ENUM_CUSTOM(mini_led_mode, "mini_led_mode", "Set the mini-LED backlight mode");

static ssize_t gpu_mux_mode_current_value_store(struct kobject *kobj,
						struct kobj_attribute *attr, const char *buf,
						size_t count)
{
	int result, err;
	u32 optimus;

	err = kstrtou32(buf, 10, &optimus);
	if (err)
		return err;

	if (optimus > 1)
		return -EINVAL;

	if (asus_wmi_is_present(ASUS_WMI_DEVID_DGPU)) {
		err = asus_wmi_get_devstate_dsts(ASUS_WMI_DEVID_DGPU, &result);
		if (err)
			return err;
		if (result && !optimus) {
			err = -ENODEV;
			pr_warn("Can not switch MUX to dGPU mode when dGPU is disabled: %02X %02X %d\n",
				result, optimus, err);
			return err;
		}
	}

	if (asus_wmi_is_present(ASUS_WMI_DEVID_EGPU)) {
		err = asus_wmi_get_devstate_dsts(ASUS_WMI_DEVID_EGPU, &result);
		if (err)
			return err;
		if (result && !optimus) {
			err = -ENODEV;
			pr_warn("Can not switch MUX to dGPU mode when eGPU is enabled: %d\n",
				err);
			return err;
		}
	}

	err = armoury_wmi_set_devstate(attr, optimus, asus_armoury.gpu_mux_dev_id);
	if (err)
		return err;

	sysfs_notify(kobj, NULL, attr->attr.name);
	asus_set_reboot_and_signal_event();

	return count;
}
WMI_SHOW_INT(gpu_mux_mode_current_value, "%d\n", asus_armoury.gpu_mux_dev_id);
ATTR_GROUP_BOOL_CUSTOM(gpu_mux_mode, "gpu_mux_mode", "Set the GPU display MUX mode");

/*
 * A user may be required to store the value twice, typical store first, then
 * rescan PCI bus to activate power, then store a second time to save correctly.
 */
static ssize_t dgpu_disable_current_value_store(struct kobject *kobj,
						struct kobj_attribute *attr, const char *buf,
						size_t count)
{
	int result, err;
	u32 disable;

	err = kstrtou32(buf, 10, &disable);
	if (err)
		return err;

	if (disable > 1)
		return -EINVAL;

	if (asus_armoury.gpu_mux_dev_id) {
		err = asus_wmi_get_devstate_dsts(asus_armoury.gpu_mux_dev_id, &result);
		if (err)
			return err;
		if (!result && disable) {
			err = -ENODEV;
			pr_warn("Can not disable dGPU when the MUX is in dGPU mode: %d\n", err);
			return err;
		}
		// TODO: handle a > 1 result, shouold do a PCI rescan and run again
	}

	err = armoury_wmi_set_devstate(attr, disable, ASUS_WMI_DEVID_DGPU);
	if (err)
		return err;

	sysfs_notify(kobj, NULL, attr->attr.name);

	return count;
}
WMI_SHOW_INT(dgpu_disable_current_value, "%d\n", ASUS_WMI_DEVID_DGPU);
ATTR_GROUP_BOOL_CUSTOM(dgpu_disable, "dgpu_disable", "Disable the dGPU");

/* The ACPI call to enable the eGPU also disables the internal dGPU */
static ssize_t egpu_enable_current_value_store(struct kobject *kobj, struct kobj_attribute *attr,
					       const char *buf, size_t count)
{
	int result, err;
	u32 enable;

	err = kstrtou32(buf, 10, &enable);
	if (err)
		return err;

	if (enable > 1)
		return -EINVAL;

	err = asus_wmi_get_devstate_dsts(ASUS_WMI_DEVID_EGPU_CONNECTED, &result);
	if (err) {
		pr_warn("Failed to get eGPU connection status: %d\n", err);
		return err;
	}

	if (asus_armoury.gpu_mux_dev_id) {
		err = asus_wmi_get_devstate_dsts(asus_armoury.gpu_mux_dev_id, &result);
		if (err) {
			pr_warn("Failed to get GPU MUX status: %d\n", result);
			return result;
		}
		if (!result && enable) {
			err = -ENODEV;
			pr_warn("Can not enable eGPU when the MUX is in dGPU mode: %d\n", err);
			return err;
		}
	}

	err = armoury_wmi_set_devstate(attr, enable, ASUS_WMI_DEVID_EGPU);
	if (err)
		return err;

	sysfs_notify(kobj, NULL, attr->attr.name);

	return count;
}
WMI_SHOW_INT(egpu_enable_current_value, "%d\n", ASUS_WMI_DEVID_EGPU);
ATTR_GROUP_BOOL_CUSTOM(egpu_enable, "egpu_enable", "Enable the eGPU (also disables dGPU)");

/* Device memory available to APU */

static ssize_t apu_mem_current_value_show(struct kobject *kobj, struct kobj_attribute *attr,
					  char *buf)
{
	int err;
	u32 mem;

	err = asus_wmi_get_devstate_dsts(ASUS_WMI_DEVID_APU_MEM, &mem);
	if (err)
		return err;

	switch (mem) {
	case 0x100:
		mem = 0;
		break;
	case 0x102:
		mem = 1;
		break;
	case 0x103:
		mem = 2;
		break;
	case 0x104:
		mem = 3;
		break;
	case 0x105:
		mem = 4;
		break;
	case 0x106:
		/* This is out of order and looks wrong but is correct */
		mem = 8;
		break;
	case 0x107:
		mem = 5;
		break;
	case 0x108:
		mem = 6;
		break;
	case 0x109:
		mem = 7;
		break;
	default:
		mem = 4;
		break;
	}

	return sysfs_emit(buf, "%u\n", mem);
}

static ssize_t apu_mem_current_value_store(struct kobject *kobj, struct kobj_attribute *attr,
					   const char *buf, size_t count)
{
	int result, err;
	u32 requested, mem;

	result = kstrtou32(buf, 10, &requested);
	if (result)
		return result;

	switch (requested) {
	case 0:
		mem = 0x000;
		break;
	case 1:
		mem = 0x102;
		break;
	case 2:
		mem = 0x103;
		break;
	case 3:
		mem = 0x104;
		break;
	case 4:
		mem = 0x105;
		break;
	case 5:
		mem = 0x107;
		break;
	case 6:
		mem = 0x108;
		break;
	case 7:
		mem = 0x109;
		break;
	case 8:
		/* This is out of order and looks wrong but is correct */
		mem = 0x106;
		break;
	default:
		return -EIO;
	}

	err = asus_wmi_set_devstate(ASUS_WMI_DEVID_APU_MEM, mem, &result);
	if (err) {
		pr_warn("Failed to set apu_mem: %d\n", err);
		return err;
	}

	pr_info("APU memory changed to %uGB, reboot required\n", requested);
	sysfs_notify(kobj, NULL, attr->attr.name);

	asus_set_reboot_and_signal_event();

	return count;
}

static ssize_t apu_mem_possible_values_show(struct kobject *kobj, struct kobj_attribute *attr,
					    char *buf)
{
	return sysfs_emit(buf, "0;1;2;3;4;5;6;7;8\n");
}
ATTR_GROUP_ENUM_CUSTOM(apu_mem, "apu_mem", "Set available system RAM (in GB) for the APU to use");

static int init_max_cpu_cores(void)
{
	u32 cores;
	int err;

	err = asus_wmi_get_devstate_dsts(ASUS_WMI_DEVID_CORES_MAX, &cores);
	if (err)
		return err;

	cores &= ~ASUS_WMI_DSTS_PRESENCE_BIT;
	asus_armoury.rog_tunables->max_power_cores = FIELD_GET(ASUS_POWER_CORE_MASK, cores);
	asus_armoury.rog_tunables->max_perf_cores = FIELD_GET(ASUS_PERF_CORE_MASK, cores);

	err = asus_wmi_get_devstate_dsts(ASUS_WMI_DEVID_CORES, &cores);
	if (err) {
		pr_err("Could not get CPU core count: error %d", err);
		return err;
	}

	asus_armoury.rog_tunables->cur_perf_cores = FIELD_GET(ASUS_PERF_CORE_MASK, cores);
	asus_armoury.rog_tunables->cur_power_cores = FIELD_GET(ASUS_POWER_CORE_MASK, cores);

	asus_armoury.rog_tunables->min_perf_cores = CPU_PERF_CORE_COUNT_MIN;
	asus_armoury.rog_tunables->min_power_cores = CPU_POWR_CORE_COUNT_MIN;

	return 0;
}

static ssize_t cores_value_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf,
				enum cpu_core_type core_type, enum cpu_core_value core_value)
{
	u32 cores;

	switch (core_value) {
	case CPU_CORE_DEFAULT:
	case CPU_CORE_MAX:
		if (core_type == CPU_CORE_PERF)
			return sysfs_emit(buf, "%d\n",
					  asus_armoury.rog_tunables->max_perf_cores);
		else
			return sysfs_emit(buf, "%d\n",
					  asus_armoury.rog_tunables->max_power_cores);
	case CPU_CORE_MIN:
		if (core_type == CPU_CORE_PERF)
			return sysfs_emit(buf, "%d\n",
					  asus_armoury.rog_tunables->min_perf_cores);
		else
			return sysfs_emit(buf, "%d\n",
					  asus_armoury.rog_tunables->min_power_cores);
	default:
		break;
	}

	if (core_type == CPU_CORE_PERF)
		cores = asus_armoury.rog_tunables->cur_perf_cores;
	else
		cores = asus_armoury.rog_tunables->cur_power_cores;

	return sysfs_emit(buf, "%d\n", cores);
}

static ssize_t cores_current_value_store(struct kobject *kobj, struct kobj_attribute *attr,
					 const char *buf, enum cpu_core_type core_type)
{
	u32 new_cores, perf_cores, power_cores, out_val, min, max;
	int result, err;

	result = kstrtou32(buf, 10, &new_cores);
	if (result)
		return result;

	if (core_type == CPU_CORE_PERF) {
		perf_cores = new_cores;
		power_cores = out_val = asus_armoury.rog_tunables->cur_power_cores;
		min = asus_armoury.rog_tunables->min_perf_cores;
		max = asus_armoury.rog_tunables->max_perf_cores;
	} else {
		perf_cores = asus_armoury.rog_tunables->cur_perf_cores;
		power_cores = out_val = new_cores;
		min = asus_armoury.rog_tunables->min_power_cores;
		max = asus_armoury.rog_tunables->max_power_cores;
	}

	if (new_cores < min || new_cores > max)
		return -EINVAL;

	out_val = 0;
	out_val |= FIELD_PREP(ASUS_PERF_CORE_MASK, perf_cores);
	out_val |= FIELD_PREP(ASUS_POWER_CORE_MASK, power_cores);

	mutex_lock(&asus_armoury.mutex);
	err = asus_wmi_set_devstate(ASUS_WMI_DEVID_CORES, out_val, &result);
	mutex_unlock(&asus_armoury.mutex);

	if (err) {
		pr_warn("Failed to set CPU core count: %d\n", err);
		return err;
	}

	if (result > 1) {
		pr_warn("Failed to set CPU core count (result): 0x%x\n", result);
		return -EIO;
	}

	pr_info("CPU core count changed, reboot required\n");
	sysfs_notify(kobj, NULL, attr->attr.name);
	asus_set_reboot_and_signal_event();

	return 0;
}

static ssize_t cores_performance_min_value_show(struct kobject *kobj,
						struct kobj_attribute *attr, char *buf)
{
	return cores_value_show(kobj, attr, buf, CPU_CORE_PERF, CPU_CORE_MIN);
}

static ssize_t cores_performance_max_value_show(struct kobject *kobj,
						struct kobj_attribute *attr, char *buf)
{
	return cores_value_show(kobj, attr, buf, CPU_CORE_PERF, CPU_CORE_MAX);
}

static ssize_t cores_performance_default_value_show(struct kobject *kobj,
						    struct kobj_attribute *attr, char *buf)
{
	return cores_value_show(kobj, attr, buf, CPU_CORE_PERF, CPU_CORE_DEFAULT);
}

static ssize_t cores_performance_current_value_show(struct kobject *kobj,
						    struct kobj_attribute *attr, char *buf)
{
	return cores_value_show(kobj, attr, buf, CPU_CORE_PERF, CPU_CORE_CURRENT);
}

static ssize_t cores_performance_current_value_store(struct kobject *kobj,
						     struct kobj_attribute *attr,
						     const char *buf, size_t count)
{
	int err;

	err = cores_current_value_store(kobj, attr, buf, CPU_CORE_PERF);
	if (err)
		return err;

	return count;
}
ATTR_GROUP_CORES_RW(cores_performance, "cores_performance",
		    "Set the max available performance cores");

static ssize_t cores_efficiency_min_value_show(struct kobject *kobj, struct kobj_attribute *attr,
					       char *buf)
{
	return cores_value_show(kobj, attr, buf, CPU_CORE_POWER, CPU_CORE_MIN);
}

static ssize_t cores_efficiency_max_value_show(struct kobject *kobj, struct kobj_attribute *attr,
					       char *buf)
{
	return cores_value_show(kobj, attr, buf, CPU_CORE_POWER, CPU_CORE_MAX);
}

static ssize_t cores_efficiency_default_value_show(struct kobject *kobj,
						   struct kobj_attribute *attr, char *buf)
{
	return cores_value_show(kobj, attr, buf, CPU_CORE_POWER, CPU_CORE_DEFAULT);
}

static ssize_t cores_efficiency_current_value_show(struct kobject *kobj,
						   struct kobj_attribute *attr, char *buf)
{
	return cores_value_show(kobj, attr, buf, CPU_CORE_POWER, CPU_CORE_CURRENT);
}

static ssize_t cores_efficiency_current_value_store(struct kobject *kobj,
						    struct kobj_attribute *attr, const char *buf,
						    size_t count)
{
	int err;

	err = cores_current_value_store(kobj, attr, buf, CPU_CORE_POWER);
	if (err)
		return err;

	return count;
}
ATTR_GROUP_CORES_RW(cores_efficiency, "cores_efficiency",
		    "Set the max available efficiency cores");

/* Device memory available to APU */

static ssize_t apu_mem_current_value_show(struct kobject *kobj, struct kobj_attribute *attr,
					  char *buf)
{
	int err;
	u32 mem;

	err = asus_wmi_get_devstate_dsts(ASUS_WMI_DEVID_APU_MEM, &mem);
	if (err)
		return err;

	switch (mem) {
	case 0x100:
		mem = 0;
		break;
	case 0x102:
		mem = 1;
		break;
	case 0x103:
		mem = 2;
		break;
	case 0x104:
		mem = 3;
		break;
	case 0x105:
		mem = 4;
		break;
	case 0x106:
		/* This is out of order and looks wrong but is correct */
		mem = 8;
		break;
	case 0x107:
		mem = 5;
		break;
	case 0x108:
		mem = 6;
		break;
	case 0x109:
		mem = 7;
		break;
	default:
		mem = 4;
		break;
	}

	return sysfs_emit(buf, "%u\n", mem);
}

static ssize_t apu_mem_current_value_store(struct kobject *kobj, struct kobj_attribute *attr,
					   const char *buf, size_t count)
{
	int result, err;
	u32 requested, mem;

	result = kstrtou32(buf, 10, &requested);
	if (result)
		return result;

	switch (requested) {
	case 0:
		mem = 0x000;
		break;
	case 1:
		mem = 0x102;
		break;
	case 2:
		mem = 0x103;
		break;
	case 3:
		mem = 0x104;
		break;
	case 4:
		mem = 0x105;
		break;
	case 5:
		mem = 0x107;
		break;
	case 6:
		mem = 0x108;
		break;
	case 7:
		mem = 0x109;
		break;
	case 8:
		/* This is out of order and looks wrong but is correct */
		mem = 0x106;
		break;
	default:
		return -EIO;
	}

	err = asus_wmi_set_devstate(ASUS_WMI_DEVID_APU_MEM, mem, &result);
	if (err) {
		pr_warn("Failed to set apu_mem: %d\n", err);
		return err;
	}

	pr_info("APU memory changed to %uGB, reboot required\n", requested);
	sysfs_notify(kobj, NULL, attr->attr.name);

	asus_set_reboot_and_signal_event();

	return count;
}

static ssize_t apu_mem_possible_values_show(struct kobject *kobj, struct kobj_attribute *attr,
					    char *buf)
{
	return sysfs_emit(buf, "0;1;2;3;4;5;6;7;8\n");
}
ATTR_GROUP_ENUM_CUSTOM(apu_mem, "apu_mem", "Set available system RAM (in GB) for the APU to use");

/* Simple attribute creation */
ATTR_GROUP_ROG_TUNABLE(ppt_pl1_spl, "ppt_pl1_spl", ASUS_WMI_DEVID_PPT_PL1_SPL, cpu_default,
		       cpu_min, cpu_max, 1, "Set the CPU slow package limit");
ATTR_GROUP_ROG_TUNABLE(ppt_pl2_sppt, "ppt_pl2_sppt", ASUS_WMI_DEVID_PPT_PL2_SPPT, cpu_default,
		       cpu_min, cpu_max, 1, "Set the CPU fast package limit");
ATTR_GROUP_ROG_TUNABLE(ppt_pl3_fppt, "ppt_pl3_fppt", ASUS_WMI_DEVID_PPT_FPPT, cpu_default, cpu_min,
		       cpu_max, 1, "Set the CPU slow package limit");
ATTR_GROUP_ROG_TUNABLE(ppt_apu_sppt, "ppt_apu_sppt", ASUS_WMI_DEVID_PPT_APU_SPPT,
		       platform_default, platform_min, platform_max, 1,
		       "Set the CPU slow package limit");
ATTR_GROUP_ROG_TUNABLE(ppt_platform_sppt, "ppt_platform_sppt", ASUS_WMI_DEVID_PPT_PLAT_SPPT,
		       platform_default, platform_min, platform_max, 1,
		       "Set the CPU slow package limit");
ATTR_GROUP_ROG_TUNABLE(nv_dynamic_boost, "nv_dynamic_boost", ASUS_WMI_DEVID_NV_DYN_BOOST,
		       nv_boost_default, nv_boost_min, nv_boost_max, 1,
		       "Set the Nvidia dynamic boost limit");
ATTR_GROUP_ROG_TUNABLE(nv_temp_target, "nv_temp_target", ASUS_WMI_DEVID_NV_THERM_TARGET,
		       nv_temp_default, nv_boost_min, nv_temp_max, 1,
		       "Set the Nvidia max thermal limit");
ATTR_GROUP_ROG_TUNABLE(dgpu_tgp, "dgpu_tgp", ASUS_WMI_DEVID_DGPU_SET_TGP, dgpu_tgp_default,
		       dgpu_tgp_min, dgpu_tgp_max, 1,
		       "Set the additional TGP on top of the base TGP");

ATTR_GROUP_INT_VALUE_ONLY_RO(dgpu_base_tgp, "dgpu_base_tgp", ASUS_WMI_DEVID_DGPU_BASE_TGP,
			     "Read the base TGP value");

ATTR_GROUP_ENUM_INT_RO(charge_mode, "charge_mode", ASUS_WMI_DEVID_CHARGE_MODE, "0;1;2",
		       "Show the current mode of charging");

ATTR_GROUP_BOOL_RW(boot_sound, "boot_sound", ASUS_WMI_DEVID_BOOT_SOUND,
		   "Set the boot POST sound");
ATTR_GROUP_BOOL_RW(mcu_powersave, "mcu_powersave", ASUS_WMI_DEVID_MCU_POWERSAVE,
		   "Set MCU powersaving mode");
ATTR_GROUP_BOOL_RW(panel_od, "panel_overdrive", ASUS_WMI_DEVID_PANEL_OD,
		   "Set the panel refresh overdrive");
ATTR_GROUP_BOOL_RW(panel_hd_mode, "panel_hd_mode", ASUS_WMI_DEVID_PANEL_HD,
		   "Set the panel HD mode to UHD<0> or FHD<1>");
ATTR_GROUP_BOOL_RO(egpu_connected, "egpu_connected", ASUS_WMI_DEVID_EGPU_CONNECTED,
		   "Show the eGPU connection status");

/* If an attribute does not require any special case handling add it here */
static const struct asus_attr_group armoury_attr_groups[] = {
	{ &egpu_connected_attr_group, ASUS_WMI_DEVID_EGPU_CONNECTED },
	{ &egpu_enable_attr_group, ASUS_WMI_DEVID_EGPU },
	{ &dgpu_disable_attr_group, ASUS_WMI_DEVID_DGPU },

	{ &ppt_pl1_spl_attr_group, ASUS_WMI_DEVID_PPT_PL1_SPL },
	{ &ppt_pl2_sppt_attr_group, ASUS_WMI_DEVID_PPT_PL2_SPPT },
	{ &ppt_pl3_fppt_attr_group, ASUS_WMI_DEVID_PPT_FPPT },
	{ &ppt_apu_sppt_attr_group, ASUS_WMI_DEVID_PPT_APU_SPPT },
	{ &ppt_platform_sppt_attr_group, ASUS_WMI_DEVID_PPT_PLAT_SPPT },
	{ &nv_dynamic_boost_attr_group, ASUS_WMI_DEVID_NV_DYN_BOOST },
	{ &nv_temp_target_attr_group, ASUS_WMI_DEVID_NV_THERM_TARGET },
	{ &dgpu_base_tgp_attr_group, ASUS_WMI_DEVID_DGPU_BASE_TGP },
	{ &dgpu_tgp_attr_group, ASUS_WMI_DEVID_DGPU_SET_TGP },
	{ &apu_mem_attr_group, ASUS_WMI_DEVID_APU_MEM },
	{ &cores_efficiency_attr_group, ASUS_WMI_DEVID_CORES_MAX },
	{ &cores_performance_attr_group, ASUS_WMI_DEVID_CORES_MAX },

	{ &ppt_pl1_spl_attr_group, ASUS_WMI_DEVID_PPT_PL1_SPL },
	{ &ppt_pl2_sppt_attr_group, ASUS_WMI_DEVID_PPT_PL2_SPPT },
	{ &ppt_pl3_fppt_attr_group, ASUS_WMI_DEVID_PPT_FPPT },
	{ &ppt_apu_sppt_attr_group, ASUS_WMI_DEVID_PPT_APU_SPPT },
	{ &ppt_platform_sppt_attr_group, ASUS_WMI_DEVID_PPT_PLAT_SPPT },
	{ &nv_dynamic_boost_attr_group, ASUS_WMI_DEVID_NV_DYN_BOOST },
	{ &nv_temp_target_attr_group, ASUS_WMI_DEVID_NV_THERM_TARGET },

	{ &charge_mode_attr_group, ASUS_WMI_DEVID_CHARGE_MODE },
	{ &boot_sound_attr_group, ASUS_WMI_DEVID_BOOT_SOUND },
	{ &mcu_powersave_attr_group, ASUS_WMI_DEVID_MCU_POWERSAVE },
	{ &panel_od_attr_group, ASUS_WMI_DEVID_PANEL_OD },
	{ &panel_hd_mode_attr_group, ASUS_WMI_DEVID_PANEL_HD },
};

static int asus_fw_attr_add(void)
{
	int err, i;

	err = fw_attributes_class_get(&fw_attr_class);
	if (err)
		return err;

	asus_armoury.fw_attr_dev = device_create(fw_attr_class, NULL, MKDEV(0, 0),
						NULL, "%s", DRIVER_NAME);
	if (IS_ERR(asus_armoury.fw_attr_dev)) {
		err = PTR_ERR(asus_armoury.fw_attr_dev);
		goto fail_class_get;
	}

	asus_armoury.fw_attr_kset = kset_create_and_add("attributes", NULL,
						&asus_armoury.fw_attr_dev->kobj);
	if (!asus_armoury.fw_attr_kset) {
		err = -ENOMEM;
		goto err_destroy_classdev;
	}

	err = sysfs_create_file(&asus_armoury.fw_attr_kset->kobj, &pending_reboot.attr);
	if (err) {
		pr_err("Failed to create sysfs level attributes\n");
		goto err_destroy_kset;
	}

	asus_armoury.mini_led_dev_id = 0;
	if (asus_wmi_is_present(ASUS_WMI_DEVID_MINI_LED_MODE)) {
		asus_armoury.mini_led_dev_id = ASUS_WMI_DEVID_MINI_LED_MODE;
	} else if (asus_wmi_is_present(ASUS_WMI_DEVID_MINI_LED_MODE2)) {
		asus_armoury.mini_led_dev_id = ASUS_WMI_DEVID_MINI_LED_MODE2;
	}

	if (asus_armoury.mini_led_dev_id) {
		err = sysfs_create_group(&asus_armoury.fw_attr_kset->kobj, &mini_led_mode_attr_group);
		if (err) {
			pr_err("Failed to create sysfs-group for mini_led\n");
			goto err_remove_file;
		}
	}

	asus_armoury.gpu_mux_dev_id = 0;
	if (asus_wmi_is_present(ASUS_WMI_DEVID_GPU_MUX)) {
		asus_armoury.gpu_mux_dev_id = ASUS_WMI_DEVID_GPU_MUX;
	} else if (asus_wmi_is_present(ASUS_WMI_DEVID_GPU_MUX_VIVO)) {
		asus_armoury.gpu_mux_dev_id = ASUS_WMI_DEVID_GPU_MUX_VIVO;
	}

	if (asus_armoury.gpu_mux_dev_id) {
		err = sysfs_create_group(&asus_armoury.fw_attr_kset->kobj, &gpu_mux_mode_attr_group);
		if (err) {
			pr_err("Failed to create sysfs-group for gpu_mux\n");
			goto err_remove_mini_led_group;
		}
	}

	for (i = 0; i < ARRAY_SIZE(armoury_attr_groups); i++) {
		if (!asus_wmi_is_present(armoury_attr_groups[i].wmi_devid))
			continue;

		err = sysfs_create_group(&asus_armoury.fw_attr_kset->kobj,
					 armoury_attr_groups[i].attr_group);
		if (err) {
			pr_err("Failed to create sysfs-group for %s\n",
			       armoury_attr_groups[i].attr_group->name);
			goto err_remove_groups;
		}
	}

	return 0;

err_remove_groups:
	while (--i >= 0) {
		if (asus_wmi_is_present(armoury_attr_groups[i].wmi_devid))
			sysfs_remove_group(&asus_armoury.fw_attr_kset->kobj, armoury_attr_groups[i].attr_group);
	}
	sysfs_remove_group(&asus_armoury.fw_attr_kset->kobj, &gpu_mux_mode_attr_group);
err_remove_mini_led_group:
	sysfs_remove_group(&asus_armoury.fw_attr_kset->kobj, &mini_led_mode_attr_group);
err_remove_file:
	sysfs_remove_file(&asus_armoury.fw_attr_kset->kobj, &pending_reboot.attr);
err_destroy_kset:
	kset_unregister(asus_armoury.fw_attr_kset);
err_destroy_classdev:
	device_destroy(fw_attr_class, MKDEV(0, 0));
fail_class_get:
	fw_attributes_class_put();
	return err;
}

/* Init / exit ****************************************************************/

/* Set up the min/max and defaults for ROG tunables */
static void init_rog_tunables(struct rog_tunables *rog)
{
	u32 platform_default = PPT_PLATFORM_DEFAULT;
	u32 cpu_default = PPT_CPU_LIMIT_DEFAULT;
	u32 platform_max = PPT_PLATFORM_MAX;
	u32 max_boost = NVIDIA_BOOST_MAX;
	u32 cpu_max = PPT_CPU_LIMIT_MAX;
	const char *product;

	/*
	 * ASUS product_name contains everything required, e.g,
	 * "ROG Flow X16 GV601VV_GV601VV_00185149B".
	 * The bulk of these defaults are gained from users reporting what
	 * ASUS Armoury Crate in Windows provides them.
	 * This should be turned in to a table eventually.
	 */
	product = dmi_get_system_info(DMI_PRODUCT_NAME);

	if (strstr(product, "GA402R")) {
		cpu_default = 125;
	} else if (strstr(product, "13QY")) {
		cpu_max = 250;
	} else if (strstr(product, "X13")) {
		cpu_max = 75;
		cpu_default = 50;
	} else if (strstr(product, "RC71") || strstr(product, "RC72")) {
		cpu_max = 50;
		cpu_default = 30;
	} else if (strstr(product, "G814") || strstr(product, "G614") ||
		   strstr(product, "G834") || strstr(product, "G634")) {
		cpu_max = 175;
	} else if (strstr(product, "GA402X") || strstr(product, "GA403") ||
		   strstr(product, "FA507N") || strstr(product, "FA507X") ||
		   strstr(product, "FA707N") || strstr(product, "FA707X")) {
		cpu_max = 90;
	} else {
		pr_notice("Using default CPU limits. Please report if these are not correct.\n");
	}

	if (strstr(product, "GZ301ZE"))
		max_boost = 5;
	else if (strstr(product, "FX507ZC4"))
		max_boost = 15;
	else if (strstr(product, "GU605"))
		max_boost = 20;

	/* ensure defaults for tunables */
	rog->cpu_default = cpu_default;
	rog->cpu_min = PPT_CPU_LIMIT_MIN;
	rog->cpu_max = cpu_max;

	rog->platform_default = platform_default;
	rog->platform_max = PPT_PLATFORM_MIN;
	rog->platform_max = platform_max;

	rog->ppt_pl1_spl = cpu_default;
	rog->ppt_pl2_sppt = cpu_default;
	rog->ppt_pl3_fppt = cpu_default;
	rog->ppt_apu_sppt = cpu_default;
	rog->ppt_platform_sppt = platform_default;

	rog->nv_boost_default = NVIDIA_BOOST_MAX;
	rog->nv_boost_min = NVIDIA_BOOST_MIN;
	rog->nv_boost_max = max_boost;
	rog->nv_dynamic_boost = NVIDIA_BOOST_MIN;

	rog->nv_temp_default = NVIDIA_TEMP_MAX;
	rog->nv_temp_min = NVIDIA_TEMP_MIN;
	rog->nv_temp_max = NVIDIA_TEMP_MAX;
	rog->nv_temp_target = NVIDIA_TEMP_MIN;

	rog->dgpu_tgp_default = NVIDIA_POWER_DEFAULT;
	rog->dgpu_tgp_min = NVIDIA_POWER_MIN;
	rog->dgpu_tgp_max = NVIDIA_POWER_MAX;
	rog->dgpu_tgp = NVIDIA_POWER_MAX;
}

/* Set up the min/max and defaults for ROG tunables */
static void init_rog_tunables(struct rog_tunables *rog)
{
	u32 platform_default = PPT_PLATFORM_DEFAULT;
	u32 cpu_default = PPT_CPU_LIMIT_DEFAULT;
	u32 platform_max = PPT_PLATFORM_MAX;
	u32 max_boost = NVIDIA_BOOST_MAX;
	u32 cpu_max = PPT_CPU_LIMIT_MAX;
	const char *product;

	/*
	 * ASUS product_name contains everything required, e.g,
	 * "ROG Flow X16 GV601VV_GV601VV_00185149B".
	 * The bulk of these defaults are gained from users reporting what
	 * ASUS Armoury Crate in Windows provides them.
	 * This should be turned in to a table eventually.
	 */
	product = dmi_get_system_info(DMI_PRODUCT_NAME);

	if (strstr(product, "GA402R")) {
		cpu_default = 125;
	} else if (strstr(product, "13QY")) {
		cpu_max = 250;
	} else if (strstr(product, "X13")) {
		cpu_max = 75;
		cpu_default = 50;
	} else if (strstr(product, "RC71") || strstr(product, "RC72")) {
		cpu_max = 50;
		cpu_default = 30;
	} else if (strstr(product, "G814") || strstr(product, "G614") ||
		   strstr(product, "G834") || strstr(product, "G634")) {
		cpu_max = 175;
	} else if (strstr(product, "GA402X") || strstr(product, "GA403") ||
		   strstr(product, "FA507N") || strstr(product, "FA507X") ||
		   strstr(product, "FA707N") || strstr(product, "FA707X")) {
		cpu_max = 90;
	} else {
		pr_notice("Using default CPU limits. Please report if these are not correct.\n");
	}

	if (strstr(product, "GZ301ZE"))
		max_boost = 5;
	else if (strstr(product, "FX507ZC4"))
		max_boost = 15;
	else if (strstr(product, "GU605"))
		max_boost = 20;

	/* ensure defaults for tunables */
	rog->cpu_default = cpu_default;
	rog->cpu_min = PPT_CPU_LIMIT_MIN;
	rog->cpu_max = cpu_max;

	rog->platform_default = platform_default;
	rog->platform_max = PPT_PLATFORM_MIN;
	rog->platform_max = platform_max;

	rog->ppt_pl1_spl = cpu_default;
	rog->ppt_pl2_sppt = cpu_default;
	rog->ppt_pl3_fppt = cpu_default;
	rog->ppt_apu_sppt = cpu_default;
	rog->ppt_platform_sppt = platform_default;

	rog->nv_boost_default = NVIDIA_BOOST_MAX;
	rog->nv_boost_min = NVIDIA_BOOST_MIN;
	rog->nv_boost_max = max_boost;
	rog->nv_dynamic_boost = NVIDIA_BOOST_MIN;

	rog->nv_temp_default = NVIDIA_TEMP_MAX;
	rog->nv_temp_min = NVIDIA_TEMP_MIN;
	rog->nv_temp_max = NVIDIA_TEMP_MAX;
	rog->nv_temp_target = NVIDIA_TEMP_MIN;
}

static int __init asus_fw_init(void)
{
	char *wmi_uid;
	int err;

	wmi_uid = wmi_get_acpi_device_uid(ASUS_WMI_MGMT_GUID);
	if (!wmi_uid)
		return -ENODEV;

	/*
	 * if equal to "ASUSWMI" then it's DCTS that can't be used for this
	 * driver, DSTS is required.
	 */
	if (!strcmp(wmi_uid, ASUS_ACPI_UID_ASUSWMI))
		return -ENODEV;

	asus_armoury.rog_tunables = kzalloc(sizeof(struct rog_tunables), GFP_KERNEL);
	if (!asus_armoury.rog_tunables)
		return -ENOMEM;

	init_rog_tunables(asus_armoury.rog_tunables);
	if (asus_wmi_is_present(ASUS_WMI_DEVID_CORES_MAX)) {
		err = init_max_cpu_cores();
		if (err) {
			kfree(asus_armoury.rog_tunables);
			pr_err("Could not initialise CPU core control %d\n", err);
			return err;
		}
	}

	asus_armoury.rog_tunables = kzalloc(sizeof(struct rog_tunables), GFP_KERNEL);
	if (!asus_armoury.rog_tunables)
		return -ENOMEM;

	init_rog_tunables(asus_armoury.rog_tunables);

	err = asus_fw_attr_add();
	if (err)
		return err;

	return 0;
}

static void __exit asus_fw_exit(void)
{
	mutex_lock(&asus_armoury.mutex);

	sysfs_remove_file(&asus_armoury.fw_attr_kset->kobj, &pending_reboot.attr);
	kset_unregister(asus_armoury.fw_attr_kset);
	device_destroy(fw_attr_class, MKDEV(0, 0));
	fw_attributes_class_put();

	mutex_unlock(&asus_armoury.mutex);
}

module_init(asus_fw_init);
module_exit(asus_fw_exit);

MODULE_IMPORT_NS("ASUS_WMI");
MODULE_AUTHOR("Luke Jones <luke@ljones.dev>");
MODULE_DESCRIPTION("ASUS BIOS Configuration Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("wmi:" ASUS_NB_WMI_EVENT_GUID);
