/*
 * drivers\media\video\sunxi\sunxi_cedar.c
 * (C) Copyright 2007-2011
 * Allwinner Technology Co., Ltd. <www.allwinnertech.com>
 * huangxin <huangxin@allwinnertech.com>
 *
 * some simple description for this code
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/preempt.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/rmap.h>
#include <linux/wait.h>
#include <linux/semaphore.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/dma.h>
#include <asm/sizes.h>
#include <asm/barrier.h>
#include <asm/compiler.h>
#include <asm/cmpxchg.h>
#include <asm/exec.h>
#include <asm/switch_to.h>
#include <asm/system_info.h>
#include <asm/system_misc.h>
#include <asm/siginfo.h>
#include <asm/signal.h>
#include <asm/proc-fns.h>
#include <linux/kernel.h>
#include <linux/clocksource.h>
#include "sunxi_cedar.h"
#include <linux/of_reserved_mem.h>
#include <linux/of_platform.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/reset.h>

#define DRV_VERSION "0.01alpha"

#ifndef CEDARDEV_MAJOR
#define CEDARDEV_MAJOR (150)
#endif
#ifndef CEDARDEV_MINOR
#define CEDARDEV_MINOR (0)
#endif

#define CEDAR_DEBUG

#define CONFIG_SW_SYSMEM_RESERVED_BASE 0x43000000
#define CONFIG_SW_SYSMEM_RESERVED_SIZE 75776

int g_dev_major = CEDARDEV_MAJOR;
int g_dev_minor = CEDARDEV_MINOR;
module_param(g_dev_major, int, S_IRUGO);//S_IRUGO represent that g_dev_major can be read,but canot be write
module_param(g_dev_minor, int, S_IRUGO);

struct clk;

struct clk *ve_moduleclk = NULL;
struct clk *ve_pll4clk = NULL;
struct clk *ahb_veclk = NULL;
struct clk *dram_veclk = NULL;
struct clk *avs_moduleclk = NULL;
struct clk *hosc_clk = NULL;
struct reset_control *rstc = NULL;

static unsigned long pll4clk_rate = 720000000;

static void *ve_start_virt;
unsigned long ve_start;
unsigned long ve_size;
extern int flush_clean_user_range(long start, long end);
struct iomap_para{
	volatile char* regs_macc;
	volatile char* regs_avs;
};

static DECLARE_WAIT_QUEUE_HEAD(wait_ve);
struct cedar_dev {
	struct cdev cdev;	         /* char device struct                 */
	struct device *dev;              /* ptr to class device struct         */
	struct class  *class;            /* class for auto create device node  */

	struct semaphore sem;            /* mutual exclusion semaphore         */

	wait_queue_head_t wq;            /* wait queue for poll ops            */

	struct iomap_para iomap_addrs;   /* io remap addrs                     */

	struct timer_list cedar_engine_timer;
	struct timer_list cedar_engine_timer_rel;

	u32 irq;                         /* cedar video engine irq number      */
	u32 irq_flag;                    /* flag of video engine irq generated */
	u32 irq_value;                   /* value of video engine irq          */
	u32 irq_has_enable;
	u32 ref_count;
};
struct cedar_dev *cedar_devp;

u32 int_sta=0,int_value;

/*
 * Video engine interrupt service routine
 * To wake up ve wait queue
 */
static irqreturn_t VideoEngineInterupt(int irq, void *dev)
{
	unsigned int ve_int_ctrl_reg;
	volatile int val;
	int modual_sel;
	struct iomap_para addrs = cedar_devp->iomap_addrs;

	printk(KERN_NOTICE "cedar: VideoEngineInterrupt\n");

	modual_sel = readl(addrs.regs_macc + 0);
	modual_sel &= 0xf;

	/* estimate Which video format */
	switch (modual_sel)
	{
		case 0: //mpeg124
			ve_int_ctrl_reg = (unsigned int)(addrs.regs_macc + 0x100 + 0x14);
			break;
		case 1: //h264
			ve_int_ctrl_reg = (unsigned int)(addrs.regs_macc + 0x200 + 0x20);
			break;
		case 2: //vc1
			ve_int_ctrl_reg = (unsigned int)(addrs.regs_macc + 0x300 + 0x24);
			break;
		case 3: //rmvb
			ve_int_ctrl_reg = (unsigned int)(addrs.regs_macc + 0x400 + 0x14);
			break;
		case 0xa: //isp
			ve_int_ctrl_reg = (unsigned int)(addrs.regs_macc + 0xa00 + 0x08);
			break;
		case 0xb: //avc enc
			ve_int_ctrl_reg = (unsigned int)(addrs.regs_macc + 0xb00 + 0x14);
			break;
		default:
			ve_int_ctrl_reg = (unsigned int)(addrs.regs_macc + 0x100 + 0x14);
			printk("macc modual sel not defined!\n");
			break;
	}

	//disable interrupt
	if(modual_sel == 0) {
		val = readl((const volatile void *)ve_int_ctrl_reg);
		writel(val & (~0x7c), (volatile void *)ve_int_ctrl_reg);
	} else {
		val = readl((const volatile void *)ve_int_ctrl_reg);
		writel(val & (~0xf), (volatile void *)ve_int_ctrl_reg);
	}

	cedar_devp->irq_value = 1;	//hx modify 2011-8-1 16:08:47
	cedar_devp->irq_flag = 1;
	//any interrupt will wake up wait queue
	wake_up_interruptible(&wait_ve);        //ioctl

	return IRQ_HANDLED;
}

