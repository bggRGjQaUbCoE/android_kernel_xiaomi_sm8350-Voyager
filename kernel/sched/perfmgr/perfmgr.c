// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */
#define DEBUG

#include <linux/kthread.h>
#include <linux/unistd.h>
#include <linux/sched.h>
#include <linux/cpufreq.h>
#include <linux/topology.h>
#include <linux/arch_topology.h>

//add for feas perfmgr +{
#include <perfmgr.h>
#include "pfmgr_ioctl.h"

#define ENABLE_DELAYED_USECS  15000000 //15s
void perfmgr_notify_qudeq(int pid,unsigned long long identifier);
void perfmgr_notify_connect(int pid,unsigned long long identifier,int connectedAPI);
static struct mutex notify_lock;
static struct mutex list_lock;

struct list_head connected_buffer_list;
struct workqueue_struct *qbuffer_notifyworkqueue;
extern int perfmgr_enable;

extern void (*perfmgr_notify_qudeq_fp)(int pid,unsigned long long identifier);
extern void (*perfmgr_notify_connect_fp)(int pid,unsigned long long identifier,int connectedAPI);
void perfmgr_notify_connect(int pid,unsigned long long identifier,int connectedAPI);

#define MAX_CONNECTED_BUFFER 25
void *perfmgr_alloc_atomic(int i32Size)
{
	void *pvBuf;

	if (i32Size <= PAGE_SIZE)
		pvBuf = kmalloc(i32Size, GFP_ATOMIC);
	else
		pvBuf = vmalloc(i32Size);
	return pvBuf;
}

void perfmgr_free(void *pvBuf, int i32Size)
{
	if (!pvBuf)
		return;
	if (i32Size <= PAGE_SIZE)
		kfree(pvBuf);
	else
		vfree(pvBuf);
}

int perfmgr_is_enable(void)
{
	ktime_t current_time;
	s64 delta_usecs64;
	int enable=0;
	static ktime_t last_time;
	static int qudeq_count=0;
	static int need_delayed_enable=0;
	if(perfmgr_enable==0){
		need_delayed_enable=1;
		qudeq_count=0;
		enable=0;
	}
	else if((perfmgr_enable==1)&&(need_delayed_enable==1)){
		current_time = ktime_get();
		qudeq_count++;
		if(qudeq_count==1)
			last_time=current_time;
		delta_usecs64 = ktime_to_us(ktime_sub(current_time, last_time));
		if((perfmgr_enable==1)&&(delta_usecs64>=ENABLE_DELAYED_USECS)){
			need_delayed_enable=0;
			qudeq_count=0;
			enable = 1;
		}
	}else{
		mutex_lock(&notify_lock);
		enable = perfmgr_enable;
		mutex_unlock(&notify_lock);
	}
	pr_debug("[perfmgr_CTRL] isenable %d\n", enable);
	return enable;
}

int perfmgr_notify_connect_cb(int pid,unsigned long long identifier,int connectedAPI)
{

	struct connected_buffer *node= NULL;
	int caculate_buffer=0;
	int perfmgr_status=perfmgr_enable;
	pr_debug("%s pid:%d identifier %llu connectedAPI=%d \n",
                 __func__,pid,identifier,connectedAPI);

	if(connectedAPI){
		list_for_each_entry(node, &connected_buffer_list, list){
			if(node->identifier==identifier){
				return 0;
			}
		}
		node = kzalloc(sizeof(*node), GFP_KERNEL);
		if (!node) {
			return -ENOMEM;
		}
		memset(node, 0, sizeof(struct connected_buffer));
		node->pid=pid;
		node->identifier=identifier;
		list_add_tail(&node->list, &connected_buffer_list);
	}else{
		mutex_lock(&list_lock);
		list_for_each_entry(node, &connected_buffer_list, list){
			if(node->identifier==identifier){
				list_del(&node->list);
				kfree(node);
				break;
			}
		}
		mutex_unlock(&list_lock);
		list_for_each_entry(node, &connected_buffer_list, list){
			caculate_buffer++;
			pr_debug("%s connected_buffer_list: pid:%d identifier：%llu caculate_buffer=%d \n",
                                 __func__,node->pid,node->identifier,caculate_buffer);
		}
		if(caculate_buffer>=MAX_CONNECTED_BUFFER){
			struct connected_buffer *tmp;
			if(perfmgr_status)
				perfmgr_enable=0;
			mutex_lock(&list_lock);
			list_for_each_entry_safe(node, tmp, &connected_buffer_list, list) {
				list_del(&node->list);
				kfree(node);
			}
			mutex_unlock(&list_lock);
			if(perfmgr_status)
				perfmgr_enable=1;
			pr_info("free connected buffer list\n");
		}
	}

	return 0;
}

int perfmgr_notify_qudeq_cb(int pid,unsigned long long identifier)
{
	struct connected_buffer *priv=NULL;
	static int find_buffer=0;
	pr_debug("%s pid %d id %llu\n",__func__,pid,identifier);
	mutex_lock(&list_lock);
	list_for_each_entry(priv, &connected_buffer_list, list){
		if(priv->identifier==identifier){
			find_buffer=1;
			perfmgr_do_policy(priv);
			break;
		}
	}
	mutex_unlock(&list_lock);
	if(find_buffer==0)
		perfmgr_notify_connect(pid,identifier,1);
	return 0;
}

