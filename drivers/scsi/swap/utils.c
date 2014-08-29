/*
 * =====================================================================================
 *   (c) Copyright 1992-2013, mincore@163.com
 *                            All Rights Reserved
 *       Filename: utils.c
 *    Description: 
 *        Created: 2013年11月24日 10时56分29秒
 *         Author: csp
 *         Modify:  
 * =====================================================================================
 */
#include <linux/module.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/ide.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <asm-generic/bitops/find.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <scsi/scsi.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_cmnd.h>

#include "utils.h"
#include "swap.h"

#define SWAP_DEFAULT_TIMEOUT            (10*HZ)
#define SWAP_DEFAULT_RETRIES            5

s32 hd_read_sector(struct scsi_device *sdev, sector_t sector, 
    u32 sec_num, void *buf, s32 len, int timeout, int retries)
{
    s8 cdb[32]={READ_10,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00,0x00};
    s8 *cmnd = cdb;
    u8 sense[SCSI_SENSE_BUFFERSIZE] = {0};
    s32 ret = 0;
    s32 host_status = 0;
    s32 resid = 0;
    struct scsi_sense_hdr sshdr;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 14)
	struct scsi_request *sreq;
#endif

    if((sdev == NULL) || (buf == NULL))
    {
        return -1;
    }
    if(len < SECTOR_SIZE * sec_num)
    {
        return -1;
    }

    cdb[2] = (sector >> 24) & 0xff;
    cdb[3] = (sector >> 16) & 0xff;
    cdb[4] = (sector >> 8) & 0xff;
    cdb[5] = sector &0xff;

    cdb[6] = (sec_num >> 16) & 0xff;
    cdb[7] = (sec_num >> 8) & 0xff;
    cdb[8] = sec_num  & 0xff;
    
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 14)
	sreq = scsi_allocate_request(sdev, GFP_KERNEL);
	if (!sreq) {
		return -1;
	}
	sreq->sr_data_direction = DMA_FROM_DEVICE;
	scsi_wait_req(sreq, cmnd, buf, len, timeout, retries);

	/* 
	 * If there was an error condition, pass the info back to the user. 
	 */
	ret = sreq->sr_result;
	sense = sreq->sr_sense_buffer;
   
#elif LINUX_VERSION_CODE >=KERNEL_VERSION(2, 6, 29)
      	ret = scsi_execute(sdev, cmnd, DMA_FROM_DEVICE, buf, len,
                                sense, timeout, retries, 0, &resid);
#else
      	ret = scsi_execute(sdev, cmnd, DMA_FROM_DEVICE, buf, len,
                                sense, timeout, retries, 0);
#endif
    if (driver_byte(ret) == DRIVER_SENSE) 
    {
        /* sense data available */
      	ret &= ~(0xFF<<24); /* DRIVER_SENSE is not an error */

      	/* If we set cc then ATA pass-through will cause a
      	* check condition even if no error. Filter that. */
      	if (ret & SAM_STAT_CHECK_CONDITION) 
        {
          	scsi_normalize_sense(sense, SCSI_SENSE_BUFFERSIZE, &sshdr);
          	if ((sshdr.sense_key == 0) && (sshdr.asc == 0) && (sshdr.ascq == 0))
            { 
              	ret &= ~SAM_STAT_CHECK_CONDITION;
            }
      	}
  	}

    if(ret != 0)
    {
        host_status = host_byte(ret);
        if ((DID_NO_CONNECT == host_status) || (DID_BAD_TARGET == host_status))
        {
            struct scsi_swap_core *core = swap_to_swap_core(&sdev->swap);
            if(core)
            {
                atomic_inc(&core->device_dead);
            }
        }
    
        return -1;
    }

    return 0;
}


s32 hd_read_sector_retry(struct scsi_device *sdev, sector_t sector, 
    u32 sec_num, void *buf, s32 len)
{
    return hd_read_sector(sdev, sector, sec_num, buf, len, SWAP_DEFAULT_TIMEOUT, SWAP_DEFAULT_RETRIES);
}

s32 hd_read_sector_no_retry(struct scsi_device *sdev, sector_t sector, 
    u32 sec_num, void *buf, s32 len)
{
    return hd_read_sector(sdev, sector, sec_num, buf, len, (5*HZ), 0);
}

s32 hd_write_sector(struct scsi_device *sdev, sector_t sector, 
    u32 sec_num, void *buf, s32 len, int timeout, int retries)
{
    s8 cdb[32]={WRITE_10,0x00,0x00, 0x00,0x00,0x00, 0x00,0x00,0x00,0x00};
    s8 *cmnd = cdb;
    u8 sense[SCSI_SENSE_BUFFERSIZE] = {0};
    s32 ret = 0;
    s32 host_status = 0;
    s32 resid = 0;
    struct scsi_sense_hdr sshdr;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 14)
	struct scsi_request *sreq;