/*
 * poll operateion for wait for ve irq
 */
unsigned int cedardev_poll(struct file *filp, struct poll_table_struct *wait)
{
	int mask = 0;
	struct cedar_dev *devp = filp->private_data;
	printk(KERN_NOTICE "cedar: cedardev_poll\n");

	poll_wait(filp, &devp->wq, wait);
	if (devp->irq_flag == 1) {
		devp->irq_flag = 0;
		mask |= POLLIN | POLLRDNORM;
	}
	return mask;
}

static int clk_status = 0;
static LIST_HEAD(run_task_list);
static LIST_HEAD(del_task_list);
static spinlock_t cedar_spin_lock;
#define CEDAR_RUN_LIST_NONULL	-1
#define CEDAR_NONBLOCK_TASK  0      //非阻塞
#define CEDAR_BLOCK_TASK 1
#define CLK_REL_TIME 10000	//10秒
#define TIMER_CIRCLE 50		//50毫秒
#define TASK_INIT      0x00
#define TASK_TIMEOUT   0x55
#define TASK_RELEASE   0xaa
#define SIG_CEDAR		35

int enable_cedar_hw_clk(void)
{
	unsigned long flags;
	int res = -EFAULT;
	printk(KERN_NOTICE "cedar: enable_cedar_hw_clk\n");

	spin_lock_irqsave(&cedar_spin_lock, flags);

	if (clk_status == 1)
		goto out;
	clk_status = 1;

	if(0 != clk_prepare_enable(ahb_veclk)){
		printk("ahb_veclk failed; \n");
		goto out;
	}
	if(0 != clk_prepare_enable(ve_moduleclk)){
		printk("ve_moduleclk failed; \n");
		goto out3;
	}
	if(0 != clk_prepare_enable(dram_veclk)){
		printk("dram_veclk failed; \n");
		goto out2;
	}
	if(0 != clk_prepare_enable(avs_moduleclk)){
		printk("ve_moduleclk failed; \n");
		goto out1;
	}
#ifdef CEDAR_DEBUG
	printk("%s,%d\n",__func__,__LINE__);
#endif
	res = 0;
	goto out;

out1:
	clk_disable(dram_veclk);
out2:
	clk_disable(ve_moduleclk);
out3:
	clk_disable(ahb_veclk);
out:
	spin_unlock_irqrestore(&cedar_spin_lock, flags);
	return res;
}

int disable_cedar_hw_clk(void)
{
	unsigned long flags;
	printk(KERN_NOTICE "cedar: disable_cedar_hw_clk\n");

	spin_lock_irqsave(&cedar_spin_lock, flags);

	if (clk_status == 0)
		goto out;
	clk_status = 0;

	clk_disable(dram_veclk);
	clk_disable(ve_moduleclk);
	clk_disable(ahb_veclk);
	clk_disable(avs_moduleclk);
#ifdef CEDAR_DEBUG
	printk("%s,%d\n",__func__,__LINE__);
#endif
out:
	spin_unlock_irqrestore(&cedar_spin_lock, flags);
	return 0;
}

int cedardev_check_delay(int check_prio)
{
	struct cedarv_engine_task *task_entry;
	int timeout_total = 0;
	unsigned long flags;
	printk(KERN_NOTICE "cedar: cedardev_check_delay\n");

	/*Get the total waiting time*/
	/*获取总的等待时间*/
	spin_lock_irqsave(&cedar_spin_lock, flags);
	list_for_each_entry(task_entry, &run_task_list, list) {
		if ((task_entry->t.task_prio >= check_prio) || (task_entry->running == 1) || (task_entry->is_first_task == 1))
			timeout_total = timeout_total + task_entry->t.frametime;
	}

	spin_unlock_irqrestore(&cedar_spin_lock, flags);
#ifdef CEDAR_DEBUG
	printk("%s,%d,%d\n", __func__, __LINE__, timeout_total);
#endif
	return timeout_total;
}

static void cedar_engine_for_timer_rel(unsigned long arg)
{
	unsigned long flags;
	printk(KERN_NOTICE "cedar: cedar_engine_for_timer_rel\n");

	spin_lock_irqsave(&cedar_spin_lock, flags);

	if(list_empty(&run_task_list)){
		disable_cedar_hw_clk();
	} else {
		printk("Warring: cedar engine timeout for clk disable, but task left, something wrong?\n");
		mod_timer( &cedar_devp->cedar_engine_timer, jiffies + msecs_to_jiffies(TIMER_CIRCLE));
	}

	spin_unlock_irqrestore(&cedar_spin_lock, flags);
}

