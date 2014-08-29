/*
 * =================================================================================
 *   (c) Copyright 1992-2013, mincore@163.com
 *                            All Rights Reserved
 *       Filename: scsi_swap_log.c
 *    Description: 
 *        Created: 2013年11月18日 17时18分12秒
 *         Author: csp
 *         Modify:  
 * =================================================================================
 */
#define CSPRING_KERNEL_CODE
#include "ring.h"
#include "swap.h"
#include "utils.h"
#include "crc32.h"

#include <linux/crc32.h>

// The initial CRC32 value used when calculating CRC checksums
#define LOG_CRC32_INIT 0xFFFFFFFFU
#define LOG_MAX_SECTOR_NUM 32								// 32*(512/32) = 32个扇区512条log
#define LOG_ITEM_SIZE (sizeof(struct scsi_swap_log_item))		// 一个条LOG 32字节
#define LOG_COUNT_PER_SECTOR (SECTOR_SIZE/LOG_ITEM_SIZE)	// 一个扇区16条LOG

/* 32 bytes */
struct scsi_swap_log_item {
	/* first 8 byte */
	u32 crc;
	u32 time;		/* seconds since 1970-1-1, will not overflow till 2016 */

	/* middle 16 byte */
	u64 sec_src;
	u64 sec_dst;

	/* last 8 byte */
	u16 sec_count;
	u8 type;
	u8 result;
	u8 reboot_count;
	u8 fix_count;
	u8 curr_count;
	u8 reserved;
};

static u32 scsi_swap_log_crc_head(struct scsi_swap_log *log)
{
	return swap_crc32(LOG_CRC32_INIT, ((char*)&log->head) + 4, 
			sizeof(log->head) - 4);
}

static u32 scsi_swap_log_crc_item(struct scsi_swap_log_item *item)
{
	return swap_crc32(LOG_CRC32_INIT, ((char *)item + 4), 
			sizeof(*item) - 4);
}

static int scsi_swap_log_update_data(struct scsi_swap_log *log, struct scsi_swap_log_item *item)
{
	struct scsi_device *sdev;
	struct scsi_swap *swap;
	
	char *base;
	char *start;
	char *data;
	sector_t sector;
	int i_sector;
	
	swap = container_of(log, struct swap_handler, log)->swap;
	sdev = container_of(swap, struct scsi_device, swap);

	sdev = log_to_scsi_device(log);

	
	base = (char *)cspring_buffer(log->ring);

	spin_lock(&log->lock);
	start = (char *)cspring_push(log->ring, item);
	spin_unlock(&log->lock);

	i_sector = (start - base) / SECTOR_SIZE;

	sector = log->sector_data + i_sector;
	data = base + i_sector;
	
	// When i_sector is the last ring sector, 
	// we will write some garbage which will nerver be accessed, 
	// so it's ok, harmless.
	return hd_write_sector_no_retry(sdev, sector, 1, data, SECTOR_SIZE);
}

static int scsi_swap_log_update_head(struct scsi_swap_log *log)
{
	char buf[SECTOR_SIZE] = {0};
	sector_t sector;
	struct scsi_device *sdev = log_to_scsi_device(log);

	// log count and next postion will change after pushed or poped
	log->head.item_num = cspring_num(log->ring);
	log->head.item_next = cspring_next(log->ring);
	log->head.crc = scsi_swap_log_crc_head(log);

	sector = log->sector_head;
	memcpy(buf, &log->head, sizeof(log->head));

	//SWAP_INFO("writting head sector %llu, crc %u, num %d, next %d\n",
	//		sector, log->head.crc, log->head.item_num, log->head.item_next);
	return hd_write_sector_no_retry(sdev, sector, 1, buf, SECTOR_SIZE);
}

static struct scsi_swap_log_item *scsi_swap_log_peek(struct scsi_swap_log *log, int index)
{
	if (index < 0 || index >= cspring_num(log->ring))
		return NULL;

	return (struct scsi_swap_log_item *)cspring_peek(log->ring, index);	
}

static struct scsi_swap_log_item *scsi_swap_log_last(struct scsi_swap_log *log)
{
	int num = cspring_num(log->ring);
	if (num == 0)
		return NULL;

	return scsi_swap_log_peek(log, num - 1);
}

static int scsi_swap_log_load(struct scsi_swap_log *log)
{
	struct scsi_device *sdev = log_to_scsi_device(log);

	char buf[SECTOR_SIZE];
	char *data;
	u32 crc;
	int len;

	// read log head
	if (hd_read_sector_no_retry(sdev, log->sector_head, 1, buf, sizeof(buf)) == -1) {
		SWAP_ERR("read swap log head [sector:%llu] failed\n", 
				(unsigned long long)log->sector_head);
		return -1;
	}

	memcpy(&log->head, buf, sizeof(log->head));

	// crc check the log head
	crc = scsi_swap_log_crc_head(log);
	if (log->head.crc != crc) { 
		SWAP_ERR("swap log head crc [%u:%u] failed\n", log->head.crc, crc);
		return -1;
	}

	data = cspring_buffer(log->ring);
	len = LOG_MAX_SECTOR_NUM * LOG_COUNT_PER_SECTOR * LOG_ITEM_SIZE;

	// read the logs
	return hd_read_sector_no_retry(sdev, log->sector_data,
				LOG_MAX_SECTOR_NUM, data, len);
}

