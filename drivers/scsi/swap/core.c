#include <scsi/scsi_swap.h>
#include <linux/delay.h>

#include "swap.h"
#include "crc32.h"
#include "utils.h"

#undef debug_info

#ifdef debug_info

#define SWAP_ASSERT(expr) do { \
	if (unlikely(!expr)) { \
		printk(KERN_ERR "SWAP: assertion failed at %s (%d)\n", \
		       __FILE__,  __LINE__); \
		dump_stack(); \
	} \
} while(0)

#else

#define SWAP_ASSERT(expr)

#endif

#define my_print(fmt, args ...) do {printk("-->%s[%d]: " fmt, __func__,__LINE__, ## args);} while(0)

#define dbg_print printk


// 64�ֽ�
typedef struct swap_table {
    sector_t src_sec;   /* Դ����ʼ������ */
    sector_t swap_sec;  /* �滻�����ʼ������ */
    u32 sec_size;       /* �����滻��������=0 ��ʾû��ӳ�� */
    u32 retrys;         /* �������Դ��� */
    u32 index;          /* �������滻������� ȡֵΪ 0 ~ MAX_SWAP_BLOCK-1 */
    u32 reserverd[6];   /* ��ʹ�ã���Ҫ��0 */
    u32 checksum;       /* У�� */
    sector_t save_sec;       /* ����������ţ�������checksum */
} swap_table_t;

typedef struct swap_info {
    struct list_head list;
    struct swap_table table;    /* ���滻�� */
    char *data;                 /* �滻����������ָ�� */
    u32 reserverd[5];
} swap_info_t;

typedef struct swap_reassign_blocks{
    int longlba;
    int longlist;
    u32 count;
    sector_t block_addrs[0];
} swap_reassign_blocks_t;

typedef enum swap_head_state{
    HEAD_OK = 0,                    /* ����״̬ */
    HEAD_EMPTY,                     /* �յ� */
    HEAD_BROKEN,                    /* �� */
    HEAD_INVALID,                   /* ��Ч��һ����Ϊblock���л��� */
    HEAD_FAIL,                      /* ��ȡʧ�� */
    UNKNOWN,
}swap_head_state_e;

typedef enum swap_table_sync{
    NO_NEED_SYNC = 0,                    /* ����״̬ */
    MASTER_NEED_SYNC,                     /* �յ� */
    BACKUP_NEED_SYNC,                    /* �� */
}swap_table_sync_e;

static inline int get_swap_block_count(sector_t start, int count)
{
    int nlen = start + count - SWAP_SECTOR_ALIGN(start); // �������ܳ���

    return nlen/SECTOR_NUM_PER_SWAP_BLOCK + (nlen%SECTOR_NUM_PER_SWAP_BLOCK == 0 ? 0 : 1);
}

// ���һ����������
static inline void swap_bitmap_set_bit(unsigned long *map, int nbit, int val)
{
    val == 0 ? 
    bitmap_clear(map, nbit, 1):
    bitmap_set(map, nbit, 1);
    return;
}

// ���ҵ�һ�����õĽ�����
static int swap_head_bitmap_get_first_zero_bit(struct swap_head *head)
{
    unsigned long nbit = find_first_zero_bit((unsigned long *)head->bitmap, sizeof(head->bitmap)*8);

    // �ҵ����Ϊ-1
    if ((int)nbit == sizeof(head->bitmap)*8)
    {
        return -1;
    }
    else
    {
        return (int)nbit;
    }
}

//  ����������, дʵ������
static inline int flush_swap_info_data(struct scsi_swap_core *core, struct swap_info *info)
{
	struct scsi_device *sdev = core_to_scsi_device(core);
    return hd_write_sector_retry(sdev, info->table.swap_sec, 
			info->table.sec_size, info->data, info->table.sec_size * SECTOR_SIZE);
}


//��������  : ����ӳ���һ�ΰ����б�����д��
//�� �� ֵ  : 0 �ɹ� -1 ʧ��
static int flush_swap_info_table(struct scsi_swap_core *core)
{
    struct swap_info *entry;
    struct swap_info *next;
    struct list_head *head = &core->info_list;
    int table_num = SECTOR_SIZE/sizeof(struct swap_table); //ÿ���������Էŵ�swap_table����
    char buffer[SECTOR_SIZE] = {0};
    sector_t sect_index = core->sector_table;
    sector_t sect_back_index = core->sector_reserve_start + SWAP_TABLE_BACKUP_OFFSET;
    int i = 0;
	struct scsi_device *device = core_to_scsi_device(core);

    spin_lock(&core->info_list_lock);

    list_for_each_entry_safe(entry, next, head, list)
    {
        /* ��������¿�һ�������������� */
        if (0 == i)
        {
            memset(buffer, 0 , sizeof(buffer));
        }

        /* ӳ������ڵ������ţ�ֻ��¼��ӳ�������ӳ���ɼ���õ� */
        entry->table.save_sec = sect_index;
        
        memcpy(buffer + i*sizeof(struct swap_table), &entry->table, sizeof(struct swap_table));

        if(i++ == (table_num - 1))
        {
            /* д����ӳ�������д�������� */
            while (0 != hd_write_sector_retry(device, sect_index++, 1, buffer, SECTOR_SIZE)) 
            {
                SWAP_ERR("flush master table failed\n");
                if (sect_index >= (core->sector_table + SWPA_TABLE_N_SECTOR))
                {
                    break;
                }
            }

            /* д�뱸��ӳ�������д�������� */
            while (0 != hd_write_sector_retry(device, sect_back_index++, 1, buffer, SECTOR_SIZE)) 
            {
                SWAP_ERR("flush master table failed\n");
                if (sect_back_index >= (core->sector_reserve_start + SWAP_TABLE_BACKUP_OFFSET + SWPA_TABLE_N_SECTOR))
                {
                    break;
                }
            }
            
            i = 0;
        }
    }
    spin_unlock(&core->info_list_lock);

    /* ���������Ϣ����һ������������д�� */
    if (0 != i)
    {
        /* д����ӳ�������д�������� */
        while (0 != hd_write_sector_retry(device, sect_index++, 1, buffer, SECTOR_SIZE)) 
        {
            SWAP_ERR("flush master table failed\n");
            if (sect_index >= (core->sector_table + SWPA_TABLE_N_SECTOR))
            {
                break;
            }
        }

        /* д�뱸��ӳ�������д�������� */
        while (0 != hd_write_sector_retry(device, sect_back_index++, 1, buffer, SECTOR_SIZE)) 
        {
            SWAP_ERR("flush master table failed\n");
            if (sect_back_index >= (core->sector_reserve_start + SWAP_TABLE_BACKUP_OFFSET + SWPA_TABLE_N_SECTOR))
            {
                break;
            }
        }
    }

    return 0;
}


/* ��scsi_device�ҵ���Ӧ�����ṹ */
//static struct scsi_swap *scsi_device_to_scsi_swap(struct scsi_device *device)
//{
//    return &device->swap;
//}