static void cedar_engine_for_events(unsigned long arg)
{
	struct cedarv_engine_task *task_entry, *task_entry_tmp;
	struct siginfo info;
	unsigned long flags;
	printk(KERN_NOTICE "cedar: cedar_engine_for_events\n");

	spin_lock_irqsave(&cedar_spin_lock, flags);

	list_for_each_entry_safe(task_entry, task_entry_tmp, &run_task_list, list) {
		mod_timer(&cedar_devp->cedar_engine_timer_rel, jiffies + msecs_to_jiffies(CLK_REL_TIME));
		if (task_entry->status == TASK_RELEASE ||
				time_after(jiffies, task_entry->t.timeout)) {
			if (task_entry->status == TASK_INIT)
				task_entry->status = TASK_TIMEOUT;
			list_move(&task_entry->list, &del_task_list);
		}
	}

	list_for_each_entry_safe(task_entry, task_entry_tmp, &del_task_list, list) {
		info.si_signo = SIG_CEDAR;
		info.si_code = task_entry->t.ID;
		if (task_entry->status == TASK_TIMEOUT){//表示任务timeout删除 -> It represents the task timeout deleted
			info.si_errno = TASK_TIMEOUT;
			send_sig_info(SIG_CEDAR, &info, task_entry->task_handle);
		}else if(task_entry->status == TASK_RELEASE){//表示任务正常运行完毕删除 -> It indicates that the task is completed properly deleted
			info.si_errno = TASK_RELEASE;
			send_sig_info(SIG_CEDAR, &info, task_entry->task_handle);
		}
		list_del(&task_entry->list);
		kfree(task_entry);
	}

	/*Activate the list of task*/
	/*激活链表中的task*/
	if(!list_empty(&run_task_list)){
		task_entry = list_entry(run_task_list.next, struct cedarv_engine_task, list);
		if(task_entry->running == 0){
			task_entry->running = 1;
			info.si_signo = SIG_CEDAR;
			info.si_code = task_entry->t.ID;
			info.si_errno = TASK_INIT;	//任务已经启动 -> The task has been started
			send_sig_info(SIG_CEDAR, &info, task_entry->task_handle);
		}

		mod_timer( &cedar_devp->cedar_engine_timer, jiffies + msecs_to_jiffies(TIMER_CIRCLE));
	}

	spin_unlock_irqrestore(&cedar_spin_lock, flags);
}

static unsigned int g_ctx_reg0;
static void save_context(void)
{
	printk(KERN_NOTICE "cedar: save_context\n");
	// TODO: A13A, A13B ???
	//if (SUNXI_VER_A10A == sw_get_ic_ver ||
	//	SUNXI_VER_A13A == sw_get_ic_ver)
	g_ctx_reg0 = readl((const volatile void *)0xf1c20e00);
}

static void restore_context(void)
{
	printk(KERN_NOTICE "cedar: restore_context\n");
	// TODO: A13A, A13B ???
	//if (SUNXI_VER_A10A == sw_get_ic_ver ||
	//	SUNXI_VER_A13A == sw_get_ic_ver)
	writel(g_ctx_reg0, (volatile void *)0xf1c20e00);
}

static long __set_ve_freq (int arg)
{
	/*
	 ** Although the Allwinner sun7i driver sources indicate that the VE
	 ** clock can go up to 500MHz, very simple JPEG and MPEG decoding
	 ** tests show it can't run reliably at even 408MHz.  Keeping the
	 ** sun4i max setting seems best until more information is available.
	 */
	int max_rate = 320000000;
	int min_rate = 100000000;
	int arg_rate = arg * 1000000;	/* arg_rate is specified in MHz */
	int divisor;

	printk(KERN_NOTICE "cedar: __set_ve_freq\n");

	if (arg_rate > max_rate)
		arg_rate = max_rate;
	if (arg_rate < min_rate)
		arg_rate = min_rate;

	/*
	 ** compute integer divisor of pll4clk_rate so that:
	 ** ve_moduleclk >= arg_rate
	 **
	 ** clamp divisor so that:
	 ** min_rate <= ve_moduleclk <= max_rate
	 ** 1 <= divisor <= 8
	 */

	divisor = pll4clk_rate / arg_rate;

	if (divisor == 0)
		divisor = 1;
	else if (pll4clk_rate / divisor < min_rate && divisor > 1)
		divisor--;
	else if (pll4clk_rate / divisor > max_rate)
		divisor++;

	/* VE PLL divisor can't be > 8 */
	if (divisor > 8)
		divisor = 8;

	if (clk_set_rate(ve_moduleclk, pll4clk_rate / divisor) == -1) {
		printk("IOCTL_SET_VE_FREQ: error setting clock; pll4clk_rate = %lu, requested = %lu\n", pll4clk_rate, pll4clk_rate / divisor);
		return -EFAULT;
	}
#ifdef CEDAR_DEBUG
	printk("IOCTL_SET_VE_FREQ: pll4clk_rate = %lu, divisor = %d, arg_rate= %d, ve_moduleclk = %lu\n", pll4clk_rate, divisor, arg_rate, clk_get_rate(ve_moduleclk));
#endif

	return 0;
}

/*
 * ioctl function
 * including : wait video engine done,
 *             AVS Counter control,
 *             Physical memory control,
 *             module clock/freq control.
 *				cedar engine
 */
