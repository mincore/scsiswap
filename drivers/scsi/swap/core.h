
/*
 * =====================================================================================
 *   (c) Copyright 1992-2013, mincore@163.com
 *                            All Rights Reserved
 *       Filename: crc32.h
 *    Description: 
 *        Created: 2013年11月24日 10时48分28秒
 *         Author: csp
 *         Modify:  
 * =====================================================================================
 */
#ifndef _BLK_SWAP_CORE_H
#define _BLK_SWAP_CORE_H

#include <linux/types.h>
#include <scsi/scsi_device.h>

#include "log.h"

#define SECTOR_SIZE                 512
#define SECTOR_1M                   (2*1024)        /* 1M空间所占的扇区 */
#define SECTOR_NUM_PER_SWAP_BLOCK   128             /* 每个替换块的扇区数, 128扇区，64K */
#define MAX_RESERVED_SECTOR         (SECTOR_1M*1024)/* 先预留，做兼容, 1024M */ 

#define DATA_SECTOR                 (SECTOR_1M*64)  /* 保留数据空间扇区数, 64M */
#define DATA_BLOCK_NUM              (DATA_SECTOR/SECTOR_NUM_PER_SWAP_BLOCK)     /* 保留空间block数, 1024 */
#define DATA_MAY_DIRTY				0xaa

/* SWAP HEAD INFO */
#define SWAP_HEAD_STRING_LEN        16
#define SWAP_HEAD_STATUS_LEN        16
#define SWAP_HEAD_VERSION_LEN       4
#define SWAP_HEAD_BITMAP_LEN        (DATA_BLOCK_NUM/32)
#define SWAP_HEAD_RESERVE_LEN       (SECTOR_SIZE				\
										-SWAP_HEAD_STRING_LEN	\
										-SWAP_HEAD_STATUS_LEN	\
										-SWAP_HEAD_VERSION_LEN	\
										-(4*SWAP_HEAD_BITMAP_LEN)-4)

#define SWAP_VERSION                    "0001"
#define SECTOR_8M                       (16*1024)       /* 8M空间所占的扇区 */
#define MAX_SWAP_BLOCK_FOR_USE          128             /* 最多能用的交换block, 128个 */
#define SWAP_HEAD_STRING                "DHSWAP"        /* 映射功能头在SWAP_HEAD_OFFEST位置固定字符表示支持映射功能 */
#define SWAP_HEAD_OFFEST                SECTOR_8M       /* 存放SWAP_HEAD_STRU的偏移地址相对于保留空间,前8M保留不用 */
#define SWAP_HEAD_N_SECTOR              8               /* SWAP_HEAD占的扇区数 */
#define SWAP_TABLE_OFFSET               (2*SECTOR_8M)   /* 存放swap映射表 的扇区偏移地址，相对于保留空间  */
#define SWPA_TABLE_N_SECTOR             32              /* SWAP_TALBE占的扇区数，每个扇区8个block信息，128个block，需要16个扇区 */
#define SWAP_DATA_OFFSET                (3*SECTOR_8M)   /* 数据从第24M开始 */
#define SWAP_HEAD_BACKUP_OFFEST         64              /* 存放SWAP_HEAD_STRU的偏移地址相对于SWAP_HEAD */
#define SWAP_HEAD_BACKUP_N_SECTOR       8               /* SWAP_HEAD占的扇区数 */
#define SWAP_TABLE_BACKUP_OFFSET        (MAX_RESERVED_SECTOR-SECTOR_8M) /* 存放备份swap映射表的扇区偏移地址，位于保留空间最后8M  */
#define SWAP_TABLE_BACKUP_N_SECTOR      32              /* SWAP_TABLE_BACKUP占的扇区数 */
#define MAX_SWAP_HEAD_BLOCK_NUM         (8*SECTOR_NUM_PER_SWAP_BLOCK)               /* 最大可用的交换扇区block */
#define MAX_SWAP_BLOCK                  (DATA_BLOCK_NUM)                            /* 系统支持的最大替换块个数 */

