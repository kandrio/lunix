/*
 * lunix-chrdev.c
 *
 * Implementation of character devices
 * for Lunix:TNG
 *
 * Konstantinos Vosinas
 * Konstantinos Andriopoulos
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

	/* Prints registers and stack trace in case of bug */
	WARN_ON (!(sensor = state->sensor));

	/* If the last data that were read are not the latest data on the lunix 
	sensor buffer, then the state needs refresh */
	if (state->buf_timestamp != sensor->msr_data[state->type]->last_update)
		return 1;

	return 0;
}

/*
 * Updates the cached state of a character device
 * based on sensor data. Must be called with the
 * character device state lock held.
 */
static int lunix_chrdev_state_update(struct lunix_chrdev_state_struct *state)
{
	struct lunix_sensor_struct *sensor;

	WARN_ON (!(sensor = state->sensor));
	
	/* lunix_chrdev_read() checks if '-EAGAIN' is returned to put processes to sleep*/
	if(!lunix_chrdev_state_needs_refresh(state)) {
		return -EAGAIN;
	}

	uint32_t newdata, last_update;
	long lookup = 0;

    debug("locking sensor spinlock\n");
	
	spin_lock(&sensor->lock);		
	/* Grabbing  the raw data quickly, and holding the spinlock for as little as possible. */
	newdata = sensor->msr_data[state->type]->values[0];
	last_update = sensor->msr_data[state->type]->last_update;
	spin_unlock(&sensor->lock);
    
	debug("unlocking sensor spinlock\n");

	state->buf_timestamp = last_update;
	
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
	}

	/*
	 * Now we can take our time to format them,
	 * holding only the private state semaphore
	 */
    
	long decimal, fractional;
	
	decimal = lookup / 1000;
	fractional = lookup % 1000; 

	if (lookup < 0){
		state->buf_lim = sprintf(state->buf_data, "-%ld.%ld\n", (-1)*decimal, (-1)*fractional);
	} else {
		state->buf_lim = sprintf(state->buf_data, "%ld.%ld\n", decimal, fractional);
	}
	
	debug("leaving update\n");
	return 0;
}

/*************************************
 * Implementation of file operations
 * for the Lunix character device
 *************************************/

static int lunix_chrdev_open(struct inode *inode, struct file *filp)
{
    debug("entering open\n");

	int ret;
	if ((ret = nonseekable_open(inode, filp)) < 0) {
		goto out;
	}

	/*
	 * Associate this open file with the relevant sensor based on
	 * the minor number of the device node [/dev/sensor<NO>-<TYPE>]
	 */
	unsigned int minorNum, type, sensorNum;
	
	minorNum = iminor(inode);
	
	/*Each minor number refers to a specific type of measurement and a specific sensor*/
	type = minorNum % 8;      //Type of measurement (currently, 3 different measurement types allowed, 0 to 2)
	sensorNum = minorNum / 8; //Number of the sensor device

	if (type >= N_LUNIX_MSR) {  //N_LUNIX_MSR is the number of different measurment types (see lunix-chrdev.h)
		ret = -EINVAL;
		goto out;
	}

	/* Allocate a new Lunix chr dev state structure */
	struct lunix_chrdev_state_struct *state;
	state = kmalloc(sizeof(struct lunix_chrdev_state_struct), GFP_KERNEL);
	/* The GFP_KERNEL flag means that the process can go to sleep while the kernel is searching
	for the memory pages to allocate */

	if(!state){
		ret = -ENOMEM;
		debug("couldn't allocate memmory\n");
		goto out;
	}
	
	state->type = type;
	state->sensor = &lunix_sensors[sensorNum]; //lunix_sensors have been initialized in lunix-module.c
	state->buf_lim = 0; 		//Length of state->buf_data
	state->buf_timestamp = 0;
	sema_init(&state->lock, 1); //Initialize semaphore for state

	/*This will be used by the other file operations of 
	the driver in order to read and update the data.*/
	filp->private_data = state; 
	ret = 0;
out:
	debug("leaving open, with ret = %d\n", ret);
	return ret;
}

static int lunix_chrdev_release(struct inode *inode, struct file *filp)
{
	kfree(filp->private_data);
	debug("released private data successfully \n");
	return 0;
}

static long lunix_chrdev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	/* Why? */
	return -EINVAL;
}

static ssize_t lunix_chrdev_read(struct file *filp, char __user *usrbuf, size_t cnt, loff_t *f_pos)
{

	struct lunix_sensor_struct *sensor;
	struct lunix_chrdev_state_struct *state;

	state = filp->private_data;
	WARN_ON(!state);

	sensor = state->sensor;
	WARN_ON(!sensor);


	/* Check if state semaphore is down. down_interruptible() wakes up the user process not only 
	when the semaphore gets unlocked, but also when a SIGNAL is received (p27, lunix guide) */

	if(down_interruptible(&state->lock))
		return -ERESTARTSYS; // If interrupted, the read() syscall can be restarted with the same arguments,
							 // after the interrupt is handled.
	
	/* 'lunix_chrdev_state_update()' must be called while the chr_dev state lock is held */
	// If there are data, return them to the user
	// otherwise sleep, droping the semaphore
	if (*f_pos == 0) {
		while (lunix_chrdev_state_update(state) == -EAGAIN) {
			/* See LDD3, page 153 for a hint */

			// No need for an updates yet. 
			// Release lock, so that other processes can access 'state'. 
			up(&state->lock);

			// Sleep, using queue, until an update event wakes you up (new data arrived).
			if(wait_event_interruptible(sensor->wq, lunix_chrdev_state_needs_refresh(state)))
				return -ERESTARTSYS;
			
			// Finally awake! Get access to 'state' again, and go for a state update.
			if(down_interruptible(&state->lock))
				return -ERESTARTSYS;
			
		}
	}

	ssize_t ret;

	// went over buffer, return 0
	if (*f_pos >= state->buf_lim){
		f_pos = 0;
		ret   = 0;
		goto out;
	}

	/* Determine the number of cached bytes to copy to userspace */

	// if pos + cnt, bytes requested by read, are greater than the read file (buf_lim)
	// change cnt, to as many bytes as possible

	if (cnt > (size_t) state->buf_lim - *f_pos) {
		cnt = (size_t) state->buf_lim - *f_pos;
	}
	//cnt = min(cnt, (size_t) state->buf_lim - *f_pos );

	if(copy_to_user(usrbuf, state->buf_data + *f_pos, cnt)){
		ret = -EFAULT;
		goto out;
	}

	*f_pos += cnt;
	ret = cnt;

	/* Auto-rewind on EOF mode */
	if(state->buf_lim == *f_pos) 
		*f_pos = 0;
out:
	up(&state->lock);
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
	ret = register_chrdev_region(dev_no, lunix_minor_cnt, "Lunix-Sensors");
	if (ret < 0) {
		debug("failed to register region, ret = %d\n", ret);
		goto out;
	}	
	/* ? */
	/* cdev_add? */
	ret = cdev_add(&lunix_chrdev_cdev, dev_no, lunix_minor_cnt);
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
