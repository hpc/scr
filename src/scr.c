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

#include <assert.h>

#include "scr_globals.h"

#include "dtcmp.h"
#include "er.h"
#include "axl_mpi.h"

/* define which state we're in for API calls, this is to help ensure
 * users call SCR functions in the correct order */
typedef enum {
    SCR_STATE_UNINIT,     /* before init and after finalize */
    SCR_STATE_IDLE,       /* between init/finalize */
    SCR_STATE_RESTART,    /* between start/complete restart */
    SCR_STATE_CHECKPOINT, /* between start/complete checkpoint */
    SCR_STATE_OUTPUT      /* between start/complete output */
} SCR_STATE;

/* initialize our state to uninit */
static SCR_STATE scr_state = SCR_STATE_UNINIT;

/* tracks set of files in current dataset */
static scr_filemap* scr_map = NULL;

/* tracks redundancy descriptor for current dataset */
static scr_reddesc* scr_rd = NULL;

/* tracks whether a checkpoint is available for restart */
static int scr_have_restart;

static double scr_time_compute_start;     /* records the start time of the current compute phase */
static double scr_time_compute_end;       /* records the end time of the current compute phase */

static double scr_time_checkpoint_start;  /* records the start time of the current checkpoint */
static double scr_time_checkpoint_end;    /* records the end time of the current checkpoint */

static time_t scr_timestamp_output_start; /* record timestamp of start of output phase */
static double scr_time_output_start;      /* records the start time of the current output phase */
static double scr_time_output_end;        /* records the end time of the current output phase */

/* look up redundancy descriptor we should use for this dataset */
static scr_reddesc* scr_get_reddesc(const scr_dataset* dataset, int ndescs, scr_reddesc* descs)
{
  int i;

  /* assume we won't find one */
  scr_reddesc* d = NULL;

  /* determine whether dataset is flagged as output */
  int is_output = scr_dataset_is_output(dataset);

  /* if it's output, and if a reddesc is marked for output, use that one */
  if (is_output) {
    for (i=0; i < ndescs; i++) {
      if (descs[i].enabled &&
          descs[i].output)
      {
        /* found a reddesc explicitly marked for output */
        d = &descs[i];
        return d;
      }
    }
  }

  /* dataset is either not output, or one redundancy descriptor has not been marked
   * explicitly for output */

  /* determine whether dataset is a checkpoint */
  int is_ckpt = scr_dataset_is_ckpt(dataset);

  /* multi-level checkpoint, pick the right level */
  if (is_ckpt) {
    /* get our checkpoint id */
    int ckpt_id;
    if (scr_dataset_get_ckpt(dataset, &ckpt_id) == SCR_SUCCESS) {
      /* got our id, now pick the redundancy descriptor that is:
       *   1) enabled
       *   2) has the highest interval that evenly divides id */
      int i;
      int interval = 0;
      for (i=0; i < ndescs; i++) {
        if (descs[i].enabled &&
            interval < descs[i].interval &&
            ckpt_id % descs[i].interval == 0)
        {
          d = &descs[i];
          interval = descs[i].interval;
        }
      }
    }
  } else {
    /* dataset is not a checkpoint, but there is no reddesc explicitly
     * for output either, pick an enabled reddesc with interval 1 */
      int i;
      for (i=0; i < ndescs; i++) {
        if (descs[i].enabled &&
            descs[i].interval == 1)
        {
          d = &descs[i];
          return d;
        }
      }
  }

  return d;
}

/* returns 1 if dataset id can be finalized in poststage, 0 otherwise */
static int scr_flush_can_poststage(int id)
{
  int poststage = 0;

  /* currently only BBAPI can support poststage */
  const scr_storedesc* storedesc = scr_cache_get_storedesc(scr_cindex, id);
  const char* type = storedesc->xfer;
  if (strcmp(type, "BBAPI") == 0) {
    poststage = 1;
  }

  return poststage;
}

static int scr_flush_finalize(void)
{
  /* When using poststage, we can finalize flushes
   * after the job completes rather than waiting on them here. */
  int poststage = scr_flush_poststage;

  /* check each outstanding async flush, if any */
  if (poststage && scr_flush_async_in_progress()) {
    /* Got at least one outstanding async flush.
     * Check whether all async flushes can be handled in poststage.
     * Iterate over all dataset ids still being flushed.
     * If any can not be completed in poststage, disable all poststage. */

    /* get ordered list of ids being flushed */
    int flush_num = 0;
    int* flush_ids = NULL;
    scr_flush_async_get_list(scr_cindex, &flush_num, &flush_ids);

    /* check that all flushes are using something we can complete in poststage */
    int i;
    for (i = 0; i < flush_num; i++) {
      int id = flush_ids[i];
      if (! scr_flush_can_poststage(id)) {
        poststage = 0;
      }
    }

    /* free list of flush ids */
    scr_free(&flush_ids);
  }

  /* check latest checkpoint if it still needs to be flushed */
  if (poststage && scr_flush > 0 && scr_flush_file_need_flush(scr_ckpt_dset_id)) {
    /* latest checkpoint needs to be flushed,
     * check whether we can also handle this checkpoint with poststage */
    if (scr_flush_can_poststage(scr_ckpt_dset_id)) {
      /* can finalize this flush in poststage */
      if (! scr_flush_file_is_flushing(scr_ckpt_dset_id)) {
        /* Initiate the flush now, but finish in the poststage.
         * Start as an async flush, even if we're otherwise using sync flush. */
        int flush_rc = scr_flush_async_start(scr_cindex, scr_ckpt_dset_id);
        if (flush_rc != SCR_SUCCESS) {
          scr_err("Flush of dataset %d failed @ %s:%d",
            scr_ckpt_dset_id, __FILE__, __LINE__
          );
        }
      }
    } else {
      /* cannot use poststage, so disable */
      poststage = 0;
    }
  }

  /* if we're not using postage, wait on all flushes now */
  if (! poststage) {
    /* wait on all async flushes to complete */
    if (scr_flush_async_in_progress()) {
      /* if any flush cannot use poststage, wait on them all to finish now */
      int flush_rc = scr_flush_async_waitall(scr_cindex);
      if (flush_rc != SCR_SUCCESS) {
        scr_err("Flush of datasets failed @ %s:%d",
          __FILE__, __LINE__
        );
      }
    }

    /* Flush checkpoint synchronously (wait for it to finish now). */
    if (scr_flush > 0 && scr_flush_file_need_flush(scr_ckpt_dset_id)) {
      int flush_rc = scr_flush_sync(scr_cindex, scr_ckpt_dset_id);
      if (flush_rc != SCR_SUCCESS) {
        scr_err("Flush of dataset %d failed @ %s:%d",
          scr_ckpt_dset_id, __FILE__, __LINE__
        );
      }
    }
  }

  if (scr_flush_async) {
    scr_flush_async_finalize();
  }

  return SCR_SUCCESS;
}

/*
=========================================
Halt logic
=========================================
*/

#define SCR_TEST_AND_HALT (1)
#define SCR_TEST_BUT_DONT_HALT (2)
#define SCR_FINALIZE_CALLED "SCR_FINALIZE_CALLED"

/* writes entry to halt file to indicate that SCR should exit job at first opportunity */
static int scr_halt(const char* reason)
{
  /* copy reason if one was given */
  if (reason != NULL) {
    kvtree_util_set_str(scr_halt_hash, SCR_HALT_KEY_EXIT_REASON, reason);
  }

  /* log the halt condition */
  if (scr_log_enable) {
    scr_log_halt(reason);
  }

  /* and write out the halt file */
  int rc = scr_halt_sync_and_decrement(scr_halt_file, scr_halt_hash, 0);
  return rc;
}

/* check whether we should halt the job */
static int scr_bool_check_halt_and_decrement(int halt_cond, int decrement)
{
  /* assume we don't have to halt */
  int need_to_halt = 0;

  /* determine whether we should halt the job by calling exit
   * if we detect an active halt condition */
  int halt_exit = ((halt_cond == SCR_TEST_AND_HALT) && scr_halt_exit);

  /* only rank 0 reads the halt file */
  if (scr_my_rank_world == 0) {
    /* TODO: all epochs are stored in ints, should be in unsigned ints? */
    /* get current epoch seconds */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int now = tv.tv_sec;

    /* locks halt file, reads it to pick up new values, decrements the
     * checkpoint counter, writes it out, and unlocks it */
    scr_halt_sync_and_decrement(scr_halt_file, scr_halt_hash, decrement);

    /* set halt seconds to value found in our halt hash */
    int halt_seconds;
    if (kvtree_util_get_int(scr_halt_hash, SCR_HALT_KEY_SECONDS, &halt_seconds) != KVTREE_SUCCESS) {
      /* didn't find anything, so set value to 0 */
      halt_seconds = 0;
    }

    /* if halt secs enabled, check the remaining time */
    if (halt_seconds > 0) {
      long int remaining = scr_env_seconds_remaining();
      if (remaining >= 0 && remaining <= halt_seconds) {
        if (halt_exit) {
          scr_dbg(0, "Job exiting: Reached time limit: (seconds remaining = %ld) <= (SCR_HALT_SECONDS = %d).",
                  remaining, halt_seconds
          );
          scr_halt("TIME_LIMIT");
        }
        need_to_halt = 1;
      }
    }

    /* check whether a reason has been specified */
    char* reason;
    if (kvtree_util_get_str(scr_halt_hash, SCR_HALT_KEY_EXIT_REASON, &reason) == KVTREE_SUCCESS) {
      if (strcmp(reason, "") != 0) {
        /* got a reason, but let's ignore SCR_FINALIZE_CALLED if it's set
         * and assume user restarted intentionally */
        if (strcmp(reason, SCR_FINALIZE_CALLED) != 0) {
          /* since reason points at the EXIT_REASON string in the halt hash, and since
           * scr_halt() resets this value, we need to copy the current reason */
          char* tmp_reason = strdup(reason);
          if (halt_exit && tmp_reason != NULL) {
            scr_dbg(0, "Job exiting: Reason: %s.", tmp_reason);
            scr_halt(tmp_reason);
          }
          scr_free(&tmp_reason);
          need_to_halt = 1;
        }
      }
    }

    /* check whether we are out of checkpoints */
    int checkpoints_left;
    if (kvtree_util_get_int(scr_halt_hash, SCR_HALT_KEY_CHECKPOINTS, &checkpoints_left) == KVTREE_SUCCESS) {
      if (checkpoints_left == 0) {
        if (halt_exit) {
          scr_dbg(0, "Job exiting: No more checkpoints remaining.");
          scr_halt("NO_CHECKPOINTS_LEFT");
        }
        need_to_halt = 1;
      }
    }

    /* check whether we need to exit before a specified time */
    int exit_before;
    if (kvtree_util_get_int(scr_halt_hash, SCR_HALT_KEY_EXIT_BEFORE, &exit_before) == KVTREE_SUCCESS) {
      if (now >= (exit_before - halt_seconds)) {
        if (halt_exit) {
          time_t time_now  = (time_t) now;
          time_t time_exit = (time_t) exit_before - halt_seconds;
          char str_now[256];
          char str_exit[256];
          strftime(str_now,  sizeof(str_now),  "%c", localtime(&time_now));
          strftime(str_exit, sizeof(str_exit), "%c", localtime(&time_exit));
          scr_dbg(0, "Job exiting: Current time (%s) is past ExitBefore-HaltSeconds time (%s).",
                  str_now, str_exit
          );
          scr_halt("EXIT_BEFORE_TIME");
        }
        need_to_halt = 1;
      }
    }

    /* check whether we need to exit after a specified time */
    int exit_after;
    if (kvtree_util_get_int(scr_halt_hash, SCR_HALT_KEY_EXIT_AFTER, &exit_after) == KVTREE_SUCCESS) {
      if (now >= exit_after) {
        if (halt_exit) {
          time_t time_now  = (time_t) now;
          time_t time_exit = (time_t) exit_after;
          char str_now[256];
          char str_exit[256];
          strftime(str_now,  sizeof(str_now),  "%c", localtime(&time_now));
          strftime(str_exit, sizeof(str_exit), "%c", localtime(&time_exit));
          scr_dbg(0, "Job exiting: Current time (%s) is past ExitAfter time (%s).", str_now, str_exit);
          scr_halt("EXIT_AFTER_TIME");
        }
        need_to_halt = 1;
      }
    }
  }

  /* broadcast halt decision from rank 0 */
  MPI_Bcast(&need_to_halt, 1, MPI_INT, 0, scr_comm_world);

  /* halt job if we need to, and flush latest checkpoint if needed */
  if (need_to_halt && halt_exit) {
    /* flush any pending datasets and shut down flush methods */
    scr_flush_finalize();

    /* sync up tasks before exiting (don't want tasks to exit so early that
     * runtime kills others after timeout) */
    MPI_Barrier(scr_comm_world);

#ifdef HAVE_LIBPMIX
    /* sync procs in pmix before shutdown */
    int retval = PMIx_Fence(NULL, 0, NULL, 0);
    if (retval != PMIX_SUCCESS) {
      scr_err("PMIx_Fence failed: rc=%d, rank: %d @ %s:%d",
        retval, scr_pmix_proc.rank, __FILE__, __LINE__
      );
    }

/*
    scr_dbg(0, "about to call pmix notify in HALT: pmix rank: %d", scr_pmix_proc.rank);
    retval = PMIx_Notify_event(-1,
                      &scr_pmix_proc,
                      PMIX_RANGE_GLOBAL,
                      NULL, 0,
                      NULL, (void *)NULL);
    if (retval != PMIX_SUCCESS) {
      scr_dbg(0, "error calling pmix_notify_event: %d", retval);
    }
*/

    /* shutdown pmix */
    retval = PMIx_Finalize(NULL, 0);
    if (retval != PMIX_SUCCESS) {
      scr_err("PMIx_Finalize failed: rc=%d, rank: %d @ %s:%d",
        retval, scr_pmix_proc.rank, __FILE__, __LINE__
      );
    }

    /* TODO: remove this once ompi has a fix?? */
    MPI_Barrier(scr_comm_world);
    MPI_Finalize();
#endif /* HAVE_LIBPMIX */

    /* and exit the job */
    exit(0);
  }

  return need_to_halt;
}

/*
=========================================
Utility functions
=========================================
*/

/* flush the specified dataset id if needed */
static int scr_check_flush_id(scr_cache_index* cindex, int id)
{
  /* assume we don't have to flush */
  int need_flush = 0;

  /* get info for current dataset */
  scr_dataset* dataset = scr_dataset_new();
  scr_cache_index_get_dataset(cindex, id, dataset);

  /* if this is output we have to flush */
  int is_output = scr_dataset_is_output(dataset);
  if (is_output) {
    need_flush = 1;
  }

  /* check whether user has flush enabled */
  if (scr_flush > 0) {
    /* TODO: get checkpoint id for dataset */
    /* if this is a checkpoint, then every scr_flush checkpoints, flush the checkpoint set */
    int is_ckpt = scr_dataset_is_ckpt(dataset);
    if (is_ckpt) {
      /* get checkpoint id for dataset */
      int ckpt_id = 0;
      scr_dataset_get_ckpt(dataset, &ckpt_id);
      if (ckpt_id > 0 && ckpt_id % scr_flush == 0) {
        need_flush = 1;
      }
    }
  }

  /* flush the dataset if needed */
  if (need_flush) {
    /* need to flush, determine whether to use async or sync flush */
    if (scr_flush_async) {
      /* start an async flush on the current dataset id */
      scr_flush_async_start(cindex, id);
    } else {
      /* synchronously flush the current dataset */
      int flush_rc = scr_flush_sync(cindex, id);
      if (flush_rc != SCR_SUCCESS) {
        scr_abort(-1, "Flush of dataset %d failed @ %s:%d",
          id, __FILE__, __LINE__
        );
      }
    }
  }

  /* free the dataset info */
  scr_dataset_delete(&dataset);

  return SCR_SUCCESS;
}

/* check whether a flush is needed, and execute flush if so */
static int scr_check_flush(scr_cache_index* cindex)
{
  int rc= scr_check_flush_id(cindex, scr_dataset_id);
  return rc;
}

/* on restart, check each cached dataset to see whether it should be flushed,
 * to be called after scr_cache_rebuild and scr_flush_file_rebuild */
