/*
 * =====================================================================================
 *   (c) Copyright 1992-2013, mincore@163.com
 *                            All Rights Reserved
 *       Filename: simulate.h
 *    Description: 
 *        Created: 2013年11月27日 15时37分01秒
 *         Author: csp
 *         Modify:  
 * =====================================================================================
 */
#include "swap.h"

struct sim_node {
	struct list_head entry;
	sector_t start;
	u32 num;
	int rw;
};

static bool sectorA_hit_sectorB(sector_t Astart, sector_t Aend, sector_t Bstart, sector_t Bend)
{
    return ((Astart >= Bstart)&&(Astart <= Bend))
        || ((Aend >= Bstart)&&(Aend <= Bend))
        || ((Astart <= Bstart)&&(Aend >= Bend));
}


int scsi_swap_sim_init(struct scsi_swap_sim *sim)
{
	spin_lock_init(&sim->list_lock);
	INIT_LIST_HEAD(&sim->list);
	return 0;
}

int scsi_swap_sim_destroy(struct scsi_swap_sim *sim)
{
	struct sim_node *node, *tmp;

	spin_lock(&sim->list_lock);
	list_for_each_entry_safe(node, tmp, &sim->list, entry) {
		list_del(&node->entry);
		kfree(node);
	}
	spin_unlock(&sim->list_lock);

	return 0;
}

int scsi_swap_sim_add(struct scsi_swap_sim *sim, sector_t sector, int num)
{
	struct sim_node *node;

	node = kmalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -1;

	node->start = sector;
	node->num = num;
	
	spin_lock(&sim->list_lock);
	list_add_tail(&node->entry, &sim->list);
	spin_unlock(&sim->list_lock);

	return 0;
}

int scsi_swap_sim_remove(struct scsi_swap_sim *sim, sector_t sector, int num)
{
	struct sim_node *node, *tmp;

	spin_lock(&sim->list_lock);
	list_for_each_entry_safe(node, tmp, &sim->list, entry) {
		if (node->start == sector) {
			list_del(&node->entry);
			kfree(node);
			break;
		}
	}
	spin_unlock(&sim->list_lock);

	return 0;
}

// FIXME: use hash table to make it more effective
bool scsi_swap_sim_hit(struct scsi_swap_sim *sim, sector_t sector, int num)
{
	struct sim_node *node;
	int ret = false;

	spin_lock(&sim->list_lock);
	list_for_each_entry(node, &sim->list, entry) {
		if (sectorA_hit_sectorB(sector, sector+num-1, 
					node->start, node->start+node->num-1)) {
			ret = true;
			break;
		}
	}
	spin_unlock(&sim->list_lock);

	return ret;
}

int scsi_swap_sim_show(struct scsi_swap_sim *sim, char *page)
{
	struct sim_node *node;
	char buf[64];
	int len;
	int left = PAGE_SIZE;

	spin_lock(&sim->list_lock);
	list_for_each_entry(node, &sim->list, entry) {
		len = snprintf(buf, sizeof(buf), "%llu %u\n", 
				(unsigned long long)node->start, node->num);
		if (left <= len)
			break;
		strcpy(page+(PAGE_SIZE-left), buf);
		left -= len;
	}
	spin_unlock(&sim->list_lock);

	return PAGE_SIZE-left;
}

