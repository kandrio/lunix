/*
 * lunix-chrdev.c
 *
 * Implementation of character devices
 * for Lunix:TNG
 *
 * < Your name here >
 *
 */

#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/cdev.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mmzone.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>

#include "lunix.h"
#include "lunix-chrdev.h"
#include "lunix-lookup.h"

/*
 * Global data
 */
struct cdev lunix_chrdev_cdev;

/*
 * Just a quick [unlocked] check to see if the cached
 * chrdev state needs to be updated from sensor measurements.
 */
static int lunix_chrdev_state_needs_refresh(struct lunix_chrdev_state_struct *state)
{
	struct lunix_sensor_struct *sensor;

	WARN_ON ( !(sensor = state->sensor));
	/* ? */

	/* If the last time that the user requested data differs from the last time that
	new data came, then the chrdev needs refresh */
	if (state->buf_timestamp != sensor->msr_data[state->type]->last_update)
		return 1;

	/* The following return is bogus, just for the stub to compile */
	return 0; /* ? */
}

/*
 * Updates the cached state of a character device
 * based on sensor data. Must be called with the
 * character device state lock held.
 */
static int lunix_chrdev_state_update(struct lunix_chrdev_state_struct *state)
{
	struct lunix_sensor_struct *sensor;
	
	sensor = state->sensor;
	
	debug("leaving\n");

	/* Read() checks if '-EAGAIN' is returned to put processes to sleep*/
	if(!lunix_chrdev_state_needs_refresh(state)) {
		return -EAGAIN;
	}

	/*
	 * Grab the raw data quickly, hold the
	 * spinlock for as little as possible.
	 */
	spin_lock(&sensor->lock); 
	uint32_t newdata = sensor->msr_data[state->type]->values[0];
	state->buf_timestamp = sensor->msr_data[state->type]->last_update;
	spin_lock(&sensor->lock);
	/* Why use spinlocks? See LDD3, p. 119 */
	
	long lookup;
	switch (state->type) {
		case BATT:
			lookup = lookup_voltage[newdata];
			break;
		case TEMP:
			lookup = lookup_temperature[newdata];
			break;
		case LIGHT:
			lookup = lookup_light[newdata];
			break;
		case N_LUNIX_MSR:
			return -EFAULT;
			break;		
	}

	/*
	 * Now we can take our time to format them,
	 * holding only the private state semaphore
	 */
    long decimal = lookup / 1000;
	long fractional = lookup % 1000; 

	if (lookup < 0){
		sprintf(state->buf_data, "-%ld.%ld", (-1)*decimal, (-1)*fractional);
	} else {
		sprintf(state->buf_data, "%ld.%ld", decimal, fractional);
	}
	
	state->buf_lim = strlen(state->buf_data);

	debug("leaving\n");
	return 0;
}

/*************************************
 * Implementation of file operations
 * for the Lunix character device
 *************************************/

static int lunix_chrdev_open(struct inode *inode, struct file *filp)
{
	/* Declarations */
	/* ? */
	int ret;
	unsigned int minorNum, type, sensor;

	debug("entering\n");
	ret = -ENODEV;  //ENODEV means "Error, no device."
	if ((ret = nonseekable_open(inode, filp)) < 0)
		goto out;

	/*
	 * Associate this open file with the relevant sensor based on
	 * the minor number of the device node [/dev/sensor<NO>-<TYPE>]
	 */

	minorNum = iminor(inode);
	
	/*Each minor number refers to a specific type of measurement
	minorNum may have other digits set to 1 so we extract the 3 last digits for safety*/
	type = minorNum & 3; 
	sensor = minorNum/8; //Number of sensor device
	if (type == N_LUNIX_MSR) //N_LUNIX_MSR is the number of different measurment types (see lunix-chrdev.h)
		goto out;

	/* Allocate a new Lunix character device private state structure */
	state = kmalloc(sizeof(struct lunix_chrdev_state_struct));
	
	state->type = type;
	state->sensor = &lunix_sensors[sensor] //A lunix_sensor_struct for each sensor
	state->buf_lim = 0; //What is this (???)
	state->buf_timestamp = 0 ; //I guess this is for checking if new data has come
	sema_init(&state->lock, 1); //Initialize semaphore

	/*This will probably be used by the other file operations of 
	the driver in order to save and update data.*/
	filp->private_data = state; 

out:
	debug("leaving, with ret = %d\n", ret);
	return ret;
}

