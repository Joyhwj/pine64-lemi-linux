/**
 * GPIO memory device driver
 *
 * Creates a chardev /dev/gpiomem which will provide user access to
 * the A64's GPIO registers when it is mmap()'d.
 * No longer need root for user GPIO access, but without relaxing permissions
 * on /dev/mem.
 *
 * Written by hongweijiang <hongwejiang@yanletech.com>
 * Copyright (c) 2022, Yanletech.com.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The names of the above-listed copyright holders may not be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2, as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Ported to Allwinner A64 from hongwejiijang 2022.11.15
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/pagemap.h>
#include <linux/io.h>

#define DEVICE_NAME "a64-gpiomem"
#define DRIVER_NAME "gpiomem-a64"
#define DEVICE_MINOR 0

struct a64_gpiomem_instance {
	unsigned long gpio_regs_phys;
	struct device *dev;
};

static struct cdev a64_gpiomem_cdev;
static dev_t a64_gpiomem_devid;
static struct class *a64_gpiomem_class;
static struct device *a64_gpiomem_dev;
static struct a64_gpiomem_instance *inst;


/****************************************************************************
*
*   GPIO mem chardev file ops
*
***************************************************************************/

static int a64_gpiomem_open(struct inode *inode, struct file *file)
{
	int dev = iminor(inode);
	int ret = 0;

	dev_info(inst->dev, "gpiomem device opened.");

	if (dev != DEVICE_MINOR) {
		dev_err(inst->dev, "Unknown minor device: %d", dev);
		ret = -ENXIO;
	}
	return ret;
}

static int a64_gpiomem_release(struct inode *inode, struct file *file)
{
	int dev = iminor(inode);
	int ret = 0;

	if (dev != DEVICE_MINOR) {
		dev_err(inst->dev, "Unknown minor device %d", dev);
		ret = -ENXIO;
	}
	return ret;
}

static const struct vm_operations_struct a64_gpiomem_vm_ops = {
#ifdef CONFIG_HAVE_IOREMAP_PROT
	.access = generic_access_phys
#endif
};

static int address_is_allowed(unsigned long pfn, unsigned long size)
{
    unsigned long address = pfn << PAGE_SHIFT;

    dev_info(inst->dev, "address_is_allowed.pfn: 0x%08lx", address);

    switch(address) {

        case 0xff750000:
        case 0xff760000:
        case 0xff780000:
        case 0xff790000:
        case 0xff7a0000:
        case 0xff7b0000:
        case 0xff7c0000:
        case 0xff7d0000:
        case 0xff7e0000:
        case 0xff7f0000:
        case 0xff7f2000:
        case 0xff770000:
        case 0xff730000:
        case 0xff680000:
            dev_info(inst->dev, "address_is_allowed.return 1");
            return 1;
            break; 
        default :
            dev_info(inst->dev, "address_is_allowed.return 0");
	        return 0;
    }
}

static int a64_gpiomem_mmap(struct file *file, struct vm_area_struct *vma)
{

    size_t size;

	size = vma->vm_end - vma->vm_start;


	//if (!address_is_allowed(vma->vm_pgoff, size))
	//	return -EPERM;

	vma->vm_page_prot = phys_mem_access_prot(file, vma->vm_pgoff,
						 size,
						 vma->vm_page_prot);

	vma->vm_ops =  &a64_gpiomem_vm_ops;

	/* Remap-pfn-range will mark the range VM_IO */
	if (remap_pfn_range(vma,
			    vma->vm_start,
			    vma->vm_pgoff,
			    size,
			    vma->vm_page_prot)) {
		return -EAGAIN;
	}

	return 0;
}

static const struct file_operations
a64_gpiomem_fops = {
	.owner = THIS_MODULE,
	.open = a64_gpiomem_open,
	.release = a64_gpiomem_release,
	.mmap = a64_gpiomem_mmap,
};


/****************************************************************************
*
*   Probe and remove functions
*
***************************************************************************/


static int a64_gpiomem_probe(struct platform_device *pdev)
{
	int err;
	void *ptr_err;
	struct device *dev = &pdev->dev;
	struct resource *ioresource;

	/* Allocate buffers and instance data */

	inst = kzalloc(sizeof(struct a64_gpiomem_instance), GFP_KERNEL);

	if (!inst) {
		err = -ENOMEM;
		goto failed_inst_alloc;
	}

	inst->dev = dev;

	ioresource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (ioresource) {
		inst->gpio_regs_phys = ioresource->start;
	} else {
		dev_err(inst->dev, "failed to get IO resource");
		err = -ENOENT;
		goto failed_get_resource;
	}

	/* Create character device entries */

	err = alloc_chrdev_region(&a64_gpiomem_devid,
				  DEVICE_MINOR, 1, DEVICE_NAME);
	if (err != 0) {
		dev_err(inst->dev, "unable to allocate device number");
		goto failed_alloc_chrdev;
	}
	cdev_init(&a64_gpiomem_cdev, &a64_gpiomem_fops);
	a64_gpiomem_cdev.owner = THIS_MODULE;
	err = cdev_add(&a64_gpiomem_cdev, a64_gpiomem_devid, 1);
	if (err != 0) {
		dev_err(inst->dev, "unable to register device");
		goto failed_cdev_add;
	}

	/* Create sysfs entries */

	a64_gpiomem_class = class_create(THIS_MODULE, DEVICE_NAME);
	ptr_err = a64_gpiomem_class;
	if (IS_ERR(ptr_err))
		goto failed_class_create;

	a64_gpiomem_dev = device_create(a64_gpiomem_class, NULL,
					a64_gpiomem_devid, NULL,
					"gpiomem");
	ptr_err = a64_gpiomem_dev;
	if (IS_ERR(ptr_err))
		goto failed_device_create;

	dev_info(inst->dev, "Initialised: Registers at 0x%08lx",
		inst->gpio_regs_phys);

	return 0;

failed_device_create:
	class_destroy(a64_gpiomem_class);
failed_class_create:
	cdev_del(&a64_gpiomem_cdev);
	err = PTR_ERR(ptr_err);
failed_cdev_add:
	unregister_chrdev_region(a64_gpiomem_devid, 1);
failed_alloc_chrdev:
failed_get_resource:
	kfree(inst);
failed_inst_alloc:
	dev_err(inst->dev, "could not load a64_gpiomem");
	return err;
}

static int a64_gpiomem_remove(struct platform_device *pdev)
{
	struct device *dev = inst->dev;

	kfree(inst);
	device_destroy(a64_gpiomem_class, a64_gpiomem_devid);
	class_destroy(a64_gpiomem_class);
	cdev_del(&a64_gpiomem_cdev);
	unregister_chrdev_region(a64_gpiomem_devid, 1);

	dev_info(dev, "GPIO mem driver removed - OK");
	return 0;
}

 /****************************************************************************
*
*   Register the driver with device tree
*
***************************************************************************/

static const struct of_device_id a64_gpiomem_of_match[] = {
	{.compatible = "allwinner-a64,a64-gpiomem",},
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, a64_gpiomem_of_match);

static struct platform_driver a64_gpiomem_driver = {
	.probe = a64_gpiomem_probe,
	.remove = a64_gpiomem_remove,
	.driver = {
		   .name = DRIVER_NAME,
		   .owner = THIS_MODULE,
		   .of_match_table = a64_gpiomem_of_match,
		   },
};

module_platform_driver(a64_gpiomem_driver);

MODULE_ALIAS("platform:gpiomem-a64");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("gpiomem driver for accessing GPIO from userspace");
MODULE_AUTHOR("Hongweijiang  <hongweijiang@yanletech.com>");
