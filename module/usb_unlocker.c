/**
 * usb_unlocker module
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/string.h>

#include "usb_unlocker.h"
#include "usb_unlocker_user.h"

#define CJ_DUBUG


MODULE_LICENSE("MIT");
MODULE_AUTHOR("Chaojie Wang");

/* key for doing encryption and decryption */
#define USB_UNLOCKER_KEY ("THIS IS KEY")

/* only allow one interface for this driver */
static atomic_t usb_unlocker_count = ATOMIC_INIT(1);

/**
 * params when installing the driver:
 *  @ioctl_on: bool, if ioctl is enabled or not.
 *  @mod_name: charp, name of the module.
 *  @crypto_folder: charp, folder to be encrypted.
 *  @helper_path: charp, path to the helper function
 *  @vendor_id: int, vendor id.
 */
static const bool ioctl_on = 0;   	                         /* default is off */
static const char *mod_name;
static const char *crypto_folder;
static const char *helper_path;
static const int vendor_id;
static const struct usb_unlocker_config usb_unlocker_config;

module_param(ioctl_on, bool, 0444);
module_param(mod_name, charp, 0444);
module_param(crypto_folder, charp, 0444);
module_param(helper_path, charp, 0444);
module_param(vendor_id, int, 0440);
module_param_named(product, usb_unlocker_config.product, charp, 0);
module_param_named(manufacturer, usb_unlocker_config.manufacturer,
		   charp, 0);
module_param_named(serial, usb_unlocker_config.serial, charp, 0);


static int usb_unlocker_probe(struct usb_interface *intf, 
			      const struct usb_device_id *id) {
#ifdef CJ_DEBUG
	printk(KERN_INFO "usb_unlocker_probe is invoke\n");
#endif

	if (atomic_add_return(-1, &usb_unlocker_count) < 0) {
		atomic_add(1, &usb_unlocker_count);
		return -EBUSY;
	}

	if (ioctl_on) {
		/* if ioctl is on, no need to set up private data,
		 * because this is in configuration stage.
		 */
		return 0;
	}
	

	if (unlikely(intf->condition != USB_INTERFACE_UNBOUND)) {
		/* some other driver has setup the data? */
		printk(KERN_ERR "other driver has taken or taking it\n");
		return -EAGAIN;
	}
	
        /* just accept this interface */
	struct usb_unlocker_data *data = NULL;
	data = kmalloc(sizeof(struct usb_unlocker_data), GFP_KERNEL);
	if (!data || !key) {
		printk(KERN_ERR "out of mem\n");
		kfree(data);
		kfree(key);
		return -ENOMEM;
	}
		
	data.key = USB_UNLOCKER_KEY;
	data.config = usb_unlocker_config;
	usb_set_intfdata(intf, data);

	/* TODO */
	/* spawn a program in userspace to do encryption */
	char *argv[] = {
		crypto_folder,
		USB_UNLOCKER_KEY,
		"0",
		NULL
	};
	char *envp[] = {
		"PATH=/sbin:/usr/sbin:/bin:/usr/bin",
		NULL
	};
	if (call_usermodehelper(helper_path, argv, envp, UMH_KILLABLE)) {
		printk(KERN_ERR "error during calling helper function?\n");
		return -ENOEXEC;
	}

	return 0;
}

static void usb_unlocker_disconnect(struct usb_interface *intf) {
#ifdef CJ_DEBUG
	printk(KERN_INFO "disconnect is invoked\n");
#endif

	struct usb_unlocker_data *data = usb_get_intfdata(intf);
	if (unlikely(!data)) {
		printk(KERN_ERR "no private data?");
		return -ENODATA;
	}

	kfree(data);
	usb_set_intfdata(intf, NULL);
	usb_put_intf(intf);
}