int scr_flush_restart(scr_cache_index* cindex)
{
  /* get ordered list of dataset ids in cache */
  int ndsets;
  int* dsets;
  scr_cache_index_list_datasets(cindex, &ndsets, &dsets);

  /* iterate over ordered list of datasets in cache,
   * flush each dataset if needed */
  int i;
  for (i = 0; i < ndsets; i++) {
    int id = dsets[i];

    /* check whether we need to flush data */
    if (scr_flush_on_restart) {
      /* TODO: We could be more efficient here.
       * This may flush some checkpoints that aren't needed.
       * For example, if we have two checkpoints in cache, we really
       * only need to flush the most recent one.  We could also
       * avoid flushing a pure-output dataset that comes after the most
       * recent checkpoint since in theory it'll be overwritten anyway. */

      /* Application wants the latest checkpoint flushed to be able
       * to read it during restart, so force a sync flush.
       * We flush everything with sync here to maintain
       * proper ordering of the current marker. */
      int flush_rc = scr_flush_sync(cindex, id);
      if (flush_rc != SCR_SUCCESS) {
        scr_abort(-1, "Flush of dataset %d failed @ %s:%d",
          id, __FILE__, __LINE__
        );
      }
    } else {
      /* otherwise, flush only if we need to flush */
      scr_check_flush_id(cindex, id);
    }
  }

  /* free allocated list of checkpoint ids */
  scr_free(&dsets);

  return SCR_SUCCESS;
}

/* given a dataset id and a filename,
 * return the full path to the file which the caller should use to access the file */
static int scr_route_file(int id, const char* file, char* newfile, int n)
{
  /* check that we got a file and newfile to write to */
  if (file == NULL || strcmp(file, "") == 0 || newfile == NULL) {
    return SCR_FAILURE;
  }

  /* check that user's filename is not too long */
  if (strlen(file) >= SCR_MAX_FILENAME) {
    scr_abort(-1, "file name (%s) is longer than SCR_MAX_FILENAME (%d) @ %s:%d",
      file, SCR_MAX_FILENAME, __FILE__, __LINE__
    );
  }

  /* convert path string to path object */
  spath* path_file = spath_from_str(file);

  /* determine whether we're in bypass mode for this dataset */
  int bypass = 0;
  scr_cache_index_get_bypass(scr_cindex, id, &bypass);

  /* if we're in bypass route file to its location in prefix directory,
   * otherwise place it in a cache directory */
  if (bypass) {
    /* build absolute path to file */
    if (! spath_is_absolute(path_file)) {
      /* the path is not absolute, so prepend the current working directory */
      char cwd[SCR_MAX_FILENAME];
      if (scr_getcwd(cwd, sizeof(cwd)) == SCR_SUCCESS) {
        spath_prepend_str(path_file, cwd);
      } else {
        /* problem acquiring current working directory */
        scr_abort(-1, "Failed to build absolute path to %s @ %s:%d",
          file, __FILE__, __LINE__
        );
      }
    }

    /* TODO: should we check path is a child in prefix here? */
  } else {
    /* lookup the cache directory for this dataset */
    char* dir = NULL;
    scr_cache_index_get_dir(scr_cindex, id, &dir);

    /* chop file to just the file name and prepend directory */
    spath_basename(path_file);
    spath_prepend_str(path_file, dir);
  }

  /* simplify the absolute path (removes "." and ".." entries) */
  spath_reduce(path_file);

  /* copy to user's buffer */
  size_t n_size = (size_t) n;
  spath_strcpy(newfile, n_size, path_file);

  /* free the file path */
  spath_delete(&path_file);

  return SCR_SUCCESS;
}

/* given the current state, abort with an informative error message */
static void scr_state_transition_error(int state, const char* function, const char* file, int line)
{
  switch(state) {
  case SCR_STATE_UNINIT:
    /* tried to call some SCR function while uninitialized */
    scr_abort(-1, "Must call SCR_Init() before %s @ %s:%d",
      function, file, line
    );
    break;
  case SCR_STATE_RESTART:
    /* tried to call some SCR function while in Start_restart region */
    scr_abort(-1, "Must call SCR_Complete_restart() before %s @ %s:%d",
      function, file, line
    );
    break;
  case SCR_STATE_CHECKPOINT:
    /* tried to call some SCR function while in Start_checkpoint region */
    scr_abort(-1, "Must call SCR_Complete_checkpoint() before %s @ %s:%d",
      function, file, line
    );
    break;
  case SCR_STATE_OUTPUT:
    /* tried to call some SCR function while in Start_output region */
    scr_abort(-1, "Must call SCR_Complete_output() before %s @ %s:%d",
      function, file, line
    );
    break;
  }

  /* fall back message, less informative, but at least something */
  scr_abort(-1, "Called %s from invalid state %d @ %s:%d",
    function, state, file, line
  );

  return;
}

/*
=========================================
Configuration parameters
=========================================
*/

/* read in environment variables */
static int scr_get_params()
{
  const char* value;
  kvtree* tmp;
  double d;
  unsigned long long ull;

  /* user may want to disable SCR at runtime, read env var to avoid reading config files */
  if ((value = getenv("SCR_ENABLE")) != NULL) {
    scr_enabled = atoi(value);
  }

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }

  /* read in our configuration parameters */
  scr_param_init();

  /* check enabled parameter again, this time including settings from config files */
  if ((value = scr_param_get("SCR_ENABLE")) != NULL) {
    scr_enabled = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_ENABLE=%d", scr_enabled);
  }

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    scr_param_finalize();
    return SCR_FAILURE;
  }

  /* set debug verbosity level */
  if ((value = scr_param_get("SCR_DEBUG")) != NULL) {
    scr_debug = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_DEBUG=%d", scr_debug);
  }

  /* set scr_prefix_path and scr_prefix */
  value = scr_param_get("SCR_PREFIX");
  scr_prefix_path = scr_get_prefix(value);
  scr_prefix = spath_strdup(scr_prefix_path);
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_PREFIX=%s", scr_prefix);
  }

  /* define the path to the .scr subdir within the prefix dir */
  spath* path_prefix_scr = spath_dup(scr_prefix_path);
  spath_append_str(path_prefix_scr, ".scr");
  scr_prefix_scr = spath_strdup(path_prefix_scr);
  spath_delete(&path_prefix_scr);

  /* TODO: create store descriptor for prefix directory */
  /* create the .scr subdirectory */
  if (scr_my_rank_world == 0) {
    mode_t mode_dir = scr_getmode(1, 1, 1);
    if (scr_mkdir(scr_prefix_scr, mode_dir) != SCR_SUCCESS) {
      scr_abort(-1, "Failed to create .scr subdirectory %s @ %s:%d",
        scr_prefix_scr, __FILE__, __LINE__
      );
    }
  }

  /* set logging */
  if ((value = scr_param_get("SCR_LOG_ENABLE")) != NULL) {
    scr_log_enable = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_LOG_ENABLE=%d", scr_log_enable);
  }

  /* check whether SCR logging DB is enabled */
  if ((value = scr_param_get("SCR_LOG_TXT_ENABLE")) != NULL) {
    scr_log_txt_enable = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_LOG_TXT_ENABLE=%d", scr_log_txt_enable);
  }

  /* check whether SCR logging DB is enabled */
  if ((value = scr_param_get("SCR_LOG_SYSLOG_ENABLE")) != NULL) {
    scr_log_syslog_enable = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_LOG_SYSLOG_ENABLE=%d", scr_log_syslog_enable);
  }

  /* check whether SCR logging DB is enabled */
  if ((value = scr_param_get("SCR_LOG_DB_ENABLE")) != NULL) {
    scr_log_db_enable = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_LOG_DB_ENABLE=%d", scr_log_db_enable);
  }

  /* read in the debug level for database log messages */
  if ((value = scr_param_get("SCR_LOG_DB_DEBUG")) != NULL) {
    scr_log_db_debug = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_LOG_DB_DEBUG=%d", scr_log_db_debug);
  }

  /* SCR log DB connection parameters */
  if ((value = scr_param_get("SCR_LOG_DB_HOST")) != NULL) {
    scr_log_db_host = strdup(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_LOG_DB_HOST=%s", scr_log_db_host);
  }

  if ((value = scr_param_get("SCR_LOG_DB_USER")) != NULL) {
    scr_log_db_user = strdup(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_LOG_DB_USER=%s", scr_log_db_user);
  }

  if ((value = scr_param_get("SCR_LOG_DB_PASS")) != NULL) {
    scr_log_db_pass = strdup(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_LOG_DB_PASS=%s", scr_log_db_pass);
  }

  if ((value = scr_param_get("SCR_LOG_DB_NAME")) != NULL) {
    scr_log_db_name = strdup(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_LOG_DB_NAME=%s", scr_log_db_name);
  }

  /* read username from SCR_USER_NAME, if not set, try to read from environment */
  if ((value = scr_param_get("SCR_USER_NAME")) != NULL) {
    scr_username = strdup(value);
  } else {
    scr_username = scr_env_username();
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_USER_NAME=%s", scr_username);
  }

  /* check that the username is defined, fatal error if not */
  if (scr_username == NULL) {
    scr_abort(-1, "Failed to record username @ %s:%d",
            __FILE__, __LINE__
    );
  }

  /* read jobid from SCR_JOB_ID, if not set, try to read from environment */
  if ((value = scr_param_get("SCR_JOB_ID")) != NULL) {
    scr_jobid = strdup(value);
  } else {
    scr_jobid = scr_env_jobid();
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_JOB_ID=%s", scr_jobid);
  }

  /* check that the jobid is defined, fatal error if not */
  if (scr_jobid == NULL) {
    /* if we don't have a job id, we may be running outside of a
     * job allocation likely for testing purposes, create a default */
    scr_jobid = strdup("defjobid");
    if (scr_jobid == NULL) {
      scr_abort(-1, "Failed to allocate memory to record jobid @ %s:%d",
              __FILE__, __LINE__
      );
    }
  }

  /* read job name from SCR_JOB_NAME */
  if ((value = scr_param_get("SCR_JOB_NAME")) != NULL) {
    scr_jobname = strdup(value);
    if (scr_jobname == NULL) {
      scr_abort(-1, "Failed to allocate memory to record jobname (%s) @ %s:%d",
              value, __FILE__, __LINE__
      );
    }
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_JOB_NAME=%s", scr_jobname);
  }

  /* read cluster name from SCR_CLUSTER_NAME, if not set, try to read from environment */
  if ((value = scr_param_get("SCR_CLUSTER_NAME")) != NULL) {
    scr_clustername = strdup(value);
  } else {
    scr_clustername = scr_env_cluster();
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_CLUSTER_NAME=%s", scr_clustername);
  }

  /* override default base control directory */
  if ((value = scr_param_get("SCR_CNTL_BASE")) != NULL) {
    scr_cntl_base = spath_strdup_reduce_str(value);
  } else {
    scr_cntl_base = spath_strdup_reduce_str(SCR_CNTL_BASE);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_CNTL_BASE=%s", scr_cntl_base);
  }

  /* override default base directory for checkpoint cache */
  if ((value = scr_param_get("SCR_CACHE_BASE")) != NULL) {
    scr_cache_base = spath_strdup_reduce_str(value);
  } else {
    scr_cache_base = spath_strdup_reduce_str(SCR_CACHE_BASE);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_CACHE_BASE=%s", scr_cache_base);
  }

  /* set maximum number of checkpoints to keep in cache */
  if ((value = scr_param_get("SCR_CACHE_SIZE")) != NULL) {
    scr_cache_size = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_CACHE_SIZE=%d", scr_cache_size);
  }

  /* fill in a hash of group descriptors */
  scr_groupdesc_hash = kvtree_new();
  tmp = (kvtree*) scr_param_get_hash(SCR_CONFIG_KEY_GROUPDESC);
  if (tmp != NULL) {
    kvtree_set(scr_groupdesc_hash, SCR_CONFIG_KEY_GROUPDESC, tmp);
  }

  /* fill in a hash of store descriptors */
  scr_storedesc_hash = kvtree_new();
  tmp = (kvtree*) scr_param_get_hash(SCR_CONFIG_KEY_STOREDESC);
  if (tmp != NULL) {
    kvtree_set(scr_storedesc_hash, SCR_CONFIG_KEY_STOREDESC, tmp);
  }

  /* ensure we have a store descriptor for the default cache directory */
  tmp = kvtree_get_kv(scr_storedesc_hash, SCR_CONFIG_KEY_STOREDESC, scr_cache_base);
  if (tmp == NULL) {
    /* create a store descriptor for the cache directory */
    tmp = kvtree_set_kv(scr_storedesc_hash, SCR_CONFIG_KEY_STOREDESC, scr_cache_base);
    kvtree_util_set_int(tmp, SCR_CONFIG_KEY_COUNT, scr_cache_size);
  }

  /* ensure we have a store descriptor for the default control directory */
  tmp = kvtree_get_kv(scr_storedesc_hash, SCR_CONFIG_KEY_STOREDESC, scr_cntl_base);
  if (tmp == NULL) {
    /* create a store descriptor for the control directory */
    tmp = kvtree_set_kv(scr_storedesc_hash, SCR_CONFIG_KEY_STOREDESC, scr_cntl_base);
    kvtree_util_set_int(tmp, SCR_CONFIG_KEY_COUNT, 0);
  }

  /* select copy method */
  if ((value = scr_param_get("SCR_COPY_TYPE")) != NULL) {
    if (strcasecmp(value, "single") == 0) {
      scr_copy_type = SCR_COPY_SINGLE;
    } else if (strcasecmp(value, "partner") == 0) {
      scr_copy_type = SCR_COPY_PARTNER;
    } else if (strcasecmp(value, "xor") == 0) {
      scr_copy_type = SCR_COPY_XOR;
    } else if (strcasecmp(value, "rs") == 0) {
      scr_copy_type = SCR_COPY_RS;
    } else {
      scr_copy_type = SCR_COPY_FILE;
    }
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_COPY_TYPE=%d", scr_copy_type);
  }

  /* specify the number of tasks in xor set */
  if ((value = scr_param_get("SCR_SET_SIZE")) != NULL) {
    scr_set_size = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_SET_SIZE=%d", scr_set_size);
  }

  /* specify the number of failures we should tolerate per set */
  if ((value = scr_param_get("SCR_SET_FAILURES")) != NULL) {
    scr_set_failures = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_SET_FAILURES=%d", scr_set_failures);
  }

  /* specify the group name to protect failures */
  if ((value = scr_param_get("SCR_GROUP")) != NULL) {
    scr_group = strdup(value);
  } else {
    scr_group = strdup(SCR_GROUP);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_GROUP=%s", scr_group);
  }

  /* fill in a hash of redundancy descriptors */
  scr_reddesc_hash = kvtree_new();
  if (scr_copy_type == SCR_COPY_SINGLE) {
    /* fill in info for one SINGLE checkpoint */
    tmp = kvtree_set_kv(scr_reddesc_hash, SCR_CONFIG_KEY_CKPTDESC, "0");
    kvtree_util_set_str(tmp, SCR_CONFIG_KEY_STORE,    scr_cache_base);
    kvtree_util_set_str(tmp, SCR_CONFIG_KEY_TYPE,     "SINGLE");
  } else if (scr_copy_type == SCR_COPY_PARTNER) {
    /* fill in info for one PARTNER checkpoint */
    tmp = kvtree_set_kv(scr_reddesc_hash, SCR_CONFIG_KEY_CKPTDESC, "0");
    kvtree_util_set_str(tmp, SCR_CONFIG_KEY_STORE,    scr_cache_base);
    kvtree_util_set_str(tmp, SCR_CONFIG_KEY_TYPE,     "PARTNER");
    kvtree_util_set_str(tmp, SCR_CONFIG_KEY_GROUP,    scr_group);
  } else if (scr_copy_type == SCR_COPY_XOR) {
    /* fill in info for one XOR checkpoint */
    tmp = kvtree_set_kv(scr_reddesc_hash, SCR_CONFIG_KEY_CKPTDESC, "0");
    kvtree_util_set_str(tmp, SCR_CONFIG_KEY_STORE,    scr_cache_base);
    kvtree_util_set_str(tmp, SCR_CONFIG_KEY_TYPE,     "XOR");
    kvtree_util_set_str(tmp, SCR_CONFIG_KEY_GROUP,    scr_group);
    kvtree_util_set_int(tmp, SCR_CONFIG_KEY_SET_SIZE, scr_set_size);
  } else if (scr_copy_type == SCR_COPY_RS) {
    /* fill in info for one RS checkpoint */
    tmp = kvtree_set_kv(scr_reddesc_hash, SCR_CONFIG_KEY_CKPTDESC, "0");
    kvtree_util_set_str(tmp, SCR_CONFIG_KEY_STORE,        scr_cache_base);
    kvtree_util_set_str(tmp, SCR_CONFIG_KEY_TYPE,         "RS");
    kvtree_util_set_str(tmp, SCR_CONFIG_KEY_GROUP,        scr_group);
    kvtree_util_set_int(tmp, SCR_CONFIG_KEY_SET_SIZE,     scr_set_size);
    kvtree_util_set_int(tmp, SCR_CONFIG_KEY_SET_FAILURES, scr_set_failures);
  } else {
    /* read info from our configuration files */
    tmp = (kvtree*) scr_param_get_hash(SCR_CONFIG_KEY_CKPTDESC);
    if (tmp != NULL) {
      kvtree_set(scr_reddesc_hash, SCR_CONFIG_KEY_CKPTDESC, tmp);
    } else {
      scr_abort(-1, "Failed to define checkpoints @ %s:%d",
              __FILE__, __LINE__
      );
    }
  }

  /* set whether to bypass cache and directly read from and write to prefix dir */
  if ((value = scr_param_get("SCR_CACHE_BYPASS")) != NULL) {
    /* if BYPASS is set explicitly, we use that */
    scr_cache_bypass = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_CACHE_BYPASS=%d", scr_cache_bypass);
  }

  /* if job has fewer than SCR_HALT_SECONDS remaining after completing a checkpoint,
   * halt it */
  if ((value = scr_param_get("SCR_HALT_SECONDS")) != NULL) {
    scr_halt_seconds = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_HALT_SECONDS=%d", scr_halt_seconds);
  }

  /* determine whether we should call exit() upon detecting a halt condition */
  if ((value = scr_param_get("SCR_HALT_EXIT")) != NULL) {
    scr_halt_exit = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_HALT_EXIT=%d", scr_halt_exit);
  }

  /* set MPI buffer size (file chunk size) */
  if ((value = scr_param_get("SCR_MPI_BUF_SIZE")) != NULL) {
    if (scr_abtoull(value, &ull) == SCR_SUCCESS) {
      scr_mpi_buf_size = (int) ull;
      if (scr_mpi_buf_size != ull) {
        scr_abort(-1, "Value %s given for %s exceeds int range @ %s:%d",
                  value, SCR_MPI_BUF_SIZE, __FILE__, __LINE__
        );
      }
    }
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_MPI_BUF_SIZE=%llu", (unsigned long long)scr_mpi_buf_size);
  }

  /* whether to delete all datasets from cache on restart,
   * primarily used for debugging */
  if ((value = scr_param_get("SCR_CACHE_PURGE")) != NULL) {
    scr_purge = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_CACHE_PURGE=%d", scr_purge);
  }

  /* whether to distribute files in filemap to ranks */
  if ((value = scr_param_get("SCR_DISTRIBUTE")) != NULL) {
    scr_distribute = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_DISTRIBUTE=%d", scr_distribute);
  }

  /* whether to fetch files from the parallel file system */
  if ((value = scr_param_get("SCR_FETCH")) != NULL) {
    scr_fetch = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_FETCH=%d", scr_fetch);
  }

  /* specify number of processes to read files simultaneously */
  if ((value = scr_param_get("SCR_FETCH_WIDTH")) != NULL) {
    scr_fetch_width = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_FETCH_WIDTH=%d", scr_fetch_width);
  }

  /* allow user to specify checkpoint to start with on fetch */
  if ((value = scr_param_get("SCR_CURRENT")) != NULL) {
    scr_fetch_current = strdup(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_CURRENT=%s", scr_fetch_current);
  }

  /* specify how often we should flush files */
  if ((value = scr_param_get("SCR_FLUSH")) != NULL) {
    scr_flush = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_FLUSH=%d", scr_flush);
  }

  /* specify number of processes to write files simultaneously */
  if ((value = scr_param_get("SCR_FLUSH_WIDTH")) != NULL) {
    scr_flush_width = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_FLUSH_WIDTH=%d", scr_flush_width);
  }

  /* specify flush transfer type */
  if ((value = scr_param_get("SCR_FLUSH_TYPE")) != NULL) {
    scr_flush_type = strdup(value);
  } else {
    scr_flush_type = strdup(SCR_FLUSH_TYPE);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_FLUSH_TYPE=%s", scr_flush_type);
  }

  /* specify whether to always flush latest checkpoint from cache on restart */
  if ((value = scr_param_get("SCR_FLUSH_ON_RESTART")) != NULL) {
    scr_flush_on_restart = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_FLUSH_ON_RESTART=%d", scr_flush_on_restart);
  }

  /* set to 1 if code must be restarted from the parallel file system */
  if ((value = scr_param_get("SCR_GLOBAL_RESTART")) != NULL) {
    scr_global_restart = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_GLOBAL_RESTART=%d", scr_global_restart);
  }

  /* set to 1 to auto-drop all datasets that come after dataset named in
   * call to SCR_Current, this provides an easy way for an application to
   * restart from a particular checkpoint and truncate all later checkpoints */
  if ((value = scr_param_get("SCR_DROP_AFTER_CURRENT")) != NULL) {
    scr_drop_after_current = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_DROP_AFTER_CURRENT=%d", scr_drop_after_current);
  }

  /* specify window of number of checkpoints to keep in prefix directory,
   * set to positive integer to enable, then older checkpoints will be deleted
   * after a successful flush */
  if ((value = scr_param_get("SCR_PREFIX_SIZE")) != NULL) {
    scr_prefix_size = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_PREFIX_SIZE=%d", scr_prefix_size);
  }

  /* Some applications provide options so their users can wipe out all checkpoints
   * and start over.  While one could call SCR_Delete for each of those in turn,
   * we provide this option as a convenience.  If set, SCR will read the index file
   * and delete *all* datasets, both checkpoint and output. */
  if ((value = scr_param_get("SCR_PREFIX_PURGE")) != NULL) {
    scr_prefix_purge = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_PREFIX_PURGE=%d ", scr_prefix_purge);
  }

  /* specify whether to use asynchronous flush */
  if ((value = scr_param_get("SCR_FLUSH_ASYNC")) != NULL) {
    scr_flush_async = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_FLUSH_ASYNC=%d", scr_flush_async);
  }

 /* Specify whether our flush will be finalized in poststage (currently
  * only supported with BBAPI). */
  if ((value = scr_param_get("SCR_FLUSH_POSTSTAGE")) != NULL) {
    scr_flush_poststage = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_FLUSH_POSTSTAGE=%d", scr_flush_poststage);
  }

  /* bandwidth limit imposed during async flush (in bytes/sec) */
  if ((value = scr_param_get("SCR_FLUSH_ASYNC_BW")) != NULL) {
    if (scr_abtoull(value, &ull) == SCR_SUCCESS) {
      scr_flush_async_bw = (double) ull;
      if (scr_flush_async_bw != ull) {
        scr_abort(-1, "Value %s given for %s exceeds double range @ %s:%d",
                  value, SCR_FLUSH_ASYNC_BW, __FILE__, __LINE__
        );
      }
    } else {
      scr_err("Failed to read SCR_FLUSH_ASYNC_BW successfully @ %s:%d",
        __FILE__, __LINE__
      );
    }
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_FLUSH_ASYNC_BW=%f", scr_flush_async_bw);
  }

  /* runtime overhead limit imposed during async flush (in percentage) */
  if ((value = scr_param_get("SCR_FLUSH_ASYNC_PERCENT")) != NULL) {
    if (scr_atod(value, &d) == SCR_SUCCESS) {
      scr_flush_async_percent = d;
    } else {
      scr_err("Failed to read SCR_FLUSH_ASYNC_PERCENT successfully @ %s:%d",
        __FILE__, __LINE__
      );
    }
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_FLUSH_ASYNC_PERCENT=%f", scr_flush_async_percent);
  }

  /* set file copy buffer size (file chunk size) */
  if ((value = scr_param_get("SCR_FILE_BUF_SIZE")) != NULL) {
    if (scr_abtoull(value, &ull) == SCR_SUCCESS) {
      scr_file_buf_size = (size_t) ull;
      if (scr_file_buf_size !=  ull) {
        scr_abort(-1, "Value %s given for %s exceeds size_t range @ %s:%d",
                  value, SCR_FILE_BUF_SIZE, __FILE__, __LINE__
        );
      }
    } else {
      scr_err("Failed to read SCR_FILE_BUF_SIZE successfully @ %s:%d",
        __FILE__, __LINE__
      );
    }
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_FILE_BUF_SIZE=%d", scr_file_buf_size);
  }

  /* whether file metadata should also be copied */
  if ((value = scr_param_get("SCR_COPY_METADATA")) != NULL) {
    scr_copy_metadata = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_COPY_METADATA=%d", scr_copy_metadata);
  }

  /* whether to have AXL create directories for files during a flush */
  if ((value = scr_param_get("SCR_AXL_MKDIR")) != NULL) {
    scr_axl_mkdir = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_AXL_MKDIR=%d", scr_axl_mkdir);
  }

  /* specify whether to compute CRC when applying redundancy scheme */
  if ((value = scr_param_get("SCR_CRC_ON_COPY")) != NULL) {
    scr_crc_on_copy = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_CRC_ON_COPY=%d", scr_crc_on_copy);
  }

  /* specify whether to compute CRC on fetch and flush */
  if ((value = scr_param_get("SCR_CRC_ON_FLUSH")) != NULL) {
    scr_crc_on_flush = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_CRC_ON_FLUSH=%d", scr_crc_on_flush);
  }

  /* specify whether to compute and check CRC when deleting files from cache */
  if ((value = scr_param_get("SCR_CRC_ON_DELETE")) != NULL) {
    scr_crc_on_delete = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_CRC_ON_DELETE=%d" , scr_crc_on_delete);
  }

  /* override default checkpoint interval
   * (number of times to call Need_checkpoint between checkpoints) */
  if ((value = scr_param_get("SCR_CHECKPOINT_INTERVAL")) != NULL) {
    scr_checkpoint_interval = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_CHECKPOINT_INTERVAL=%d", scr_checkpoint_interval);
  }

  /* override default minimum number of seconds between checkpoints */
  if ((value = scr_param_get("SCR_CHECKPOINT_SECONDS")) != NULL) {
    scr_checkpoint_seconds = atoi(value);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_CHECKPOINT_SECONDS=%d", scr_checkpoint_seconds); 
  }

  /* override default maximum allowed checkpointing overhead */
  if ((value = scr_param_get("SCR_CHECKPOINT_OVERHEAD")) != NULL) {
    if (scr_atod(value, &d) == SCR_SUCCESS) {
      scr_checkpoint_overhead = d;
    } else {
      scr_err("Failed to read SCR_CHECKPOINT_OVERHEAD successfully @ %s:%d",
        __FILE__, __LINE__
      );
    }
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "SCR_CHECKPOINT_OVERHEAD=%f", scr_checkpoint_overhead);
  }

  if (scr_my_rank_world == 0) {
    scr_dbg(1, "Group descriptors:");
    kvtree_print_mode(scr_groupdesc_hash, 4, KVTREE_PRINT_KEYVAL);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "Store descriptors:");
    kvtree_print_mode(scr_storedesc_hash, 4, KVTREE_PRINT_KEYVAL);
  }
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "Redundancy descriptors:");
    kvtree_print_mode(scr_reddesc_hash, 4, KVTREE_PRINT_KEYVAL);
  }

  /* TODO: allow someone to silence this if they are not using scripts? */
  /* check that user didn't set something different in $SCR_PREFIX or current working dir */
  value = getenv("SCR_PREFIX");
  spath* prefix_path = scr_get_prefix(value);
  char* prefix_str = spath_strdup(prefix_path);
  if (strcmp(prefix_str, scr_prefix) != 0) {
    if (scr_my_rank_world == 0) {
      scr_warn("SCR_PREFIX in environment or cwd `%s' does not match value from config `%s' @ %s:%d",
        prefix_str, scr_prefix, __FILE__, __LINE__
      );
    }
  }
  scr_free(&prefix_str);
  spath_delete(&prefix_path);

  /* done reading parameters, can release the data structures now */
  scr_param_finalize();

  return SCR_SUCCESS;
}

