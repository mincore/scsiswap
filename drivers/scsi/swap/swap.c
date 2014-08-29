/*
 * =================================================================================
 *   (c) Copyright 1992-2013, mincore@163.com
 *                            All Rights Reserved
 *       Filename: swap.c
 *    Description: swap interface
 *        Created: 2013年11月18日 11时14分23秒
 *         Author: csp
 *         Modify:  
 * =================================================================================
 */
#include <linux/bio.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_swap.h>

#include "swap.h"
#include "sysfs.h"
#include "utils.h"

struct swap_bio_item{
    struct work_struct work;
	struct scsi_swap *swap; struct bio *bio;
    sector_t start_sec;
    int size;
    sector_t bad_sec;
    bool may_create;
};

struct swap_bio_private{
    void *orgi_private;
    bio_end_io_t *orgi_bi_end_io;
};

static struct workqueue_struct *g_swap_wq = NULL;	

static struct scsi_device *sdev_from_bdev(struct block_device *bdev)
{
    struct gendisk *gendisk;
	struct device *sdev_gendev;

    if(!bdev)
        return NULL;

    gendisk = bdev->bd_disk;
     if (!gendisk)
		 return NULL;

    sdev_gendev = gendisk->part0.__dev.parent;
    if (!sdev_gendev)
		return NULL;

    if (!scsi_is_sdev_device(sdev_gendev))
		return NULL;

    return to_scsi_device(sdev_gendev);
}

static struct scsi_swap *bio_get_scsi_swap(struct bio *bio)
{
    struct scsi_device *sdev;
	struct scsi_swap *swap;

    sdev = sdev_from_bdev(bio->bi_bdev);
	if (!sdev)
		return NULL;
    
	if (sdev->removable || sdev->changed)
		return NULL;
	
	swap = &sdev->swap;
	if (!swap->enable)
		return NULL;

	return swap;
}


static void buf_fill_bio(struct bio *bio, char *buf, int len)
{
    struct bio_vec *bvec;
    char *p = buf;
    char *addr= NULL;
    int i;

    bio_for_each_segment(bvec, bio, i) {
        if(p < buf+len){
            addr = page_address(bvec->bv_page);
            if(addr)
                memcpy(addr + bvec->bv_offset, p, bvec->bv_len);
            else {
                addr = kmap_atomic(bvec->bv_page);
                if (addr) {
                    memcpy(addr + bvec->bv_offset, p, bvec->bv_len);
                    kunmap_atomic(addr);
                }
                else
                    SWAP_DEBUG("error: NULL page addr, will lost data on sector(%llu: %d) read\n", 
							(unsigned long long)bio->bi_sector, (bio->bi_size >> 9));
            }
            p += bvec->bv_len;
        }
    }
}

static void bio_fill_buf(struct bio *bio, char *buf, int len)
{
    struct bio_vec *bvec;
    char *p = buf;
    char *addr = NULL;
    int i;

    bio_for_each_segment(bvec, bio, i) {
        if(p < buf+len){
            addr = page_address(bvec->bv_page);
            if(addr)
                memcpy(p, addr + bvec->bv_offset, bvec->bv_len);
            else {
                addr = kmap_atomic(bvec->bv_page);
                if (addr) {
                    memcpy(p, addr + bvec->bv_offset, bvec->bv_len);
                    kunmap_atomic(addr);
                }
                else
                    SWAP_DEBUG("error: NULL page addr, will lost data on sector(%llu: %d) write\n", 
							(unsigned long long)bio->bi_sector, (bio->bi_size >> 9));
            }
            p += bvec->bv_len;
        }
    }
}

