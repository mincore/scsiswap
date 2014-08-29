/*
 * =================================================================================
 *   (c) Copyright 1992-2013, mincore@163.com
 *                            All Rights Reserved
 *       Filename: cspring_buffer.h
 *    Description: ring buffer for scsi swap logging
 *        Created: 2013年11月18日 14时46分39秒
 *         Author: csp
 *         Modify:  
 * =================================================================================
 */
#ifndef _CSPRING_BUFFER_H
#define _CSPRING_BUFFER_H

#ifdef CSPRING_KERNEL_CODE
	#include <linux/slab.h>
	#define cspring_malloc(size) kmalloc(size, GFP_KERNEL)
	#define cspring_free kfree
#else
	#define cspring_malloc(size) malloc(size)
	#define cspring_free free
#endif

#define cspring_empty(r)	(r->item_num == 0)
#define cspring_full(r)		(r->item_num == r->item_cap)
#define cspring_size(r)		(r->item_cap * r->item_size)
#define cspring_next(r)		(r->item_next)
#define cspring_num(r)		(r->item_num)
#define cspring_buffer(r)	(r->buffer)
#define cspring_cap(r)		(r->item_cap)

#define cspring_get_pop_index(r)	\
	((r->item_next - r->item_num + r->item_cap) % r->item_cap)

#define cspring_get_data(r, index)	\
	((void *)((char*)r->buffer + index * r->item_size))

struct cspring {
	int item_cap;		/* capacity of slots */
	int item_size;		/* size of each item */
	int item_num;		/* number of elements */
	int item_next;		/* next push index */
	void *buffer;		/* cspring buffer */
};

static void cspring_update(struct cspring *r)
{
	if (r->item_num > r->item_cap)	
		r->item_num = r->item_cap;

	if (r->item_next >= r->item_cap)
		r->item_next = 0;
		
	if (r->item_num <= 0)
		r->item_next = 0;
}

static void cspring_destroy(struct cspring *r)
{
	if (r) {
		if (r->buffer) {
			cspring_free(r->buffer);
			r->buffer = NULL;
		}
		cspring_free(r);
	}
}

static struct cspring *cspring_create(int item_size, int item_cap)
{
	struct cspring *r;
	
	r = cspring_malloc(item_size * item_cap);
	if (!r)
		goto err;

	r->item_cap = item_cap;
	r->item_size = item_size;
	r->item_num = 0;
	r->item_next = 0;
	r->buffer = cspring_malloc(cspring_size(r));
	if (!r->buffer)
		goto err;

	return r;

err:
	cspring_destroy(r);
	return NULL;
}

static void *cspring_push(struct cspring *r, void *data)
{
	void *ret;

	ret = cspring_get_data(r, r->item_next);
	memcpy(ret, data, r->item_size);
	r->item_next++;
	r->item_num++;
	cspring_update(r);

	return ret;
}

static void *cspring_peek(struct cspring *r, int index)
{
	int pop_index;
	
	pop_index = cspring_get_pop_index(r);
	index = (pop_index + index) % r->item_cap;
	return cspring_get_data(r, index);
}

#if 0
static void *cspring_pop(struct cspring *r)
{
	int pop_index;

	if (cspring_empty(r))
		return NULL;

	pop_index = cspring_get_pop_index(r);
	r->item_num--;
	cspring_update(r);

	return cspring_get_data(r, pop_index);
}
#endif

#endif
