/*
 * Copyright (c) 2009, Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * Written by Adam Moody <moody20@llnl.gov>.
 * LLNL-CODE-411039.
 * All rights reserved.
 * This file is part of The Scalable Checkpoint / Restart (SCR) library.
 * For details, see https://sourceforge.net/projects/scalablecr/
 * Please also read this file: LICENSE.TXT.
*/

/* All rights reserved. This program and the accompanying materials
 * are made available under the terms of the BSD-3 license which accompanies this
 * distribution in LICENSE.TXT
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the BSD-3  License in
 * LICENSE.TXT for more details.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform,
 * display, or disclose this software are subject to the terms of the BSD-3
 * License as provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 *
 * Author: Christopher Holguin <christopher.a.holguin@intel.com>
 *
 * (C) Copyright 2015-2016 Intel Corporation.
 */

#ifndef SCR_UTIL_H
#define SCR_UTIL_H

#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>

#include "spath.h"
#include "kvtree.h"

/* given a string, convert it to a double and write that value to val */
int scr_atod(const char* str, double* val);

/* converts string like 10mb to unsigned long long integer value of 10*1024*1024 */
int scr_abtoull(const char* str, unsigned long long* val);


/* allocate size bytes, returns NULL if size == 0,
 * calls scr_abort if allocation fails */
#define SCR_MALLOC(X) scr_malloc(X, __FILE__, __LINE__);
void* scr_malloc(size_t size, const char* file, int line);

/* pass address of pointer to be freed, frees memory if not NULL and sets pointer to NULL */
void scr_free(void* ptr);

/* allocates a block of memory and aligns it to specified alignment */
void* scr_align_malloc(size_t size, size_t align);

/* frees a blocked allocated with a call to scr_align_malloc */
void scr_align_free(void* buf);

/*sprintfs a formatted string into an newly allocated string */
char* scr_strdupf(const char* format, ...);

/* returns the current linux timestamp (in microseconds) */
int64_t scr_time_usecs(void);

/* returns the current linux timestamp (secs + usecs since epoch) as a double */
double scr_seconds(void);

/* convenience to read kvtree from an spath */
int kvtree_read_path(const spath* path, kvtree* tree);

/* convenience to write kvtree to an spath */
int kvtree_write_path(const spath* path, const kvtree* tree);

/* given a string defining SCR_PREFIX value as given by user
 * return spath of fully qualified path, user should free */
spath* scr_get_prefix(const char* prefix);

#endif