long cedardev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	long   ret = 0;
	unsigned int v;
	int ve_timeout = 0;
	struct cedar_dev *devp;
	unsigned long flags;
	printk(KERN_NOTICE "cedar: cedardev_ioctl %u\n", cmd);

	devp = filp->private_data;

	switch (cmd)
	{
		case IOCTL_ENGINE_REQ:
			enable_cedar_hw_clk();
			cedar_devp->ref_count++;
			break;
		case IOCTL_ENGINE_REL:
			disable_cedar_hw_clk();
			cedar_devp->ref_count--;
			return ret;
		case IOCTL_ENGINE_CHECK_DELAY:
			{
				struct cedarv_engine_task_info task_info;
				/*Being from user space to query task priorities, total_time by task priority, the total time statistics to wait. In this interface, but also to the user to pass the current frametime task (it reduces the interface, but the user space to set up a multi-empty frametime value) for the current task of frametime, it can also be used to obtain additional interfaces, but to do so, and total_time frametime is in a different interface. benefit? ? ?*/
				/*从用户空间中获取要查询的任务优先级，通过任务优先级，统计需要等待的总时间total_time.
				 * 在这个接口中，同时也给用户传递了当前任务的frametime（这样做可以减少接口，但是用户空间要多设置一个空的frametime值）
				 *对于当前task的frametime，也可以用额外的接口获取，但是这样做，frametime和total_time就处于不同接口中。好处？？？
				 */
				if(copy_from_user(&task_info, (void __user*)arg, sizeof(struct cedarv_engine_task_info))){
					printk("IOCTL_ENGINE_CHECK_DELAY copy_from_user fail\n");
					return -EFAULT;
				}
				task_info.total_time = cedardev_check_delay(task_info.task_prio);//task_info.task_prio是传递过来的优先级 <- taskinfo.task prio priority is to pass over
#ifdef CEDAR_DEBUG
				printk("%s,%d,%d\n", __func__, __LINE__, task_info.total_time);
#endif
				task_info.frametime = 0;
				spin_lock_irqsave(&cedar_spin_lock, flags);
				if(!list_empty(&run_task_list)){
					/*Get run_task list chain in the first task, the task that is currently running, get frametime time by the currently running task*/
					/*获取run_task_list链表中的第一个任务，也就是当前运行的任务，通过当前运行的任务获取frametime时间*/
					struct cedarv_engine_task *task_entry;
#ifdef CEDAR_DEBUG
					printk("%s,%d\n",__func__,__LINE__);
#endif
					task_entry = list_entry(run_task_list.next, struct cedarv_engine_task, list);
					if(task_entry->running == 1)
						task_info.frametime = task_entry->t.frametime;
#ifdef CEDAR_DEBUG
					printk("%s,%d,%d\n",__func__,__LINE__,task_info.frametime);
#endif
				}
				spin_unlock_irqrestore(&cedar_spin_lock, flags);
				/*The task priority, total_time, frametime copied to the user space. The value of the task priority is set by the user, total_time is the total time needed to wait, frametime is currently running time of the task. In fact, the best information on the current task with another interface. Reducing coupling and extensibility interfaces.*/
				/*
				 *将任务优先级，total_time,frametime拷贝到用户空间。任务优先级还是用户设置的值，total_time是需要等待的总时间，
				 *frametime是当前任务的运行时间.其实当前任务的信息最好用另一个接口实现.减少耦合度和接口的拓展性.
				 */
				if (copy_to_user((void *)arg, &task_info, sizeof(struct cedarv_engine_task_info))){
					printk("IOCTL_ENGINE_CHECK_DELAY copy_to_user fail\n");
					return -EFAULT;
				}
			}
			break;
		case IOCTL_WAIT_VE:
			//wait_event_interruptible(wait_ve, cedar_devp->irq_flag);
			ve_timeout = (int)arg;
			cedar_devp->irq_value = 0;

			spin_lock_irqsave(&cedar_spin_lock, flags);
			if(cedar_devp->irq_flag)
				cedar_devp->irq_value = 1;
			spin_unlock_irqrestore(&cedar_spin_lock, flags);

			wait_event_interruptible_timeout(wait_ve, cedar_devp->irq_flag, ve_timeout*HZ);
			printk("%s,%d,ve_timeout:%d,cedar_devp->irq_value:%d\n", __func__, __LINE__, ve_timeout, cedar_devp->irq_value);
			cedar_devp->irq_flag = 0;
			/*It returns 1, indicating that the interrupt returns, returns 0, indicating timeout return*/
			/*返回1，表示中断返回，返回0，表示timeout返回*/
			return cedar_devp->irq_value;

		case IOCTL_ENABLE_VE:
			clk_prepare_enable(ve_moduleclk);
			break;

		case IOCTL_DISABLE_VE:
			clk_disable(ve_moduleclk);
			break;

		case IOCTL_RESET_VE:
			clk_disable(dram_veclk);
			reset_control_assert(rstc);
			reset_control_deassert(rstc);
			clk_prepare_enable(dram_veclk);
			break;

		case IOCTL_SET_VE_FREQ:
			return __set_ve_freq((int) arg);
		case IOCTL_GETVALUE_AVS2:
			/* Return AVS1 counter value */
			return readl(cedar_devp->iomap_addrs.regs_avs + 0x88);

		case IOCTL_ADJUST_AVS2:
			{
				int arg_s = (int)arg;
				int temp;
				save_context();
				v = readl(cedar_devp->iomap_addrs.regs_avs + 0x8c);
				temp = v & 0xffff0000;
				temp =temp + temp*arg_s/100;
				temp = temp > (244<<16) ? (244<<16) : temp;
				temp = temp < (234<<16) ? (234<<16) : temp;
				v = (temp & 0xffff0000) | (v&0x0000ffff);
#ifdef CEDAR_DEBUG
				printk("Kernel AVS ADJUST Print: 0x%x\n", v);
#endif
				writel(v, cedar_devp->iomap_addrs.regs_avs + 0x8c);
				restore_context();
				break;
			}

		case IOCTL_ADJUST_AVS2_ABS:
			{
				int arg_s = (int)arg;
				int v_dst;

				switch(arg_s){
					case -2:
						v_dst = 234;
						break;
					case -1:
						v_dst = 236;
						break;
					case 1:
						v_dst = 242;
						break;
					case 2:
						v_dst = 244;
						break;
					default:
						v_dst = 239;
						break;
				}

				save_context();
				v = readl(cedar_devp->iomap_addrs.regs_avs + 0x8c);
				v = (v_dst<<16)  | (v&0x0000ffff);
				writel(v, cedar_devp->iomap_addrs.regs_avs + 0x8c);
				restore_context();
				break;
			}

		case IOCTL_CONFIG_AVS2:
			save_context();
			/* Set AVS counter divisor */
			v = readl(cedar_devp->iomap_addrs.regs_avs + 0x8c);
			v = 239 << 16 | (v & 0xffff);
			writel(v, cedar_devp->iomap_addrs.regs_avs + 0x8c);

			/* Enable AVS_CNT1 and Pause it */
			v = readl(cedar_devp->iomap_addrs.regs_avs + 0x80);
			v |= 1 << 9 | 1 << 1;
			writel(v, cedar_devp->iomap_addrs.regs_avs + 0x80);

			/* Set AVS_CNT1 init value as zero  */
			writel(0, cedar_devp->iomap_addrs.regs_avs + 0x88);
			restore_context();
			break;

		case IOCTL_RESET_AVS2:
			/* Set AVS_CNT1 init value as zero */
			save_context();
			writel(0, cedar_devp->iomap_addrs.regs_avs + 0x88);
			restore_context();
			break;

		case IOCTL_PAUSE_AVS2:
			/* Pause AVS_CNT1 */
			save_context();
			v = readl(cedar_devp->iomap_addrs.regs_avs + 0x80);
			v |= 1 << 9;
			writel(v, cedar_devp->iomap_addrs.regs_avs + 0x80);
			restore_context();
			break;

		case IOCTL_START_AVS2:
			/* Start AVS_CNT1 : do not pause */
			save_context();
			v = readl(cedar_devp->iomap_addrs.regs_avs + 0x80);
			v &= ~(1 << 9);
			writel(v, cedar_devp->iomap_addrs.regs_avs + 0x80);
			restore_context();
			break;

		case IOCTL_GET_ENV_INFO:
			{
				struct cedarv_env_infomation env_info;
				env_info.phymem_start = (unsigned int)phys_to_virt(ve_start);
				env_info.phymem_total_size = ve_size;
				env_info.address_macc = (unsigned int)cedar_devp->iomap_addrs.regs_macc;
				if (copy_to_user((char *)arg, &env_info, sizeof(struct cedarv_env_infomation)))
					return -EFAULT;
			}
			break;
		case IOCTL_GET_IC_VER:
			{
				// TODO: A13A, A13B ???
/*				if (SUNXI_VER_A10A == sw_get_ic_ver ||	SUNXI_VER_A13A == sw_get_ic_ver) {
					return 0x0A10000A;
				} else if (SUNXI_VER_A10B == sw_get_ic_ver ||
						SUNXI_VER_A10C == sw_get_ic_ver ||
						SUNXI_VER_A13B == sw_get_ic_ver ||
						SUNXI_VER_A20 == sw_get_ic_ver) {
					return 0x0A10000B;
				}else{
					printk("IC_VER get error:%s,%d\n", __func__, __LINE__);
					return -EFAULT;
				}*/
				return 0x0A10000A;
			}
		case IOCTL_FLUSH_CACHE:
			{
				struct cedarv_cache_range cache_range;
				if(copy_from_user(&cache_range, (void __user*)arg, sizeof(struct cedarv_cache_range))){
					printk("IOCTL_FLUSH_CACHE copy_from_user fail\n");
					return -EFAULT;
				}
				flush_clean_user_range(cache_range.start, cache_range.end);
			}
			break;

		case IOCTL_SET_REFCOUNT:
			cedar_devp->ref_count = (int)arg;
			break;

		case IOCTL_READ_REG:
			{
				struct cedarv_regop reg_para;
				if(copy_from_user(&reg_para, (void __user*)arg, sizeof(struct cedarv_regop)))
					return -EFAULT;
				return readl((const volatile void *)reg_para.addr);
			}

		case IOCTL_WRITE_REG:
			{
				struct cedarv_regop reg_para;
				if(copy_from_user(&reg_para, (void __user*)arg, sizeof(struct cedarv_regop)))
					return -EFAULT;
				writel(reg_para.value, (volatile void *)reg_para.addr);
				break;
			}

		default:
			break;
	}
	return ret;
}