/* 日志相关定义 */
#define SWAP_LOG_TOTAL_SECTOR		(SECTOR_8M)
#define SWAP_LOG_HEAD_OFFSET		(SWAP_TABLE_BACKUP_OFFSET-SWAP_LOG_TOTAL_SECTOR)
#define SWAP_LOG_HEAD_SECTOR_COUNT	32 // must smaller than SECTOR_1M(2048)， 32 means 32*16 == 512 of log
#define SWAP_LOG_DATA_OFFSET		(SWAP_LOG_HEAD_OFFSET+SECTOR_1M)

#define SWAP_BLOCK_INDEX(sector)        ((sector)/SECTOR_NUM_PER_SWAP_BLOCK)        /* 128扇区对齐索引号 */
#define SWAP_BLOCK_SECTOR(i)            ((i)*SECTOR_NUM_PER_SWAP_BLOCK)             /* 128扇区对齐索引号对应的扇区号 */
#define SWAP_SECTOR_ALIGN(sector)       (SWAP_BLOCK_SECTOR(SWAP_BLOCK_INDEX(sector)))

#define SWAP_BLOCK_SIZE                 (SECTOR_NUM_PER_SWAP_BLOCK * SECTOR_SIZE)

#define SWAP_REASSIGN_BLKS_CMDLEN       6
#define SWAP_MAX_REASSIGN_BLKS_NUM      1024

#define swap_for_each_blk(blk_start, sector_start, sector_count)    \
    for(sector_t blk_start=SWAP_SECTOR_ALIGN(sector_start);         \
        blk_start<SWAP_SECTOR_ALIGN(sector_start+sector_count);     \
        blk_start += SECTOR_NUM_PER_SWAP_BLOCK)

#define swap_next_blk(sector_start)    \
    (SWAP_SECTOR_ALIGN(SWAP_BLOCK_SECTOR(SWAP_BLOCK_INDEX(sector_start)+1)))

// 512字节
typedef struct swap_head {
    char head_string[SWAP_HEAD_STRING_LEN];     /* swap保留区头 固定为宏SWAP_HEAD */
    char status[SWAP_HEAD_STATUS_LEN];          /* swap头的状态，valid:有效 invalid:无效 */
    char version[SWAP_HEAD_VERSION_LEN];        /* swap 版本号 */
    u32 bitmap[SWAP_HEAD_BITMAP_LEN];           /* 1024 bit */
    char reserved[SWAP_HEAD_RESERVE_LEN];       /* 不使用，需要清0 */
    u32 checksum;                               /* 校验和 */
} swap_head_t;

struct scsi_swap_core {
	struct scsi_device *sdev;
    atomic_t device_dead;
    atomic_t info_num;
    atomic_t user;
    struct swap_head head;
    struct list_head info_list;
	spinlock_t info_list_lock;
    spinlock_t bitmap_lock;

    sector_t capacity;              /* size in 512-byte sectors */
    sector_t sector_reserve_start;
    sector_t sector_head;
    sector_t sector_table;
    sector_t sector_data;
};


int scsi_swap_core_init(struct scsi_swap_core *core, sector_t reserve_sector);
int scsi_swap_core_destroy(struct scsi_swap_core *core);
int scsi_swap_core_read(struct scsi_swap_core *core, sector_t src, u32 num, sector_t bad, void *buf, u32 len); 
int scsi_swap_core_write(struct scsi_swap_core *core, sector_t src, u32 num, sector_t bad, const void *buf, u32 len); 
int scsi_swap_core_can_swap(struct scsi_swap_core *core, sector_t sector, u32 num);
int scsi_swap_core_swapped(struct scsi_swap_core *core, sector_t sector, u32 num);
int scsi_swap_core_show(struct scsi_swap_core *core, char *page);

#endif

