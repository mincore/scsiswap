/*
 * =====================================================================================
 *   (c) Copyright 1992-2013, mincore@163.com
 *                            All Rights Reserved
 *       Filename: swap.h
 *    Description: 
 *        Created: 2013年11月27日 19时30分35秒
 *         Author: csp
 *         Modify:  
 * =====================================================================================
 */
#ifndef _SCSI_SWAP_SWAP_H
#define _SCSI_SWAP_SWAP_H

#include <scsi/scsi_device.h>
#include <scsi/scsi_swap.h>

#include "core.h"
#include "log.h"
#include "sim.h"

#define SWAP_INFO(fmt, ...)	\
		printk(KERN_INFO "[" "%s:%d" "] " fmt, __func__, __LINE__, ##__VA_ARGS__)

#define SWAP_ERR(fmt, ...)	\
		printk(KERN_ERR "[" "%s:%d" "] " fmt, __func__, __LINE__, ##__VA_ARGS__)

#define SWAP_DEBUG(fmt, ...)	\
		printk(KERN_DEBUG "[" "%s:%d" "] " fmt, __func__, __LINE__, ##__VA_ARGS__)


struct swap_handler {
	struct scsi_swap *swap;
	struct scsi_swap_core core;
	struct scsi_swap_log log;
#ifdef CONFIG_SCSI_SIM_BADSECTORS
	struct scsi_swap_sim sim;
#endif
};

#define swap_to_swap_handler(swap)	\
	((struct swap_handler *)((swap)->private_data))

#define swap_to_swap_core(swap)	\
	(&swap_to_swap_handler(swap)->core)

#define swap_to_swap_sim(swap)	\
	(&swap_to_swap_handler(swap)->sim)

#define swap_to_swap_log(swap)	\
	(&swap_to_swap_handler(swap)->log)

#define swap_to_scsi_device(swap)	\
	container_of(swap, struct scsi_device, swap)

#define core_to_swap_handler(core)	\
	container_of(core, struct swap_handler, core)

#define sim_to_swap_handler(sim)	\
	container_of(sim, struct swap_handler, sim)

#define log_to_swap_handler(log)	\
	container_of(log, struct swap_handler, log)

static inline struct scsi_device *
core_to_scsi_device(struct scsi_swap_core *core)
{
	struct scsi_swap *swap = core_to_swap_handler(core)->swap;
	return swap_to_scsi_device(swap);
}

static inline struct scsi_device *
log_to_scsi_device(struct scsi_swap_log *log)
{
	struct scsi_swap *swap = log_to_swap_handler(log)->swap;
	return swap_to_scsi_device(swap);
}

#ifdef CONFIG_SCSI_SIM_BADSECTORS
static inline struct scsi_device *
sim_to_scsi_device(struct scsi_swap_sim *sim)
{
	struct scsi_swap *swap = sim_to_swap_handler(sim)->swap;
	return swap_to_scsi_device(swap);
}
#endif

#endif