/*****************************************************************************
 �� �� ��  : swap_find_swap_info
 ��������  : �ø���������,����swap_info�ṹ
 �������  : 
 �������  : 
 �� �� ֵ  : �ɹ����� swap_info_t ָ��, ʧ�ܷ���NULL
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static swap_info_t *swap_find_swap_info(struct scsi_swap_core *core, sector_t sector)
{
    swap_info_t *info;
    swap_info_t *next;

    if (NULL == core)
    {
        return NULL;
    }

    spin_lock(&core->info_list_lock);

    list_for_each_entry_safe(info, next, &core->info_list, list) 
    {
        if (NULL != info)
        {
            if (info->table.src_sec == sector) 
            {
                spin_unlock(&core->info_list_lock);
                return info;
            }
        }
    }
    
    spin_unlock(&core->info_list_lock);
    
    return NULL;
}

/*****************************************************************************
 �� �� ��  : flush_swap_head
 ��������  : д����ͷ, ��������
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ� -1 ʧ��
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static int flush_swap_head(struct scsi_swap_core *core)
{
    struct scsi_device *device = core_to_scsi_device(core);
	struct swap_head *head = &core->head;

    int ret = 0;

    SWAP_ASSERT(sizeof (struct swap_head) == SECTOR_SIZE);
    
    head->checksum = swap_crc32(~0, head, SECTOR_SIZE - sizeof(u32));

    /* д��Ӳ�� */
    ret = hd_write_sector_retry(device, core->sector_head, 1, head, SECTOR_SIZE);
    if (0 != ret)
    {
        /* ���ﲻ���أ�����������б��� */
        SWAP_ERR("write to swap head failed\n");
    }

    /* ���� */
    ret = hd_write_sector_retry(device, core->sector_head + SWAP_HEAD_BACKUP_OFFEST, 1, head, SECTOR_SIZE);
    if (0 != ret)
    {
        SWAP_ERR("write to swap back head failed\n");
        return -1;
    }

    return 0;
}

/*****************************************************************************
 �� �� ��  : swap_check_block_by_sector
 ��������  : ���һ��block�Ƿ��л�������һ������һ��������
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ� -1 ʧ��
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static int swap_check_block_by_sector(struct scsi_device *sdev, sector_t sec_start, int *bad_index)
{
    char buffer[SECTOR_SIZE] = {0};
    int i = 0;

    memset(buffer, 0 , sizeof(buffer));

    // һ������һ������д0���г�ʼ��
    for (i = 0; i < SECTOR_NUM_PER_SWAP_BLOCK; i++)
    {
        if(0 != hd_write_sector_retry(sdev, (sector_t)(sec_start + i), 1, buffer, sizeof(buffer)))
        {
            /* ֻҪ��һ������������block������ */
            SWAP_ERR("new head has bad block(sec %d write 0 error)\n", i);
            *bad_index = i;
            return -1;
        }
    }

    return 0;
}

/*****************************************************************************
 �� �� ��  : swap_check_block
 ��������  : ���һ��block�Ƿ��л���������������һ����
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ� -1 ʧ��
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static int swap_check_block(struct scsi_device *sdev, sector_t sec_start)
{
    char * buffer = NULL;

    size_t block_size = SECTOR_SIZE*SECTOR_NUM_PER_SWAP_BLOCK;

    buffer = kmalloc(block_size, GFP_KERNEL);
    if (NULL == buffer)
    {
        SWAP_ERR("kmalloc fail\n");
        return -1;
    }

    memset(buffer, 0 , block_size);
    if(0 != hd_write_sector_retry(sdev, sec_start , SECTOR_NUM_PER_SWAP_BLOCK, buffer, block_size))
    {
        SWAP_ERR("sector %llu write 0 fail\n", (unsigned long long)sec_start);
        kfree(buffer);
        return -1;
    }
    
    kfree(buffer);

    return 0;
}

/*****************************************************************************
 �� �� ��  : _swap_create_new_head
 ��������  : ӳ���ͷ��Ϣд��Ӳ��
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ� -1 ʧ��
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static int _swap_create_new_head(struct scsi_swap_core *core)
{
    struct swap_head *head = &core->head;

    strncpy((char *)head->head_string, SWAP_HEAD_STRING, 16);
    strncpy((char *)head->version, SWAP_VERSION, 4);

    return flush_swap_head(core);
}


/*****************************************************************************
 �� �� ��  : swap_create_new_head
 ��������  : ����ӳ���ͷ��Ϣ
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ� -1 ʧ��
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static int swap_create_new_head(struct scsi_swap_core *core)
{
    int ret = 0;
    int i = 0;
    int bad_index = 0;
	struct scsi_device *device = core_to_scsi_device(core);
	struct swap_head *head = &core->head;

    memset(head, 0, sizeof(struct swap_head));

    for (i = 0; i < MAX_SWAP_HEAD_BLOCK_NUM; i++)
    {
        /* ����block��û�л��� */
        ret = swap_check_block_by_sector(device, core->sector_head, &bad_index);
        if (0 == ret)
        {
            strncpy((char *)head->status, "valid", 5);
            break;
        }
        else
        {
            /* ����ǵ�0�����𻵣����账�����0����OK����0����д��invalid״̬ */
            if (0 != bad_index)
            {
                strncpy((char *)head->status, "invalid", 7);
                _swap_create_new_head(core);
            }
        }

        memset((char *)head->status, 0, SWAP_HEAD_STATUS_LEN);
        core->sector_head += SECTOR_NUM_PER_SWAP_BLOCK;
    }

    if (i >= MAX_SWAP_HEAD_BLOCK_NUM)
    {
        SWAP_ERR("no valid head block\n");
        return -1;
    }

    return _swap_create_new_head(core);
}


/*****************************************************************************
 �� �� ��  : swap_create_new_head
 ��������  : ����info����, ��block�Ƿ��Ѿ�ӳ���
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ� -1 ʧ��
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static bool _did_block_swapped(struct scsi_swap_core *core, sector_t sector, int count)
{
    sector_t end = sector + count;
    sector_t start = SWAP_SECTOR_ALIGN(sector);
    bool ret = false;

    for (; start < end; start += SECTOR_NUM_PER_SWAP_BLOCK) {
        if (NULL != swap_find_swap_info(core, start)) {
            ret = true;
            SWAP_DEBUG("sector = %llu, count = %d, start = %llu, end = %llu, return: true\n", 
					(unsigned long long)sector, count, 
					(unsigned long long)start, 
					(unsigned long long)end);
            break;
        }
    }

    return ret;
}

/*****************************************************************************
 �� �� ��  : _swap_alloc_info
 ��������  : ����swap_info_t�ṹ
 �������  : 
 �������  : 
 �� �� ֵ  : �ɹ�����infoָ��  ʧ�ܷ���NULL
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static swap_info_t *_swap_alloc_info(void)
{
    swap_info_t *info = NULL;
    
    info = (swap_info_t *)kmalloc(sizeof(swap_info_t), GFP_KERNEL);
    if(info == NULL)
    {
        SWAP_ERR("kmalloc fail\n");
        return NULL;
    }
    
    memset (info, 0, sizeof(swap_info_t));

    info->data = kmalloc(SWAP_BLOCK_SIZE, GFP_KERNEL);
    if(NULL == info->data)
    {
        SWAP_ERR("kmalloc fail\n");
        kfree(info);
        return NULL;
    }
    
    memset(info->data, 0, SWAP_BLOCK_SIZE);

    return info;
}

/*****************************************************************************
 �� �� ��  : _swap_dealloc_info
 ��������  : ����swap_info_t�ṹ
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ� -1 ʧ��
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static void _swap_dealloc_info(swap_info_t *info)
{
    if (NULL == info)
    {
        return;
    }

    if (NULL != info->data)
    {
        kfree (info->data);
        info->data = NULL;
    }
    kfree(info);
    info = NULL;
    return;
}

/*****************************************************************************
 �� �� ��  : _swap_alloc_new_block
 ��������  : ����bitmap������һ�����õ�ӳ���
 �������  : 
 �������  : 
 �� �� ֵ  : �ɹ�ӳ����index ʧ�ܷ���-1
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static int _swap_alloc_new_block(struct scsi_swap_core *core)
{
    int index = 0;
    int err_cnt = 0;
	struct swap_head *head = &core->head;
	struct scsi_device *device = core_to_scsi_device(core);

    index = swap_head_bitmap_get_first_zero_bit(head);
    
    if (-1 == index)
    {
        SWAP_ERR("no more free blocks for swap\n");
        return -1;
    }

    /* ��ⱻӳ��block�Ƿ���� */
    while(0 != swap_check_block(device, core->sector_data + SECTOR_NUM_PER_SWAP_BLOCK * index))
    {
        if (err_cnt++ >= 16)
        {
            atomic_inc(&core->device_dead);
            SWAP_ERR("too many error sector while get free blocks\n");
            return -1;
        }
        /* ����ǻ��ģ�bitmap��1��Ȼ�����������һ�����õ�block */
        swap_bitmap_set_bit((unsigned long *)core->head.bitmap, index, 1);
        index = swap_head_bitmap_get_first_zero_bit(head);
        if (-1 == index)
        {
            SWAP_ERR("no more free blocks for swap\n");
            return -1;
        }
    }
    /* ����bitmap */
    swap_bitmap_set_bit((unsigned long *)core->head.bitmap, index, 1);

    return index;
}



