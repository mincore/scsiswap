/*
 * =================================================================================
 *   (c) Copyright 1992-2013, mincore@163.com
 *                            All Rights Reserved
 *       Filename: scsi_swap_log.h
 *    Description: 
 *        Created: 2013年11月18日 17时16分32秒
 *         Author: csp
 *         Modify:  
 * =================================================================================
 */
#ifndef _SCSI_SWAP_LOG_H
#define _SCSI_SWAP_LOG_H

#define LOG_SUCCESS 0
#define LOG_FAILED 1

struct scsi_device;
struct scsi_swap;
struct cspring;

enum LOG_TYPE {
	LOG_TYPE_CREATE,
	LOG_TYPE_FIX,
	LOG_TYPE_CRC_FAILED,
};

enum LOG_CREATE_FAILED {
	LOG_FAILED_CREATE_MAXCOUNT,
	LOG_FAILED_CREATE_MALLOC,
	LOG_FAILED_CREATE_WRITE_DST,
	LOG_FAILED_CREATE_FLUSH_HEAD,
};

#define LOG_RESULT(fail_reason) \
	(fail_reason == -1 ? LOG_SUCCESS : (LOG_FAILED | (fail_reason << 1)))

// must be smaller than one page size
struct scsi_swap_log_head {
	u32 crc;
	u16 item_num;
	u16 item_next;
};

struct scsi_swap_log {
	struct scsi_swap_log_head head;
	struct cspring *ring;
	sector_t sector_head;
	sector_t sector_data;
	spinlock_t lock;
};

int scsi_swap_log_init(struct scsi_swap_log *log, sector_t head, sector_t data);
int scsi_swap_log_destroy(struct scsi_swap_log *log);
int scsi_swap_log_push(struct scsi_swap_log *log, enum LOG_TYPE type, u8 result, 
		sector_t src, sector_t dst, u16 count);
int scsi_swap_log_show(struct scsi_swap_log *log, char *page);

#endif
