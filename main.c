#include <linux/init.h>
#include <linux/module.h>
#include "usb.h"

MODULE_AUTHOR("Antoni Przybylik");
MODULE_DESCRIPTION("v1.0");
MODULE_LICENSE("GPL");

static int __init hpg_init(void)
{
        int retval;

        retval = hpg_register();
        if (retval) {
        	printk(KERN_ERR "hp-gadget: Failed to register "
				"usb driver. Cleaning up.\n");
                return retval;
	}

        return 0;
}

static void __exit hpg_exit(void)
{
        hpg_deregister();
}

module_init(hpg_init);
module_exit(hpg_exit);
