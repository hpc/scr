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

#ifndef SCR_GLOBALS_H
#define SCR_GLOBALS_H

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* variable length args */
#include <stdarg.h>
#include <errno.h>

/* gethostbyname */
#include <netdb.h>

/* basename/dirname */
#include <unistd.h>
#include <libgen.h>

/* gettimeofday */
#include <sys/time.h>

/* localtime, asctime */
#include <time.h>

/* compute crc32 */
#include <zlib.h>

#include "mpi.h"

#include "spath.h"
#include "kvtree.h"
#include "kvtree_util.h"
#include "kvtree_mpi.h"
#include "rankstr_mpi.h"

#include "scr_conf.h"
#include "scr_keys.h"
#include "scr.h"
#include "scr_err.h"
#include "scr_io.h"
#include "scr_util.h"
#include "scr_util_mpi.h"
#include "spath_mpi.h"
#include "scr_meta.h"
#include "scr_dataset.h"
#include "scr_halt.h"
#include "scr_log.h"
#include "scr_cache_index.h"
#include "scr_filemap.h"
#include "scr_config.h"
#include "scr_param.h"
#include "scr_env.h"
#include "scr_index_api.h"

#include "scr_groupdesc.h"
#include "scr_storedesc.h"
#include "scr_reddesc.h"
#include "scr_summary.h"
#include "scr_flush_file_mpi.h"
#include "scr_cache.h"
#include "scr_cache_rebuild.h"
#include "scr_prefix.h"
#include "scr_fetch.h"
#include "scr_flush.h"
#include "scr_flush_sync.h"
#include "scr_flush_async.h"

#ifdef HAVE_LIBPMIX
#include "pmix.h"
#endif

/*
=========================================
Globals
=========================================
*/

extern char* scr_cntl_base;  /* base directory for control directory */
extern char* scr_cache_base; /* base directory for cache directory */

extern char* scr_cntl_prefix; /* path of control directory (adds to base directory) */

extern char* scr_prefix;       /* path of SCR_PREFIX directory on PFS */
extern char* scr_prefix_scr;   /* path to .scr subdir in SCR_PREFIX dir */
extern spath* scr_prefix_path; /* scr_prefix in spath form */

/* these files live in the control directory */
extern spath* scr_cindex_file;
extern char* scr_transfer_file;

/* we keep the halt, flush, and nodes files in the prefix directory
 * so that the batch script and / or external commands can access them */
extern spath* scr_halt_file;
extern spath* scr_flush_file;
extern spath* scr_nodes_file;

extern scr_cache_index* scr_cindex; /* tracks datasets in cache */
extern kvtree* scr_halt_hash; /* memory cache of halt file contents */

extern char* scr_username;    /* username of owner for running job */
extern char* scr_jobid;       /* unique job id string of current job */
extern char* scr_jobname;     /* jobname string, used to tie different runs together */
extern char* scr_clustername; /* name of cluster job is running on */
extern int scr_dataset_id;    /* keeps track of the current dataset id */
extern int scr_checkpoint_id; /* keeps track of the current checkpoint id */
extern int scr_ckpt_dset_id;  /* keeps track of the dataset id for the current checkpoint */
extern int scr_in_output;     /* flag tracks whether we are between start and complete calls */
extern int scr_initialized;   /* indicates whether the library has been initialized */
extern int scr_enabled;       /* indicates whether the library is enabled */
extern int scr_debug;         /* set debug verbosity */
extern int scr_page_size;     /* records block size for aligning MPI and file buffers */

extern int scr_log_enable;        /* whether to log SCR events at all */
extern int scr_log_txt_enable;    /* whether to log SCR events to text file */
extern int scr_log_syslog_enable; /* whether to log SCR events to syslog */
extern int scr_log_db_enable;     /* whether to log SCR events to database */
extern int scr_log_db_debug;      /* debug level for logging to database */
extern char* scr_log_db_host;     /* mysql host name */
extern char* scr_log_db_user;     /* mysql user name */
extern char* scr_log_db_pass;     /* mysql password */
extern char* scr_log_db_name;     /* mysql database name */

extern int scr_cache_size;    /* number of checkpoints to keep in cache at one time */
extern int scr_copy_type;     /* select which redundancy algorithm to use */
extern char* scr_group;       /* name of process group likely to fail */
extern int scr_set_size;      /* specify number of tasks in redundancy set */
extern int scr_set_failures;  /* specify number of failures to tolerate per set */
extern int scr_cache_bypass;  /* default bypass, whether to directly read/write parallel file system */