#endif

    if((sdev == NULL) || (buf == NULL))
    {
        return -1;
    }
    if(len < SECTOR_SIZE * sec_num)
    {
        return -1;
    }

    cdb[2] = (sector >> 24) & 0xff;
    cdb[3] = (sector >> 16) & 0xff;
    cdb[4] = (sector >> 8) & 0xff;
    cdb[5] = sector &0xff;

    cdb[6] = (sec_num >> 16) & 0xff;
    cdb[7] = (sec_num >> 8) & 0xff;
    cdb[8] = sec_num  & 0xff;
    
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 14)
	sreq = scsi_allocate_request(sdev, GFP_KERNEL);
	if (!sreq) {
		return -1;
	}
	sreq->sr_data_direction = DMA_TO_DEVICE;
	scsi_wait_req(sreq, cmnd, buf, len, timeout, retries);

	/* 
	 * If there was an error condition, pass the info back to the user. 
	 */
	ret = sreq->sr_result;
	sense = sreq->sr_sense_buffer;
   
#elif LINUX_VERSION_CODE >=KERNEL_VERSION(2, 6, 29)
      	ret = scsi_execute(sdev, cmnd, DMA_TO_DEVICE, buf, len,
                                sense, timeout, retries, 0, &resid);
#else
      	ret = scsi_execute(sdev, cmnd, DMA_TO_DEVICE, buf, len,
                                sense, timeout, retries, 0);
#endif
    if (driver_byte(ret) == DRIVER_SENSE) 
    {
        /* sense data available */
      	ret &= ~(0xFF<<24); /* DRIVER_SENSE is not an error */

      	/* If we set cc then ATA pass-through will cause a
      	* check condition even if no error. Filter that. */
      	if (ret & SAM_STAT_CHECK_CONDITION) 
        {
          	scsi_normalize_sense(sense, SCSI_SENSE_BUFFERSIZE, &sshdr);
          	if ((sshdr.sense_key == 0) && (sshdr.asc == 0) && (sshdr.ascq == 0))
            { 
              	ret &= ~SAM_STAT_CHECK_CONDITION;
            }
      	}
  	}
    
    if(ret != 0)
    {
        host_status = host_byte(ret);
        if ((DID_NO_CONNECT == host_status) || (DID_BAD_TARGET == host_status))
        {
            struct scsi_swap_core *core = swap_to_swap_core(&sdev->swap);
            if(core)
            {
                atomic_inc(&core->device_dead);
            }
        }
    
        return -1;
    }

    return 0;
}

s32 hd_write_sector_retry(struct scsi_device *sdev, sector_t sector, 
    u32 sec_num, void *buf, s32 len)
{
    return hd_write_sector(sdev, sector, sec_num, buf, len, SWAP_DEFAULT_TIMEOUT, SWAP_DEFAULT_RETRIES);
}

s32 hd_write_sector_no_retry(struct scsi_device *sdev, sector_t sector, 
    u32 sec_num, void *buf, s32 len)
{
    return hd_write_sector(sdev, sector, sec_num, buf, len, (5*HZ), 0);
}

 //功能描述  : 用REASSIGN_BLOCKS命令进行坏扇区映射
s32 hd_reassign_blocks(struct scsi_device *sdev, int longlba, int longlist, void * paramp,
                      int param_len, int timeout, int retries)

{
    s8 cdb[SWAP_REASSIGN_BLKS_CMDLEN]={REASSIGN_BLOCKS, 0, 0, 0, 0, 0};
    s8 *cmnd = cdb;
    u8 sense[SCSI_SENSE_BUFFERSIZE] = {0};
    s32 ret = 0;
    s32 host_status = 0;
    s32 resid = 0;
    struct scsi_sense_hdr sshdr;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 14)
	struct scsi_request *sreq;
#endif

    if((sdev == NULL) || (paramp == NULL))
    {
        return -1;
    }

    cdb[1] = (s8)(((longlba << 1) & 0x2) | (longlist & 0x1));

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 14)
	sreq = scsi_allocate_request(sdev, GFP_KERNEL);
	if (!sreq) {
		return -1;
	}
	sreq->sr_data_direction = DMA_TO_DEVICE;
	scsi_wait_req(sreq, cmnd, paramp, param_len, timeout, retries);

	/* 
	 * If there was an error condition, pass the info back to the user. 
	 */
	ret = sreq->sr_result;
	sense = sreq->sr_sense_buffer;
   
#elif LINUX_VERSION_CODE >=KERNEL_VERSION(2, 6, 29)
      	ret = scsi_execute(sdev, cmnd, DMA_TO_DEVICE, paramp, param_len,
                                sense, timeout, retries, 0, &resid);
#else
      	ret = scsi_execute(sdev, cmnd, DMA_TO_DEVICE, paramp, param_len,
                                sense, timeout, retries, 0);