/*****************************************************************************
 �� �� ��  : _swap_create
 ��������  : �½�һ��������
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ� -1 ʧ��
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static int _swap_create(struct scsi_swap_core *core, swap_info_t *info, sector_t src, int need_zero_offset, int need_zero_len)
{
	struct scsi_device *sdev = core_to_scsi_device(core);
    int front_len;
    int back_len;
    int index = 0;

    spin_lock(&core->bitmap_lock);
    index = _swap_alloc_new_block(core);
    spin_unlock(&core->bitmap_lock);
    
    if(-1 == index)
    {
        SWAP_ERR("no reserved space left\n");
        return -1;
    }

    if(need_zero_offset + need_zero_len > SECTOR_NUM_PER_SWAP_BLOCK)
    {
        SWAP_ERR("invaild param\n");
        need_zero_len = SECTOR_NUM_PER_SWAP_BLOCK - need_zero_offset;
    }

    front_len = need_zero_offset;
    back_len = SECTOR_NUM_PER_SWAP_BLOCK - (need_zero_offset + need_zero_len);

    SWAP_ERR("created a swap %llu, %d\n", (unsigned long long)src, 128);

    if (front_len > 0)
    {
		// part ӳ��
        if(0 != hd_read_sector_no_retry(sdev, src, front_len, info->data, front_len*SECTOR_SIZE))
        {
            /* ������������ */
            SWAP_ERR("read (%llu, %d) failed\n", (unsigned long long)src, front_len);
            //goto error;
        } 
        //memset(info->data, 0, front_len*SECTOR_SIZE);
    }

    if (back_len > 0)
    {
        if(0 != hd_read_sector_no_retry(sdev, src + need_zero_offset + need_zero_len, back_len, 
        info->data + (need_zero_offset + need_zero_len)*SECTOR_SIZE, back_len*SECTOR_SIZE))
        {
            /* ������������ */
            SWAP_ERR("read (%llu, %d) failed\n", 
					(unsigned long long)src + need_zero_offset + need_zero_len, back_len);
            //goto error;
        }
        //memset(info->data + (need_zero_offset + need_zero_len)*SECTOR_SIZE, 0, back_len*SECTOR_SIZE);
    }

    info->table.src_sec = src;
    info->table.sec_size = SECTOR_NUM_PER_SWAP_BLOCK;
    info->table.index = index;
    info->table.swap_sec = core->sector_data + SECTOR_NUM_PER_SWAP_BLOCK * info->table.index;
    info->table.checksum = swap_crc32(~0, &info->table, sizeof(struct swap_table) - sizeof(u32) - sizeof(sector_t));

    return 0;

}

/*****************************************************************************
 �� �� ��  : _swap_read
 ��������  : ӳ���Ķ�����
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ� -1 ʧ��
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static int _swap_read(struct scsi_swap_core *core, swap_info_t *info, sector_t sector_start, int sector_count, char *buf, int buf_size)
{
    sector_t sector_src;
    int data_offset;

    if (NULL == info)
    {
        SWAP_DEBUG("info == NULL\n");
        return -1;
    }

    sector_src = SWAP_SECTOR_ALIGN(sector_start);
    data_offset = (sector_start-sector_src)*SECTOR_SIZE;
    buf_size = min((int)sector_count*SECTOR_SIZE, buf_size);

    memcpy(buf, info->data + data_offset, buf_size);

    //buf_show("_swap_read", info->data, buf_size);

    return 0;
}


/*****************************************************************************
 �� �� ��  : _swap_write
 ��������  : ӳ����д����
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ� -1 ʧ��
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static int _swap_write(struct scsi_swap_core *core, swap_info_t *info, sector_t sector_start, int sector_count, const char *buf, int buf_size)
{
    u32 sector_src;
    int data_offset;

    if(NULL == info)
    {
        SWAP_ERR("\n");
        return -1;
    }

    sector_src = SWAP_SECTOR_ALIGN(sector_start);
    data_offset = (sector_start-sector_src)*SECTOR_SIZE;
    buf_size = min((int)sector_count*SECTOR_SIZE, buf_size);

    // ���½�����BUF
    memcpy(info->data + data_offset, buf, buf_size);

    //buf_show("after _swap_write", info->data, 65536);

    // ����ӳ���

    if (0 != flush_swap_info_data(core, info))
    {
        SWAP_ERR("flush_swap_info_data fail\n");
        return -1;
    }

    return 0;
}

/*****************************************************************************
 �� �� ��  : swap_create
 ��������  : ����һ���µ�ӳ��
 �������  : 
 �������  : 
 �� �� ֵ  : �ɹ�����swap_info_tָ��  ʧ�ܷ���NULL
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static swap_info_t *swap_create(struct scsi_swap_core *core, sector_t sector_start, u32 sector_count)
{
    sector_t sector_bswap = 0;
    swap_info_t *info = NULL;
    int ret = 0;
	int fail_reason = -1;
	struct scsi_swap_log *log;

    /* ���֧��MAX_SWAP_BLOCK_FOR_USE(128)��ӳ�� */
    if (atomic_read(&core->info_num) >= MAX_SWAP_BLOCK_FOR_USE)
    {
        SWAP_ERR("no more reserved block for swap\n");
		fail_reason = LOG_FAILED_CREATE_MAXCOUNT;
		goto out;
    }

    sector_bswap = SWAP_SECTOR_ALIGN(sector_start);

    info = _swap_alloc_info();
    if (NULL == info)
    {
        SWAP_ERR("error: no memory to alloc info\n");
		fail_reason = LOG_FAILED_CREATE_MALLOC;
		goto out;
    }

    ret = _swap_create(core, info, sector_bswap, sector_start-sector_bswap, sector_count);
    if(0 != ret)
    {
        SWAP_ERR("_swap_create fail\n");
        _swap_dealloc_info(info);
		info = NULL;
		fail_reason = LOG_FAILED_CREATE_WRITE_DST;
		goto out;
    }

    ret = flush_swap_head(core);
    if (0 != ret)
    {
        SWAP_ERR("flush_swap_head fail\n");
        swap_bitmap_set_bit((unsigned long *)core->head.bitmap, (int)info->table.index, 0);
        _swap_dealloc_info(info);
		info = NULL;
		fail_reason = LOG_FAILED_CREATE_FLUSH_HEAD; 
		goto out;
    }