extern int scr_mpi_buf_size;     /* set MPI buffer size to chunk file transfer, int due to MPI limits */
extern size_t scr_file_buf_size; /* set buffer size to chunk file copies to/from parallel file system */
extern int scr_copy_metadata;    /* whether file metadata should also be copied */
extern int scr_axl_mkdir;        /* whether to have AXL create directories for files during a flush */

extern int scr_halt_seconds; /* secs remaining in allocation before job should be halted */
extern int scr_halt_exit;    /* whether SCR will call exit if halt condition is detected */

extern int   scr_purge;            /* delete all datasets from cache on restart for debugging */
extern int   scr_distribute;       /* whether to call scr_distribute_files during SCR_Init */
extern int   scr_fetch;            /* whether to call scr_fetch_files during SCR_Init */
extern int   scr_fetch_width;      /* specify number of processes to read files simultaneously */
extern int   scr_fetch_bypass;     /* whether to use implied bypass on fetch operations */
extern char* scr_fetch_current;    /* specify name of checkpoint to start with in fetch_latest */
extern int   scr_flush;            /* how many checkpoints between flushes */
extern char* scr_flush_type;       /* AXL type to use when flushing datasets */
extern int   scr_flush_width;      /* specify number of processes to write files simultaneously */
extern int   scr_flush_on_restart; /* specify whether to flush cache on restart */
extern int   scr_global_restart;   /* set if code must be restarted from parallel file system */
extern int   scr_drop_after_current; /* auto-drop datasets from index that come after named checkpoint when calling SCR_Current */

extern int scr_prefix_size;  /* max number of checkpoints to keep in prefix directory */
extern int scr_prefix_purge; /* whether to delete all datasets listed in index file during SCR_Init */

extern int scr_flush_async;            /* whether to use asynchronous flush */
extern double scr_flush_async_bw;      /* bandwidth limit imposed during async flush */
extern double scr_flush_async_percent; /* runtime limit imposed during async flush */

extern int scr_flush_poststage; /* whether to use scr_poststage.sh to finalize transfers */

extern int scr_crc_on_copy;   /* whether to enable crc32 checks during scr_swap_files() */
extern int scr_crc_on_flush;  /* whether to enable crc32 checks during flush and fetch */
extern int scr_crc_on_delete; /* whether to enable crc32 checks when deleting checkpoints */

extern int    scr_checkpoint_interval;   /* times to call Need_checkpoint between checkpoints */
extern int    scr_checkpoint_seconds;    /* min number of seconds between checkpoints */
extern double scr_checkpoint_overhead;   /* max allowed overhead for checkpointing */
extern int    scr_need_checkpoint_count; /* tracks the number of times Need_checkpoint has been called */
extern double scr_time_checkpoint_total; /* keeps a running total of the time spent to checkpoint */
extern int    scr_time_checkpoint_count; /* keeps a running count of the number of checkpoints taken */

extern char* scr_my_hostname; /* hostname of local process */
extern int   scr_my_hostid;   /* unique id of the node on which this rank resides */
extern int   scr_my_rank_host; /* my rank within the node */

extern MPI_Comm scr_comm_world;   /* dup of MPI_COMM_WORLD */
extern int scr_ranks_world;       /* number of ranks in the job */
extern int  scr_my_rank_world;    /* my rank in world */

extern MPI_Comm scr_comm_node; /* communicator of all tasks on the same node */

extern kvtree* scr_app_hash; /* records params set through SCR_Config */

extern kvtree* scr_groupdesc_hash; /* hash defining group descriptors to be used */
extern kvtree* scr_storedesc_hash; /* hash defining store descriptors to be used */
extern kvtree* scr_reddesc_hash;   /* hash defining redudancy descriptors to be used */

extern int scr_ngroupdescs;           /* number of descriptors in scr_groupdescs */
extern scr_groupdesc* scr_groupdescs; /* group descriptor structs */

extern int scr_nstoredescs;               /* number of descriptors in scr_storedescs */
extern scr_storedesc* scr_storedescs;     /* store descriptor structs */
extern scr_storedesc* scr_storedesc_cntl; /* store descriptor struct for control directory */

extern int scr_nreddescs;         /* number of redundancy descriptors in scr_reddescs list */
extern scr_reddesc* scr_reddescs; /* pointer to list of redundancy descriptors */

#ifdef HAVE_LIBPMIX
extern pmix_proc_t scr_pmix_proc; /*process handle for pmix */
#endif

#endif