static void perfmgr_notifer_wq_cb(struct work_struct *psWork)
{
	struct PERFMGR_NOTIFIER_PUSH_TAG *vpPush =
		 container_of(psWork,
				struct PERFMGR_NOTIFIER_PUSH_TAG, sWork);

	if (!vpPush) {
		pr_debug("[perfmgr_CTRL] ERROR\n");
		return;
	}
	pr_debug("[perfmgr_CTRL] perfmgr_notifer_wq_cb push type = %d\n",
			vpPush->ePushType);

	switch (vpPush->ePushType) {

	case PERFMGR_NOTIFIER_QUEUE_DEQUEUE:
		perfmgr_notify_qudeq_cb(vpPush->pid,vpPush->identifier);
		break;
	case PERFMGR_NOTIFIER_CONNECT:
		perfmgr_notify_connect_cb(vpPush->pid,vpPush->identifier,
				vpPush->connectedAPI);
		break;

	default:
		pr_debug("[perfmgr_CTRL] unhandled push type  = %d\n",
				vpPush->ePushType);
		break;
	}
	perfmgr_free(vpPush, sizeof(struct PERFMGR_NOTIFIER_PUSH_TAG));
}

void perfmgr_notify_connect(int pid,unsigned long long identifier,int connectedAPI)
{
	struct PERFMGR_NOTIFIER_PUSH_TAG *vpPush=NULL;
	pr_debug("%s pid:%d identifier:%llu connectedAPI %d\n",
                 __func__,pid,identifier,connectedAPI);
	if (!perfmgr_is_enable())
		return;
	vpPush =
		(struct PERFMGR_NOTIFIER_PUSH_TAG *)
		perfmgr_alloc_atomic(sizeof(struct PERFMGR_NOTIFIER_PUSH_TAG));
	if (!vpPush) {
		pr_debug("[perfmgr_CTRL] OOM\n");
		return;
	}
	if (!qbuffer_notifyworkqueue) {
		pr_debug("[perfmgr_CTRL] NULL WorkQueue\n");
		perfmgr_free(vpPush, sizeof(struct PERFMGR_NOTIFIER_PUSH_TAG));
		return;
	}
	vpPush->ePushType = PERFMGR_NOTIFIER_CONNECT;
	vpPush->pid = pid;
	vpPush->identifier = identifier;
	vpPush->connectedAPI = connectedAPI;

	INIT_WORK(&vpPush->sWork, perfmgr_notifer_wq_cb);
	queue_work(qbuffer_notifyworkqueue, &vpPush->sWork);
}

void perfmgr_notify_qudeq(int pid,unsigned long long identifier)
{
	struct PERFMGR_NOTIFIER_PUSH_TAG *vpPush=NULL;
	pr_debug("%s pid %d id %llu  \n",__func__,pid,identifier);
	if (!perfmgr_is_enable())
		return;
	vpPush =
	(struct PERFMGR_NOTIFIER_PUSH_TAG *)
		perfmgr_alloc_atomic(sizeof(struct connected_buffer));
	if (!vpPush) {
		pr_debug("[perfmgr_CTRL] OOM\n");
		return;
	}

	if (!qbuffer_notifyworkqueue) {
		pr_debug("[perfmgr_CTRL] NULL WorkQueue\n");
		perfmgr_free(vpPush, sizeof(struct PERFMGR_NOTIFIER_PUSH_TAG));
		return;
	}

	vpPush->ePushType = PERFMGR_NOTIFIER_QUEUE_DEQUEUE;
	vpPush->pid = pid;
	vpPush->identifier = identifier;
	INIT_WORK(&vpPush->sWork, perfmgr_notifer_wq_cb);
	queue_work(qbuffer_notifyworkqueue, &vpPush->sWork);
}
//add for feas perfmgr +}

void fpsgo_notify_qudeq(int qudeq,
		unsigned int startend,
		int pid, unsigned long long id)
{
	if((qudeq==1)&&(startend==0)){
		pr_debug("[FPSGO_CTRL] perfmgr qudeq:%d startend:%d id %llu\n",
                           qudeq, startend, id);
		perfmgr_notify_qudeq(pid,id);
	}
}

void fpsgo_notify_connect(int pid,
		int connectedAPI, unsigned long long id)
{
	pr_debug(
		"[FPSGO_CTRL] perfmgr connect pid %d, id %llu, API %d\n",
		pid, id, connectedAPI);
	perfmgr_notify_connect(pid,id,connectedAPI);
}

static void __exit perfmgr_exit(void) {}
static int __init perfmgr_init(void)
{
	int i;
	int ret;

	pr_debug("[FPSGO_CTRL] init\n");

	fpsgo_notify_qudeq_fp = fpsgo_notify_qudeq;
	fpsgo_notify_connect_fp = fpsgo_notify_connect;

	qbuffer_notifyworkqueue =
	alloc_ordered_workqueue("%s", WQ_MEM_RECLAIM | WQ_HIGHPRI, "perfmgr_wq");
	if (qbuffer_notifyworkqueue == NULL)
		return -EFAULT;
	mutex_init(&notify_lock);
	mutex_init(&list_lock);
	INIT_LIST_HEAD(&connected_buffer_list);
	perfmgr_policy_init();

	return 0;
}

module_init(perfmgr_init);
module_exit(perfmgr_exit);