out:
	log  = &(core_to_swap_handler(core)->log);

	scsi_swap_log_push(log, LOG_TYPE_CREATE, LOG_RESULT(fail_reason), 
			info == NULL ? sector_start : info->table.src_sec, 
			info == NULL ? -1 : info->table.swap_sec, 
			info == NULL ? sector_count : info->table.sec_size);

    return info;
}

/*****************************************************************************
 �� �� ��  : swap_recreate
 ��������  : ӳ����𻵺󣬴�����ӳ�亯��
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ� -1 ʧ��
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static int swap_recreate(struct scsi_swap_core *core, swap_info_t *info)
{
    int i = 0;
    int index = 0;
	struct scsi_device *sdev = core_to_scsi_device(core);
    
    /* ���֧��MAX_SWAP_BLOCK_FOR_USE(128)��ӳ�� */
    if (atomic_read(&core->info_num) >= MAX_SWAP_BLOCK_FOR_USE)
    {
        SWAP_ERR("no more reserved blocks for swap\n");
        return -1;
    }

    spin_lock(&core->bitmap_lock);
    index = _swap_alloc_new_block(core);
    spin_unlock(&core->bitmap_lock);

    if(-1 == index)
    {
        SWAP_ERR("no reserved space left\n");
        return -1;
    }

    /* Ϊ�˾��������������������������ж�ȡ */
    for (i = 0; i < SECTOR_NUM_PER_SWAP_BLOCK; i++)
    {
        if(0 != hd_read_sector_no_retry(sdev, info->table.swap_sec + i, 1, 
                    info->data + i * SECTOR_SIZE, SECTOR_SIZE))
        {
            /* �������������� */
            //SWAP_ERR("read (%llu, %d) failed\n", info->table.swap_sec + i, SECTOR_SIZE);
            memset(info->data + i*SECTOR_SIZE, 0, SECTOR_SIZE);
        }
    }    

    /* ����ӳ��������Ϣ */
    info->table.index = index;
    info->table.swap_sec = core->sector_data + SECTOR_NUM_PER_SWAP_BLOCK * info->table.index;
    info->table.checksum = swap_crc32(~0, &info->table, sizeof(struct swap_table) - sizeof(u32) - sizeof(sector_t));

    // ����Ŀ������
    if (0 != flush_swap_info_data(core, info))
    {
        SWAP_ERR("flush_swap_info_data fail\n");
        goto err;
    }

    if (0 != flush_swap_info_table(core))
    {
        SWAP_ERR("flush_swap_info_table fail\n");
        goto err;
    }

    if (0 != flush_swap_head(core))
    {
        SWAP_ERR("flush_swap_head fail\n");
        goto err;
    }

    return 0;
err:
    // ����ͷ
    swap_bitmap_set_bit((unsigned long *)core->head.bitmap, index, 0);
    return -1;
    
}

/*****************************************************************************
 �� �� ��  : swap_write
 ��������  : ӳ����д���������дʧ�ܣ��������ӳ��
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ� -1 ʧ��
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static int swap_write(struct scsi_swap_core *core, swap_info_t *info, sector_t sector_start, int sector_count, const char *buf, int buf_size)
{
    if (0 == _swap_write(core, info, sector_start, sector_count, buf, buf_size))
    {
        return 0;
    }
    
    SWAP_ERR("_swap_write fail, realloc swap sector\n");
    if (0 != swap_recreate(core, info))
    {
        SWAP_ERR("swap_recreate fail\n");
        return -1;
    }
    
    if (0 != _swap_write(core, info, sector_start, sector_count, buf, buf_size))
    {
        SWAP_ERR("_swap_write fail\n");
        return -1;
    }
    return -DATA_MAY_DIRTY;
}

// ����ӳ��ͷ��У��ֵ
static inline u32 scsi_swap_check(struct swap_head *head)
{
    return swap_crc32(~0, head, SECTOR_SIZE - sizeof(u32));
}

// ӳ��ͷ��Ч
static inline int swap_head_valid (struct swap_head *head)
{
    return strncmp((char *)head->status, "valid", 5) == 0;
}

// ӳ��ͷ��Ч�����ڵ�һ�������Ǻõģ����м��л�������block
static inline int swap_head_invalid (struct swap_head *head)
{
    return strncmp((char *)head->status, "invalid", 7) == 0;
}

// ӳ��ͷ�Ѵ���
static inline int swap_head_exist (struct swap_head *head)
{
    return strncmp((char *)head->head_string, SWAP_HEAD_STRING, 6) == 0;
}

/*****************************************************************************
 �� �� ��  : swap_get_head_state
 ��������  : ��ȡӳ��ͷ״̬
 �������  : 
 �������  : 
 �� �� ֵ  : HEAD_OK �ɹ� ���� ʧ��
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static swap_head_state_e swap_get_head_state(struct scsi_device *scsidev, sector_t sector, 
    u32 sec_num, struct swap_head *head)
{
    /* ��ȡ */
    if (0 != hd_read_sector_retry(scsidev, sector, sec_num, (char *)head, SECTOR_SIZE)) 
    {
        /* ������ */
        return HEAD_FAIL;
    }
    
    /* ��HWSWAP��ʶ��ûд������������ */
    if (!swap_head_exist(head))
    {
        return HEAD_EMPTY;
    }

    if (swap_head_invalid(head))  /* block���л����� */
    {
        return HEAD_INVALID;
    }
    else if (swap_head_valid(head))
    {
        if (scsi_swap_check(head) != head->checksum)  /* У�鲻������ */
        {
            return HEAD_BROKEN;
        }
        else
        {
            return HEAD_OK;
        }
    }
    else
    {
        /* ���� */
        return HEAD_BROKEN;
    }
}

#if 0
/*****************************************************************************
 �� �� ��  : swap_repair_one_sector
 ��������  : �޸�һ������
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ� -1 ʧ��
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static int swap_repair_one_sector(struct scsi_device *device, sector_t sector)
{
    char buf[SECTOR_SIZE] = {0};
    struct scsi_swap *swap = NULL;
    
    memset (buf, 0, SECTOR_SIZE);

    /* ��д0�޸� */
    if (0 == hd_write_sector_retry(device, sector, 1, buf, SECTOR_SIZE))
    {
        //SWAP_ERR("sector %llu count %u repair success by write 0\n", sector, 1);
        return 0;
    }

    /* д0�޸�ʧ�ܣ�������ӳ�� */
    if (0 == hd_reassign_successive_sectors(device, sector, 1))
    {
        //SWAP_ERR("sector %llu count %u repair success by reassign\n", sector, 1);
        return 0;
    }

    if (0 != hd_test_unit_ready(device))
    {
        //SWAP_ERR("hd_test_unit_ready fail, device may dead, need reset\n");
        swap = scsi_device_to_scsi_swap(device);
        if(NULL != swap)
        {
            atomic_inc(&core->device_dead);
        }
    }
    
    return -1;
}
#endif

