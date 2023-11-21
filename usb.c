#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/list.h>
#include <linux/cdev.h>
#include <linux/fs.h>

#include "usb.h"

struct hpg_usb_dev {
	struct usb_device      *udev;
	struct usb_interface   *interface;
	struct class	       *class;
	struct list_head	pwm_devices_list;
	int8_t			devfs_num;
	dev_t			first_number;
	size_t			minors_cnt;
	struct kref		kref;
};
static void hpg_usb_dev_delete(struct kref *kref);
#define to_hpg_usb_dev(d) container_of(d, struct hpg_usb_dev, kref)

struct hpg_pwm_devfs_dev {
	struct hpg_usb_dev     *my_dev;
	struct cdev		cdev;
	uint8_t			endpoint_addr;
	int32_t			current_pwm;
	bool			did_read;
	struct list_head	list;
	struct kref		kref;
};
static void hpg_pwm_devfs_dev_delete(struct kref *kref);
#define to_hpg_pwm_devfs_dev(d) container_of(d, struct hpg_pwm_devfs_dev, kref)

static int hpg_probe(struct usb_interface *interface, const struct usb_device_id *id);
static ssize_t hpg_send_data(struct hpg_pwm_devfs_dev *pwm_dev, char *data, size_t count);
static void hpg_disconnect(struct usb_interface *interface);
static int hpg_pwm_open(struct inode *inode, struct file *file);
static ssize_t hpg_pwm_read(struct file *file, char __user *buffer, size_t count, loff_t *ppos);
static ssize_t hpg_pwm_write(struct file *file, const char __user *buffer, size_t count, loff_t *ppos);
static int hpg_pwm_release(struct inode *inode, struct file *file);

static const struct usb_device_id usb_id_table[] = {
	/* Hexapod PWM (pid.codes, pid.codes Test PID) */
	{ USB_DEVICE(0x1209, 0x0001) },

	/* Terminating entry */
	{ }
};
MODULE_DEVICE_TABLE (usb, usb_id_table);

static struct usb_driver hpg_usb_driver = {
	.name = "hp-gadget",
	.probe = hpg_probe,
	.disconnect = hpg_disconnect,
	.id_table = usb_id_table,
};

static const struct file_operations hpg_pwm_fops =
{
	.owner		= THIS_MODULE,
	.open		= hpg_pwm_open,
	.release	= hpg_pwm_release,
	.read		= hpg_pwm_read,
	.write		= hpg_pwm_write,
};

#define MAX_DEVFS_DEVS	    64
#define MAX_DEVFS_DEVS_str "64"
static bool devfs_dev_created[MAX_DEVFS_DEVS];

static int8_t alloc_new_devfs_num(void)
{
	int8_t i;

	for (i = 0; i < MAX_DEVFS_DEVS; i++) {
		if (!devfs_dev_created[i]) {
			devfs_dev_created[i] = true;
			return i;
		}
	}

	return -1;
}

static void free_devfs_num(int8_t num)
{
	devfs_dev_created[num] = false;
}

int hpg_register(void)
{
	int retval;
	
	retval = usb_register(&hpg_usb_driver);
	if (retval) {
		printk(KERN_ERR "hp-gadget: usb_register failed. "
				"Error number %d.", retval);
	}

	return retval;
}

static void hpg_usb_dev_delete(struct kref *kref)
{
	struct hpg_usb_dev *dev =
		to_hpg_usb_dev(kref);

	struct list_head *pos ; list_for_each(pos, &dev->pwm_devices_list) {
		struct hpg_pwm_devfs_dev *pwm_dev =
			list_entry(pos, struct hpg_pwm_devfs_dev, list);
		
		kref_put(&pwm_dev->kref, hpg_pwm_devfs_dev_delete);
	}

	unregister_chrdev_region(dev->first_number,
				 dev->minors_cnt);
	class_destroy(dev->class);
	usb_put_dev(dev->udev);
	free_devfs_num(dev->devfs_num);

	kfree(dev);
}

static void hpg_pwm_devfs_dev_delete(struct kref *kref)
{
	struct hpg_pwm_devfs_dev *pwm_dev =
		to_hpg_pwm_devfs_dev(kref);

	cdev_del(&pwm_dev->cdev);
	device_destroy(pwm_dev->my_dev->class, pwm_dev->cdev.dev);
	kfree(pwm_dev);
}

void hpg_deregister(void)
{
	usb_deregister(&hpg_usb_driver);
}