static int cedardev_open(struct inode *inode, struct file *filp)
{
	struct cedar_dev *devp;
	printk(KERN_NOTICE "cedar: cedardev_open\n");
	devp = container_of(inode->i_cdev, struct cedar_dev, cdev);
	filp->private_data = devp;
	if (down_interruptible(&devp->sem)) {
		return -ERESTARTSYS;
	}
	/* init other resource here */
	devp->irq_flag = 0;
	up(&devp->sem);
	nonseekable_open(inode, filp);
	return 0;
}

static int cedardev_release(struct inode *inode, struct file *filp)
{
	struct cedar_dev *devp;
	printk(KERN_NOTICE "cedar: cedardev_release\n");

	devp = filp->private_data;
	if (down_interruptible(&devp->sem)) {
		return -ERESTARTSYS;
	}
	/* release other resource here */
	devp->irq_flag = 1;
	up(&devp->sem);
	return 0;
}

void cedardev_vma_open(struct vm_area_struct *vma)
{
	printk(KERN_NOTICE "cedar: cedardev_vma_open\n");
}

void cedardev_vma_close(struct vm_area_struct *vma)
{
	printk(KERN_NOTICE "cedar: cedardev_vma_close\n");
}

static struct vm_operations_struct cedardev_remap_vm_ops = {
	.open  = cedardev_vma_open,
	.close = cedardev_vma_close,
};