/*****************************************************************************
 �� �� ��  : swap_repair_successive_sectors
 ��������  : �޸������Ķ������
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ� -1 ʧ��
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static int swap_repair_successive_sectors(struct scsi_device *device, sector_t sector, int count)
{
    struct scsi_swap_core *core = swap_to_swap_core(&device->swap);
    
    /* ���Ӳ���Ѿ����ڴ���״̬���������޸� */
    if (0 != hd_test_unit_ready(device))
    {
        SWAP_ERR("hd_test_unit_ready fail, device may dead, need reset\n");
        atomic_inc(&core->device_dead);
        return -EIO;
    }
    return -1;   /* �Զ��޸����������⣬�Ȳ����룬ֱ�ӷ���ʧ�� */

#if 0
    int i = 0;
    int ret = 0;

    /* ��������޸� */    
    for (i = 0; i < count; i++)
    {
        /* ֻҪ��һ�������޸�ʧ�ܣ��ͷ���ʧ�� */
        ret = swap_repair_one_sector(device, sector + i);
        if (0 != ret)
        {
            return -1;
        }
    }

    return 0;
#endif
}

/*****************************************************************************
 �� �� ��  : init_swap_head
 ��������  : ��ʼ��ӳ��ͷ
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ� -1 ʧ��
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static int init_swap_head(struct scsi_swap_core *core, sector_t start, u32 num)
{
	struct scsi_device *device = core_to_scsi_device(core);
    struct swap_head *head = &core->head;
    char head_back[SECTOR_SIZE] = {0};
    sector_t sec_start = 0;
    swap_head_state_e head_state = 0;
    swap_head_state_e head_back_state = 0;
    int i = 0;
    int ret = 0;

    for (i = 0; i < MAX_SWAP_HEAD_BLOCK_NUM; i++)
    {
        /* ֻ����ÿ��block�ĵ�һ������ */
        sec_start = start + i*SECTOR_NUM_PER_SWAP_BLOCK;

        /* ��ȡ������״̬ */
        head_state = swap_get_head_state(device, sec_start, 1, head);
        
        /* block���л����� */
        if (HEAD_INVALID == head_state)
        {
            continue;
        }

        /* ��ȡ��������״̬ */
        head_back_state = swap_get_head_state(device, sec_start + SWAP_HEAD_BACKUP_OFFEST, 1, (struct swap_head *)head_back);
        if ((HEAD_FAIL == head_state) && (HEAD_FAIL == head_back_state))
        {
            continue;
        }        
        /* �������״̬������Ӧ���� */
        /* ������OKʱ����Ҫ�鿴���������Ƿ�OK */
        if (HEAD_OK == head_state)
        {
            SWAP_ERR("master head ok\n");
            if (HEAD_OK != head_back_state)
            {
                SWAP_ERR("back head fail\n");
                /* ���� */
                ret = hd_write_sector_retry(device, sec_start + SWAP_HEAD_BACKUP_OFFEST, 1, head, SECTOR_SIZE);
                if (0 != ret)
                {
                    SWAP_ERR("write to swap back head failed\n");
                }
            }
            /* ��������ͷ��λ�� */
            core->sector_head = sec_start;
            
            return 0;
        }
        else if (HEAD_OK == head_back_state) /* ���𻵣�����OKʱ���ñ��ݣ�ͬʱ�޸������� */
        {
            SWAP_ERR("master head fail\n");
            SWAP_ERR("back head ok\n");
            memcpy(head, head_back, SECTOR_SIZE);
            /* �޸� */
            ret = hd_write_sector_retry(device, sec_start, 1, head, SECTOR_SIZE);
            if (0 != ret)
            {
                SWAP_ERR("repair swap head failed\n");
            }
            /* ��������ͷ��λ�� */
            core->sector_head = sec_start;
            
            return 0;
        }
        else  /* ���������ڿջ�����״̬ */
        {
            SWAP_ERR("master and back head all fail\n");
            if(-1 == swap_create_new_head(core))
            {
                SWAP_ERR("swap_create_new_head failed\n");
                atomic_inc(&core->device_dead);
                return -1;
            }
            
            SWAP_ERR("creat new head(%llu) success\n", (unsigned long long)sec_start);
            return 0;
        }

    }

    /* �ĸ������������⣬���Ϊdead device�����ش��� */
    atomic_inc(&core->device_dead);
    return -1;
}


/*****************************************************************************
 �� �� ��  : alloc_swap_info_data
 ��������  : ����ӳ������ݿռ�, ����ȡ�����鱸��
 �������  : 
 �������  : 
 �� �� ֵ  : �ɹ�����ָ�����ݵ�ָ��  ʧ�ܷ���NULL
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static char *alloc_swap_info_data(struct scsi_device *device, sector_t start, u32 num)
{
    u32 nbytes = num * SECTOR_SIZE;
    char *data = kmalloc(nbytes, GFP_KERNEL);

    if (NULL == data) 
    {
        SWAP_ERR("nbytes %u\n", nbytes);
        return NULL;
    }

    memset (data, 0, nbytes);

    //SWAP_ERR("\n");
    if (0 != hd_read_sector_retry(device, start, num, data, nbytes))
    {
        SWAP_ERR("nbytes %u\n", nbytes);
        kfree(data);
        return NULL;
    }

    // for test only
    /* {
        char buf[32]; 
        memcpy(buf, data, 31);
        buf[31] = 0;
        printk("-->read data:%s\n", buf);
    } */

    return data;
}

/*****************************************************************************
 �� �� ��  : calc_swap_info_num
 ��������  : ��ȡ������ӳ�����
 �������  : 
 �������  : 
 �� �� ֵ  : �����ܵ�ӳ�����
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static int calc_swap_info_num(struct scsi_device *device, sector_t start, u32 num)
{
    char buffer[SECTOR_SIZE] = {0};
    struct swap_table *table = NULL;
    int swap_table_len = 0;
    int i = 0;
    int j = 0;
    int table_num_per_sect = 0;
    int total_table_num = 0;

    swap_table_len = sizeof (struct swap_table);

    /* ÿ���������Էŵ�swap_table���� */
    table_num_per_sect = SECTOR_SIZE / swap_table_len;    


    /* ��ȡÿ������, ������ÿ��swap_table, ֱ����ȡ����Ч��ֵ */
    for (i = 0; i < num; ++i)
    {
        memset(buffer, 0, SECTOR_SIZE);
        if (0 != hd_read_sector_retry(device, start++, 1, buffer, SECTOR_SIZE)) 
        {
            continue;
        }

        for (j = 0; j < table_num_per_sect; ++j)
        {
            table = (struct swap_table *)(buffer + swap_table_len * j);

            /* �����ڶ����ֵ����Ч */
            if (table->sec_size != SECTOR_NUM_PER_SWAP_BLOCK)
            {
                return total_table_num;
            }

            if (swap_crc32(~0, table, swap_table_len - sizeof(u32) - sizeof(sector_t)) != table->checksum)
            {
                return total_table_num;
            }

            /* �����ܵĽ��������� */
            total_table_num++;
        }
    }
    return total_table_num;
}

