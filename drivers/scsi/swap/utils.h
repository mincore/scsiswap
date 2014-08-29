/*
 * =====================================================================================
 *   (c) Copyright 1992-2013, mincore@163.com
 *                            All Rights Reserved
 *       Filename: util.h
 *    Description: 
 *        Created: 2013年11月24日 10时51分21秒
 *         Author: csp
 *         Modify:  
 * =====================================================================================
 */
#include <linux/types.h>

struct scsi_device;

s32 hd_read_sector(struct scsi_device *sdev, sector_t sector, 
    u32 sec_num, void *buf, s32 len, int timeout, int retries);

s32 hd_read_sector_retry(struct scsi_device *sdev, sector_t sector, 
    u32 sec_num, void *buf, s32 len);

s32 hd_read_sector_no_retry(struct scsi_device *sdev, sector_t sector, 
    u32 sec_num, void *buf, s32 len);

s32 hd_write_sector(struct scsi_device *sdev, sector_t sector, 
    u32 sec_num, void *buf, s32 len, int timeout, int retries);

s32 hd_write_sector_retry(struct scsi_device *sdev, sector_t sector, 
    u32 sec_num, void *buf, s32 len);

s32 hd_write_sector_no_retry(struct scsi_device *sdev, sector_t sector, 
    u32 sec_num, void *buf, s32 len);

s32 hd_reassign_blocks(struct scsi_device *sdev, int longlba, int longlist, 
	void *paramp, int param_len, int timeout, int retries);

s32 hd_reassign_successive_sectors(struct scsi_device *sdev, sector_t sector, int count);

int hd_test_unit_ready(struct scsi_device *sdev);

int hd_sync_cache(struct scsi_device *sdev);