static int cedardev_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long temp_pfn;
	unsigned int  VAddr;
	struct iomap_para addrs;

	unsigned int io_ram = 0;
	VAddr = vma->vm_pgoff << 12;
	addrs = cedar_devp->iomap_addrs;
	printk(KERN_NOTICE "cedar: cedardev_mmap\n");

	if (VAddr == (unsigned int)addrs.regs_macc) {
		temp_pfn = MACC_REGS_BASE >> 12;
		io_ram = 1;
	} else {
		temp_pfn = (__pa(vma->vm_pgoff << 12))>>12;
		io_ram = 0;
	}

	if (io_ram == 0) {
		/* Set reserved and I/O flag for the area. */
		vma->vm_flags |= VM_IO | (VM_DONTEXPAND | VM_DONTDUMP);

		/* Select uncached access. */
		//vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

		if (remap_pfn_range(vma, vma->vm_start, temp_pfn,
					vma->vm_end - vma->vm_start, vma->vm_page_prot)) {
			return -EAGAIN;
		}
	} else {
		/* Set reserved and I/O flag for the area. */
		vma->vm_flags |= VM_IO | (VM_DONTEXPAND | VM_DONTDUMP);
		/* Select uncached access. */
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

		if (io_remap_pfn_range(vma, vma->vm_start, temp_pfn,
					vma->vm_end - vma->vm_start, vma->vm_page_prot)) {
			return -EAGAIN;
		}
	}

	vma->vm_ops = &cedardev_remap_vm_ops;
	cedardev_vma_open(vma);

	return 0;
}

static int snd_sw_cedar_suspend(struct platform_device *pdev,pm_message_t state)
{
	printk(KERN_NOTICE "cedar: snd_sw_cedar_suspend\n");
	disable_cedar_hw_clk();

	return 0;
}

static int snd_sw_cedar_resume(struct platform_device *pdev)
{
	printk(KERN_NOTICE "cedar: snd_sw_cedar_resume\n");
	if(cedar_devp->ref_count == 0){
		return 0;
	}
	enable_cedar_hw_clk();

	return 0;
}

static struct file_operations cedardev_fops = {
	.owner   = THIS_MODULE,
	.mmap    = cedardev_mmap,
	.poll    = cedardev_poll,
	.open    = cedardev_open,
	.release = cedardev_release,
	.llseek  = no_llseek,
	.unlocked_ioctl   = cedardev_ioctl,
};

/*data relating*/
static struct platform_device sw_device_cedar = {
	.name = "sunxi-cedar",
};

/*method relating*/
static struct platform_driver sw_cedar_driver = {
#ifdef CONFIG_PM
	.suspend	= snd_sw_cedar_suspend,
	.resume		= snd_sw_cedar_resume,
#endif
	.driver		= {
		.name	= "sunxi-cedar",
	},
};

#define SW_PA_SDRAM_START                 0x40000000

static const struct of_device_id sun4i_drv_of_table[] = {
	{ .compatible = "allwinner,sun5i-a13-video-engine" },
	{ }
};
MODULE_DEVICE_TABLE(of, sun4i_drv_of_table);