/*****************************************************************************
 �� �� ��  : init_swap_info
 ��������  : ��ʼ��ӳ�����Ϣ: �ú���ʧ���Ժ󣬶�֮ǰ�������Դ������
             �ں������غ�ͳһ����
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ� -1 ʧ��
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
static int init_swap_info(struct scsi_swap_core *core, u32 num)
{
    struct swap_info *info = NULL;
    sector_t start = 0;
    sector_t start_master = 0;
    sector_t start_back = 0;
    char buffer[SECTOR_SIZE] = {0};
    int table_num_per_sect = 0;
    u32 crc_val = 0;
    int swap_table_len = 0;
    int table_num = 0;
    int table_num_back = 0;
    swap_table_sync_e sync = NO_NEED_SYNC;
    int i, j;
    int end_flag = 0;
	struct scsi_device *device = core_to_scsi_device(core);
    
    start_master = core->sector_table;
    start_back = core->sector_reserve_start + SWAP_TABLE_BACKUP_OFFSET;

    swap_table_len = sizeof (struct swap_table);

    /* ÿ���������Էŵ�swap_table���� */
    table_num_per_sect = SECTOR_SIZE / swap_table_len;    

    INIT_LIST_HEAD(&core->info_list);

    atomic_set(&core->info_num, 0);

    /* �ȷֱ�������ͱ���table���� */
    table_num = calc_swap_info_num(device, start_master, SWPA_TABLE_N_SECTOR);
    table_num_back = calc_swap_info_num(device, start_back, SWPA_TABLE_N_SECTOR);

    SWAP_ERR("table num: %d %d\n", table_num, table_num_back);
    
    /* ���������������������table�࣬�ӱ��ݶ�ȡ */
    if (table_num_back > table_num)
    {
        SWAP_ERR("use backup table,table num: %d %d\n", table_num, table_num_back);
        start = start_back;
        sync = MASTER_NEED_SYNC;
    }
    else if (table_num_back < table_num)
    {
        SWAP_ERR("use master table,table num: %d %d\n", table_num, table_num_back);
        start = start_master;
        sync = BACKUP_NEED_SYNC;
    }
    else
    {
        if (0 == table_num)
        {
            return 0;
        }
        start = start_master;
        sync = NO_NEED_SYNC;
    }

    /* ��ȡÿ������, ������ÿ��swap_table, ֱ����ȡ����Ч��ֵ */
    for (i = 0; i < SWPA_TABLE_N_SECTOR; ++i)
    {
        memset(buffer, 0, SECTOR_SIZE);
        if (0 != hd_read_sector_retry(device, start++, 1, buffer, SECTOR_SIZE)) 
        {
            SWAP_ERR("read swap table fail\n");
            //return -1;
            continue;
        }
        
        /* ����table�� */
        for (j = 0; j < table_num_per_sect; ++j)
        {
            info = (struct swap_info *)kmalloc(sizeof (struct swap_info), GFP_KERNEL);
            if (info == NULL)
            {
                SWAP_ERR("kmalloc info fail\n");
                return -1;
            }

            memset(info, 0, sizeof (struct swap_info));
            
            memcpy(&info->table, buffer + swap_table_len * j, swap_table_len);

            /* �����ڶ����ֵ����Ч */
            if (info->table.sec_size != SECTOR_NUM_PER_SWAP_BLOCK)
            {
                SWAP_ERR("all done, sector %d, index %d, size: %d\n", i, j, info->table.sec_size);
                kfree(info);
                end_flag = 1;
                break;
            }
            
            /* �����������С�ڲ��ڱ���������Χ�������𻵵�Դ�������ڱ������� */
            if ((info->table.swap_sec < core->sector_data) || (info->table.src_sec > core->sector_reserve_start))
            {
                SWAP_ERR("source sector or dest sector error\n");
                kfree(info);
                return -1;
            }
            
            crc_val = swap_crc32(~0, &info->table, swap_table_len - sizeof(u32) - sizeof(sector_t));

            if (crc_val != info->table.checksum)
            {
                SWAP_ERR("crc check fail\n");
                kfree(info);
                return -1;
            }
            
            info->data = alloc_swap_info_data(device, info->table.swap_sec, info->table.sec_size);

            if (NULL == info->data)
            {
                SWAP_ERR("realloc swap sector\n");
                if (0 == swap_recreate(core, info))
                {
                    memcpy(buffer + swap_table_len * j, &info->table, swap_table_len);
                }
                else
                {
                    SWAP_ERR("swap_recreate fail\n");
                    kfree(info);
                    return -1;
                }
            }
            
            // ��ʼ���׶�
            spin_lock(&core->info_list_lock);
            list_add_tail(&info->list, &core->info_list);
            spin_unlock(&core->info_list_lock);
            /* �����ܵĽ��������� */
            atomic_inc(&core->info_num);
        }

        /* ��������ͬ�� */
        if (MASTER_NEED_SYNC == sync)
        {
            while (0 != hd_write_sector_retry(device, start_master++, 1, buffer, SECTOR_SIZE)) 
            {
                SWAP_ERR("sync to swap master table failed\n");
                if (start_master >= (core->sector_table + SWPA_TABLE_N_SECTOR))
                {
                    break;
                }
            }
        }
        else if (BACKUP_NEED_SYNC == sync)
        {
            while (0 != hd_write_sector_retry(device, start_back++, 1, buffer, SECTOR_SIZE)) 
            {
                SWAP_ERR("sync to swap backup table failed\n");
                if (start_back >= (core->sector_reserve_start + SWAP_TABLE_BACKUP_OFFSET + SWPA_TABLE_N_SECTOR))
                {
                    break;
                }
            }
        }
        
        if (1 == end_flag)
        {
            break;
        }
        
    }

    SWAP_ERR("total info num: %d\n", atomic_read(&core->info_num));
    
    return 0;
}

static int swap_info_destroy(struct scsi_swap_core *core)
{
    struct swap_info *entry;
    struct swap_info *next;

    spin_lock(&core->info_list_lock);

    list_for_each_entry_safe(entry, next, &core->info_list, list) 
    {
        if (NULL != entry)
        {
            list_del(&entry->list);
            if (NULL != entry->data)
            {
                kfree(entry->data);
                entry->data = NULL;
            }
            kfree(entry);
            entry = NULL;
        }
    }

    list_del_init(&core->info_list);
    spin_unlock(&core->info_list_lock);

    return 0;
}

int scsi_swap_core_init(struct scsi_swap_core *core, sector_t reserve_sector)
{
	struct gendisk *disk;

	disk = core_to_swap_handler(core)->swap->disk;

    core->sector_reserve_start = reserve_sector;
    core->sector_head = reserve_sector + SWAP_HEAD_OFFEST;
    core->sector_data = reserve_sector + SWAP_DATA_OFFSET;
    core->sector_table = reserve_sector + SWAP_TABLE_OFFSET;
        
	SWAP_DEBUG("%s, reserve_sector %llu, sector_head %llu, "
			"sector_data %llu, sector_table %llu\n", 
			disk->disk_name, 
			(unsigned long long)reserve_sector, 
			(unsigned long long)core->sector_head, 
			(unsigned long long)core->sector_data, 
			(unsigned long long)core->sector_table);

    spin_lock_init(&core->info_list_lock);

    if (0 != init_swap_head(core, core->sector_head, SWAP_HEAD_N_SECTOR))
    {
        SWAP_ERR("[%s]init head failed\n", disk->disk_name);
		return -1;
    }

    if (0 != init_swap_info(core, SWPA_TABLE_N_SECTOR)) 
    {
        SWAP_ERR("[%s]init info failed\n", disk->disk_name);
		swap_info_destroy(core);
		return -1;
    }

    atomic_set(&core->device_dead, 0);
    atomic_set(&core->user, 0);

    return 0;
}

