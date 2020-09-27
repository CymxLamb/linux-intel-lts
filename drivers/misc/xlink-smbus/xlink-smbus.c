// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xlink I2C Adapter Driver
 *
 * SMBus transfer over Xlink
 *
 * Copyright (C) 2020 Intel Corporation
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/stddef.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/xlink.h>
#include <linux/time.h>
#include <linux/kmod.h>
#include <linux/kthread.h>
#include <linux/hddl_device.h>
#include <linux/slab.h>

/* Define the xlink debug device structures to be used with dev_dbg() et al */

static struct device_driver dbg_name = {
	.name = "xlink_i2c_dbg"
};

static struct device dbg_subname = {
	.init_name = "xlink_i2c_dbg",
	.driver = &dbg_name
};

static struct device *dbgxi2c = &dbg_subname;

struct xlink_msg {
	u16			addr;
	unsigned short		flags;
	char			read_write;
	u8			command;
	int			protocol;
	union i2c_smbus_data	data;
	int			status;
	struct list_head	node;
};

struct xlink_adapter_data {
	struct xlink_handle *xhandle;
	u32		channel;
	struct completion work;
	struct task_struct *task_recv;
	struct i2c_client *slave;
	struct list_head head;
	struct i2c_adapter *adap;
};


#define XLINKI2C_XLINK_CHANNEL_BASE	1055

static struct i2c_adapter *get_adapter_from_channel(u32 channel)
{
#if defined(CONFIG_XLINKI2C_ADAPTER)
	// this is an adapter by itself.
	//it doesn't proxy transfer on another adapter
	return NULL;
#else
	/* PROXY the commands usign existing adapter */
	s32 nr = channel - XLINKI2C_XLINK_CHANNEL_BASE;

	nr = 2;
	if (nr < 0)
		return NULL;

	return i2c_get_adapter(nr);
#endif
}

#if IS_ENABLED(CONFIG_I2C_SLAVE)
static s32 handle_slave_mode(struct i2c_client *slave, struct xlink_msg *msg)
{
	//the complete slave protocol is implemented in one shot here as
	//the whole chunk of data is transferred or received via xlink
	//, not byte-by-byte
	u8 temp;

	/* refer https://lwn.net/Articles/640346/ for protocol */

	/* send the command as first write */
	i2c_slave_event(slave, I2C_SLAVE_WRITE_REQUESTED, 0 /* unused */);
	i2c_slave_event(slave, I2C_SLAVE_WRITE_RECEIVED, &(msg->command));

	/* now handle specifics to read/write */
	if (msg->read_write == I2C_SMBUS_WRITE) {
		if (msg->protocol == I2C_SMBUS_BYTE_DATA) {
			i2c_slave_event(slave, I2C_SLAVE_WRITE_RECEIVED,
					&(msg->data.byte));
		} else if (msg->protocol == I2C_SMBUS_WORD_DATA) {
			temp = msg->data.word & 0xFF;
			i2c_slave_event(slave,
					I2C_SLAVE_WRITE_RECEIVED,
					&temp);
			temp = (msg->data.word >> 8) & 0xFF;
			i2c_slave_event(slave,
					I2C_SLAVE_WRITE_RECEIVED,
					&temp);
		} else if (msg->protocol == I2C_SMBUS_BLOCK_DATA) {
			int i;

			for (i = 1; (i < msg->data.block[0] ||
					i <= I2C_SMBUS_BLOCK_MAX); ++i) {
				i2c_slave_event(slave,
						I2C_SLAVE_WRITE_RECEIVED,
						&(msg->data.block[i]));
			}
		} else {
			dev_err(dbgxi2c,
				"unknown protocol (%d) received in %s\n",
				msg->protocol,
				__func__
				);
		}
	} else {
		if (msg->protocol == I2C_SMBUS_BYTE_DATA) {
			i2c_slave_event(slave,
					I2C_SLAVE_READ_REQUESTED,
					&(msg->data.byte));
		} else if (msg->protocol == I2C_SMBUS_WORD_DATA) {
			i2c_slave_event(slave,
					I2C_SLAVE_READ_REQUESTED,
					&temp);
			msg->data.word = temp << 8;
			i2c_slave_event(slave,
					I2C_SLAVE_READ_REQUESTED,
					&temp);
			msg->data.word |= temp;
		} else if (msg->protocol == I2C_SMBUS_BLOCK_DATA) {
			int i;

			for (i = 1; (i < msg->data.block[0] ||
					i <= I2C_SMBUS_BLOCK_MAX); ++i) {
				i2c_slave_event(slave,
						I2C_SLAVE_READ_REQUESTED,
						&(msg->data.block[i]));
			}
		} else {
			dev_err(dbgxi2c,
				"unknown protocol (%d) received in %s\n",
				msg->protocol,
				__func__);
		}
		i2c_slave_event(slave, I2C_SLAVE_READ_PROCESSED, &temp);
	}
	i2c_slave_event(slave, I2C_SLAVE_STOP, 0 /* unused */);
	return 0;
}
#endif

