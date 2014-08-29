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
#include <linux/types.h>

u32 swap_crc32(u32 crc, const void *ss, int len);