static int lunix_chrdev_release(struct inode *inode, struct file *filp)
{
	/* ? */
	kfree(filp->private_data);
	debug("Release filp private_data successfully.")
	return 0;
}

static long lunix_chrdev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	/* Why? */
	return -EINVAL;
}

static ssize_t lunix_chrdev_read(struct file *filp, char __user *usrbuf, size_t cnt, loff_t *f_pos)
{
	ssize_t ret;

	struct lunix_sensor_struct *sensor;
	struct lunix_chrdev_state_struct *state;

	state = filp->private_data;
	WARN_ON(!state);

	sensor = state->sensor;
	WARN_ON(!sensor);

	/* Lock? */
	/*
	 * If the cached character device state needs to be
	 * updated by actual sensor data (i.e. we need to report
	 * on a "fresh" measurement, do so
	 */
	if (*f_pos == 0) {
		while (lunix_chrdev_state_update(state) == -EAGAIN) {
			/* ? */
			/* The process needs to sleep */
			/* See LDD3, page 153 for a hint */
		}
	}

	/* End of file */
	/* ? */
	
	/* Determine the number of cached bytes to copy to userspace */
	/* ? */

	/* Auto-rewind on EOF mode? */
	/* ? */
out:
	/* Unlock? */
	return ret;
}

static int lunix_chrdev_mmap(struct file *filp, struct vm_area_struct *vma)
{
	return -EINVAL;
}

static struct file_operations lunix_chrdev_fops = 
{
    .owner          = THIS_MODULE,
	.open           = lunix_chrdev_open,
	.release        = lunix_chrdev_release,
	.read           = lunix_chrdev_read,
	.unlocked_ioctl = lunix_chrdev_ioctl,
	.mmap           = lunix_chrdev_mmap
};

int lunix_chrdev_init(void)
{
	/*
	 * Register the character device with the kernel, asking for
	 * a range of minor numbers (number of sensors * 8 measurements / sensor)
	 * beginning with LINUX_CHRDEV_MAJOR:0
	 */
	int ret;
	dev_t dev_no;
	unsigned int lunix_minor_cnt = lunix_sensor_cnt << 3;
	
	debug("initializing character device\n");
	cdev_init(&lunix_chrdev_cdev, &lunix_chrdev_fops);
	lunix_chrdev_cdev.owner = THIS_MODULE;
	
	dev_no = MKDEV(LUNIX_CHRDEV_MAJOR, 0);
	/* ? */
	/* register_chrdev_region? */
	if (ret < 0) {
		debug("failed to register region, ret = %d\n", ret);
		goto out;
	}	
	/* ? */
	/* cdev_add? */
	if (ret < 0) {
		debug("failed to add character device\n");
		goto out_with_chrdev_region;
	}
	debug("completed successfully\n");
	return 0;

out_with_chrdev_region:
	unregister_chrdev_region(dev_no, lunix_minor_cnt);
out:
	return ret;
}

void lunix_chrdev_destroy(void)
{
	dev_t dev_no;
	unsigned int lunix_minor_cnt = lunix_sensor_cnt << 3;
		
	debug("entering\n");
	dev_no = MKDEV(LUNIX_CHRDEV_MAJOR, 0);
	cdev_del(&lunix_chrdev_cdev);
	unregister_chrdev_region(dev_no, lunix_minor_cnt);
	debug("leaving\n");
}