int scsi_swap_log_init(struct scsi_swap_log *log, sector_t head, sector_t data)
{
	struct gendisk *gd;

	BUG_ON(sizeof(log->head) > SECTOR_SIZE);
	
	log->ring = cspring_create(sizeof(struct scsi_swap_log_item), 
			LOG_MAX_SECTOR_NUM * LOG_COUNT_PER_SECTOR);
	if (log->ring == NULL) {
		SWAP_ERR("cspring_create failed\n");
		return -1;
	}

	log->sector_head = head;
	log->sector_data = data;
	
	gd = log_to_swap_handler(log)->swap->disk;

	SWAP_DEBUG("%s log head:%llu, log data:%llu\n", gd->disk_name,
			(unsigned long long)log->sector_head, 
			(unsigned long long)log->sector_data);

	if (scsi_swap_log_load(log) == -1) {
		SWAP_ERR("scsi_swap_log_load failed\n");
		return -1;
	}
	
	log->ring->item_num = log->head.item_num;
	log->ring->item_next = log->head.item_next;

	spin_lock_init(&log->lock);

	return 0;
}

int scsi_swap_log_destroy(struct scsi_swap_log *log)
{
	if (log->ring)
		cspring_destroy(log->ring);
	return 0;
}

static int scsi_swap_log_item_init(
		struct scsi_swap_log *log,
		struct scsi_swap_log_item *item, 
		enum LOG_TYPE type, u8 result,
		sector_t src, sector_t dst, u16 count)
{
	struct scsi_swap_log_item *last;
	struct timespec now;

	memset(item, 0, sizeof(*item));

	getnstimeofday(&now);
	item->time = (u32)now.tv_sec;

	last = scsi_swap_log_last(log);
	if (last) {
		item->reboot_count = last->reboot_count;
		item->fix_count = last->fix_count;
		item->curr_count = last->curr_count;
	}
	item->curr_count++;

	item->crc = scsi_swap_log_crc_item(item);

	item->sec_src = src;
	item->sec_dst = dst;
	item->sec_count = count;
	item->type = type;
	item->result = result;

	return 0;
}

static void scsi_swap_log_item_dump(struct scsi_swap_log_item *item)
{
	SWAP_DEBUG("crc:%u time:%u\n"
			"src:%llu dst:%llu count:%u\n"
			"type:%d result:%d reboot_count:%d\n"
			"fix_count:%d curr_count:%d\n",
			item->crc, item->time,
			item->sec_src, item->sec_dst, item->sec_count,
			item->type, item->result, item->reboot_count,
			item->fix_count, item->curr_count);
}


int scsi_swap_log_push(struct scsi_swap_log *log, enum LOG_TYPE type, u8 result, 
		sector_t src, sector_t dst, u16 count)
{
	struct scsi_swap_log_item item;

	// 直接从ringbuffer里面拿到指针，再用指针填充可能效率更好点
	// 不过这里效率没有那么重要，还是简单点, 先准备好，再复制进去。
	scsi_swap_log_item_init(log, &item, type, result, src, dst, count);

	// update data and write to disk
	if (scsi_swap_log_update_data(log, &item) == -1) {
		SWAP_ERR("write log to disk failed\n");
		return -1;
	}

	// update head and write to disk
	if (scsi_swap_log_update_head(log) == -1) {
		SWAP_ERR("write log head to disk failed\n");
		return -1;
	}

	scsi_swap_log_item_dump(&item);

	return 0;
}

int scsi_swap_log_show(struct scsi_swap_log *log, char *page)
{
	struct scsi_swap_log_item *item;
	char buf[256];
	int len;
	int left = PAGE_SIZE;
	int i = 0;

	// 打印prepare
	spin_lock(&log->lock);
	while ((item = scsi_swap_log_peek(log, i++))) {
		len = snprintf(buf, sizeof(buf), "%u %llu %llu %u %d %d %d %d %d\n", 
				item->time, item->sec_src, item->sec_dst, item->sec_count,
				item->type, item->result, item->reboot_count, item->fix_count, 
				item->curr_count);
		if (left <= len)
			break;
		strcpy(page+(PAGE_SIZE-left), buf);
		left -= len;
	}
	spin_unlock(&log->lock);

	return PAGE_SIZE-left;
}

