/*
 * =====================================================================================
 *   (c) Copyright 1992-2013, mincore@163.com
 *                            All Rights Reserved
 *       Filename: sysfs.h
 *    Description: 
 *        Created: 2013年11月25日 15时33分19秒
 *         Author: csp
 *         Modify:  
 * =====================================================================================
 */
#ifndef _SCSI_SWAP_SYSFS_H
#define _SCSI_SWAP_SYSFS_H

struct scsi_swap;

int scsi_swap_register_sysfs(struct scsi_swap *swap);
int scsi_swap_unregister_sysfs(struct scsi_swap *swap);

#endif