static s32 xlink_smbus_xfer(struct i2c_adapter *adap, u16 addr,
				unsigned short flags, char read_write,
				u8 command, int protocol,
				union i2c_smbus_data *data)
{
	struct xlink_msg *msg;
	u32 rc = 0;
#if defined(CONFIG_XLINKI2C_ADAPTER)
	enum xlink_error xerr;
	struct xlink_handle *devH = NULL;
#endif
	struct xlink_adapter_data *adapt_data = i2c_get_adapdata(adap);

	dev_dbg(dbgxi2c, "%s was called with the following parameters:\n",
		__func__);
	dev_dbg(dbgxi2c, "addr = %.4x\n", addr);
	dev_dbg(dbgxi2c, "flags = %.4x\n", flags);
	dev_dbg(dbgxi2c, "read_write = %s\n",
			read_write == I2C_SMBUS_WRITE ? "write" : "read");
	dev_dbg(dbgxi2c, "command = %d\n", command);
	dev_dbg(dbgxi2c, "protocol = %d\n", protocol);
	dev_dbg(dbgxi2c, "data = %p\n", data);
	msg = kzalloc(sizeof(struct xlink_msg), GFP_KERNEL);
	if (!msg)
		return X_LINK_ERROR;
#if defined(CONFIG_XLINKI2C_ADAPTER)
	msg->addr = addr;
	msg->flags = flags;
	msg->read_write = read_write;
	msg->command = command;
	msg->protocol = protocol;
	if (data)
		msg->data = *data;
	msg->status = 0;
	devH = adapt_data->xhandle;
	xerr = xlink_write_data(adapt_data->xhandle, adapt_data->channel,
				(u8 *)msg,
				sizeof(struct xlink_msg));
	//kfree(msg);
	if (xerr != X_LINK_SUCCESS) {
		dev_err(dbgxi2c,
			"xlink_write_data failed (%d) dropping packet.\n",
			xerr);
		return -ENODEV;
	}
#endif	/* CONFIG_XLINKI2C_ADAPTER */
	if (wait_for_completion_interruptible_timeout(
			&adapt_data->work,
			4*HZ) > 0) {
		msg = list_first_entry(
			&adapt_data->head,
			struct xlink_msg,
			node);
		list_del(&msg->node);
		if (data)
			*data = msg->data;
		rc = msg->status;
		//kfree(msg);
	} else {
		rc = -ETIMEDOUT;
	}
	return rc;
}



static int xlinki2c_receive_thread(void *param)
{
	enum xlink_error xerr;
	struct i2c_adapter *adap;
	struct xlink_adapter_data *adapt_data =
				(struct xlink_adapter_data *)param;
	u32 size;
	struct xlink_msg *msg;
	struct device *dev = &adapt_data->adap->dev;

	dev_dbg(dbgxi2c, "xlinknet receive thread started [%p].\n",
			adapt_data);
	dev_dbg(dbgxi2c, "xlinknet adapt_data channel [%d].\n",
			adapt_data->channel);
	dev_dbg(dbgxi2c, "xlinknet adapt_data xhandle[%p].\n",
			adapt_data->xhandle);
	while (!kthread_should_stop()) {
		msg = kzalloc(sizeof(struct xlink_msg), GFP_KERNEL);
		if (!msg)
			return X_LINK_ERROR;
		xerr = xlink_read_data_to_buffer(adapt_data->xhandle,
						adapt_data->channel,
						(uint8_t *)msg, &size);
		if (xerr != X_LINK_SUCCESS) {
			if (xerr != X_LINK_TIMEOUT) {
				dev_warn(dev,
					"[%d] Error (%d) dropping packet.\n",
					adapt_data->adap->nr, xerr);
			}
			kfree(msg);
			continue;
		}
		xlink_release_data(adapt_data->xhandle, adapt_data->channel,
						NULL);
		adap = get_adapter_from_channel(adapt_data->channel);
		if (adap) {
#if IS_ENABLED(CONFIG_I2C_SLAVE)
			if (adapt_data->slave != NULL) {
				msg->status =
				handle_slave_mode(adapt_data->slave, msg);
			} else {
#endif
				/* this is a proxy for an existing adapter. */
				msg->status = i2c_smbus_xfer(
						adap,
						msg->addr,
						msg->flags,
						msg->read_write,
						msg->command,
						msg->protocol,
						&msg->data);
#if IS_ENABLED(CONFIG_I2C_SLAVE)
			}
#endif
	/* send back the complete message that carries status back to sender */
			xlink_write_data(adapt_data->xhandle,
			adapt_data->channel, (u8 *)msg,
			sizeof(struct xlink_msg));
			//kfree(msg);
		} else {
			/* this is an adapter on its own. */
			list_add_tail(&msg->node, &adapt_data->head);
			complete(&adapt_data->work);
		}
	}
	dev_dbg(dev, "[%d] %s stopped\n", adapt_data->adap->nr, __func__);

	return 0;
}

static u32 xlink_smbus_func(struct i2c_adapter *adapter)
{
	u32 func = I2C_FUNC_SMBUS_QUICK | I2C_FUNC_SMBUS_BYTE |
		I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |
		I2C_FUNC_SMBUS_BLOCK_DATA;

	return func;
}