/*
=========================================
Common code for Start/Complete output/checkpoint
=========================================
*/

/* start phase for a new output dataset */
static int scr_start_output(const char* name, int flags)
{
  /* bail out if user called Start_output twice without Complete_output in between */
  if (scr_in_output) {
    scr_abort(-1, "scr_complete_output must be called before scr_start_output is called again @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* set the output flag to indicate we have started a new output dataset */
  scr_in_output = 1;

  /* make sure everyone is ready to start before we delete any existing checkpoints */
  MPI_Barrier(scr_comm_world);

  /* determine whether this is a checkpoint */
  int is_ckpt = (flags & SCR_FLAG_CHECKPOINT);

  /* if we have a checkpoint, stop clock recording compute time,
   * we count normal output cost as part of compute time for
   * computing optimal checkpoint frequency */
  if (is_ckpt && scr_my_rank_world == 0) {
    /* stop the clock for measuring the compute time */
    scr_time_compute_end = MPI_Wtime();

    /* log the end of this compute phase */
    if (scr_log_enable) {
      double time_diff = scr_time_compute_end - scr_time_compute_start;
      scr_log_event("COMPUTE_END", NULL, NULL, NULL, NULL, &time_diff);
    }
  }

  /* If we loaded a checkpoint, but the user didn't restart from it,
   * then we really have no idea where they are in their sequence.
   * The app may be restarting from the parallel file system on its own,
   * or maybe they reset the run to start over.  We could also end up
   * in a similar situation if the user did not attempt to or failed to
   * fetch but there happens to be an existing checkpoint.  To avoid
   * colliding with existing checkpoints, set dataset_id and checkpoint_id
   * to be max of all known values. */
  if (scr_have_restart || scr_dataset_id == 0) {
    /* if we find larger dataset or checkpoint id values in the index file,
     * use those instead */
    int ids[4]; /* return code, dataset_id, checkpoint_id, ckpt_dset_id */
    if (scr_my_rank_world == 0) {
      ids[0] = scr_index_get_max_ids(scr_prefix_path, &ids[1], &ids[2], &ids[3]);
    }
    MPI_Bcast(ids, 4, MPI_INT, 0, scr_comm_world);
    if (ids[0] == SCR_SUCCESS) {
      /* got some values from the index file,
       * update our values if they are larger */
      if (ids[1] > scr_dataset_id) {
        scr_dataset_id = ids[1];
      }
      if (ids[2] > scr_checkpoint_id) {
        scr_checkpoint_id = ids[2];
        scr_ckpt_dset_id  = ids[3];
      }
    }

    /* forget that we have a restart loaded */
    scr_have_restart = 0;
  }

  /* increment our dataset counter */
  scr_dataset_id++;

  /* increment our checkpoint counters if needed */
  if (is_ckpt) {
    scr_checkpoint_id++;
    scr_ckpt_dset_id = scr_dataset_id;
  }

  /* check that we got valid name, and use a default name if not */
  const char* dataset_name = name;
  char dataset_name_default[SCR_MAX_FILENAME];
  if (name == NULL || strcmp(name, "") == 0) {
    /* caller didn't provide a name, so use our default */
    snprintf(dataset_name_default, sizeof(dataset_name_default), "scr.dataset.%d", scr_dataset_id);
    dataset_name = dataset_name_default;
  }

  /* TODO: if we know of an existing dataset with the same name
   * delete all files */

  /* ensure that name and flags match across ranks,
   * broadcast values from rank 0 and compare to that */
  char* root_name = NULL;
  int root_flags;
  if (scr_my_rank_world == 0) {
    root_name  = strdup(dataset_name);
    root_flags = flags;
  }
  scr_str_bcast(&root_name, 0, scr_comm_world);
  MPI_Bcast(&root_flags, 1, MPI_INT, 0, scr_comm_world);
  if (strcmp(dataset_name, root_name) != 0) {
    scr_abort(-1, "Dataset name provided to SCR_Start_output must be identical on all processes @ %s:%d",
      __FILE__, __LINE__
    );
  }
  if (root_flags != flags) {
    scr_abort(-1, "Dataset flags provided to SCR_Start_output must be identical on all processes @ %s:%d",
      __FILE__, __LINE__
    );
  }
  scr_free(&root_name);

  /* rank 0 builds dataset object and broadcasts it out to other ranks */
  scr_dataset* dataset = scr_dataset_new();
  if (scr_my_rank_world == 0) {
    /* capture time and build name of dataset */
    int64_t dataset_time = scr_time_usecs();

    /* fill in fields for dataset */
    scr_dataset_set_id(dataset, scr_dataset_id);
    scr_dataset_set_name(dataset, dataset_name);
    scr_dataset_set_flags(dataset, flags);
    scr_dataset_set_created(dataset, dataset_time);
    scr_dataset_set_username(dataset, scr_username);
    if (scr_jobname != NULL) {
      scr_dataset_set_jobname(dataset, scr_jobname);
    }
    scr_dataset_set_jobid(dataset, scr_jobid);
    if (scr_clustername != NULL) {
      scr_dataset_set_cluster(dataset, scr_clustername);
    }
    if (is_ckpt) {
      scr_dataset_set_ckpt(dataset, scr_checkpoint_id);
    }
  }
  kvtree_bcast(dataset, 0, scr_comm_world);

  /* allocate a fresh filemap for this output set */
  scr_map = scr_filemap_new();

  /* get the redundancy descriptor for this dataset */
  scr_rd = scr_get_reddesc(dataset, scr_nreddescs, scr_reddescs);

  /* start the clock to record how long it takes to write output */
  if (scr_my_rank_world == 0) {
    scr_time_output_start = MPI_Wtime();
    if (is_ckpt) {
      scr_time_checkpoint_start = scr_time_output_start;
    }

    /* log the start of this output phase */
    if (scr_log_enable) {
      scr_timestamp_output_start = scr_log_seconds();
      if (is_ckpt) {
        scr_log_event("CHECKPOINT_START", scr_rd->base, &scr_dataset_id, dataset_name, &scr_timestamp_output_start, NULL);
      } else {
        scr_log_event("OUTPUT_START", scr_rd->base, &scr_dataset_id, dataset_name, &scr_timestamp_output_start, NULL);
      }
    }
  }

  /* get an ordered list of the datasets currently in cache */
  int ndsets;
  int* dsets = NULL;
  scr_cache_index_list_datasets(scr_cindex, &ndsets, &dsets);

  /* lookup the number of datasets we're allowed to keep in
   * the base for this dataset */
  int size = 0;
  int store_index = scr_storedescs_index_from_name(scr_rd->base);
  if (store_index >= 0) {
    size = scr_storedescs[store_index].max_count;
  }

  int i;
  char* base = NULL;

  /* run through each of our datasets and count how many we have in this base */
  int nckpts_base = 0;
  for (i=0; i < ndsets; i++) {
    /* get base for this dataset and increase count if it matches the target base */
    char* dataset_dir;
    scr_cache_index_get_dir(scr_cindex, dsets[i], &dataset_dir);
    int store_index = scr_storedescs_index_from_child_path(dataset_dir);
    if (store_index >= 0) {
      scr_storedesc* sd = &scr_storedescs[store_index];
      base = sd->name;
      if (base != NULL) {
        if (strcmp(base, scr_rd->base) == 0) {
          nckpts_base++;
        }
      }
    }
  }

  /* run through and delete datasets from base until we make room for the current one */
  int flushing = -1;
  for (i=0; i < ndsets && nckpts_base >= size; i++) {
    char* dataset_dir;
    scr_cache_index_get_dir(scr_cindex, dsets[i], &dataset_dir);
    int store_index = scr_storedescs_index_from_child_path(dataset_dir);
    if (store_index >= 0) {
      scr_storedesc* sd = &scr_storedescs[store_index];
      base = sd->name;
      if (base != NULL) {
        if (strcmp(base, scr_rd->base) == 0) {
          if (! scr_flush_file_is_flushing(dsets[i])) {
            /* this dataset is in our base, and it's not being flushed, so delete it */
            scr_cache_delete(scr_cindex, dsets[i]);
            nckpts_base--;
          } else if (flushing == -1) {
            /* this dataset is in our base, but we're flushing it, don't delete it */
            flushing = dsets[i];
          }
        }
      }
    }
  }

  /* if we still don't have room and we're flushing,
   * the dataset we need to delete must be flushing, so wait for it to finish */
  if (nckpts_base >= size && flushing != -1) {
    /* in case there are ongoing flush operations from other cache
     * base locations, finalize datasets in order up to our target id
     * so that we don't mess up our current pointer */

    /* get full list of ids for ongoing flushes */
    int flush_num = 0;
    int* flush_ids = NULL;
    scr_flush_async_get_list(scr_cindex, &flush_num, &flush_ids);

    /* wait for each dataset up to and including the one we need
     * to complete */
    for (i = 0; i < flush_num; i++) {
      /* wait for this dataset to complete its flush */
      int id = flush_ids[i];
      int flush_rc = scr_flush_async_wait(scr_cindex, id);
      if (flush_rc != SCR_SUCCESS) {
        scr_abort(-1, "Flush of dataset %d failed @ %s:%d",
          id, __FILE__, __LINE__
        );
      }

      /* can break once the target id is done */
      if (id == flushing) {
        break;
      }
    }

    /* free list of flush ids */
    scr_free(&flush_ids);

    /* now dataset is no longer flushing, we can delete it and continue on */
    scr_cache_delete(scr_cindex, flushing);
    nckpts_base--;
  }

  /* free the list of datasets */
  scr_free(&dsets);

  /* update our file map with this new dataset */
  scr_cache_index_set_dataset(scr_cindex, scr_dataset_id, dataset);

  /* store the name of the directory we're about to create */
  const char* dir = scr_cache_dir_get(scr_rd, scr_dataset_id);
  scr_cache_index_set_dir(scr_cindex, scr_dataset_id, dir);
  scr_free(&dir);

  /* mark whether dataset should bypass cache */
  scr_cache_index_set_bypass(scr_cindex, scr_dataset_id, scr_rd->bypass);

  /* save cache index to disk before creating directory, so we have a record of it */
  scr_cache_index_write(scr_cindex_file, scr_cindex);

  /* make directory in cache to store files for this dataset */
  scr_cache_dir_create(scr_rd, scr_dataset_id);

  /* since bypass will start writing files to prefix directory immediately,
   * go ahead and create the initial entry in the index file */
  if (scr_rd->bypass) {
    scr_flush_init_index(dataset);
  }

  /* free dataset object */
  scr_dataset_delete(&dataset);

  /* print a debug message to indicate we've started the dataset */
  if (scr_my_rank_world == 0) {
    scr_dbg(1, "Starting dataset %d `%s'", scr_dataset_id, dataset_name);
  }

  return SCR_SUCCESS;
}

/* detect files that have been registered by more than one process,
 * drop filemap entries from all but one process */
static int scr_assign_ownership(scr_filemap* map, int bypass)
{
  int rc = SCR_SUCCESS;

  /* allocate buffers to hold index info for each file */
  int count = scr_filemap_num_files(map);
  char**    mapfiles    = (char**)    SCR_MALLOC(sizeof(char*)    * count);
  char**    filelist    = (char**)    SCR_MALLOC(sizeof(char*)    * count);
  uint64_t* group_id    = (uint64_t*) SCR_MALLOC(sizeof(uint64_t) * count);
  uint64_t* group_ranks = (uint64_t*) SCR_MALLOC(sizeof(uint64_t) * count);
  uint64_t* group_rank  = (uint64_t*) SCR_MALLOC(sizeof(uint64_t) * count);

  /* build list of files with their full path under prefix directory */
  int i = 0;
  kvtree_elem* elem;
  for (elem = scr_filemap_first_file(map);
       elem != NULL;
       elem = kvtree_elem_next(elem))
  {
    /* get the filename */
    char* file = kvtree_elem_key(elem);

    /* make a copy of the file name,
     * we need this string to potentially remove it later */
    mapfiles[i] = strdup(file);

    /* get metadata for this file from file map */
    scr_meta* meta = scr_meta_new();
    scr_filemap_get_meta(map, file, meta);

    /* get the directory to the file in the prefix directory */
    char* origpath;
    if (scr_meta_get_origpath(meta, &origpath) != SCR_SUCCESS) {
    }

    /* get the name of the file in the prefix directory */
    char* origname;
    if (scr_meta_get_origname(meta, &origname) != SCR_SUCCESS) {
    }

    /* build full path for file in prefix dir */
    spath* path = spath_from_str(origpath);
    spath_append_str(path, origname);
    filelist[i] = spath_strdup(path);
    spath_delete(&path);

    /* free meta data */
    scr_meta_delete(&meta);

    /* move on to the next file */
    i++;
  }

  /* identify the set of unique files across all ranks */
  uint64_t groups;
  int dtcmp_rc = DTCMP_Rankv_strings(
    count, (const char **) filelist, &groups, group_id, group_ranks, group_rank,
    DTCMP_FLAG_NONE, scr_comm_world
  );
  if (dtcmp_rc != DTCMP_SUCCESS) {
    rc = SCR_FAILURE;
  }

  /* keep rank 0 for each file as its owner, remove any entry from the filemap
   * for which we are not rank 0 */
  int multiple_owner = 0;
  for (i = 0; i < count; i++) {
    /* check whether this file exists on multiple ranks */
    if (group_ranks[i] > 1) {
      /* found the same file on more than one rank */
      multiple_owner = 1;

      /* print error if we're not in bypass */
      if (! bypass) {
        scr_err("Multiple procs registered file while not in bypass mode: `%s' @ %s:%d",
          filelist[i], __FILE__, __LINE__
        );
      }
    }

    /* only keep entry for this file in filemap if we're the
     * first rank in the set of ranks that have this file */
    if (group_rank[i] != 0) {
      scr_filemap_remove_file(map, mapfiles[i]);
    }
  }

  /* fatal error if any file is on more than one rank and not in bypass */
  int any_multiple_owner = 0;
  MPI_Allreduce(&multiple_owner, &any_multiple_owner, 1, MPI_INT, MPI_LOR, scr_comm_world);
  if (any_multiple_owner && !bypass) {
    scr_abort(-1, "Shared file access detected while not in bypass mode @ %s:%d",
      __FILE__, __LINE__
    );
  }

  /* free dtcmp buffers */
  scr_free(&group_id);
  scr_free(&group_ranks);
  scr_free(&group_rank);

  /* free list of file names */
  for (i = 0; i < count; i++) {
    scr_free(&mapfiles[i]);
    scr_free(&filelist[i]);
  }
  scr_free(&mapfiles);
  scr_free(&filelist);

  /* determine whether all leaders successfully created their directories */
  return rc;
}

/* end phase for current output dataset */
static int scr_complete_output(int valid)
{
  /* bail out if there is no active call to Start_output */
  if (! scr_in_output) {
    scr_abort(-1, "scr_start_output must be called before scr_complete_output @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* assume we'll succeed */
  int rc = SCR_SUCCESS;

  /* When using bypass mode, we allow different procs to write to the same file,
   * in which case, both should have registered the file in Route_file and thus
   * have an entry in the file map.  The proper thing to do here is to list the
   * set of ranks that share a file, however, that requires fixing up lots of
   * other parts of the code.  For now, ensure that at most one file lists the
   * file in their file map. */
  rc = scr_assign_ownership(scr_map, scr_rd->bypass);

  /* count number of files, number of bytes, and record filesize for each file
   * as written by this process */
  int files_valid = valid;
  unsigned long my_counts[3] = {0, 0, 0};
  kvtree_elem* elem;
  for (elem = scr_filemap_first_file(scr_map);
       elem != NULL;
       elem = kvtree_elem_next(elem))
  {
    /* get the filename */
    char* file = kvtree_elem_key(elem);
    my_counts[0]++;

    /* start with valid flag from caller for this file */
    int file_valid = valid;

    /* check that we can read the file */
    if (scr_file_is_readable(file) != SCR_SUCCESS) {
      scr_dbg(2, "Do not have read access to file: %s @ %s:%d",
        file, __FILE__, __LINE__
      );
      file_valid  = 0;
      files_valid = 0;
    }

    /* stat the file to get its size and other metadata */
    unsigned long filesize = 0;
    struct stat stat_buf;
    int stat_rc = stat(file, &stat_buf);
    if (stat_rc == 0) {
      filesize = (unsigned long) stat_buf.st_size;
    }

    /* get size of this file */
    //unsigned long filesize = scr_file_size(file);
    my_counts[1] += filesize;

    /* TODO: record permissions and/or timestamps? */

    /* fill in filesize and complete flag in the meta data for the file */
    scr_meta* meta = scr_meta_new();
    scr_filemap_get_meta(scr_map, file, meta);
    scr_meta_set_filesize(meta, filesize);
    scr_meta_set_complete(meta, file_valid);
    if (stat_rc == 0) {
      scr_meta_set_stat(meta, &stat_buf);
    }
    scr_filemap_set_meta(scr_map, file, meta);
    scr_meta_delete(&meta);
  }

  /* we execute a sum as a logical allreduce to determine whether everyone is valid
   * we interpret the result to be true only if the sum adds up to the number of processes */
  if (files_valid) {
    my_counts[2] = 1;
  }

  /* execute allreduce to total up number of files, bytes, and number of valid ranks */
  unsigned long total_counts[3];
  MPI_Allreduce(my_counts, total_counts, 3, MPI_UNSIGNED_LONG, MPI_SUM, scr_comm_world);
  unsigned long total_files = total_counts[0];
  unsigned long total_bytes = total_counts[1];
  unsigned long total_valid = total_counts[2];

  /* get dataset from filemap */
  scr_dataset* dataset = scr_dataset_new();
  scr_cache_index_get_dataset(scr_cindex, scr_dataset_id, dataset);

  /* get flags for this dataset */
  int is_ckpt   = scr_dataset_is_ckpt(dataset);
  int is_output = scr_dataset_is_output(dataset);

  /* store total number of files, total number of bytes, and complete flag in dataset */
  scr_dataset_set_files(dataset, (int) total_files);
  scr_dataset_set_size(dataset,        total_bytes);
  if (total_valid == scr_ranks_world) {
    /* got a valid=1 for every rank, we're complete */
    scr_dataset_set_complete(dataset, 1);
  } else {
    /* at least one rank has valid=0, so incomplete,
     * consider output to be invalid */
    scr_dataset_set_complete(dataset, 0);
    rc = SCR_FAILURE;
  }
  scr_cache_index_set_dataset(scr_cindex, scr_dataset_id, dataset);
  scr_cache_index_write(scr_cindex_file, scr_cindex);

  /* write out info to filemap */
  scr_cache_set_map(scr_cindex, scr_dataset_id, scr_map);

  /* record the cost of the output before copy */
  int files    = (int) total_files;
  double bytes = (double) total_bytes;
  if (scr_my_rank_world == 0) {
    /* stop the clock for this output */
    double end = MPI_Wtime();
    double time_diff = end - scr_time_output_start;
    double bw = 0.0;
    if (time_diff > 0.0) {
      bw = bytes / (1024.0 * 1024.0 * time_diff);
    }
    scr_dbg(1, "scr_complete_output: %f secs, %e bytes, %f MB/s, %f MB/s per proc",
            time_diff, bytes, bw, bw/scr_ranks_world
    );

    /* log data on the output */
    if (scr_log_enable) {
      /* log the end of this write phase */
      char* dset_name;
      scr_dataset_get_name(dataset, &dset_name);
      char* dir = scr_cache_dir_get(scr_rd, scr_dataset_id);
      scr_log_transfer("WRITE", scr_rd->base, dir, &scr_dataset_id, dset_name,
        &scr_timestamp_output_start, &time_diff, &bytes, &files
      );
      scr_free(&dir);
    }
  }

  /* apply redundancy scheme if we're still valid */
  if (rc == SCR_SUCCESS) {
    rc = scr_reddesc_apply(scr_map, scr_rd, scr_dataset_id);
  }

  /* record the cost of the output and log its completion */
  if (scr_my_rank_world == 0) {
    /* stop the clock for this output */
    scr_time_output_end = MPI_Wtime();
    if (is_ckpt) {
      scr_time_checkpoint_end = scr_time_output_end;
    }

    /* compute and record the cost for this output */
    double time_diff = scr_time_output_end - scr_time_output_start;
    if (time_diff < 0.0) {
      scr_err("Output end time (%f) is less than start time (%f) @ %s:%d",
        scr_time_output_end, scr_time_output_start, __FILE__, __LINE__
      );
      time_diff = 0.0;
    }

    /* tally up running total of checkpoint costs */
    if (is_ckpt) {
      scr_time_checkpoint_total += time_diff;
      scr_time_checkpoint_count++;
    }

    /* log data on the output */
    if (scr_log_enable) {
      /* log the end of this output phase */
      char* dset_name;
      scr_dataset_get_name(dataset, &dset_name);
      char* dir = scr_cache_dir_get(scr_rd, scr_dataset_id);
      if (is_ckpt) {
        scr_log_event("CHECKPOINT_END", scr_rd->base, &scr_dataset_id, dset_name, NULL, &time_diff);
        scr_log_transfer("CHECKPOINT", scr_rd->base, dir, &scr_dataset_id, dset_name,
          &scr_timestamp_output_start, &time_diff, &bytes, &files
        );
      } else {
        scr_log_event("OUTPUT_END", scr_rd->base, &scr_dataset_id, dset_name, NULL, &time_diff);
        scr_log_transfer("OUTPUT", scr_rd->base, dir, &scr_dataset_id, dset_name,
          &scr_timestamp_output_start, &time_diff, &bytes, &files
        );
      }
      scr_free(&dir);
    }

    /* print out a debug message with the result of the copy */
    scr_dbg(2, "Completed dataset %d with return code %d",
      scr_dataset_id, rc
    );
  }

  /* if copy is good, check whether we need to flush or halt,
   * otherwise delete the checkpoint to conserve space */
  if (rc == SCR_SUCCESS) {
    /* record entry in flush file for this dataset */
    char* dset_name;
    scr_dataset_get_name(dataset, &dset_name);
    scr_flush_file_new_entry(scr_dataset_id, dset_name, dataset, SCR_FLUSH_KEY_LOCATION_CACHE, is_ckpt, is_output);

    /* go ahead and flush any bypass dataset since
     * it's just a bit more work to finish at this point */
    if (scr_rd->bypass) {
      int flush_rc = scr_flush_sync(scr_cindex, scr_dataset_id);
      if (flush_rc != SCR_SUCCESS) {
        scr_abort(-1, "Flush of dataset %d failed @ %s:%d",
          scr_dataset_id, __FILE__, __LINE__
        );
      }
    }

    /* check_flush may start an async flush, whereas check_halt will call sync flush,
     * so place check_flush after check_halt */
    if (is_ckpt) {
      /* only halt on checkpoints */
      scr_bool_check_halt_and_decrement(SCR_TEST_AND_HALT, 1);
    }
    scr_check_flush(scr_cindex);
  } else {
    /* something went wrong, so delete this checkpoint from the cache */
    scr_cache_delete(scr_cindex, scr_dataset_id);

    /* TODODSET: probably should return error or abort if this is output */
  }

  /* if we have an async flush ongoing, take this chance to check whether it's completed */
  if (scr_flush_async_in_progress()) {
    /* got an outstanding async flush, let's check it */
    scr_flush_async_progall(scr_cindex);
  }

  /* done with dataset */
  scr_dataset_delete(&dataset);

  /* free off the filemap we allocated in the start call */
  scr_filemap_delete(&scr_map);

  /* set redundancy descriptor back to NULL */
  scr_rd = NULL;

  /* make sure everyone is ready before we exit */
  MPI_Barrier(scr_comm_world);

  /* unset the output flag to indicate we have exited the current output phase */
  scr_in_output = 0;

  /* start the clock for measuring the compute time,
   * we count output time as compute time for non-checkpoint datasets */
  if (is_ckpt && scr_my_rank_world == 0) {
    scr_time_compute_start = MPI_Wtime();

    /* log the start time of this compute phase */
    if (scr_log_enable) {
      scr_log_event("COMPUTE_START", NULL, NULL, NULL, NULL, NULL);
    }
  }

  return rc;
}

/*
=========================================
User interface functions
=========================================
*/

int SCR_Init()
{
  int i;

  /* manage state transition */
  if (scr_state != SCR_STATE_UNINIT) {
    scr_abort(-1, "Called SCR_Init() when already initialized @ %s:%d",
      __FILE__, __LINE__
    );
  }
  scr_state = SCR_STATE_IDLE;

  /* check whether user has disabled library via environment variable */
  char* value = NULL;
  if ((value = getenv("SCR_ENABLE")) != NULL) {
    scr_enabled = atoi(value);
  }

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }

  /* NOTE: SCR_ENABLE can also be set in a config file, but to read
   * a config file, we must at least create scr_comm_world and call
   * scr_get_params() */

  /* create a context for the library */
  if (scr_comm_world == MPI_COMM_NULL) {
    MPI_Comm_dup(MPI_COMM_WORLD,  &scr_comm_world);
    MPI_Comm_rank(scr_comm_world, &scr_my_rank_world);
    MPI_Comm_size(scr_comm_world, &scr_ranks_world);
  }

  /* get my hostname (used in debug and error messages) */
  scr_my_hostname = scr_env_nodename();
  if (scr_my_hostname == NULL) {
    scr_err("Failed to get hostname @ %s:%d",
      __FILE__, __LINE__
    );
    MPI_Abort(scr_comm_world, 0);
  }

  /* get the page size (used to align communication buffers) */
  scr_page_size = getpagesize();
  if (scr_page_size <= 0) {
    scr_err("Call to getpagesize failed @ %s:%d",
      __FILE__, __LINE__
    );
    MPI_Abort(scr_comm_world, 0);
  }

  /* initialize the DTCMP library for sorting and ranking routines
   * if we're using it */
  int dtcmp_rc = DTCMP_Init();
  if (dtcmp_rc != DTCMP_SUCCESS) {
    scr_abort(-1, "Failed to initialize DTCMP library @ %s:%d",
      __FILE__, __LINE__
    );
  }

  /* initialize ER for encode/rebuild */
  int er_rc = ER_Init(NULL);
  if (er_rc != ER_SUCCESS) {
    scr_abort(-1, "Failed to initialize ER library @ %s:%d",
      __FILE__, __LINE__
    );
  }

  /* initialize AXL for data transfers */
  int axl_rc = AXL_Init_comm(scr_comm_world);
  if (axl_rc != AXL_SUCCESS) {
    scr_abort(-1, "Failed to initialize AXL library @ %s:%d",
      __FILE__, __LINE__
    );
  }

  /* read our configuration: environment variables, config file, etc. */
  scr_get_params();

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    /* shut down the AXL library */
    int axl_rc = AXL_Finalize_comm(scr_comm_world);
    if (axl_rc != AXL_SUCCESS) {
      scr_abort(-1, "Failed to finalize AXL library @ %s:%d",
        __FILE__, __LINE__
      );
    }

    /* shut down the ER library */
    int er_rc = ER_Finalize();
    if (er_rc != ER_SUCCESS) {
      scr_abort(-1, "Failed to finalize ER library @ %s:%d",
        __FILE__, __LINE__
      );
    }

    /* shut down the DTCMP library if we're using it */
    int dtcmp_rc = DTCMP_Finalize();
    if (dtcmp_rc != DTCMP_SUCCESS) {
      scr_abort(-1, "Failed to finalize DTCMP library @ %s:%d",
        __FILE__, __LINE__
      );
    }

    scr_free(&scr_my_hostname);

    /* we dup'd comm_world to broadcast parameters in scr_get_params,
     * need to free it here */
    MPI_Comm_free(&scr_comm_world);

    return SCR_FAILURE;
  }

  /* coonfigure used libraries */
  kvtree* axl_config = kvtree_new();
  if (kvtree_util_set_bytecount(axl_config,
      AXL_KEY_CONFIG_FILE_BUF_SIZE, scr_file_buf_size) != KVTREE_SUCCESS)
  {
    scr_abort(-1, "Failed to set AXL config option %s @ %s:%d",
      AXL_KEY_CONFIG_FILE_BUF_SIZE, __FILE__, __LINE__
    );
  }
  if (kvtree_util_set_int(axl_config,
      AXL_KEY_CONFIG_DEBUG, scr_debug) != KVTREE_SUCCESS)
  {
    scr_abort(-1, "Failed to set AXL config option %s @ %s:%d",
      AXL_KEY_CONFIG_DEBUG, __FILE__, __LINE__
    );
  }
  if (kvtree_util_set_int(axl_config,
      AXL_KEY_CONFIG_MKDIR, scr_axl_mkdir) != KVTREE_SUCCESS)
  {
    scr_abort(-1, "Failed to set AXL config option %s @ %s:%d",
      AXL_KEY_CONFIG_MKDIR, __FILE__, __LINE__
    );
  }
  if (kvtree_util_set_int(axl_config,
      AXL_KEY_CONFIG_COPY_METADATA, scr_copy_metadata) != KVTREE_SUCCESS)
  {
    scr_abort(-1, "Failed to set AXL config option %s @ %s:%d",
      AXL_KEY_CONFIG_COPY_METADATA, __FILE__, __LINE__
    );
  }
  if (kvtree_util_set_int(axl_config,
      AXL_KEY_CONFIG_RANK, scr_my_rank_world) != KVTREE_SUCCESS)
  {
    scr_abort(-1, "Failed to set AXL config option %s @ %s:%d",
      AXL_KEY_CONFIG_RANK, __FILE__, __LINE__
    );
  }
  if (AXL_Config(axl_config) == NULL) {
    scr_abort(-1, "Failed to configure AXL @ %s:%d",
      __FILE__, __LINE__
    );
  }
  kvtree_delete(&axl_config);

  kvtree* er_config = kvtree_new();
  if (kvtree_util_set_int(er_config,
      ER_KEY_CONFIG_DEBUG, scr_debug) != KVTREE_SUCCESS)
  {
    scr_abort(-1, "Failed to set ER config option %s @ %s:%d",
      ER_KEY_CONFIG_DEBUG, __FILE__, __LINE__
    );
  }
  if (kvtree_util_set_int(er_config,
      ER_KEY_CONFIG_SET_SIZE, scr_set_size) != KVTREE_SUCCESS)
  {
    scr_abort(-1, "Failed to set ER config option %s @ %s:%d",
      ER_KEY_CONFIG_SET_SIZE, __FILE__, __LINE__
    );
  }
  if (kvtree_util_set_int(er_config,
      ER_KEY_CONFIG_MPI_BUF_SIZE, scr_mpi_buf_size) != KVTREE_SUCCESS)
  {
    scr_abort(-1, "Failed to set ER config option %s @ %s:%d",
      ER_KEY_CONFIG_MPI_BUF_SIZE, __FILE__, __LINE__
    );
  }
  if (kvtree_util_set_int(er_config,
      ER_KEY_CONFIG_CRC_ON_COPY, scr_crc_on_copy) != KVTREE_SUCCESS)
  {
    scr_abort(-1, "Failed to set ER config option %s @ %s:%d",
      ER_KEY_CONFIG_CRC_ON_COPY, __FILE__, __LINE__
    );
  }
  if (ER_Config(er_config) == NULL)
  {
    scr_abort(-1, "Failed to configure ER @ %s:%d",
      __FILE__, __LINE__
    );
  }
  kvtree_delete(&er_config);

  /* check that some required parameters are set */
  if (scr_username == NULL || scr_jobid == NULL) {
    scr_abort(-1, "Jobid or username is not set; set SCR_JOB_ID or SCR_USER_NAME @ %s:%d",
      __FILE__, __LINE__
    );
  }

  /* setup group descriptors */
  if (scr_groupdescs_create(scr_comm_world) != SCR_SUCCESS) {
    if (scr_my_rank_world == 0) {
      scr_err("Failed to prepare one or more group descriptors @ %s:%d",
        __FILE__, __LINE__
      );
    }
  }

  /* setup store descriptors (refers to group descriptors) */
  if (scr_storedescs_create(scr_comm_world) != SCR_SUCCESS) {
    if (scr_my_rank_world == 0) {
      scr_err("Failed to prepare one or more store descriptors @ %s:%d",
        __FILE__, __LINE__
      );
    }
  }

  /* setup redundancy descriptors (refers to store descriptors) */
  if (scr_reddescs_create() != SCR_SUCCESS) {
    if (scr_my_rank_world == 0) {
      scr_err("Failed to prepare one or more redundancy descriptors @ %s:%d",
        __FILE__, __LINE__
      );
    }
  }

  /* check that we have an enabled redundancy descriptor with
   * interval of one, this is necessary so a reddesc is defined
   * for every checkpoint */
  int found_one = 0;
  for (i=0; i < scr_nreddescs; i++) {
    /* check that we have at least one descriptor enabled with
     * an interval of one */
    if (scr_reddescs[i].enabled && scr_reddescs[i].interval == 1) {
      found_one = 1;
    }
  }
  if (! found_one) {
    if (scr_my_rank_world == 0) {
      scr_abort(-1, "Failed to find an enabled redundancy descriptor with interval 1 @ %s:%d",
        __FILE__, __LINE__
      );
    }
  }

  /* create global communicator of all ranks on the node */
  scr_groupdesc* groupdesc_node = scr_groupdescs_from_name(SCR_GROUP_NODE);
  if (groupdesc_node != NULL) {
    /* just dup the communicator from the NODE group */
    MPI_Comm_dup(groupdesc_node->comm, &scr_comm_node);
  } else {
    scr_abort(-1, "Failed to create communicator for procs on each node @ %s:%d",
      __FILE__, __LINE__
    );
  }

  /* get our local rank within our node */
  MPI_Comm_rank(scr_comm_node, &scr_my_rank_host);

  /* num_nodes will be used later, this line is moved above cache_dir creation
   * to make sure scr_my_hostid is set before we try to create directories.
   * The logic that uses num_nodes can't be moved here because it relies on the
   * scr_node_file variable computed later */
  int num_nodes;
  rankstr_mpi(scr_my_hostname, scr_comm_world, 0, 1, &num_nodes, &scr_my_hostid);

  /* check that scr_prefix is set */
  if (scr_prefix == NULL || strcmp(scr_prefix, "") == 0) {
    if (scr_my_rank_world == 0) {
      scr_halt("SCR_INIT_FAILED");
    }
    SCR_ALLABORT(-1, "SCR_PREFIX must be set");
  }

  /* initialize our logging if enabled */
  if (scr_my_rank_world == 0 && scr_log_enable) {
    if (scr_log_txt_enable) {
      scr_log_init_txt(scr_prefix);
    }
    if (scr_log_syslog_enable) {
      scr_log_init_syslog();
    }
    if (scr_log_db_enable) {
      scr_log_init_db(scr_log_db_debug, scr_log_db_host, scr_log_db_user, scr_log_db_pass, scr_log_db_name);
    }
  }

  /* register this job in the logging database */
  if (scr_my_rank_world == 0 && scr_log_enable) {
    if (scr_username != NULL && scr_prefix != NULL) {
      time_t job_start = scr_log_seconds();
      if (scr_log_job(scr_username, scr_my_hostname, scr_jobid, scr_prefix, job_start) == SCR_SUCCESS) {
        /* record the start time for this run */
        scr_log_run(job_start, scr_ranks_world, num_nodes);
      } else {
        scr_warn("Failed to log job for username %s and prefix %s, disabling logging @ %s:%d",
          scr_username, scr_prefix, __FILE__, __LINE__
        );
        scr_log_enable = 0;
      }
    } else {
      scr_warn("Failed to read username or prefix from environment, disabling logging @ %s:%d",
        __FILE__, __LINE__
      );
      scr_log_enable = 0;
    }
  }

  /* TODO MEMFS: mount storage for control directory */

  /* build the control directory name: CNTL_BASE/username/scr.jobid */
  spath* path_cntl_prefix = spath_from_str(scr_cntl_base);
  spath_append_str(path_cntl_prefix, scr_username);
  spath_append_strf(path_cntl_prefix, "scr.%s", scr_jobid);
  spath_reduce(path_cntl_prefix);
  scr_cntl_prefix = spath_strdup(path_cntl_prefix);
  spath_delete(&path_cntl_prefix);

  /* create the control directory */
  if (scr_storedesc_dir_create(scr_storedesc_cntl, scr_cntl_prefix)
      != SCR_SUCCESS)
  {
    scr_abort(-1, "Failed to create control directory: %s @ %s:%d",
      scr_cntl_prefix, __FILE__, __LINE__
    );
  }

  /* TODO: should we check for access and required space in cntl
   * directory at this point? */

  /* create the cache directories */
  for (i=0; i < scr_nreddescs; i++) {
    /* TODO: if checkpoints can be enabled at run time,
     * we'll need to create them all up front */
    scr_reddesc* reddesc = &scr_reddescs[i];
    if (reddesc->enabled) {
      scr_storedesc* store = scr_reddesc_get_store(reddesc);
      if (store != NULL) {
        /* TODO MEMFS: mount storage for cache directory */

        if (scr_storedesc_dir_create(store, reddesc->directory) != SCR_SUCCESS) {
          scr_abort(-1, "Failed to create cache directory: %s @ %s:%d",
            reddesc->directory, __FILE__, __LINE__
          );
        }

        /* set up artificially node-local directories if the store view is global */
        if (! strcmp(store->view, "GLOBAL")) {
          /* make sure we can create directories */
          if (! store->can_mkdir) {
            scr_abort(-1, "Cannot use global view storage %s without mkdir enabled: @%s:%d",
              store->name, __FILE__, __LINE__
            );
          }

          /* create directory on rank 0 of each node */
          int node_rank;
          MPI_Comm_rank(scr_comm_node, &node_rank);
          if(node_rank == 0) {
            spath* path = spath_from_str(reddesc->directory);
            spath_append_strf(path, "node.%d", scr_my_hostid);
            spath_reduce(path);
            char* path_str = spath_strdup(path);
            spath_delete(&path);
            scr_mkdir(path_str, S_IRWXU | S_IRWXG);
            scr_free(&path_str);
          }
        }
      } else {
        scr_abort(-1, "Invalid store for redundancy descriptor @ %s:%d",
          __FILE__, __LINE__
        );
      }
    }
  }

  /* TODO: should we check for access and required space in cache
   * directories at this point? */

  /* ensure that the control and cache directories are ready */
  MPI_Barrier(scr_comm_world);

  scr_env_init();

  /* place the halt, flush, and nodes files in the prefix directory */
  scr_halt_file = spath_from_str(scr_prefix_scr);
  spath_append_str(scr_halt_file, "halt.scr");

  scr_flush_file = spath_from_str(scr_prefix_scr);
  spath_append_str(scr_flush_file, "flush.scr");

  scr_nodes_file = spath_from_str(scr_prefix_scr);
  spath_append_str(scr_nodes_file, "nodes.scr");

  /* build the file names using the control directory prefix */
  scr_cindex_file = spath_from_str(scr_cntl_prefix);
  spath_append_strf(scr_cindex_file, "cindex.scrinfo", scr_storedesc_cntl->rank);

  /* TODO: should we also record the list of nodes and / or MPI rank to node mapping? */
  /* record the number of nodes being used in this job to the nodes file */
  /* Each rank records its node number in the global scr_my_hostid */
  if (scr_my_rank_world == 0) {
    kvtree* nodes_hash = kvtree_new();
    kvtree_util_set_int(nodes_hash, SCR_NODES_KEY_NODES, num_nodes);
    kvtree_write_path(scr_nodes_file, nodes_hash);
    kvtree_delete(&nodes_hash);
  }

  /* initialize halt info before calling scr_bool_check_halt_and_decrement
   * and set the halt seconds in our halt data structure,
   * this will be overridden if a value is already set in the halt file */
  scr_halt_hash = kvtree_new();

  /* record the halt seconds if they are set */
  if (scr_halt_seconds > 0) {
    kvtree_util_set_unsigned_long(scr_halt_hash, SCR_HALT_KEY_SECONDS, scr_halt_seconds);
  }

  /* sync everyone up */
  MPI_Barrier(scr_comm_world);

  /* now all processes are initialized (be careful when moving this line up or down) */
  scr_initialized = 1;

  /* since we shuffle files around below, stop any ongoing async flush */
  if (scr_flush_async) {
    scr_flush_async_init();
    scr_flush_async_stop();
  }

  /* exit right now if we need to halt */
  scr_bool_check_halt_and_decrement(SCR_TEST_AND_HALT, 0);

  /* if the code is restarting from the parallel file system,
   * disable fetch and enable flush_on_restart */
  if (scr_global_restart) {
    scr_flush_on_restart = 1;
    scr_fetch_bypass = 1;
  }

  /* allocate a new global filemap object */
  scr_cindex = scr_cache_index_new();

  /* leader on each node reads all filemaps and distributes them to other ranks
   * on the node, we take this step in case the number of ranks on this node
   * has changed since the last run */
  scr_cache_index_read(scr_cindex_file, scr_cindex);

  /* delete all files in cache on restart if asked to purge,
   * this is useful during development so the user does not
   * have to manually delete files from all nodes */
  if (scr_purge) {
    /* clear the cache of all files */
    scr_cache_purge(scr_cindex);
  }

  /* delete all datasets listed in the index file if
   * asked to purge the prefix directory */
  if (scr_prefix_purge) {
    scr_prefix_delete_all();
  }

  /* attempt to distribute files for a restart */
  int rc = SCR_FAILURE;
  if (rc != SCR_SUCCESS && scr_distribute) {
    /* distribute and rebuild files in cache,
     * sets scr_dataset_id and scr_checkpoint_id upon success */
    rc = scr_cache_rebuild(scr_cindex);

    /* if distribute succeeds, check whether we should flush on restart */
    if (rc == SCR_SUCCESS) {
      /* since the flush file is not deleted between job allocations,
       * we need to rebuild it based on what's currently in cache,
       * if the rebuild failed, we'll delete the flush file after purging the cache below */
      scr_flush_file_rebuild(scr_cindex);

      /* iterate over all datasets in cache and
       * check whether each needs to be flushed */
      scr_flush_restart(scr_cindex);
    }
  }

  /* TODO: there is some risk here of cleaning the cache when we shouldn't
   * if given a badly placed nodeset for a restart job step within an
   * allocation with lots of spares. */

  /* if the distribute fails, or if the code must restart from the parallel
   * file system, clear the cache */
  if (rc != SCR_SUCCESS || scr_global_restart) {
    /* clear the cache of all files */
    scr_cache_purge(scr_cindex);
    scr_dataset_id    = 0;
    scr_checkpoint_id = 0;
    scr_ckpt_dset_id  = 0;

    /* delete the flush file which may be stale */
    scr_flush_file_rebuild(scr_cindex);
  }

  /* attempt to fetch files from parallel file system */
  int fetch_attempted = 0;
  if ((rc != SCR_SUCCESS || scr_global_restart) && scr_fetch) {
    /* sets scr_dataset_id and scr_checkpoint_id upon success */
    rc = scr_fetch_latest(scr_cindex, &fetch_attempted);
    if (scr_my_rank_world == 0) {
      scr_dbg(2, "scr_fetch_latest attempted on restart");
    }
  }

  /* TODO: there is some risk here of cleaning the cache when we shouldn't
   * if given a badly placed nodeset for a restart job step within an
   * allocation with lots of spares. */

  /* if the fetch fails, lets clear the cache */
  if (rc != SCR_SUCCESS) {
    /* clear the cache of all files */
    scr_cache_purge(scr_cindex);
    scr_dataset_id    = 0;
    scr_checkpoint_id = 0;
    scr_ckpt_dset_id  = 0;
  }

  /* both the distribute and the fetch failed */
  if (rc != SCR_SUCCESS) {
    /* if a fetch was attempted but failed, print a warning */
    if (scr_my_rank_world == 0 && fetch_attempted) {
      scr_err("Failed to fetch checkpoint set into cache. Restarting from the beginning @ %s:%d",
        __FILE__, __LINE__
      );
    }
    /* We are restarting from the trivial checkpoint (full restart) */
    rc = SCR_SUCCESS;
  }

  /* set flag depending on whether checkpoint_id is greater than 0,
   * we'll take this to mean that we have a checkpoint in cache */
  scr_have_restart = (scr_checkpoint_id > 0);

  /* sync everyone before returning to ensure that subsequent
   * calls to SCR functions are valid */
  MPI_Barrier(scr_comm_world);

  /* start the clocks for measuring the compute time and time of last checkpoint */
  if (scr_my_rank_world == 0) {
    /* set the checkpoint end time, we use this time in Need_checkpoint */
    scr_time_checkpoint_end = MPI_Wtime();

    /* start the clock for measuring the compute time */
    scr_time_compute_start = scr_time_checkpoint_end;

    /* log the start time of this compute phase */
    if (scr_log_enable) {
      scr_log_event("COMPUTE_START", NULL, NULL, NULL, NULL, NULL);
    }
  }

  /* all done, ready to go */
  return rc;
}

/* Close down and clean up */
int SCR_Finalize()
{
  /* manage state transition */
  if (scr_state != SCR_STATE_IDLE) {
    scr_state_transition_error(scr_state, "SCR_Finalize()", __FILE__, __LINE__);
  }
  scr_state = SCR_STATE_UNINIT;

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }

  /* bail out if not initialized -- will get bad results */
  if (! scr_initialized) {
    scr_abort(-1, "SCR has not been initialized @ %s:%d", __FILE__, __LINE__);
    return SCR_FAILURE;
  }

  /* this is not required, but it helps ensure apps
   * are calling this as a collective */
  MPI_Barrier(scr_comm_world);

#if 0
  /* free user hash if one was allocated */
  kvtree_delete(&scr_app_hash);
#endif

  if (scr_my_rank_world == 0) {
    /* stop the clock for measuring the compute time */
    scr_time_compute_end = MPI_Wtime();

    /* if we reach SCR_Finalize, assume that we should not restart the job */
    scr_halt(SCR_FINALIZE_CALLED);
  }

  /* flush any pending datasets and shut down flush methods */
  scr_flush_finalize();

  /* free off the memory allocated for our descriptors */
  scr_reddescs_free();
  scr_storedescs_free();
  scr_groupdescs_free();

  /* delete the descriptor hashes */
  kvtree_delete(&scr_reddesc_hash);
  kvtree_delete(&scr_storedesc_hash);
  kvtree_delete(&scr_groupdesc_hash);

  /* Free memory cache of a halt file */
  kvtree_delete(&scr_halt_hash);

  /* free off our global filemap object */
  scr_filemap_delete(&scr_map);

  /* free off our global filemap object */
  scr_cache_index_delete(&scr_cindex);

  /* disconnect from database */
  if (scr_my_rank_world == 0 && scr_log_enable) {
    scr_log_finalize();
  }

#ifdef HAVE_LIBPMIX
  /* sync procs in pmix before shutdown */
  int retval = PMIx_Fence(NULL, 0, NULL, 0);
  if (retval != PMIX_SUCCESS) {
    scr_err("PMIx_Fence failed: rc=%d, rank: %d @ %s:%d",
      retval, scr_pmix_proc.rank, __FILE__, __LINE__
    );
  }

  /* shutdown pmix */
  retval = PMIx_Finalize(NULL, 0);
  if (retval != PMIX_SUCCESS) {
    scr_err("PMIx_Finalize failed: rc=%d, rank: %d @ %s:%d",
      retval, scr_pmix_proc.rank, __FILE__, __LINE__
    );
  }
#endif /* HAVE_LIBPMIX */

  /* shut down the AXL library */
  int axl_rc = AXL_Finalize_comm(scr_comm_world);
  if (axl_rc != AXL_SUCCESS) {
    scr_abort(-1, "Failed to finalize AXL library @ %s:%d",
        __FILE__, __LINE__
        );
  }

  /* shut down the ER library */
  int er_rc = ER_Finalize();
  if (er_rc != ER_SUCCESS) {
    scr_abort(-1, "Failed to finalize ER library @ %s:%d",
      __FILE__, __LINE__
    );
  }

  /* shut down the DTCMP library if we're using it */
  int dtcmp_rc = DTCMP_Finalize();
  if (dtcmp_rc != DTCMP_SUCCESS) {
    scr_abort(-1, "Failed to finalized DTCMP library @ %s:%d",
      __FILE__, __LINE__
    );
  }

  /* free memory allocated for variables */
  scr_free(&scr_flush_type);
  scr_free(&scr_fetch_current);
  scr_free(&scr_log_db_host);
  scr_free(&scr_log_db_user);
  scr_free(&scr_log_db_pass);
  scr_free(&scr_log_db_name);
  scr_free(&scr_username);
  scr_free(&scr_jobid);
  scr_free(&scr_jobname);
  scr_free(&scr_clustername);
  scr_free(&scr_group);
  scr_free(&scr_prefix_scr);
  scr_free(&scr_prefix);
  scr_free(&scr_cntl_prefix);
  scr_free(&scr_cntl_base);
  scr_free(&scr_cache_base);
  scr_free(&scr_my_hostname);

  spath_delete(&scr_cindex_file);
  spath_delete(&scr_nodes_file);
  spath_delete(&scr_flush_file);
  spath_delete(&scr_halt_file);
  spath_delete(&scr_prefix_path);

  /* free off the library's communicators */
  if (scr_comm_node != MPI_COMM_NULL) {
    MPI_Comm_free(&scr_comm_node);
  }
  if (scr_comm_world != MPI_COMM_NULL) {
    MPI_Comm_free(&scr_comm_world);
  }

  /* we're no longer in an initialized state */
  scr_initialized = 0;

  return SCR_SUCCESS;
}

/* sets or gets a configuration option */
const char* SCR_Config(const char* config_string)
{
  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return NULL;
  }

  /* TODO: how do we clean this up if user does not call Init/Finalize */
  /* since SCR_Init has not been called yet,
   * we need to create a context for the library */
  if (scr_comm_world == MPI_COMM_NULL) {
    MPI_Comm_dup(MPI_COMM_WORLD,  &scr_comm_world);
    MPI_Comm_rank(scr_comm_world, &scr_my_rank_world);
    MPI_Comm_size(scr_comm_world, &scr_ranks_world);
  }

  /* ensure all ranks specified identical value for config_string */
  char* tmpstr = NULL;
  if (scr_my_rank_world == 0 && config_string != NULL) {
    tmpstr = strdup(config_string);
  }
  scr_str_bcast(&tmpstr, 0, scr_comm_world);
  if ((config_string == NULL && tmpstr != NULL) ||
      (config_string != NULL && tmpstr == NULL) ||
      (config_string != NULL && tmpstr != NULL && strcmp(config_string, tmpstr) != 0))
  {
    scr_abort(-1, "SCR_Config string must be identical on all processes @ %s:%d",
      __FILE__, __LINE__
    );
  }
  scr_free(&tmpstr);

  /* this is not required, but it helps ensure apps
   * are calling this as a collective */
  MPI_Barrier(scr_comm_world);

  if (config_string == NULL || strlen(config_string) == 0) {
    return NULL;
  }

#if 0
  /* allocate a hash to record params set through SCR_Config */
  if (scr_app_hash == NULL) {
    scr_app_hash = kvtree_new();
  }
#endif

  /* create directory to hold app config file */
  if (scr_my_rank_world == 0) {
    /* get the prefix directory */
    char* value = getenv("SCR_PREFIX");
    spath* prefix_path = scr_get_prefix(value);
    spath_append_str(prefix_path, ".scr");
    const char* dirname = spath_strdup(prefix_path);
    spath_delete(&prefix_path);

    /* create the directory */
    mode_t mode_dir = scr_getmode(1, 1, 1);
    if (scr_mkdir(dirname, mode_dir) != SCR_SUCCESS) {
      scr_abort(-1, "Failed to create directory %s @ %s:%d",
        dirname, __FILE__, __LINE__
      );
    }

    scr_free(&dirname);
  }
  MPI_Barrier(scr_comm_world);

  /* read in our configuration parameters */
  scr_param_init();

  /* after parsing these values will hold values like the following:
   *
   * given a string like "SCR_PREFIX"
   *   toplevel_key   = "SCR_PREFIX"
   *   toplevel_value = NULL
   *   key            = "SCR_PREFIX" (N/A)
   *   value_hash     = NULL
   *   is_query       = 1
   *
   * given a string like "SCR_PREFIX="
   *   toplevel_key   = "SCR_PREFIX"
   *   toplevel_value = NULL
   *   key            = "SCR_PREFIX" (N/A)
   *   value_hash     = NULL
   *   is_query       = 0
   *
   * given a string like "SCR_PREFIX=/path/to/prefix"
   *   toplevel_key   = "SCR_PREFIX"
   *   toplevel_value = "/path/to/prefix"
   *   key            = NULL
   *   value_hash     = NULL
   *   is_query       = 0
   *
   * given an string like "CKPT=0 TYPE"
   *   toplevel_key   = "CKPT"
   *   toplevel_value = "0"
   *   key            = "TYPE"
   *   value_hash     = NULL
   *   is_query       = 1
   *
   * given an string like "CKPT=0 TYPE=XOR STORE=/tmp"
   *   toplevel_key   = "CKPT"
   *   toplevel_value = "0"
   *   key            = NULL
   *   value_hash     = TYPE
   *                      0
   *                    STORE
   *                      /tmp
   *   is_query       = 0
   */
  char* toplevel_key   = NULL;
  char* toplevel_value = NULL;
  char* key            = NULL;
  kvtree* value_hash   = NULL;
  int is_query         = -1; /* basically tracks if I have seen a '=' but no value yet */

  /* make a copy of the input string, so we can modify it */
  char* writable_config_string = strdup(config_string);
  assert(writable_config_string);

  /* this is a small state machine to parse name=value pairs of settings,
   * and while I could encode all of this as a table of transitions, it seems
   * that people are usually unhappy with the table form and like an explicit
   * set of case / if better. */
  char* value = NULL;
  char* conf = writable_config_string;
  enum states {before_key, in_key, after_key, before_value, in_value, done};
  int state = before_key;
  while (state != done) {
    switch(state) {
      case before_key:
        if (*conf == '\0') {
          state = done;
        } else if (*conf == ' ') {
          state = before_key;
        } else {
          key = conf;
          if (toplevel_key == NULL) {
            toplevel_key = key;
          }
          state = in_key;
          is_query = 1;
        }
        break;
      case in_key:
        if (*conf == '\0') {
          state = done;
        } else if (*conf == ' ') {
          *conf = '\0';
          state = after_key;
        } else if (*conf == '=') {
          *conf = '\0';
          state = before_value;
          is_query = 0;
        }
        break;
      case after_key:
        if (*conf == '\0') {
          state = done;
        } else if (*conf == '=') {
          state = before_value;
          is_query = 0;
        } else if (*conf == ' ') {
          state = after_key;
        } else {
          scr_abort(-1, "Invalid configuration string '%s' @ %s:%d",
            config_string, __FILE__, __LINE__
          );
        }
        break;
      case before_value:
        if (*conf == '\0') {
          state = done;
        } else if (*conf == ' ') {
          state = before_value;
        } else if (*conf == '=') {
          scr_abort(-1, "Invalid configuration string '%s' @ %s:%d",
            config_string, __FILE__, __LINE__
          );
        } else {
          value = conf;
          if (toplevel_value == NULL) {
            /* we use the first value as the top level value */
            toplevel_value = value;
          } else if (value_hash == NULL) {
            /* starting a value after we already have a top level value,
             * so we're at a point like "CKPT=0 TYPE=<char>", need to create
             * a hash to hold the rest */
            value_hash = kvtree_new();
          }
          state = in_value;
        }
        break;
      case in_value:
        if (*conf == '\0') {
          state = done;
        } else if (*conf == ' ') {
          *conf = '\0';
          state = before_key;
        } else if (*conf == '=') {
          scr_abort(-1, "Invalid configuration string '%s' @ %s:%d",
            config_string, __FILE__, __LINE__
          );
        } else {
          state = in_value;
        }

        /* check whether we have just completed a value string */
        if (state != in_value) {
          /* just finished a value string, if we have a value_hash, insert new key/value */
          if (value_hash) {
            kvtree_set_kv(value_hash, key, value);
          }
          key   = NULL;
          value = NULL;
        }
        break;
    }

    conf += 1;
  }

  /* done parsing, now actually do something */
  assert(is_query != -1);

  /* sanity checks */
  if (toplevel_key == NULL) {
    scr_abort(-1,
      "Could not extract key from config string. '%s' @ %s:%d",
      config_string, __FILE__, __LINE__
    );
  }
  assert(toplevel_key);

  const char* retval = NULL;
  if (is_query) {
    /* user wants to query the current value of a setting */
    if (value_hash) {
      scr_abort(-1,
        "Cannot get config options at same time as setting them. '%s' @ %s:%d",
        config_string, __FILE__, __LINE__
      );
    }
    assert(value_hash == NULL);

    /* lookup the value for the given parameter */
    if (toplevel_value == NULL) {
      /* user is trying to query for the value of a simple key/value pair,
       * given a parameter name like "SCR_PREFIX" */
      kvtree* toplevel_hash = (kvtree*) scr_param_get_hash(toplevel_key);
      if (kvtree_size(toplevel_hash) <= 1) {
        /* the value string is well defined if we have at most one value */
        const char* value = scr_param_get(toplevel_key);
        if (value != NULL) {
          /* found a setting for this parameter, strdup and return it */
          retval = strdup(value);
        }
      } else {
        /* this key has multiple values set, so it's not clear which to return */
        if (scr_my_rank_world == 0) {
          scr_err("Multiple values are set for '%s', so query return value is ill-defined @ %s:%d",
            toplevel_key, __FILE__, __LINE__
          );
        }
      }
      kvtree_delete(&toplevel_hash);
    } else {
      /* user is trying to query the subvalue of a two-level parameter
       * given an input like "CKPT=0 TYPE" with
       *   toplevel_key   = CKPT
       *   toplevel_value = 0
       *   key            = TYPE */
      kvtree* toplevel_hash = (kvtree*) scr_param_get_hash(toplevel_key);
      if (toplevel_hash) {
        const kvtree* toplevel_value_hash = kvtree_get(toplevel_hash, toplevel_value);
        if (toplevel_value_hash) {
          const char* value = kvtree_elem_get_first_val(toplevel_value_hash, key);
          if (value != NULL) {
            /* found a setting for this parameter, strdup and return it */
            retval = strdup(value);
          }
        }
      }
      kvtree_delete(&toplevel_hash);
    }
  } else {
    /* user wants to set or unset a parameter,
     * only allow changing parameters before SCR_Init */
    if (scr_state != SCR_STATE_UNINIT) {
      scr_state_transition_error(scr_state, "SCR_Config()", __FILE__, __LINE__);
    }

    /* determine whether we have a simple key/value pair or a two-level param */
    if (value_hash == NULL) {
      /* dealing with a simple key/value parameter pair */
      if (toplevel_value) {
        /* user want to set a value has given a
         * string like "SCR_PREFIX=/path/to/prefix" */
        scr_param_set(toplevel_key, toplevel_value);
      } else {
        /* user wants to unset a value has given
         * a string like "SCR_PREFIX=" */
        scr_param_set_hash(toplevel_key, NULL);
      }
    } else {
      /* user wants to set or unset a two-level parameter
       * as in CKPT=0 TYPE=XOR */

      /* lookup hash for top level key, as in "CKPT" */
      kvtree* toplevel_hash = (kvtree*)scr_param_get_hash(toplevel_key);
      if (toplevel_hash == NULL) {
        /* did not find an existing hash for the given key, so create a new one */
        toplevel_hash = kvtree_new();
      }

      /* lookup hash for top level value, as in "0" */
      kvtree* toplevel_value_hash = kvtree_get(toplevel_hash, toplevel_value);
      if (toplevel_value_hash == NULL) {
        toplevel_value_hash = kvtree_set(toplevel_hash, toplevel_value, kvtree_new());
      }

      /* copy hash provided by user into two-level parameter */
      kvtree_merge(toplevel_value_hash, value_hash);

      /* need to overwrite the full toplevel hash since there is no function to
       * modify it */
      scr_param_set_hash(toplevel_key, toplevel_hash);

      kvtree_delete(&value_hash);

      /* do not free toplevel_hash since I passed ownership to the parameter code */
    }
  }

  free(writable_config_string);

  scr_param_finalize();

  return retval;
}

