// SPDX-License-Identifier: GPL-2.0
/* Small from-scratch INA219 I2C driver for learning and experimentation. */
#include <linux/err.h>
#include <linux/bitops.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/version.h>

#define INA219_REG_CONFIG             0x00
#define INA219_REG_SHUNT_VOLTAGE      0x01
#define INA219_REG_BUS_VOLTAGE        0x02
#define INA219_REG_POWER              0x03
#define INA219_REG_CURRENT            0x04
#define INA219_REG_CALIBRATION        0x05
#define INA219_CONFIG_RESET           0x8000
#define INA219_CONFIG_CONTINUOUS      0x399F
#define INA219_BUS_OVF                BIT(0)
#define INA219_DEFAULT_SHUNT_UOHM     100000
#define INA219_DEFAULT_CURRENT_LSB_UA 100
#define INA219_MIN_SHUNT_UOHM         1000
#define INA219_MAX_SHUNT_UOHM         10000000

struct ina219_data {
	struct i2c_client *client;
	struct mutex lock;
	u32 shunt_uohm;
	u32 current_lsb_ua;
	u16 calibration;
};

static int ina219_read_reg(struct ina219_data *data, u8 reg)
{
	return i2c_smbus_read_word_swapped(data->client, reg);
}

static int ina219_write_reg(struct ina219_data *data, u8 reg, u16 value)
{
	return i2c_smbus_write_word_swapped(data->client, reg, value);
}

static int ina219_calculate_calibration(u32 shunt_uohm, u32 current_lsb_ua,
					u16 *calibration)
{
	u64 denominator = (u64)shunt_uohm * current_lsb_ua;
	u64 result;

	if (!denominator)
		return -EINVAL;
	result = div_u64(40960000000ULL, denominator);
	if (!result || result > U16_MAX)
		return -ERANGE;
	*calibration = result;
	return 0;
}

/* The register transactions share one mutex with calibration changes. */
static int ina219_read_bus_locked(struct ina219_data *data, int *millivolts,
				  bool *overflow)
{
	int raw = ina219_read_reg(data, INA219_REG_BUS_VOLTAGE);

	if (raw < 0)
		return raw;
	*millivolts = ((raw >> 3) & 0x1fff) * 4;
	*overflow = raw & INA219_BUS_OVF;
	return 0;
}

static ssize_t voltage_mv_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct ina219_data *data = dev_get_drvdata(dev);
	int millivolts, ret;
	bool overflow;

	mutex_lock(&data->lock);
	ret = ina219_read_bus_locked(data, &millivolts, &overflow);
	mutex_unlock(&data->lock);
	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%d\n", millivolts);
}

static ssize_t shunt_voltage_uv_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct ina219_data *data = dev_get_drvdata(dev);
	int raw, ret;

	mutex_lock(&data->lock);
	raw = ina219_read_reg(data, INA219_REG_SHUNT_VOLTAGE);
	mutex_unlock(&data->lock);
	if (raw < 0)
		return raw;
	ret = (s16)raw * 10;
	return sysfs_emit(buf, "%d\n", ret);
}

static ssize_t current_ua_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct ina219_data *data = dev_get_drvdata(dev);
	int raw, mv, ret;
	bool overflow;
	s64 current;

	mutex_lock(&data->lock);
	ret = ina219_read_bus_locked(data, &mv, &overflow);
	if (!ret && overflow)
		ret = -EOVERFLOW;
	if (!ret)
		raw = ina219_read_reg(data, INA219_REG_CURRENT);
	else
		raw = ret;
	if (!ret && raw < 0)
		ret = raw;
	current = (s64)(s16)raw * data->current_lsb_ua;
	mutex_unlock(&data->lock);
	if (ret == -EOVERFLOW)
		dev_warn_ratelimited(dev, "INA219 overflow; current is invalid\n");
	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%lld\n", current);
}

static ssize_t power_uw_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct ina219_data *data = dev_get_drvdata(dev);
	int raw, mv, ret;
	bool overflow;
	s64 power;

	mutex_lock(&data->lock);
	ret = ina219_read_bus_locked(data, &mv, &overflow);
	if (!ret && overflow)
		ret = -EOVERFLOW;
	if (!ret)
		raw = ina219_read_reg(data, INA219_REG_POWER);
	else
		raw = ret;
	if (!ret && raw < 0)
		ret = raw;
	power = (s64)raw * 20 * data->current_lsb_ua;
	mutex_unlock(&data->lock);
	if (ret == -EOVERFLOW)
		dev_warn_ratelimited(dev, "INA219 overflow; power is invalid\n");
	if (ret < 0)
		return ret;
	return sysfs_emit(buf, "%lld\n", power);
}

static ssize_t shunt_resistor_uohm_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct ina219_data *data = dev_get_drvdata(dev);
	u32 value;

	mutex_lock(&data->lock);
	value = data->shunt_uohm;
	mutex_unlock(&data->lock);
	return sysfs_emit(buf, "%u\n", value);
}