#if IS_ENABLED(CONFIG_I2C_SLAVE)

//this will be called when slave client driver
//register itself to an adapter
static int xlink_smbus_reg_slave(struct i2c_client *slave)
{
	struct xlink_adapter_data *adapt_data =
				i2c_get_adapdata(slave->adapter);

	adapt_data->slave = slave;

	return 0;
}

static int xlink_smbus_unreg_slave(struct i2c_client *slave)
{
	struct xlink_adapter_data *adapt_data =
				i2c_get_adapdata(slave->adapter);

	adapt_data->slave = NULL;

	return 0;
}
#endif

static struct i2c_algorithm xlink_algorithm = {
	.smbus_xfer     = xlink_smbus_xfer,
	.functionality  = xlink_smbus_func,
#if IS_ENABLED(CONFIG_I2C_SLAVE)
	.reg_slave      = xlink_smbus_reg_slave,
	.unreg_slave    = xlink_smbus_unreg_slave,
#endif
};

static int xlink_i2c_probe(struct platform_device *pdev)
{
	struct xlink_adapter_data *adapt_data;
	uint32_t rc = 0;
	struct intel_hddl_clients *c = pdev->dev.platform_data;
	struct xlink_handle *devH = &c->xlink_dev;
	struct i2c_adapter *adap;
	struct device *dev = &pdev->dev;

	dev_dbg(dev, "Registering xlink I2C adapter...\n");

	adap = kzalloc(sizeof(struct i2c_adapter), GFP_KERNEL);
	c->adap[pdev->id & 0x3] = adap;
	memset(adap, 0, sizeof(struct i2c_adapter));
	adap->class = 0; //I2C_CLASS_HWMON;
	adap->owner  = THIS_MODULE;
	adap->algo   = &xlink_algorithm;
	strcpy(&adap->name[0], "xlink adapter");
	platform_set_drvdata(pdev, adap);

	adapt_data = kzalloc(sizeof(struct xlink_adapter_data), GFP_KERNEL);

	init_completion(&adapt_data->work);

	INIT_LIST_HEAD(&adapt_data->head);
	/* TODO: connect and assign channel number */
	adapt_data->channel = c->xlink_i2c_ch[pdev->id & 0x3];
	adapt_data->slave = NULL;
	adapt_data->xhandle = devH;
	adapt_data->adap = adap;

	rc = xlink_open_channel(devH,
			adapt_data->channel,
			RXB_TXB,  /* mode */
			64*1024,
			100   /* timeout */);
	if (rc != X_LINK_SUCCESS) {
		dev_err(dev, "xlink_open_channel failed[%d][%d][%p]\n", rc,
		  adapt_data->channel,
		  adapt_data->xhandle);
		kfree(adap);
		return -EINVAL;
	}
	dev_dbg(dev, "xlink_open_channel completed[%d][%d][%p]\n", rc,
			adapt_data->channel,
			adapt_data->xhandle);

	i2c_set_adapdata(adap, adapt_data);

	rc = i2c_add_adapter(adap);

	dev_info(&adap->dev, "xlink_smbus_adapter[%x] [%d]\n",
		devH->sw_device_id,
		adap->nr);
	/* create receiver thread */
	adapt_data->task_recv = kthread_run(xlinki2c_receive_thread,
					adapt_data,
					"xlinki2c_receive_thread");
	if (adapt_data->task_recv == NULL)
		dev_err(dev, "%s Thread creation failed", __func__);
	return rc;
}

static int xlink_i2c_remove(struct platform_device *pdev)
{
	struct i2c_adapter *adap = platform_get_drvdata(pdev);
	struct xlink_adapter_data *adapt_data = i2c_get_adapdata(adap);

	kthread_stop(adapt_data->task_recv);

	/* close the channel and disconnect */
	xlink_close_channel(adapt_data->xhandle, adapt_data->channel);
	/* This will block the dynamic registration */
	i2c_del_adapter(adapt_data->adap);
	kfree(adapt_data);
	dev_info(&adap->dev, "delete the adapter[%d]\n", adap->nr);

	return 0;
}

static struct platform_driver xlink_i2c_driver = {
	.probe = xlink_i2c_probe,
	.remove = xlink_i2c_remove,
	.driver = {
		.name   = "i2c_xlink"
	}
};

static void __exit xlink_adapter_exit(void)
{
	dev_dbg(dbgxi2c, "Unloading XLink I2C module...\n");
	platform_driver_unregister(&xlink_i2c_driver);
}

static int __init xlink_adapter_init(void)
{
	dev_dbg(dbgxi2c, "Loading XLink I2C module...\n");
	platform_driver_register(&xlink_i2c_driver);
	return 0;

}

module_init(xlink_adapter_init);
module_exit(xlink_adapter_exit);

MODULE_AUTHOR("Raja Subramanian, Lakshmi Bai <lakshmi.bai.raja.subramanian@intel.com>");
MODULE_AUTHOR("Thalaiappan, Rathina <rathina.thalaiappan@intel.com>");
MODULE_DESCRIPTION("xlink i2c adapter");
MODULE_LICENSE("GPL");
