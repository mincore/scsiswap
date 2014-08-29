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
#ifndef _SCSI_SWAP_SIMULATE_H
#define _SCSI_SWAP_SIMULATE_H

#include <linux/types.h>
#include <linux/spinlock.h>

struct scsi_swap_sim {
	struct list_head list;
	spinlock_t list_lock;
};

int scsi_swap_sim_init(struct scsi_swap_sim *sim);
int scsi_swap_sim_destroy(struct scsi_swap_sim *sim);
int scsi_swap_sim_add(struct scsi_swap_sim *sim, sector_t sector, int num);
int scsi_swap_sim_remove(struct scsi_swap_sim *sim, sector_t sector, int num);
bool scsi_swap_sim_hit(struct scsi_swap_sim *sim, sector_t sector, int num);
int scsi_swap_sim_show(struct scsi_swap_sim *sim, char *page);

#endif