static int __init cedardev_init(void)
{
	int ret = 0;
	int err = 0;
	int devno;
	int irq_no;
	unsigned int val;
	dev_t dev = 0;
	struct platform_device *pdev = NULL;
	struct device_node *dt_node;
	resource_size_t pa;
	void *pll4_clk_addr, *ahb_clk_addr, *ve_clk_addr, 
	     *sdram_clk_addr, *sram_addr;

	printk(KERN_NOTICE "cedar: cedardev_init\n");
	dt_node = of_find_node_by_path("/soc@01c00000/video-engine");
	pll4_clk_addr = of_iomap(dt_node, 0);
	ahb_clk_addr = of_iomap(dt_node, 1);
	ve_clk_addr = of_iomap(dt_node, 2);
	sdram_clk_addr = of_iomap(dt_node, 3);
	sram_addr = of_iomap(dt_node, 4);

	if (!dt_node) {
		printk(KERN_ERR "(E) Failed to find device-tree node\n");
		return -ENODEV;
	}

	pdev = of_find_device_by_node(dt_node);

	if (!pdev) {
		printk(KERN_ERR "(E) Failed to find device-tree dev\n");
		return -ENODEV;
	}

	ret = of_reserved_mem_device_init(&pdev->dev);
	if (ret) {
		printk(KERN_ERR "(E) Failed to reserve mem\n");
		return -ENODEV;
	}

	irq_no = of_irq_get(dt_node, 0);

	/* If having CMA enabled, just rely on CMA for memory allocation */
	ve_size = 80 * SZ_1M;
	ve_start_virt = dma_alloc_coherent(&pdev->dev, ve_size, &pa,
			GFP_KERNEL | GFP_DMA);
	if (!ve_start_virt) {
		printk(KERN_NOTICE "cedar: failed to allocate memory buffer\n");
		return -ENODEV;
	}
	ve_start = pa;
	if (ve_start + ve_size > SW_PA_SDRAM_START + SZ_256M) {
		printk(KERN_NOTICE "cedar: buffer is above 256MB limit\n");
		dma_free_coherent(&pdev->dev, ve_size, ve_start_virt, ve_start);
		ve_start_virt = 0;
		ve_size = 0;
		return -ENODEV;
	}

	printk("[cedar dev]: install start!!!\n");
	if((platform_device_register(&sw_device_cedar))<0)
		return err;

	if ((err = platform_driver_register(&sw_cedar_driver)) < 0)
		return err;
	/*register or alloc the device number.*/
	if (g_dev_major) {
		dev = MKDEV(g_dev_major, g_dev_minor);
		ret = register_chrdev_region(dev, 1, "cedar_dev");
	} else {
		ret = alloc_chrdev_region(&dev, g_dev_minor, 1, "cedar_dev");
		g_dev_major = MAJOR(dev);
		g_dev_minor = MINOR(dev);
	}

	if (ret < 0) {
		printk(KERN_WARNING "cedar_dev: can't get major %d\n", g_dev_major);
		return ret;
	}
	spin_lock_init(&cedar_spin_lock);
	cedar_devp = kmalloc(sizeof(struct cedar_dev), GFP_KERNEL);
	if (cedar_devp == NULL) {
		printk("malloc mem for cedar device err\n");
		return -ENOMEM;
	}
	memset(cedar_devp, 0, sizeof(struct cedar_dev));
	cedar_devp->irq = irq_no;

	sema_init(&cedar_devp->sem, 1);
	init_waitqueue_head(&cedar_devp->wq);

	memset(&cedar_devp->iomap_addrs, 0, sizeof(struct iomap_para));

	ret = request_irq(irq_no, VideoEngineInterupt, 0, "cedar_dev", NULL);
	if (ret < 0) {
		printk("request irq err\n");
		return -EINVAL;
	}
	/* map for macc io space */
	cedar_devp->iomap_addrs.regs_macc = ioremap(MACC_REGS_BASE, 4096);
	if (!cedar_devp->iomap_addrs.regs_macc){
		printk("cannot map region for macc");
	}
	cedar_devp->iomap_addrs.regs_avs = ioremap(AVS_REGS_BASE, 1024);

	//VE_SRAM mapping to AC320
	val = readl((const volatile void *)sram_addr);
	val &= 0x80000000;
	writel(val,(volatile void *)sram_addr);
	//remapping SRAM to MACC for codec test
	val = readl((const volatile void *)sram_addr);
	val |= 0x7fffffff;
	writel(val,(volatile void *)sram_addr);

	ve_pll4clk = of_clk_get_by_name(dt_node, "ve_pll");
	pll4clk_rate = clk_get_rate(ve_pll4clk);
	/* getting ahb clk for ve!(macc) */
	ahb_veclk = of_clk_get_by_name(dt_node,"ahb_ve");
	ve_moduleclk = of_clk_get_by_name(dt_node,"ve");
	if (IS_ERR(ve_moduleclk)) {
		printk("ve is wrong!\n");
	}
	if (IS_ERR(ahb_veclk)) {
		printk("ahb_ve is wrong!\n");
	}
	if(clk_set_parent(ve_moduleclk, ve_pll4clk)){
		printk("set parent of ve_moduleclk to ve_pll4clk failed!\n");
		return -EFAULT;
	}

	//if (SUNXI_VER_A20 == sw_get_ic_ver) /* default the ve freq to 300M for A20 (from sun7i_cedar.c) */
	//	__set_ve_freq(300);
	__set_ve_freq(160);

	/*geting dram clk for ve!*/
	dram_veclk = of_clk_get_by_name(dt_node, "sdram_ve"); // dram-ve ?????
	hosc_clk = of_clk_get_by_name(dt_node, "hosc");
	avs_moduleclk = of_clk_get_by_name(dt_node,"avs"); // avs
	if(clk_set_parent(avs_moduleclk, hosc_clk)){ // needed ???
		printk("set parent of avs_moduleclk to hosc_clk failed!\n");
		return -EFAULT;
	}

	rstc = of_reset_control_get(dt_node, NULL);

	/*for clk test*/
#ifdef CEDAR_DEBUG
	printk("PLL4 CLK:0xf1c20018 is:%x\n", *(volatile int *)pll4_clk_addr); //0x01c20018);
	printk("AHB CLK:0xf1c20064 is:%x\n", *(volatile int *)ahb_clk_addr); //0xf1c20064);
	printk("VE CLK:0xf1c2013c is:%x\n", *(volatile int *)ve_clk_addr); //0xf1c2013c);
	printk("SDRAM CLK:0xf1c20100 is:%x\n", *(volatile int *)sdram_clk_addr); //0xf1c20100);
	printk("SRAM:0xf1c00000 is:%x\n", *(volatile int *)sram_addr); //0xf1c00000);
#endif

	/* Create char device */
	devno = MKDEV(g_dev_major, g_dev_minor);
	cdev_init(&cedar_devp->cdev, &cedardev_fops);
	cedar_devp->cdev.owner = THIS_MODULE;
	cedar_devp->cdev.ops = &cedardev_fops;
	ret = cdev_add(&cedar_devp->cdev, devno, 1);
	if (ret) {
		printk(KERN_NOTICE "Err:%d add cedardev", ret);
	}
	cedar_devp->class = class_create(THIS_MODULE, "cedar_dev");
	cedar_devp->dev   = device_create(cedar_devp->class, NULL, devno, NULL, "cedar_dev");
	/*在cedar drv初始化的时候，初始化定时器并设置它的成员
	 * 在有任务插入run_task_list的时候，启动定时器，并设置定时器的时钟为当前系统的jiffies，参考cedardev_insert_task
	 */
	setup_timer(&cedar_devp->cedar_engine_timer, cedar_engine_for_events, (unsigned long)cedar_devp);
	setup_timer(&cedar_devp->cedar_engine_timer_rel, cedar_engine_for_timer_rel, (unsigned long)cedar_devp);
	printk("[cedar dev]: install end!!!\n");
	return 0;
}
module_init(cedardev_init);