int scsi_swap_core_destroy(struct scsi_swap_core *core)
{
    int users = 0;
    int i = 0;
    struct scsi_device *sdev = core_to_scsi_device(core);
    
    atomic_inc(&core->device_dead);
    users = atomic_read(&core->user);
    if (0 != users)
    {
        SWAP_ERR("%d IOs hasn't completed\n", users);
        hd_sync_cache(sdev);
    }

    /* �ȴ���д��ɣ����30s���� */
    while ((i++ < 100) && (0 != users))
    {
        //SWAP_ERR("waiting %d IOs to be completed\n", users);
        msleep(300);
        users = atomic_read(&core->user);
    }
    SWAP_ERR("%s\n", (i>=100)?"waiting r/w timeout":"r/w completed\n");

	swap_info_destroy(core);

    return 0;
}

int scsi_swap_core_swapped(struct scsi_swap_core *core, sector_t sector, u32 num)
{
    return _did_block_swapped(core, sector, num);
}

int scsi_swap_core_can_swap(struct scsi_swap_core *core, sector_t sector, u32 num)
{
    return atomic_read(&core->device_dead) == 0
		&& sector < core->sector_reserve_start;
}

/*****************************************************************************
 �� �� ��  : core_repair_sectors
 ��������  : �޸��������������
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ� -1 ʧ��
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
//int core_repair_sectors(struct scsi_device *device, sector_t start, u32 count, sector_t bad)
//{
//    char * buf = NULL;
//	struct scsi_swap *swap = &device->swap;
//
//	if (!core->enable)
//		return -1;
//
//    /* ���Ӳ���Ѿ����ڴ���״̬���������޸� */
//    if (0 != hd_test_unit_ready(device))
//    {
//        SWAP_ERR("hd_test_unit_ready fail, device may dead, need reset\n");
//        atomic_inc(&core->device_dead);
//        return -EIO;
//    }
//    
//    buf = kmalloc (count * SECTOR_SIZE, GFP_KERNEL);
//    if (NULL == buf)
//    {
//        SWAP_ERR("kmalloc fail\n");
//        return -1;
//    }
//    
//    memset (buf, 0, count * SECTOR_SIZE);
//
//    /* ��д0�޸� */
//    if (0 == hd_write_sector_no_retry(device, start, count, buf, SECTOR_SIZE * count))
//    {
//        SWAP_ERR("sector %llu count %u has been repaired success by write 0\n", start, count);
//        kfree (buf);
//        return 0;
//    }
//    kfree(buf);
//
//    /* д0�޸�ʧ�ܣ�������ӳ�� */
//    if (0 == hd_reassign_successive_sectors(device, bad, 1))
//    {
//        printk("sector %llu count %u has been repaired success by reassign\n", bad, 1);
//        return 0;
//    }
//
//    /* ��ЩӲ���ڷ��ʳ���󣬻�������������Ӧ */
//    if (0 != hd_test_unit_ready(device))
//    {
//        SWAP_ERR("hd_test_unit_ready fail after repair, device may dead, need reset\n");
//        atomic_inc(&core->device_dead);
//        return -EIO;
//    }
//    
//    return -1;
//}

/*****************************************************************************
 �� �� ��  : core_read
 ��������  : ӳ��������
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ�, -1 ʧ��, -DATA_MAY_DIRTY ��ʾ�ɹ��������ݿ��ܲ�����
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
int scsi_swap_core_read(struct scsi_swap_core *core, sector_t start, u32 count, sector_t bad, void *buf, u32 buf_size)
{
    int i;
    int b_count = get_swap_block_count(start, count);
    int i_start = SWAP_BLOCK_INDEX(start);
    int i_bad = ((bad == -1) ? -1 : SWAP_BLOCK_INDEX(bad));
    u32 b_start;
    sector_t s_start;
    int s_count;
    int s_len;
    swap_info_t *info;
    int data_dirty = 0;
    int ret = 0;
    struct scsi_device *device = core_to_scsi_device(core);

    if (0 != atomic_read(&core->device_dead))
    {
        /* �豸�Ѿ������ã����� */
        SWAP_ERR("device may dead\n");
        return -1;
    }

    SWAP_DEBUG("start %llu, count %u ,bad %llu\n", 
			(unsigned long long)start, count,
			(unsigned long long)bad);

    atomic_inc(&core->user);

    for(i=0; i<b_count; ++i)
    {
        b_start = SWAP_BLOCK_SECTOR(i_start+i);
        s_start = i == 0 ? start : b_start;
        s_count = min(count, (u32)(SWAP_BLOCK_SECTOR(i_start+i+1)-s_start));
        s_len = s_count * SECTOR_SIZE;
        //SWAP_DEBUG("b_start %u, s_start %llu, s_count %d\n", b_start, s_start, s_count);
                
        info = swap_find_swap_info(core, b_start);
        if(NULL != info)
        {
            // �ҵ�, ֱ�Ӵ��ڴ��.
            _swap_read(core, info, s_start, s_count, buf, s_len);
            //SWAP_ERR("read from swap memory, sector %llu, count %u\n", s_start, s_count);
        }
        else
        {
            if(i_bad != i_start+i)
            {
                // ���ǻ���, ֱ�Ӷ�����
                if(0 == hd_read_sector_retry(device, s_start, s_count, (void*)buf, s_len))
                {
                    count -= s_count;
                    buf += s_len;
                    buf_size -= s_len;
                    continue;
                }
                else if (0 != atomic_read(&core->device_dead))
                {
                    /* �豸�Ѿ������ã����� */
                    goto err;
                }
                else
                {
                    /* �����ʧ�ܣ�˵�����л��飬������������Ļ���ӳ�� */
                    SWAP_ERR("hd_read_sector_retry failed\n");
                }
            }

            /* ������������޸� */
            ret = swap_repair_successive_sectors(device, s_start, s_count);
            if (0 == ret)
            {
                /* �޸��ɹ����������ݣ�ȫ0 */
                memset(buf, 0, (size_t)s_len);
                count -= s_count;
                buf += s_len;
                buf_size -= s_len;
                SWAP_ERR("sector %llu, %d repair success\n", 
						(unsigned long long)s_start, s_count);
                continue;
            }
            else
            {
                SWAP_ERR("sector %llu, %d repair fail\n", 
						(unsigned long long)s_start, s_count);
                if (0 != atomic_read(&core->device_dead))
                {
                    /* �豸�Ѿ������ã����� */
                    goto err;
                }
            }
            
            
            // ����,  ���߶�ʧ��, ��Ҫ����һ��������
            info = swap_create(core, s_start, s_count);
            if(NULL == info)
            {
                SWAP_ERR("create swap %llu, %d failed\n", 
						(unsigned long long)s_start, s_count);
                goto err;
            }

            memcpy(buf, (void *)(info->data + (u32)(s_start - b_start)* SECTOR_SIZE), 
					(size_t)min((u32)s_len, (u32)SWAP_BLOCK_SIZE));

            // ����Ŀ������
            if (0 != flush_swap_info_data(core, info))
            {
                swap_bitmap_set_bit((unsigned long *)core->head.bitmap, (int)info->table.index, 0);
                _swap_dealloc_info(info);
                goto err;
            }

            // ���뵽����
            spin_lock(&core->info_list_lock);
            list_add_tail(&info->list, &core->info_list);
            spin_unlock(&core->info_list_lock);

            // ����table
            if (0 != flush_swap_info_table(core))
            {
                swap_bitmap_set_bit((unsigned long *)core->head.bitmap, (int)info->table.index, 0);
                _swap_dealloc_info(info);
                goto err;
            }

            /* �����ܵĽ��������� */
            atomic_inc(&core->info_num);

            /* ��Ϊ�����󴴽���ӳ��ʱ����Ҫ���� */
            data_dirty = 1;
            
        }


        //buf_show("after read a swap", buf, buf_size);

        count -= s_count;
        buf += s_len;
        buf_size -= s_len;
    }

    atomic_dec(&core->user);

    if (1 == data_dirty)
    {
        return -DATA_MAY_DIRTY;
    }

    return 0;
    