static struct hpg_pwm_devfs_dev*
new_pwm_dev(struct hpg_usb_dev *dev, uint8_t endpoint_addr)
{
	struct hpg_pwm_devfs_dev *pwm_dev = NULL;

	/* Get (zeroed) memory for our device state */
	pwm_dev = kzalloc(sizeof(struct hpg_pwm_devfs_dev), GFP_KERNEL);
	if (!pwm_dev) {
		printk(KERN_ERR "hp-gadget: Out of memory. "
				"Cleaning up.");
		return 0;
	}
	pwm_dev->my_dev = dev;
	pwm_dev->endpoint_addr = endpoint_addr;
	pwm_dev->current_pwm = -1;
	pwm_dev->did_read = false;
	INIT_LIST_HEAD(&pwm_dev->list);
	kref_init(&pwm_dev->kref);

	return pwm_dev;
}

static char *tmp_class_name(int8_t devfs_num)
{
	static char class_name[14];

	sprintf(class_name, "hp-gadget%hhd", (int8_t) devfs_num);
	
	return class_name;
}

static char *tmp_hpg_pwm_name(char *class_name, int8_t num)
{
	static char name[22];
	char pattern[22];
	uint8_t i;

	for (i = 0; i < strlen(class_name) && i < 15; i++)
		pattern[i] = class_name[i];

	pattern[i++] = 'p';
	pattern[i++] = 'w';
	pattern[i++] = 'm';
	pattern[i++] = '%';
	pattern[i++] = 'h';
	pattern[i++] = 'h';
	pattern[i++] = 'd';
	pattern[i++] = '\0';

	sprintf(name, pattern, num);

	return name;
}

static int hpg_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct usb_host_interface *iface_desc = interface->cur_altsetting;
	struct hpg_usb_dev *dev = 0;
	struct class *class = 0;
	int8_t devfs_num = -1;
	char *class_name;

	int result = -1;
	int i;

	/* Create class for device */
	devfs_num = alloc_new_devfs_num();
	if (devfs_num < 0) {
      		printk(KERN_ERR "hp-gadget: Can't handle more than "
				MAX_DEVFS_DEVS_str " devices.");
		goto error;
	}
	class_name = tmp_class_name(devfs_num);
	class = class_create(class_name);

	/* Alloc hpg_usb_dev */
	dev = kzalloc(sizeof(struct hpg_usb_dev), GFP_KERNEL);
	if (!dev) {
		printk(KERN_ERR "hp-gadget: Out of memory. "
				"Cleaning up.");
		goto error;
	}
	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->interface = interface;
	dev->class = class;
	INIT_LIST_HEAD(&dev->pwm_devices_list);
	dev->devfs_num = devfs_num;
	dev->first_number = 0;
	dev->minors_cnt = 0;
	kref_init(&dev->kref);

	/* Ask for minors range */
	dev->minors_cnt = iface_desc->desc.bNumEndpoints;
	result = alloc_chrdev_region(&dev->first_number, 0,
				     dev->minors_cnt,
				     class_name);
	if (result < 0) {
      		printk(KERN_ERR "hp-gadget: Couldn't get the minors.");
		goto error;
	}

	for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
		struct hpg_pwm_devfs_dev *devfs_dev = NULL;
		struct usb_endpoint_descriptor *endpoint  = &iface_desc->endpoint[i].desc;
		char *hpg_pwm_name;

		if (endpoint->bEndpointAddress >> 4 == USB_DIR_IN) {
      			printk(KERN_ERR "hp-gadget: Device has unexpected in direction endpoint.");
			continue;
		}

		if ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) !=
		    USB_ENDPOINT_XFER_BULK) {
      			printk(KERN_ERR "hp-gadget: Wrong XFER_TYPE for endpoint. Expected BULK.");
			continue;
		}

		devfs_dev = new_pwm_dev(dev, endpoint->bEndpointAddress);
		if (!devfs_dev) {
      			printk(KERN_ERR "hp-gadget: Failed to create new devfs device.");
			continue;
		}

		cdev_init(&devfs_dev->cdev, &hpg_pwm_fops);
		devfs_dev->cdev.owner = THIS_MODULE;
		result = cdev_add(&devfs_dev->cdev, 
				  MKDEV(MAJOR(dev->first_number),
					MINOR(dev->first_number)+i),
				  1);
		if (result < 0) {
			kref_put(&devfs_dev->kref,
				 hpg_pwm_devfs_dev_delete);
			cdev_del(&devfs_dev->cdev);
			goto error;
		}

		hpg_pwm_name = tmp_hpg_pwm_name(class_name, i);
		device_create(class, NULL,
			      devfs_dev->cdev.dev, NULL,
			      hpg_pwm_name);

		list_add_tail(&devfs_dev->list, &dev->pwm_devices_list);
	}

	usb_set_intfdata(interface, dev);
	dev_info(&interface->dev, "New device hp-gadget%hhd.", dev->devfs_num);

	return 0;

