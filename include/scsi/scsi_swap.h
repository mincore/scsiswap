#ifndef _BSWAP_H
#define _BSWAP_H

#include <linux/types.h>
#include <linux/kobject.h>
#include <linux/mutex.h>

struct bio;
struct gendisk;
struct scsi_swap;
struct scsi_cmnd;
struct scsi_disk;
struct scsi_device;

struct scsi_swap {
    bool enable;
    struct kobject kobj;
    struct gendisk *disk;
	struct mutex sysfs_lock;
	sector_t offset;
	void *private_data;
};

int module_scsi_swap_init(void);
int module_scsi_swap_exit(void);

sector_t scsi_get_reserve_sectors_for_swap(struct scsi_device *sdp);

int scsi_swap_init(struct scsi_swap *swap, struct gendisk *disk, sector_t capacity);
int scsi_swap_destroy(struct scsi_swap *swap);

int scsi_swap_register_sysfs(struct scsi_swap *swap);
int scsi_swap_unregister_sysfs(struct scsi_swap *swap);

bool scmd_should_be_bad(struct scsi_cmnd *scmd);
bool bio_has_bad_block (struct bio *bio);
bool swap_bio(struct bio *bio, sector_t sector, int size, sector_t bad_sec, int error, int may_create);
                                                                                                                                                  
#endif 