static void swap_bio_work_handler(struct work_struct *work)
{
    struct swap_bio_item *item = container_of(work, struct swap_bio_item, work);
	struct scsi_swap *swap = item->swap;
    struct bio *bio = item->bio;
    int rw = bio->bi_rw & WRITE;
    char *buf = NULL;
    sector_t sector;
    sector_t bad;
    int size;
    int error = -EIO;
    int done = 0;
    int ret = 0;
	int num;
	struct scsi_swap_core *core = swap_to_swap_core(swap);

    sector = item->start_sec;
    size = item->size;
    bad = item->bad_sec;
	num = size >> 9;

    buf = kmalloc(size, GFP_KERNEL);

    if (NULL == buf)
    {
        SWAP_ERR("kmalloc buf fail\n");
        goto out;
    }
    else
    {
        if(rw == WRITE)
        {
            bio_fill_buf(bio, buf, size);
            //SWAP_INFO("core_write size = %d\n", size);
            ret = scsi_swap_core_write(core, sector, num, bad, buf, size);
            if(0 == ret)
            {
                done = 1;
            }
            else if (-DATA_MAY_DIRTY == ret)
            {
                //set_bit(BIO_RAID_REREAD, &bio->bi_flags);
                done = 1;
            }
        }
        else
        {
            //SWAP_INFO("core_read, size = %d\n", size);
            ret = scsi_swap_core_read(core, sector, num, bad, buf, size);
            if(0 == ret)
            {
                done = 1;
                buf_fill_bio(bio, buf, size);
            }
            else if (-DATA_MAY_DIRTY == ret)
            {
                //set_bit(BIO_RAID_REREAD, &bio->bi_flags);
                done = 1;
                buf_fill_bio(bio, buf, size);
            }
        }

        SWAP_DEBUG("%s sector:%llu, count:%u done:%d\n", rw == WRITE?"core_write":"core_read", 
				(unsigned long long)sector, size>>9, done);

        if (done) 
        {
            set_bit(BIO_UPTODATE, &bio->bi_flags);
            error = 0;
        }

        kfree(buf);
    }
    if (bio->bi_size >= size)
    {
        bio->bi_size -= size;
        bio->bi_sector += (size >> 9);
    }

out:
    kfree(item);
    if (bio->bi_end_io)
        bio->bi_end_io(bio, error);

}

static const char *swap_filter_table[] = {
	"mv64xx",
	"pm8001",
};

static bool scsi_device_can_swap(struct scsi_device *sdp)
{
	const char *proc_name;
	int i;
	
	if (sdp && sdp->host && sdp->host->hostt) {
		proc_name = sdp->host->hostt->proc_name;
		if (!proc_name)
			return false;

		for (i=0; i<ARRAY_SIZE(swap_filter_table); i++) {
			if (strcmp(proc_name, swap_filter_table[i]) == 0)
				return true;
		}
	}

	return false;
}

sector_t scsi_get_reserve_sectors_for_swap(struct scsi_device *sdp)
{
	return scsi_device_can_swap(sdp) ? MAX_RESERVED_SECTOR : 0;
}

bool swap_bio(struct bio *bio, sector_t sector, int size, sector_t bad_sec, int error, int may_create)
{
    struct swap_bio_item *item;
	struct scsi_swap *swap;
	struct scsi_swap_core *core;
	int rw;

    if (bio && error == -EIO) {
		/* !write error will not swap */
        rw = bio->bi_rw & WRITE;
        if ((WRITE != rw) && (-1 != bad_sec)) {
            goto err;
        }
		
		swap = bio_get_scsi_swap(bio);
		if (!swap)
			goto err;

		core = swap_to_swap_core(swap);

		if (!scsi_swap_core_can_swap(core, sector, size))
			goto err;

        item = kmalloc(sizeof(struct swap_bio_item), GFP_ATOMIC);
        if(!item){
            SWAP_ERR("kmalloc size %lu failed\n", sizeof(struct swap_bio_item));
            goto err;
        }

		item->swap = swap;
        item->bio = bio;
        item->start_sec = sector;
        item->size = size;
		item->bad_sec = bad_sec;
        item->may_create = may_create;

        SWAP_DEBUG("start sector = %llu, size = %d, bad_sec = %llu, "
				"error = %d, may_create = %d\n", 
				(unsigned long long)sector, size>>9, 
				(unsigned long long)bad_sec, error, may_create);

        INIT_WORK(&item->work, swap_bio_work_handler);
        queue_work(g_swap_wq, &item->work);

        return true;
    }

err:
    return false;
}

bool bio_has_bad_block (struct bio *bio)
{
	struct scsi_swap *swap;
	struct scsi_swap_core *core;
	
	swap = bio_get_scsi_swap(bio);
	if (!swap)
		return false;

	core = swap_to_swap_core(swap);
    
	return scsi_swap_core_swapped(core, bio->bi_sector, bio_sectors(bio));
}