static ssize_t shunt_resistor_uohm_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct ina219_data *data = dev_get_drvdata(dev);
	u32 shunt;
	u16 calibration;
	int ret;

	ret = kstrtou32(buf, 10, &shunt);
	if (ret)
		return ret;
	if (shunt < INA219_MIN_SHUNT_UOHM || shunt > INA219_MAX_SHUNT_UOHM)
		return -EINVAL;
	ret = ina219_calculate_calibration(shunt, data->current_lsb_ua,
					  &calibration);
	if (ret)
		return ret;

	mutex_lock(&data->lock);
	ret = ina219_write_reg(data, INA219_REG_CALIBRATION, calibration);
	if (!ret) {
		data->shunt_uohm = shunt;
		data->calibration = calibration;
	}
	mutex_unlock(&data->lock);
	return ret ? ret : count;
}

static DEVICE_ATTR_RO(voltage_mv);
static DEVICE_ATTR_RO(shunt_voltage_uv);
static DEVICE_ATTR_RO(current_ua);
static DEVICE_ATTR_RO(power_uw);
static DEVICE_ATTR_RW(shunt_resistor_uohm);

static struct attribute *ina219_attrs[] = {
	&dev_attr_voltage_mv.attr,
	&dev_attr_shunt_voltage_uv.attr,
	&dev_attr_current_ua.attr,
	&dev_attr_power_uw.attr,
	&dev_attr_shunt_resistor_uohm.attr,
	NULL,
};
ATTRIBUTE_GROUPS(ina219);

static int ina219_probe(struct i2c_client *client
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
			, const struct i2c_device_id *id
#endif
)
{
	struct ina219_data *data;
	u32 shunt = INA219_DEFAULT_SHUNT_UOHM;
	int ret, value;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_WORD_DATA))
		return -EOPNOTSUPP;
	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->client = client;
	data->current_lsb_ua = INA219_DEFAULT_CURRENT_LSB_UA;
	ret = device_property_read_u32(&client->dev,
				       "shunt-resistor-micro-ohms", &shunt);
	if (ret && ret != -EINVAL && ret != -ENODATA)
		return ret;
	if (shunt < INA219_MIN_SHUNT_UOHM || shunt > INA219_MAX_SHUNT_UOHM)
		return -EINVAL;
	data->shunt_uohm = shunt;
	mutex_init(&data->lock);
	i2c_set_clientdata(client, data);

	ret = ina219_calculate_calibration(shunt, data->current_lsb_ua,
					  &data->calibration);
	if (ret)
		return ret;
	mutex_lock(&data->lock);
	ret = ina219_write_reg(data, INA219_REG_CONFIG, INA219_CONFIG_RESET);
	if (!ret)
		ret = ina219_write_reg(data, INA219_REG_CONFIG,
				       INA219_CONFIG_CONTINUOUS);
	if (!ret)
		ret = ina219_write_reg(data, INA219_REG_CALIBRATION,
				       data->calibration);
	if (!ret) {
		value = ina219_read_reg(data, INA219_REG_CALIBRATION);
		if (value < 0)
			ret = value;
		else if (value != data->calibration)
			ret = -ENODEV;
	}
	if (ret)
		ina219_write_reg(data, INA219_REG_CONFIG,
				 INA219_CONFIG_CONTINUOUS & ~0x0007);
	mutex_unlock(&data->lock);
	if (ret) {
		dev_err(&client->dev, "failed to initialize INA219: %d\n", ret);
		return ret;
	}
	ret = devm_device_add_group(&client->dev, &ina219_group);
	if (ret) {
		dev_err(&client->dev, "failed to create sysfs attributes: %d\n", ret);
		return ret;
	}
	dev_info(&client->dev, "INA219 demo driver ready (shunt %u uOhm)\n",
		 shunt);
	return 0;
}

static void ina219_power_down(struct ina219_data *data)
{
	int config;

	mutex_lock(&data->lock);
	config = ina219_read_reg(data, INA219_REG_CONFIG);
	if (config >= 0)
		ina219_write_reg(data, INA219_REG_CONFIG, config & ~0x0007);
	mutex_unlock(&data->lock);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
static void ina219_remove(struct i2c_client *client)
#else
static int ina219_remove(struct i2c_client *client)
#endif
{
	struct ina219_data *data = i2c_get_clientdata(client);

	ina219_power_down(data);
	dev_info(&client->dev, "INA219 demo driver removed\n");
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
	return 0;
#endif
}

static const struct of_device_id ina219_of_match[] = {
	{ .compatible = "custom,ina219-demo" },
	{ }
};
MODULE_DEVICE_TABLE(of, ina219_of_match);

static const struct i2c_device_id ina219_id[] = {
	{ "ina219_demo", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ina219_id);

static struct i2c_driver ina219_driver = {
	.driver = {
		.name = "ina219_demo",
		.of_match_table = ina219_of_match,
	},
	.probe = ina219_probe,
	.remove = ina219_remove,
	.id_table = ina219_id,
};
module_i2c_driver(ina219_driver);

MODULE_AUTHOR("INA219 learning project");
MODULE_DESCRIPTION("Learning I2C driver for the INA219 power monitor");
MODULE_LICENSE("GPL");