static int usb_unlocker_ioctl(struct usb_interface *intf, unsigned int code
			      void *buf) {
#ifdef CJ_DEBUG
	printk(KERN_INFO "ioctl is invoked\n");
	printk(KERN_INFO "the interface data address: %p", usb_get_intfdata(intf));
#endif
	if (!ioctl_on) {
		/* if ioctl is off */
		return -EPERM;
	}

	/* we might need to do some "configuration" here */
	struct usb_unlocker_config *config = buf;
	int ret;

	memset(config, 0, sizeof(struct usb_unlocker_config));

	switch (code) {
	case UNLOCKER_GET_CONFIG:
		/* getting the device that is plugged in */
		struct usb_device *unlocker_device = usb_get_dev(interface_to_usbdev(intf));
		if (!unlocker_device) {
			printk(KERN_ERR "not able to get the device?\n");
			ret = -ENXIO;
			goto error;
		}

		/* copy the configuration data to the user */
		char *prod = NULL, *mft = NULL, *srl = NULL;
		prod = kmalloc(CONFIG_BUFFER_MAX_SIZE*sizeof(char), GFP_KERNEL);
		mft = kmalloc(CONFIG_BUFFER_MAX_SIZE*sizeof(char), GFP_KERNEL);
		srl = kmalloc(CONFIG_BUFFER_MAX_SIZE*sizeof(char), GFP_KERNEL);
		memset(prod, 0, CONFIG_BUFFER_MAX_SIZE);
		memset(mft, 0, CONFIG_BUFFER_MAX_SIZE);
		memset(srl, 0, CONFIG_BUFFER_MAX_SIZE);
		if (!prod || !mft || !srl) {
			printk(KERN_ERR "no mem\n");
			kfree(prod);
			kfree(mft);
			kfree(srl);
			goto error;
		}
		strncpy(prod, unlocker_device->product, CONFIG_BUFFER_MAX_SIZE-1);
		strncpy(mft, unlocker_device->manufacturer, CONFIG_BUFFER_MAX_SIZE-1);
		strncpy(srl, unlocker_device->serial, CONFIG_BUFFER_MAX_SIZE-1);

		if (copy_to_user(buf->product, prod, CONFIG_BUFFER_MAX_SIZE) 
		    || copy_to_user(buf->manufacturer, mft, CONFIG_BUFFER_MAX_SIZE)
		    || copy_to_user(buf->serial, srl, CONFIG_BUFFER_MAX_SIZE)) {
			ret = -EFAULT;
			goto error;
		}
		
		kfree(prod);
		kfree(mft);
		kfree(srl);
		prod = NULL;
		mft = NULL;
		srl = NULL;

		/* dec the ref count */
		usb_put_dev(device_conn);
		break;
	default:
		printk(KERN_ERR "unknown ioctl\n");
		ret = -EINVAL;
		goto error;
	}
	
	return 0;
error:
	return ret;
	
}


static struct usb_device_id usb_unlocker_id_table[] = {
	{
		.match_flags = USB_DEVICE_ID_MATCH_VENDOR,
		.idVendor = vendor_id
	},
	{}			/* terminator */
};

MODULE_DEVICE_TABLE(usb, usb_unlocker_id_table);

static struct usb_driver usb_unlocker_driver = {
	.probe = usb_unlocker_probe,
	.disconnect = usb_unlocker_disconnect,
	.unlocked_ioctl = usb_unlocker_ioctl,
	.usb_device_id = usb_unlocker_id_table
}

	
static int __init usb_unlocker_init(void) {
#ifdef CJ_DEBUG
	printk(KERN_INFO "usb_unlocker is initialized\n");

	/* print out all param from the user */
	printk(KERN_INFO "ioctl_on is %d\n", (int) ioctl_on);
	printk(KERN_INFO "mod_name is %s\n", mod_name);
	printk(KERN_INFO "crypto_folder is %s\n", crypto_folder);
	printk(KERN_INFO "helper_path is %s\n", helper_path);
	printk(KERN_INFO "vendor_id is %d\n", vendor_id);
#endif 
	
	/* checking parameters from user*/
	if (!mod_name || !crypto_folder || !helper_path || !vendor_id) {
		printk(KERN_ERR "user did not supply enough parameters\n");
		goto error;
	}

	if (!ioctl_on) {
		/* configuration is done. Specific data should be availabe */
		if (!usb_unlocker_config.product || !usb_unlocker_config.manufacturer
		    || !usb_unlocker_config.serial) {
			printk(KERN_ERR "missing key parameter");
			goto error;
		}
		
	}

	/* register the driver */
	int ret = usb_register_driver(&usb_unlocker_driver, THIS_MODULE, mod_name);
	if (!ret) {
		printk(KERN_ERR "register usb_unlocker_driver failed\n");
		goto error;
	}
	
	return 0;

error:
	return -EINVAL;
}

	
static void usb_unlocker_exit(void) {
	usb_deregister(&usb_unlocker_driver);
}

module_init(usb_unlocker_init);
module_exit(usb_unlocker_exit);