#ifdef CONFIG_SCSI_SIM_BADSECTORS
// 从scmd里得到扇区, 返回扇区个数
static u32 sector_from_scmd(struct scsi_cmnd *scmd, sector_t *sector)
{
	u8 *cdb = scmd->cmnd;
	u32 low = 0;
	u32 hight = 0;
	u32 count = 0;
	
	switch (cdb[0])
	{
        case READ_6:
        case WRITE_6:
                low = (u32)((((u32)(cdb[1] & 0x1F))<<16) |
                                        ((u32)cdb[2]<<8) |
                                        ((u32)cdb[3]));
                count = (u32)cdb[4];
                break;
        case READ_10:
        case WRITE_10:
                low = (u32)(((u32)cdb[2]<<24) |
                                        ((u32)cdb[3]<<16) |
                                        ((u32)cdb[4]<<8) |
                                        ((u32)cdb[5]));
                count = ((u32)cdb[7]<<8) | (u32)cdb[8];
                break;
        case READ_12:
        case WRITE_12:
                low = (u32)(((u32)cdb[2]<<24) |
                                        ((u32)cdb[3]<<16) |
                                        ((u32)cdb[4]<<8) |
                                        ((u32)cdb[5]));
                count = (u32)(((u32)cdb[6]<<24) |
                                          ((u32)cdb[7]<<16) |
                                          ((u32)cdb[8]<<8) |
                                          ((u32)cdb[9]));
                break;
        case READ_16:
        case WRITE_16:
                hight = (u32)(((u32)cdb[2]<<24) |
                                       ((u32)cdb[3]<<16) |
                                       ((u32)cdb[4]<<8) |
                                       ((u32)cdb[5]));
                low = (u32)(((u32)cdb[6]<<24) |
                                      ((u32)cdb[7]<<16) |
                                      ((u32)cdb[8]<<8) |
                                      ((u32)cdb[9]));

                count = (u32)(((u32)cdb[10]<<24) |
                                          ((u32)cdb[11]<<16) |
                                          ((u32)cdb[12]<<8) |
                                          ((u32)cdb[13]));
                break;
	}
	
	*sector = ((u64)hight << 32) | low;
	
	return count;
}

#define SCSI_ACTION_UNKNOWN 0
#define SCSI_ACTION_READ 1
#define SCSI_ACTION_WRITE 2

static int scmd_get_action(struct scsi_cmnd *scmd)
{
	u8 *cdb = scmd->cmnd;
	
	switch(*cdb)
	{
		case READ_6:
		case READ_10:
		case READ_12:
		case READ_16:
			return SCSI_ACTION_READ;
			
		case WRITE_6:
		case WRITE_10:
		case WRITE_12:
		case WRITE_16:
			return SCSI_ACTION_WRITE;
	}
	
	return SCSI_ACTION_UNKNOWN;
}

bool scmd_should_be_bad(struct scsi_cmnd *scmd)
{
	struct scsi_swap_sim *sim;
	sector_t sector;
	u32 num;

	// read int in x86 is atomic
	if (!scmd->device->swap.enable)
		return false;

	// only write operation can be simulated
	if (scmd_get_action(scmd) != SCSI_ACTION_WRITE)
		return false;

	num = sector_from_scmd(scmd, &sector);
	sim = swap_to_swap_sim(&scmd->device->swap);

	return scsi_swap_sim_hit(sim, sector, num);
}
#endif

int scsi_swap_init(struct scsi_swap *swap, struct gendisk *gd, sector_t reserve_sector)
{
	struct swap_handler *handler;

	if (!scsi_device_can_swap(swap_to_scsi_device(swap)))
		return -1;

	swap->disk = gd;

	handler = kzalloc(sizeof(*handler), GFP_KERNEL);
	if (!handler)
		return -1;

	handler->swap = swap;
	swap->private_data = handler;
	
	if (scsi_swap_core_init(&handler->core, reserve_sector) < 0) {
		kfree(handler);
		return -1;
	}
	
	scsi_swap_log_init(&handler->log, 
			reserve_sector + SWAP_LOG_HEAD_OFFSET, 
			reserve_sector + SWAP_LOG_DATA_OFFSET);

#ifdef CONFIG_SCSI_SIM_BADSECTORS
	scsi_swap_sim_init(&handler->sim);
#endif

	mutex_init(&swap->sysfs_lock);

	// core is ok, enable it
	swap->enable = true;

	return 0;
}

int scsi_swap_destroy(struct scsi_swap *swap)
{
	if (!swap->enable)
		return -1;

	scsi_swap_core_destroy(swap_to_swap_core(swap));
	scsi_swap_log_destroy(swap_to_swap_log(swap));
#ifdef CONFIG_SCSI_SIM_BADSECTORS
	scsi_swap_sim_destroy(swap_to_swap_sim(swap));
#endif
	mutex_destroy(&swap->sysfs_lock);
	kfree(swap->private_data);
	return 0;
}

int module_scsi_swap_init(void)
{
    g_swap_wq = create_workqueue("blkswap");
    if (NULL == g_swap_wq)
    {
        SWAP_ERR("create blkswap workqueue fail\n");
        return -1;
    }

    return 0;
}

int module_scsi_swap_exit(void)
{
    destroy_workqueue(g_swap_wq);
    return 0;
}
 
