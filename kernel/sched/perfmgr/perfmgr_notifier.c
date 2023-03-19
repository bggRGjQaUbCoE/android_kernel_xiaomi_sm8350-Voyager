#define DEBUG

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include "perfmgr.h"
#include <linux/cpufreq.h>
#include "pfmgr_ioctl.h"

extern struct _FPSGO_PACKAGE *msgKM;
extern int perfmgr_enable;
extern int perfmgr_notify_qudeq_cb(int pid,unsigned long long identifier);

static int perfmgr_notify(struct notifier_block *nb,unsigned long mode, void *_unused)
{
	if (!perfmgr_enable)
	return 1;

	if (msgKM && msgKM->identifier == 1)
		perfmgr_notify_qudeq_cb(1,1);
	printk(KERN_EMERG "notifier: perfmgr_notify! identifier=d% \n" ,msgKM->identifier);        //回调处理函数
	return 0;
}

static struct notifier_block perfmgr_nb = {
	.notifier_call = perfmgr_notify,
};

static int perfmgr_notifier_init(void)
{
	if (fpsgo_notify_connect_fp)
		fpsgo_notify_connect_fp(0,1,1);
	if (fpsgo_notify_qudeq_fp)
		fpsgo_notify_qudeq_fp(1,0,1,1);
	printk(KERN_EMERG "notifier: perfmgr_notifier_init!\n");        
	cpufreq_register_notifier(&perfmgr_nb,
					 CPUFREQ_TRANSITION_NOTIFIER);   //注册notifier事件

	return 0;
}

static void perfmgr_notifier_exit(void)
{
	printk(KERN_EMERG "notifier: perfmgr_notifier_exit!\n");
	cpufreq_unregister_notifier(&perfmgr_nb,
					 CPUFREQ_TRANSITION_NOTIFIER);
//	call_notifier_call_chain(123); //在init函数中触发事件
}

module_init(perfmgr_notifier_init);
module_exit(perfmgr_notifier_exit);
MODULE_LICENSE("GPL v2"); 