error:

	printk(KERN_ERR "hp-gadget: Failed to initialize new device.\n");

	if (!dev || !dev->first_number) {
		if (class) kfree(class);
		if (devfs_num >= 0)
			free_devfs_num(devfs_num);
	}

	return -1;
}

static ssize_t hpg_send_data(struct hpg_pwm_devfs_dev *pwm_dev, char *data, size_t count)
{
	return usb_bulk_msg(pwm_dev->my_dev->udev,
			    usb_sndbulkpipe(pwm_dev->my_dev->udev, pwm_dev->endpoint_addr),
			    data, count, (int*) &count, HZ * 10);
}

static void hpg_disconnect(struct usb_interface *interface)
{
	struct hpg_usb_dev *dev = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);

	/* Annouce device disconnected */
	dev_info(&interface->dev, "hp-gadget%hhd disconnected.", dev->devfs_num);

	/* Destroy hpg_usb_dev */
	kref_put(&dev->kref, hpg_usb_dev_delete);
}

static int hpg_pwm_open(struct inode *inode, struct file *file)
{
	struct hpg_pwm_devfs_dev *pwm_dev;

	pwm_dev = container_of(inode->i_cdev,
			       struct hpg_pwm_devfs_dev, cdev);
	pwm_dev->did_read = false;

	/* Increment our usage count for the device */
	kref_get(&pwm_dev->kref);

	/* Set our data as file's private struct */
	file->private_data = pwm_dev;

	return 0;
}

static ssize_t hpg_pwm_read(struct file *file, char __user *buffer, size_t count, loff_t *ppos)
{
	struct hpg_pwm_devfs_dev *pwm_dev;
	char buf[10];
	
	pwm_dev = (struct hpg_pwm_devfs_dev*) file->private_data;
	if (pwm_dev == NULL)
		return -ENODEV;

	if (pwm_dev->did_read)
		return 0;

	if (pwm_dev->current_pwm < 0) {
		pwm_dev->current_pwm = 0;

		/* Send zero value to the gadget on first read */
		hpg_send_data(pwm_dev, "0", 1);
	}

	snprintf(buf, 10, "%d\n", (int32_t) pwm_dev->current_pwm);
	if (copy_to_user(buffer, buf, strlen(buf))) {
		return -EFAULT;
	} else {
		pwm_dev->did_read = true;
		return strlen(buf);
	}
}

static ssize_t hpg_pwm_write(struct file *file, const char __user *buffer, size_t count, loff_t *ppos)
{
	struct hpg_pwm_devfs_dev *pwm_dev;
	char to_write[9];
	char to_write_size;
	bool buffer_good;

	int retval;

	buffer_good = true;
	to_write_size = count;
	if (count > 9) {
		buffer_good = false;
	} else {
		if (copy_from_user(to_write, buffer, count))
			return -EFAULT;

		int i;
		for (i = 0; i < count; i++) {
			if (to_write[i] == '\n' &&
			    i == count-1) {
				to_write[i] = '\0';
				to_write_size -= 1;
				break;
			}

			if (to_write[i] < '0' ||
			    to_write[i] > '9') {
				buffer_good = false;
				break;
			}
		}
	}

	if (to_write_size > 8)
		buffer_good = false;

	pwm_dev = (struct hpg_pwm_devfs_dev*) file->private_data;
	if (pwm_dev == NULL)
		return -ENODEV;

	if (buffer_good) {
		sscanf(to_write, "%d",
		       (int32_t*) &pwm_dev->current_pwm);
	} else {
		snprintf(to_write, 9, "%d",
			 (int32_t) pwm_dev->current_pwm);
		to_write_size = strlen(to_write);
	}

	retval = hpg_send_data(pwm_dev, to_write, to_write_size);
	if (retval < 0) {
		return -EFAULT;
	} else {
		/* Device can be read again */
		pwm_dev->did_read = false;

		/* Return how much bytes we received,
		 * Not how much we sent. */
		return count;
	}
}

static int hpg_pwm_release(struct inode *inode, struct file *file)
{
	struct hpg_pwm_devfs_dev *pwm_dev;

	pwm_dev = (struct hpg_pwm_devfs_dev*) file->private_data;
	if (pwm_dev == NULL)
		return -ENODEV;

	/* Decrement the count on our device */
	kref_put(&pwm_dev->kref, hpg_pwm_devfs_dev_delete);
	return 0;
}