static void __exit cedardev_exit(void)
{
	int irq_no;
	dev_t dev;
	struct platform_device *pdev = NULL;
	struct device_node *dt_node;
	printk(KERN_NOTICE "cedar: cedardev_exit\n");

	dt_node = of_find_node_by_path("/soc@01c00000/video-engine");

	if (!dt_node) {
		printk(KERN_ERR "(E) Failed to find device-tree node\n");
		return;
	}

	pdev = of_find_device_by_node(dt_node);

	if (!pdev) {
		printk(KERN_ERR "(E) Failed to find device-tree dev\n");
		return;
	}

	irq_no = of_irq_get(dt_node, 0);

	dev = MKDEV(g_dev_major, g_dev_minor);

	free_irq(irq_no, NULL);
	iounmap(cedar_devp->iomap_addrs.regs_macc);
	iounmap(cedar_devp->iomap_addrs.regs_avs);
	/* Destroy char device */
	if(cedar_devp){
		del_timer(&cedar_devp->cedar_engine_timer);
		del_timer(&cedar_devp->cedar_engine_timer_rel);
		cdev_del(&cedar_devp->cdev);
		device_destroy(cedar_devp->class, dev);
		class_destroy(cedar_devp->class);
	}
	clk_disable(dram_veclk);
	clk_put(dram_veclk);

	clk_disable(ve_moduleclk);
	clk_put(ve_moduleclk);

	clk_disable(ahb_veclk);
	clk_put(ahb_veclk);

	clk_put(ve_pll4clk);

	clk_disable(avs_moduleclk);
	clk_put(avs_moduleclk);

	unregister_chrdev_region(dev, 1);
	platform_driver_unregister(&sw_cedar_driver);
	platform_device_unregister(&sw_device_cedar);
	if (cedar_devp) {
		kfree(cedar_devp);
	}

	if (ve_start_virt) {
		dma_free_coherent(&pdev->dev, ve_size, ve_start_virt, ve_start);
		ve_start_virt = 0;
		ve_size = 0;
	}
	of_reserved_mem_device_release(&pdev->dev);
}
module_exit(cedardev_exit);

MODULE_AUTHOR("Soft-Allwinner");
MODULE_DESCRIPTION("User mode CEDAR device interface");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