err:
    atomic_dec(&core->user);
    return -1;
}

/*****************************************************************************
 �� �� ��  : core_write
 ��������  : ӳ���д����
 �������  : 
 �������  : 
 �� �� ֵ  : 0 �ɹ�, -1 ʧ��, -DATA_MAY_DIRTY ��ʾ�ɹ��������ݿ��ܲ�����
 ���ú���  : 
 ��������  : 
 
 �޸���ʷ      :
  1.��    ��   : 2012��10��25��
    ��    ��   : mincore@163.com
    �޸�����   : �����ɺ���

*****************************************************************************/
int scsi_swap_core_write(struct scsi_swap_core *core, sector_t start, u32 count, sector_t bad, const void *buf, u32 buf_size)
{
    int i;
    int b_count = get_swap_block_count(start, count);
    int i_start = SWAP_BLOCK_INDEX(start);
    int i_bad = bad == -1 ? -1 : SWAP_BLOCK_INDEX(bad);
    u32 b_start;
    sector_t s_start;
    int s_count;
    int s_len;
    swap_info_t *info;
    int data_dirty = 0;
    int ret = 0;
    struct scsi_device *device = core_to_scsi_device(core);
    
    if (0 != atomic_read(&core->device_dead))
    {
        /* �豸�Ѿ������ã����� */
        SWAP_ERR("device may dead\n");
        return -1;
    }

    atomic_inc(&core->user);

    for(i=0; i<b_count; ++i)
    {
        b_start = SWAP_BLOCK_SECTOR(i_start+i);
        s_start = ((i == 0) ? start : b_start);
        s_count = min(count, (u32)(SWAP_BLOCK_SECTOR(i_start+i+1)-s_start));
        s_len = s_count * SECTOR_SIZE;
        
        info = swap_find_swap_info(core, b_start);
        if(NULL != info)
        {
            // �ҵ�, ֱ��д�ڴ�, Ȼ����µ�����
            ret = swap_write(core, info, s_start, s_count, buf, s_len);
            if (0 != ret)
            {
                if (-DATA_MAY_DIRTY == ret)  /* ��ӳ��ɹ��������ݿ�����Ҫ���� */
                {
                    /* ����д����������һ��blockʱ����Ҫ���� */
                    if (s_count < SECTOR_NUM_PER_SWAP_BLOCK)
                    {
                        data_dirty = 1;
                    }
                }
                else
                {
                    SWAP_ERR("swap_write fail\n");
                    goto err;
                }
            }
            //SWAP_ERR("write to swap memory, sector %llu, count %u\n", s_start, s_count);
            
        }
        else
        {
            /* ���ǻ���, ֱ��д���� */
            if(i_bad != i_start+i)
            {
                /* д�ɹ������ؼ���������������  */
                if(0 == hd_write_sector_retry(device, s_start, s_count, (void*)buf, s_len))
                {
                    count -= s_count;
                    buf += s_len;
                    buf_size -= s_len;
                    continue;
                }
                else if (0 != atomic_read(&core->device_dead))
                {
                    /* �豸�Ѿ������ã����� */
                    goto err;
                }
                else
                {
                    /* ���дʧ�ܣ�˵�����л��飬������������Ļ���ӳ�� */
                    SWAP_ERR("hd_write_sector failed\n");
                }
            }

            /* ������������޸� */
            ret = swap_repair_successive_sectors(device, s_start, s_count);
            if (0 == ret)
            {
                /* �޸��ɹ����������� */
                if(0 == hd_write_sector_retry(device, s_start, s_count, (void*)buf, s_len))
                {
                    count -= s_count;
                    buf += s_len;
                    buf_size -= s_len;
                    continue;
                }
            }
            else
            {
                if (0 != atomic_read(&core->device_dead))
                {
                    /* �豸�Ѿ������ã����� */
                    goto err;
                }
            }

            // ����, ����дʧ��, ��Ҫ����һ��������
            info = swap_create(core, s_start, s_count);
            if(NULL == info)
            {
                SWAP_ERR("create swap %llu, %u failed\n", 
						(unsigned long long)s_start, s_count);
                goto err;
            }
            
            // �����ڴ�, Ȼ����µ�����
            if (0 != swap_write(core, info, s_start, s_count, buf, s_len))
            {
                swap_bitmap_set_bit((unsigned long *)core->head.bitmap, (int)info->table.index, 0);
                _swap_dealloc_info(info);
                goto err;
            }
            //SWAP_ERR("find a bad sector %llu, %u, created a swap.\n", s_start, s_count);

            // ���뵽����
            spin_lock(&core->info_list_lock);
            list_add_tail(&info->list, &core->info_list);
            spin_unlock(&core->info_list_lock);

            // ����table
            if (0 != flush_swap_info_table(core))
            {
                swap_bitmap_set_bit((unsigned long *)core->head.bitmap, (int)info->table.index, 0);
                _swap_dealloc_info(info);
                goto err;
            }

            /* �����ܵĽ��������� */
            atomic_inc(&core->info_num);

            /* ����д����������һ��blockʱ����Ҫ���� */
            if (s_count < SECTOR_NUM_PER_SWAP_BLOCK)
            {
                data_dirty = 1;
            }
            
        }

        //buf_show("after write a swap", buf, buf_size);

        count -= s_count;
        buf += s_len;
        buf_size -= s_len;
    }

    atomic_dec(&core->user);

    if (1 == data_dirty)
    {
        return -DATA_MAY_DIRTY;
    }

    return 0;
    
err:
    atomic_dec(&core->user);
    return -1;
}

int scsi_swap_core_show(struct scsi_swap_core *core, char *page)
{
	struct swap_info *info;
	char buf[64];
	int len;
	int left = PAGE_SIZE;

    spin_lock(&core->info_list_lock);
    list_for_each_entry(info, &core->info_list, list) {
		len = snprintf(buf, sizeof(buf), "%llu %llu %u\n", 
				(unsigned long long)info->table.src_sec, 
				(unsigned long long)info->table.swap_sec, info->table.sec_size);
		if (left <= len)
			break;
		strcpy(page+(PAGE_SIZE-left), buf);
		left -= len;
	}
    spin_unlock(&core->info_list_lock);

	return PAGE_SIZE-left;
}