const char* SCR_Configf(const char* format, ...)
{
  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return NULL;
  }

  /* compute the size of the string we need to allocate */
  va_list args;
  va_start(args, format);
  int size = vsnprintf(NULL, 0, format, args) + 1;
  va_end(args);

  /* allocate and print the string */
  char* str = (char*) SCR_MALLOC(size);
  va_start(args, format);
  vsnprintf(str, size, format, args);
  va_end(args);

  /* delegate work to SCR_Config and get result */
  const char* ret = SCR_Config(str);

  /* free the temporary string */
  scr_free(&str);

  return ret;
}

/* sets flag to 1 if a checkpoint should be taken, flag is set to 0 otherwise */
int SCR_Need_checkpoint(int* flag)
{
  /* manage state transition */
  if (scr_state != SCR_STATE_IDLE) {
    scr_state_transition_error(scr_state, "SCR_Need_checkpoint()", __FILE__, __LINE__);
  }

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    *flag = 0;
    return SCR_FAILURE;
  }

  /* say no if not initialized */
  if (! scr_initialized) {
    *flag = 0;
    scr_abort(-1, "SCR has not been initialized @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* this is not required, but it helps ensure apps
   * are calling this as a collective */
  MPI_Barrier(scr_comm_world);

  /* track the number of times a user has called SCR_Need_checkpoint */
  scr_need_checkpoint_count++;

  /* assume we don't need to checkpoint */
  *flag = 0;

  /* check whether a halt condition is active (don't halt,
   * just be sure to return 1 in this case) */
  if (!*flag && scr_bool_check_halt_and_decrement(SCR_TEST_BUT_DONT_HALT, 0)) {
    *flag = 1;
  }

  /* have rank 0 make the decision and broadcast the result */
  if (scr_my_rank_world == 0) {
    /* TODO: account for MTBF, time to flush, etc. */
    /* if we don't need to halt, check whether we can afford to checkpoint */

    /* if checkpoint interval is set, check the current checkpoint id */
    if (!*flag && scr_checkpoint_interval > 0 && scr_need_checkpoint_count % scr_checkpoint_interval == 0) {
      *flag = 1;
    }

    /* if checkpoint seconds is set, check the time since the last checkpoint */
    if (!*flag && scr_checkpoint_seconds > 0) {
      double now_seconds = MPI_Wtime();
      if ((int)(now_seconds - scr_time_checkpoint_end) >= scr_checkpoint_seconds) {
        *flag = 1;
      }
    }

    /* check whether we can afford to checkpoint based on the max allowed
     * checkpoint overhead, if set */
    if (!*flag && scr_checkpoint_overhead > 0) {
      /* TODO: could init the cost estimate via environment variable or
       * stats from previous run */
      if (scr_time_checkpoint_count == 0) {
        /* if we haven't taken a checkpoint, we need to take one in order
         * to get a cost estimate */
        *flag = 1;
      } else if (scr_time_checkpoint_count > 0) {
        /* based on average time of checkpoint, current time, and time
         * that last checkpoint ended, determine overhead of checkpoint
         * if we took one right now */
        double now = MPI_Wtime();
        double avg_cost = scr_time_checkpoint_total / (double) scr_time_checkpoint_count;
        double percent_cost = avg_cost / (now - scr_time_checkpoint_end + avg_cost) * 100.0;

        /* if our current percent cost is less than allowable overhead,
         * indicate that it's time for a checkpoint */
        if (percent_cost < scr_checkpoint_overhead) {
          *flag = 1;
        }
      }
    }

    /* no way to determine whether we need to checkpoint, so always say yes */
    if (!*flag &&
        scr_checkpoint_interval <= 0 &&
        scr_checkpoint_seconds  <= 0 &&
        scr_checkpoint_overhead <= 0)
    {
      *flag = 1;
    }
  }

  /* rank 0 broadcasts the decision */
  MPI_Bcast(flag, 1, MPI_INT, 0, scr_comm_world);

  return SCR_SUCCESS;
}

/* inform library that a new output dataset is starting */
int SCR_Start_output(const char* name, int flags)
{
  /* manage state transition */
  if (scr_state != SCR_STATE_IDLE) {
    scr_state_transition_error(scr_state, "SCR_Start_output()", __FILE__, __LINE__);
  }
  scr_state = SCR_STATE_OUTPUT;

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }

  /* bail out if not initialized -- will get bad results */
  if (! scr_initialized) {
    scr_abort(-1, "SCR has not been initialized @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* delegate the rest to start_output */
  return scr_start_output(name, flags);
}

/* informs SCR that a fresh checkpoint set is about to start */
int SCR_Start_checkpoint()
{
  /* manage state transition */
  if (scr_state != SCR_STATE_IDLE) {
    scr_state_transition_error(scr_state, "SCR_Start_checkpoint()", __FILE__, __LINE__);
  }
  scr_state = SCR_STATE_CHECKPOINT;

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }

  /* bail out if not initialized -- will get bad results */
  if (! scr_initialized) {
    scr_abort(-1, "SCR has not been initialized @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* delegate the rest to start_output */
  return scr_start_output(NULL, SCR_FLAG_CHECKPOINT);
}

/* given a filename, return the full path to the file which the user should write to */
int SCR_Route_file(const char* file, char* newfile)
{
  /* manage state transition */
  if (scr_state != SCR_STATE_RESTART    &&
      scr_state != SCR_STATE_CHECKPOINT &&
      scr_state != SCR_STATE_OUTPUT)
  {
    /* Route does not fail outside a start/complete pair,
       instead it returns a copy of the original filename */
    scr_dbg(3, "SCR_Route_file() called outside of a Start/Complete pair @ %s:%d",
            __FILE__, __LINE__);

    /* check that we got a file and newfile to write to */
    if (file == NULL || strcmp(file,"") == 0 || newfile == NULL) {
        return SCR_FAILURE;
    }

    /* check that user's filename is not too long */
    if (strlen(file) >= SCR_MAX_FILENAME) {
        scr_abort(-1, "file name (%s) is longer than SCR_MAX_FILENAME (%d) @ %s:%d",
                  file, SCR_MAX_FILENAME, __FILE__, __LINE__
                  );
    }

    /* return a copy of given file name */
    strncpy(newfile, file, SCR_MAX_FILENAME);
    return SCR_SUCCESS;
  }

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }

  /* bail out if not initialized -- will get bad results */
  if (! scr_initialized) {
    scr_abort(-1, "SCR has not been initialized @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* route the file based on current redundancy descriptor */
  int n = SCR_MAX_FILENAME;
  if (scr_route_file(scr_dataset_id, file, newfile, n) != SCR_SUCCESS) {
    return SCR_FAILURE;
  }

  /* if we are in a new dataset, record this file in our filemap,
   * otherwise, we are likely in a restart, so check whether the file exists */
  if (scr_in_output) {
    /* TODO: to avoid duplicates, check that the file is not already in the filemap,
     * at the moment duplicates just overwrite each other, so there's no harm */

    /* add the file to the filemap */
    scr_filemap_add_file(scr_map, newfile);

    /* read meta data for this file */
    scr_meta* meta = scr_meta_new();
    scr_filemap_get_meta(scr_map, newfile, meta);

    /* set parameters for the file */
    scr_meta_set_complete(meta, 0);
    /* TODO: move the ranks field elsewhere, for now it's needed by scr_index.c */
    scr_meta_set_ranks(meta, scr_ranks_world);
    scr_meta_set_orig(meta, file);

    /* build absolute path to file */
    spath* path_abs = spath_from_str(file);
    if (! spath_is_absolute(path_abs)) {
      /* the path is not absolute, so prepend the current working directory */
      char cwd[SCR_MAX_FILENAME];
      if (scr_getcwd(cwd, sizeof(cwd)) == SCR_SUCCESS) {
        spath_prepend_str(path_abs, cwd);
      } else {
        /* problem acquiring current working directory */
        scr_abort(-1, "Failed to build absolute path to %s @ %s:%d",
          file, __FILE__, __LINE__
        );
      }
    }

    /* simplify the absolute path (removes "." and ".." entries) */
    spath_reduce(path_abs);

    /* check that file is somewhere under prefix */
    if (! spath_is_child(scr_prefix_path, path_abs)) {
      /* found a file that's outside of prefix, throw an error */
      char* path_abs_str = spath_strdup(path_abs);
      scr_abort(-1, "File `%s' must be under SCR_PREFIX `%s' @ %s:%d",
        path_abs_str, scr_prefix, __FILE__, __LINE__
      );
    }

    /* cut absolute path into direcotry and file name */
    spath* path_name = spath_cut(path_abs, -1);

    /* store the full path and name of the original file */
    char* path = spath_strdup(path_abs);
    char* name = spath_strdup(path_name);
    scr_meta_set_origpath(meta, path);
    scr_meta_set_origname(meta, name);

    /* TODO: would be nice to limit mkdir ops here */
    /* if we're in bypass mode, we need to be sure directory exists
     * for this file before user starts to write to it */
    if (scr_rd->bypass) {
      mode_t mode_dir = scr_getmode(1, 1, 1);
      if (scr_mkdir(path, mode_dir) != SCR_SUCCESS) {
        scr_abort(-1, "Failed to create directory %s @ %s:%d",
          path, __FILE__, __LINE__
        );
      }
    }

    /* free full path and name of original file */
    scr_free(&name);
    scr_free(&path);

    /* free directory and file name paths */
    spath_delete(&path_name);
    spath_delete(&path_abs);

    /* record the meta data for this file */
    scr_filemap_set_meta(scr_map, newfile, meta);

    /* write out the filemap */
    scr_cache_set_map(scr_cindex, scr_dataset_id, scr_map);

    /* delete the meta data object */
    scr_meta_delete(&meta);
  } else {
    /* if user specified path to file within prefix, return */
    if (scr_file_is_readable(newfile) == SCR_SUCCESS) {
      return SCR_SUCCESS;
    }

    /* TODO: To support backwards compatibility, the user is allowed
     * to pass just the file name with no path component during restart.
     * This means that they cannot have two files in the same checkpoint
     * with the same basename even if those files would be in two
     * different directories, e.g., one can't do something like:
     *   ckpt.1.root
     *   ckpt.1/ckpt.1.root
     *
     * With bypass, we need to figure out which directory the file is
     * in, so we have to scan through the filemap to find a match on
     * the basename.
     *
     * The proper fix would be to force users to include path components
     * even in restart.  This would make route_file symmetric in output
     * and restart, which is better.  It would take a step in enabling
     * two files with the same basename but in different directories.
     * However, it also requires that users names their checkpoints,
     * so SCR_Start_checkpoint must be deprecated ro changed to take a
     * name argument. */

    /* compute basename of new file */
    spath* path = spath_from_str(newfile);
    spath_basename(path);
    char* newfilebase = spath_strdup(path);
    spath_delete(&path);

    /* get the filemap for this checkpoint */
    scr_filemap* map = scr_filemap_new();
    scr_cache_get_map(scr_cindex, scr_dataset_id, map);

    /* loop over each file in the map */
    int found_file = 0;
    kvtree_elem* file_elem;
    for (file_elem = scr_filemap_first_file(map);
         file_elem != NULL;
         file_elem = kvtree_elem_next(file_elem))
    {
      /* get the filename */
      char* mapfile = kvtree_elem_key(file_elem);

      /* get meta data for this file */
      scr_meta* meta = scr_meta_new();
      if (scr_filemap_get_meta(map, mapfile, meta) == SCR_SUCCESS) {
        /* lookup basename for this file from meta data */
        char* origname = NULL;
        if (scr_meta_get_origname(meta, &origname) == SCR_SUCCESS) {
          /* check whether basename in meta matches basename of input file */
          if (strcmp(origname, newfilebase) == 0) {
            /* found a matching base name in our file map,
             * overwrite output file path in newfile with
             * full path to checkpoint file */
            strncpy(newfile, mapfile, SCR_MAX_FILENAME);
            found_file = 1;
          }
        }
      }
      scr_meta_delete(&meta);

      /* stop looping early if we found the file */
      if (found_file) {
        break;
      }
    }

    /* free the filemap */
    scr_filemap_delete(&map);

    /* free the base name of new file */
    scr_free(&newfilebase);

    /* return an error if we failed to find the basename in the file map */
    if (! found_file) {
      return SCR_FAILURE;
    }

    /* if we can't read the file, return an error */
    if (scr_file_is_readable(newfile) != SCR_SUCCESS) {
      return SCR_FAILURE;
    }
  }

  return SCR_SUCCESS;
}

/* inform library that the current dataset is complete */
int SCR_Complete_output(int valid)
{
  /* manage state transition */
  if (scr_state != SCR_STATE_OUTPUT) {
    scr_abort(-1, "Must call SCR_Start_output() before SCR_Complete_output() @ %s:%d",
      __FILE__, __LINE__
    );
  }
  scr_state = SCR_STATE_IDLE;

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }

  /* bail out if not initialized -- will get bad results */
  if (! scr_initialized) {
    scr_abort(-1, "SCR has not been initialized @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  return scr_complete_output(valid);
}

/* completes the checkpoint set and marks it as valid or not */
int SCR_Complete_checkpoint(int valid)
{
  /* manage state transition */
  if (scr_state != SCR_STATE_CHECKPOINT) {
    scr_abort(-1, "Must call SCR_Start_checkpoint() before SCR_Complete_checkpoint() @ %s:%d",
      __FILE__, __LINE__
    );
  }
  scr_state = SCR_STATE_IDLE;

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }

  /* bail out if not initialized -- will get bad results */
  if (! scr_initialized) {
    scr_abort(-1, "SCR has not been initialized @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  return scr_complete_output(valid);
}

/* determine whether SCR has a restart available to read,
 * and get name of restart if one is available */
int SCR_Have_restart(int* flag, char* name)
{
  /* manage state transition */
  if (scr_state != SCR_STATE_IDLE) {
    scr_state_transition_error(scr_state, "SCR_Have_restart()", __FILE__, __LINE__);
  }

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    *flag = 0;
    return SCR_FAILURE;
  }

  /* say no if not initialized */
  if (! scr_initialized) {
    *flag = 0;
    scr_abort(-1, "SCR has not been initialized @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* this is not required, but it helps ensure apps
   * are calling this as a collective */
  MPI_Barrier(scr_comm_world);

  /* TODO: a more proper check would be to examine the filemap, perhaps across ranks */

  /* set flag depending on whether checkpoint_id is greater than 0,
   * we'll take this to mean that we have a checkpoint in cache */
  *flag = scr_have_restart;

  /* read dataset name from filemap */
  if (scr_have_restart) {
    if (name != NULL) {
      char* dset_name;
      scr_dataset* dataset = scr_dataset_new();
      scr_cache_index_get_dataset(scr_cindex, scr_ckpt_dset_id, dataset);
      scr_dataset_get_name(dataset, &dset_name);
      strncpy(name, dset_name, SCR_MAX_FILENAME);
      scr_dataset_delete(&dataset);
    }
  }

  return SCR_SUCCESS;
}

/* inform library that restart is starting, get name of restart that is available */
int SCR_Start_restart(char* name)
{
  /* manage state transition */
  if (scr_state != SCR_STATE_IDLE) {
    scr_state_transition_error(scr_state, "SCR_Start_restart()", __FILE__, __LINE__);
  }
  scr_state = SCR_STATE_RESTART;

  /* only valid to call this if we have a checkpoint to restart from */
  if (! scr_have_restart) {
    scr_abort(-1, "Can only call SCR_Start_restart() if SCR_Have_restart() indicates a checkpoint is available @ %s:%d",
      __FILE__, __LINE__
    );
  }

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }

  /* bail out if not initialized -- will get bad results */
  if (! scr_initialized) {
    scr_abort(-1, "SCR has not been initialized @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* this is not required, but it helps ensure apps
   * are calling this as a collective */
  MPI_Barrier(scr_comm_world);

  /* bail out if there is no checkpoint to restart from */
  if (! scr_have_restart) {
    scr_abort(-1, "SCR has no checkpoint for restart @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* read dataset name from filemap */
  if (name != NULL) {
    char* dset_name;
    scr_dataset* dataset = scr_dataset_new();
    scr_cache_index_get_dataset(scr_cindex, scr_ckpt_dset_id, dataset);
    scr_dataset_get_name(dataset, &dset_name);
    strncpy(name, dset_name, SCR_MAX_FILENAME);
    scr_dataset_delete(&dataset);
  }

  return SCR_SUCCESS;
}

/* inform library that the current restart is complete */
int SCR_Complete_restart(int valid)
{
  /* manage state transition */
  if (scr_state != SCR_STATE_RESTART) {
    scr_abort(-1, "Must call SCR_Start_restart() before SCR_Complete_restart() @ %s:%d",
      __FILE__, __LINE__
    );
  }
  scr_state = SCR_STATE_IDLE;

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }

  /* bail out if not initialized -- will get bad results */
  if (! scr_initialized) {
    scr_abort(-1, "SCR has not been initialized @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* turn off our restart flag */
  scr_have_restart = 0;

  /* since we have no output flag to return to user whether all procs
   * passed in valid=1, we'll overload the return code for that purpose,
   * this should eventually be changed to use an output flag instead */
  int rc = SCR_SUCCESS;

  /* check that all procs read valid data */
  if (! scr_alltrue(valid, scr_comm_world)) {
    /* if some process fails, attempt to restart from
     * the next most recent checkpoint and cycle through
     * the have/start/complete restart calls again,
     * we should also record this current checkpoint as failed in the
     * index file so that we don't fetch it again*/

    /* use the return code to indicate that some process failed to
     * read its checkpoint file */
    rc = SCR_FAILURE;

    /* mark current checkpoint as bad in our index file so that
     * we don't attempt to fetch it again */
    if (scr_my_rank_world == 0) {
      /* get dataset for current id */
      scr_dataset* dataset = scr_dataset_new();
      scr_cache_index_get_dataset(scr_cindex, scr_dataset_id, dataset);

      /* get name of current dataset */
      char* name;
      scr_dataset_get_name(dataset, &name);

      /* read the index file */
      kvtree* index_hash = kvtree_new();
      if (scr_index_read(scr_prefix_path, index_hash) == SCR_SUCCESS) {
        /* if there is an entry for this dataset in the index,
         * mark it as failed so we don't try to restart it with it again */
        int id;
        if (scr_index_get_id_by_name(index_hash, name, &id) == SCR_SUCCESS) {
          /* found an entry, mark it as failed and update index file */
          scr_index_unset_current(index_hash);
          scr_index_mark_failed(index_hash, id, name);
          scr_index_write(scr_prefix_path, index_hash);
        }
      }
      kvtree_delete(&index_hash);

      /* free our dataset object */
      scr_dataset_delete(&dataset);
    }

    /* delete the current (bad) checkpoint from cache */
    scr_cache_delete(scr_cindex, scr_dataset_id);
    scr_dataset_id    = 0;
    scr_checkpoint_id = 0;
    scr_ckpt_dset_id  = 0;

    /* get ordered list of datasets we have in our cache */
    int ndsets;
    int* dsets;
    scr_cache_index_list_datasets(scr_cindex, &ndsets, &dsets);

    int found_checkpoint = 0;

    /* loop backwards through datasets looking for most recent checkpoint */
    int idx = ndsets - 1;
    while (idx >= 0 && scr_checkpoint_id == 0) {
      /* get next most recent dataset */
      int current_id = dsets[idx];

      /* get dataset for this id */
      scr_dataset* dataset = scr_dataset_new();
      scr_cache_index_get_dataset(scr_cindex, current_id, dataset);

      /* see if we have a checkpoint */
      int is_ckpt = scr_dataset_is_ckpt(dataset);
      if (is_ckpt) {
        /* if we rebuild any checkpoint, return success */
        found_checkpoint = 1;

        /* if id of dataset we just rebuilt is newer,
         * update scr_dataset_id */
        if (current_id > scr_dataset_id) {
          scr_dataset_id = current_id;
        }

        /* get checkpoint id for dataset */
        int ckpt_id;
        scr_dataset_get_ckpt(dataset, &ckpt_id);

        /* if checkpoint id of dataset we just rebuilt is newer,
         * update scr_checkpoint_id and scr_ckpt_dset_id */
        if (ckpt_id > scr_checkpoint_id) {
          /* got a more recent checkpoint, update our checkpoint info */
          scr_checkpoint_id = ckpt_id;
          scr_ckpt_dset_id = current_id;
        }
      }

      /* release the dataset object */
      scr_dataset_delete(&dataset);

      /* move on to next most recent dataset */
      idx--;
    }

    /* free our list of dataset ids */
    scr_free(&dsets);

    /* if we still don't have a checkpoint and fetch is enabled,
     * attempt to fetch files from parallel file system */
    if (!found_checkpoint && scr_fetch) {
      /* sets scr_dataset_id and scr_checkpoint_id upon success */
      int fetch_attempted = 0;
      scr_fetch_latest(scr_cindex, &fetch_attempted);
    }

    /* set flag depending on whether checkpoint_id is greater than 0,
     * we'll take this to mean that we have a checkpoint in cache */
    scr_have_restart = (scr_checkpoint_id > 0);
  }

  return rc;
}

/* get and return the SCR version */
char* SCR_Get_version()
{
  return SCR_VERSION;
}

/* query whether it is time to exit */
int SCR_Should_exit(int* flag)
{
  /* manage state transition */
  if (scr_state != SCR_STATE_IDLE) {
    scr_state_transition_error(scr_state, "SCR_Should_exit()", __FILE__, __LINE__);
  }

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }

  /* bail out if not initialized -- will get bad results */
  if (! scr_initialized) {
    scr_abort(-1, "SCR has not been initialized @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* this is not required, but it helps ensure apps
   * are calling this as a collective */
  MPI_Barrier(scr_comm_world);

  /* check that we have a flag variable to write to */
  if (flag == NULL) {
    return SCR_FAILURE;
  }

  /* assume we don't have to stop */
  *flag = 0;

  /* check whether a halt condition is active */
  if (scr_bool_check_halt_and_decrement(SCR_TEST_BUT_DONT_HALT, 0)) {
    *flag = 1;
  }

  return SCR_SUCCESS;
}

/* user is telling us which checkpoint they loaded,
 * lookup the dataset and checkpoint ids from the index file,
 * update the current marker */
int SCR_Current(const char* name)
{
  int rc = SCR_SUCCESS;

  /* manage state transition */
  if (scr_state != SCR_STATE_IDLE) {
    scr_state_transition_error(scr_state, "SCR_Current()", __FILE__, __LINE__);
  }

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }

  /* bail out if not initialized -- will get bad results */
  if (! scr_initialized) {
    scr_abort(-1, "SCR has not been initialized @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* this is not required, but it helps ensure apps
   * are calling this as a collective */
  MPI_Barrier(scr_comm_world);

  /* have rank 0 look for named dataset in the prefix directory, and if it exists,
   * set this dataset to be current and initialize our dataset and checkpoint ids */
  int found = 0;
  scr_dataset* dataset = scr_dataset_new();
  if (scr_my_rank_world == 0) {
    /* read the index file */
    kvtree* index_hash = kvtree_new();
    if (scr_index_read(scr_prefix_path, index_hash) == SCR_SUCCESS) {
      /* if there is an entry for this dataset in the index,
       * get dataset info and set as current in index file */
      int id;
      if (scr_index_get_id_by_name(index_hash, name, &id) == SCR_SUCCESS) {
        /* get dataset info */
        scr_index_get_dataset(index_hash, id, name, dataset);

        /* check whether the dataset is a checkpoint */
        int is_ckpt = scr_dataset_is_ckpt(dataset);
        if (is_ckpt) {
          /* found named dataset in index file */
          found = 1;

          /* set dataset to be current */
          scr_index_set_current(index_hash, name);

          /* optionally drop checkpoints that follow this one */
          if (scr_drop_after_current) {
            scr_index_remove_later(index_hash, id);
          }

          /* update the index file */
          scr_index_write(scr_prefix_path, index_hash);
        }
      }
    }
    kvtree_delete(&index_hash);
  }

  /* determine whether rank 0 found the dataset in the index file */
  MPI_Bcast(&found, 1, MPI_INT, 0, scr_comm_world);

  /* if rank 0 found the dataset, update our dataset and checkpoint ids */
  if (found) {
    /* get dataset info from rank 0 */
    kvtree_bcast(dataset, 0, scr_comm_world);

    /* get the dataset id for this dataset */
    int dset_id;
    scr_dataset_get_id(dataset, &dset_id);

    /* get the checkpoint id for this dataset */
    int ckpt_id;
    scr_dataset_get_ckpt(dataset, &ckpt_id);

    /* initialize internal scr counters to assume job restarted
     * from this dataset */
    scr_dataset_id    = dset_id;
    scr_checkpoint_id = ckpt_id;
    scr_ckpt_dset_id  = dset_id;

    /* TODO: optionally delete any checkpoints that follow this one? */
    /* get list of datasets in cache */
    int ndsets;
    int* dsets;
    scr_cache_index_list_datasets(scr_cindex, &ndsets, &dsets);

    /* delete any dataset from cache that follows dset_id */
    int i;
    for (i = 0; i < ndsets; i++) {
      int id = dsets[i];
      if (id > dset_id) {
        if (! scr_flush_file_is_flushing(id)) {
          /* this dataset is after the current dataset,
           * delete it from cache */
          scr_cache_delete(scr_cindex, id);
        } else {
          scr_warn("Skipping delete of dataset %d from cache because it is flushing @ %s:%d",
            id, __FILE__, __LINE__
          );
        }
      }
    }

    /* free list of dataset ids */
    scr_free(&dsets);

    /* we don't want to support a restart from this since it is not
     * loaded, we just allow the user to initialize the counters */
    scr_have_restart = 0;
  }

  /* free the dataset object */
  scr_dataset_delete(&dataset);

  return rc;
}

/* drop named dataset from index */
int SCR_Drop(const char* name)
{
  int rc = SCR_SUCCESS;

  /* manage state transition */
  if (scr_state != SCR_STATE_IDLE) {
    scr_state_transition_error(scr_state, "SCR_Drop()", __FILE__, __LINE__);
  }

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }

  /* bail out if not initialized -- will get bad results */
  if (! scr_initialized) {
    scr_abort(-1, "SCR has not been initialized @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* this is not required, but it helps ensure apps
   * are calling this as a collective */
  MPI_Barrier(scr_comm_world);

  /* delete dataset from prefix directory, if it exists */
  if (scr_my_rank_world == 0) {
      /* read the index file */
      kvtree* index_hash = kvtree_new();
      if (scr_index_read(scr_prefix_path, index_hash) == SCR_SUCCESS) {
        /* if there is an entry for this dataset in the index,
         * mark it as failed so we don't try to restart it with it again */
        int id;
        if (scr_index_get_id_by_name(index_hash, name, &id) == SCR_SUCCESS) {
          /* found an entry, remove it from the index */
          scr_index_remove(index_hash, name);
          scr_index_write(scr_prefix_path, index_hash);
        }
      }
      kvtree_delete(&index_hash);
  }

  /* hold everyone until delete is complete */
  MPI_Barrier(scr_comm_world);

  return rc;
}

/* delete named checkpoint from cache and parallel file system */
int SCR_Delete(const char* name)
{
  int rc = SCR_SUCCESS;

  /* manage state transition */
  if (scr_state != SCR_STATE_IDLE) {
    scr_state_transition_error(scr_state, "SCR_Delete()", __FILE__, __LINE__);
  }

  /* if not enabled, bail with an error */
  if (! scr_enabled) {
    return SCR_FAILURE;
  }

  /* bail out if not initialized -- will get bad results */
  if (! scr_initialized) {
    scr_abort(-1, "SCR has not been initialized @ %s:%d",
      __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* this is not required, but it helps ensure apps
   * are calling this as a collective */
  MPI_Barrier(scr_comm_world);

  /* NOTE: It is possible that two datasets exist with the same name
   * if one is on the parallel file system and a newer one is in cache
   * but has yet to have been flushed.  Those will have two different
   * id values */

  /* delete dataset from cache first, if it exists */
  scr_cache_delete_by_name(scr_cindex, name);

  /* delete dataset from prefix directory, if it exists */
  int id = -1;
  if (scr_my_rank_world == 0) {
    /* read the index file */
    kvtree* index_hash = kvtree_new();
    if (scr_index_read(scr_prefix_path, index_hash) == SCR_SUCCESS) {
      /* if there is an entry for this dataset in the index,
       * mark it as failed so we don't try to restart it with it again */
      int tmp_id;
      if (scr_index_get_id_by_name(index_hash, name, &tmp_id) == SCR_SUCCESS) {
        /* found an entry to delete */
        id = tmp_id;
      }
    }
    kvtree_delete(&index_hash);
  }

  /* broadcast id for the named dataset from rank 0 */
  MPI_Bcast(&id, 1, MPI_INT, 0, scr_comm_world);

  /* delete the dataset if we found it */
  if (id != -1) {
    scr_prefix_delete(id, name);
  }

  /* hold everyone until delete is complete */
  MPI_Barrier(scr_comm_world);

  return rc;
}
