/*
 * =================================================================================
 *   (c) Copyright 1992-2013, mincore@163.com
 *                            All Rights Reserved
 *       Filename: cspring_buffer.h
 *    Description: Functions related to sysfs handling
 *        Created: 2013年11月22日 15时23分31秒
 *         Author: csp
 *         Modify:  
 * =================================================================================
 */
#include "swap.h"

struct swap_sysfs_entry {
	struct attribute attr;
	ssize_t (*show)(struct scsi_swap *, char *);
	ssize_t (*store)(struct scsi_swap *, const char *, size_t);
};

static ssize_t 
swap_swap_show(struct scsi_swap *swap, char *page)
{
	return scsi_swap_core_show(swap_to_swap_core(swap), page);
}

static ssize_t
swap_swap_store(struct scsi_swap *s, const char *page, size_t count)
{
	return -1;
}

static struct swap_sysfs_entry swap_swap_entry = {
	.attr = {.name = "swap", .mode = S_IRUGO | S_IWUSR },
	.show = swap_swap_show,
	.store = swap_swap_store,
};

static ssize_t
swap_log_show(struct scsi_swap *swap, char *page)
{
	return scsi_swap_log_show(swap_to_swap_log(swap), page);
}

static ssize_t
swap_log_store(struct scsi_swap *s, const char *page, size_t count)
{
	return -1;
}

static struct swap_sysfs_entry swap_log_entry = {
	.attr = {.name = "log", .mode = S_IRUGO | S_IWUSR },
	.show = swap_log_show,
	.store = swap_log_store,
};


#ifdef CONFIG_SCSI_SIM_BADSECTORS
static ssize_t 
swap_sim_show(struct scsi_swap *swap, char *page)
{
	return scsi_swap_sim_show(swap_to_swap_sim(swap), page);
}

static ssize_t
swap_sim_store(struct scsi_swap *swap, const char *page, size_t count)
{	
	sector_t sector;
	u32 num;

	if (sscanf(page, "add %llu %u", 
				(unsigned long long*)&sector, &num) == 2)
		scsi_swap_sim_add(swap_to_swap_sim(swap), sector, num);
	else if (sscanf(page, "remove %llu %u", 
				(unsigned long long*)&sector, &num) == 2)
		scsi_swap_sim_remove(swap_to_swap_sim(swap), sector, num);

	return count;
}

static struct swap_sysfs_entry swap_sim_entry = {
	.attr = {.name = "simulate", .mode = S_IRUGO | S_IWUSR },
	.show = swap_sim_show,
	.store = swap_sim_store,
};
#endif

#if 0
static ssize_t
swap_logging_level_show(struct scsi_swap *s, char *page)
{
	if (s->logging_level_show)
		return s->logging_level_show(s);
	return sprintf(page, "%s\n", "no implemented yet!");
}

static ssize_t
swap_logging_level_store(struct scsi_swap *s, const char *page, size_t count)
{
	if (s->logging_level_store)
		return s->logging_level_store(s);
	return -1;
}

static struct swap_sysfs_entry swap_log_entry = {
	.attr = {.name = "logging_level", .mode = S_IRUGO | S_IWUSR },
	.show = swap_logging_level_show,
	.store = swap_logging_level_store,
};
#endif

static struct attribute *default_attrs[] = {
	&swap_swap_entry.attr,
	&swap_log_entry.attr,
#ifdef CONFIG_SCSI_SIM_BADSECTORS
	&swap_sim_entry.attr,
#endif
	//&swap_logging_level_entry.attr,
	NULL,
};

#define to_swap(atr) container_of((atr), struct swap_sysfs_entry, attr)

static ssize_t
swap_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
	struct swap_sysfs_entry *entry = to_swap(attr);
	struct scsi_swap *swap =
		container_of(kobj, struct scsi_swap, kobj);
	ssize_t res;

	if (!entry->show)
		return -EIO;
	mutex_lock(&swap->sysfs_lock);
	res = entry->show(swap, page);
	mutex_unlock(&swap->sysfs_lock);
	return res;
}

static ssize_t
swap_attr_store(struct kobject *kobj, struct attribute *attr,
		    const char *page, size_t length)
{
	struct swap_sysfs_entry *entry = to_swap(attr);
	struct scsi_swap *swap =
		container_of(kobj, struct scsi_swap, kobj);
	ssize_t res;

	if (!entry->store)
		return -EIO;
	mutex_lock(&swap->sysfs_lock);
	res = entry->store(swap, page, length);
	mutex_unlock(&swap->sysfs_lock);
	return res;
}

static void scsi_release_swap(struct kobject *kobj)
{
}

static const struct sysfs_ops swap_sysfs_ops = {
	.show	= swap_attr_show,
	.store	= swap_attr_store,
};

static struct kobj_type scsi_swap_ktype = {
	.sysfs_ops	= &swap_sysfs_ops,
	.default_attrs	= default_attrs,
	.release	= scsi_release_swap,
};

int scsi_swap_register_sysfs(struct scsi_swap *swap)
{
	struct kobject *kobj = &swap->kobj;
	struct device *dev = disk_to_dev(swap->disk);
	int ret;

	if (!swap->enable)
		return -1;

	memset(kobj, 0, sizeof(*kobj));
	kobject_init(kobj, &scsi_swap_ktype);
	ret = kobject_add(kobj, kobject_get(&dev->kobj), "%s", "swap");
	if (ret < 0)
		return ret;

	kobject_uevent(kobj, KOBJ_ADD);
	return 0;
}

int scsi_swap_unregister_sysfs(struct scsi_swap *swap)
{
	struct kobject *kobj = &swap->kobj;
	struct device *dev = disk_to_dev(swap->disk);

	if (!swap->enable)
		return -1;

	kobject_uevent(kobj, KOBJ_REMOVE);
	kobject_del(kobj);
	kobject_put(&dev->kobj);
	return 0;
}