#endif
    if (driver_byte(ret) == DRIVER_SENSE) 
    {
        /* sense data available */
      	ret &= ~(0xFF<<24); /* DRIVER_SENSE is not an error */

      	/* If we set cc then ATA pass-through will cause a
      	* check condition even if no error. Filter that. */
      	if (ret & SAM_STAT_CHECK_CONDITION) 
        {
          	scsi_normalize_sense(sense, SCSI_SENSE_BUFFERSIZE, &sshdr);
          	if ((sshdr.sense_key == 0) && (sshdr.asc == 0) && (sshdr.ascq == 0))
            { 
              	ret &= ~SAM_STAT_CHECK_CONDITION;
            }
      	}
  	}
    
    if(ret != 0)
    {
        host_status = host_byte(ret);
        if ((DID_NO_CONNECT == host_status) || (DID_BAD_TARGET == host_status))
        {
            struct scsi_swap_core *core = swap_to_swap_core(&sdev->swap);
            if(core)
            {
                atomic_inc(&core->device_dead);
            }
        }
    
        return -1;
    }

    return 0;
}

 //功能描述  : 用REASSIGN_BLOCKS命令对连续的坏扇区进行映射
s32 hd_reassign_successive_sectors(struct scsi_device *sdev, sector_t sector, int count)
{
    /* longlba & longlist */
    /****************************************
     * BYTES  0-3 : LEN                     *
     * BYTES  4-11: SECTOR                  * 
     * BYTES 12-19: SECTOR                  *
     * BYTES 20-27: .......                 *
     ****************************************
     */
    unsigned char * param_arr = NULL;
    int param_len = 0;
    int j = 0;
    int k = 0;
    int ret = 0;

    if ((sdev == NULL) || (count > SWAP_MAX_REASSIGN_BLKS_NUM))
    {
        return -1;
    }

    /* 长度4bytes, 每个扇区8字节 */
    param_len = 4 + 8 * count;

    param_arr = kmalloc(param_len, GFP_KERNEL);
    if (NULL == param_arr)
    {
        return -1;
    }

    /* 长度部分，不包括前4字节本身 */
    param_arr[0] = ((param_len - 4) >> 24) & 0xff;
    param_arr[1] = ((param_len - 4) >> 16) & 0xff;
    param_arr[2] = ((param_len - 4) >> 8) & 0xff;
    param_arr[3] = (param_len - 4) & 0xff;

    k = 4;
    for (j = 0; j < count; ++j) 
    {
        param_arr[k++] = ((sector + j) >> 56) & 0xff;
        param_arr[k++] = ((sector + j) >> 48) & 0xff;
        param_arr[k++] = ((sector + j) >> 40) & 0xff;
        param_arr[k++] = ((sector + j) >> 32) & 0xff;
        param_arr[k++] = ((sector + j) >> 24) & 0xff;
        param_arr[k++] = ((sector + j) >> 16) & 0xff;
        param_arr[k++] = ((sector + j) >> 8) & 0xff;
        param_arr[k++] = (sector + j) & 0xff;
    }

    ret = hd_reassign_blocks(sdev, 1, 1, param_arr, param_len, (10*HZ), 0);

    kfree(param_arr);

    return ret;
    
}

int hd_test_unit_ready(struct scsi_device *sdev)
{
	char cmd[] = {TEST_UNIT_READY, 0, 0, 0, 0, 0};
	struct scsi_sense_hdr *sshdr;
	int result = 0;
    int retries = 3;

    sshdr = kzalloc(sizeof(*sshdr), GFP_KERNEL);

	/* try to eat the UNIT_ATTENTION if there are enough retries */
	do {
		result = scsi_execute_req(sdev, cmd, DMA_NONE, NULL, 0, sshdr, (10*HZ), 0, NULL);
		if (sdev->removable && scsi_sense_valid(sshdr) &&
		    sshdr->sense_key == UNIT_ATTENTION)
			sdev->changed = 1;
	} while (scsi_sense_valid(sshdr) &&
		 sshdr->sense_key == UNIT_ATTENTION && --retries);

	if (!sshdr)
		/* could not allocate sense buffer, so can't process it */
		return result;

	if (sdev->removable && scsi_sense_valid(sshdr) &&
	    (sshdr->sense_key == UNIT_ATTENTION ||
	     sshdr->sense_key == NOT_READY)) {
		sdev->changed = 1;
		result = 0;
	}
	kfree(sshdr);
	return result;
}

int hd_sync_cache(struct scsi_device *sdev)
{
	int retries, res;
	struct scsi_sense_hdr sshdr;

	if (!scsi_device_online(sdev))
		return -ENODEV;


	for (retries = 3; retries > 0; --retries) {
		unsigned char cmd[10] = { 0 };

		cmd[0] = SYNCHRONIZE_CACHE;
		/*
		 * Leave the rest of the command zero to indicate
		 * flush everything.
		 */
		res = scsi_execute_req(sdev, cmd, DMA_NONE, NULL, 0, &sshdr,
				       (60*HZ), 2, NULL);
		if (res == 0)
			break;
	}

	if (res)
		return -EIO;
	return 0;
}

