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

/* This is a utility program that lets one insert log entries into the
 * SCR log.  It's a small C program which must run on the
 * same node where rank 0 runs -- it requires the same environment as
 * the running job to identify the proper logging target.
*/

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include "scr_conf.h"
#include "scr.h"
#include "scr_io.h"
#include "scr_err.h"
#include "scr_log.h"
#include "scr_param.h"

#ifdef SCR_GLOBALS_H
#error "globals.h accessed from tools"
#endif

struct arglist {
  char*  prefix;
  char*  username;
  char*  jobname;
  char*  jobid;
  time_t start;

  int     transfer;
  char*   transfer_type;
  char*   transfer_from;
  char*   transfer_to;
  int*    transfer_dset;
  char*   transfer_name;
  time_t* transfer_start;
  double* transfer_secs;
  double* transfer_bytes;
  int*    transfer_files;
};

int scr_log_enable = SCR_LOG_ENABLE;

int global_dset;
time_t global_start;
double global_secs;
double global_bytes;
int    global_files;

void print_usage()
{
  printf("\n");
  printf("scr_log_transfer -- record a file transfer operation in the SCR log\n");
  printf("\n");
  printf("Options:\n");
  printf("  -p <prefix>    Prefix directory\n");
  printf("  -u <username>  Username of job owner, reads $USER if not specified\n");
  printf("  -j <jobname>   Job name of job, reads $SCR_JOB_NAME if not specified\n");
  printf("  -i <jobid>     Job id\n");
  printf("  -s <seconds>   Job start time, uses current UNIX timestamp if not specified\n");
  printf("\n");
  printf("  -T <type>      Event type (string)\n");
  printf("  -X <from>      From directory (string)\n");
  printf("  -Y <to>        To directory (string)\n");
  printf("  -D <id>        Dataset id (integer)\n");
  printf("  -n <name>      Dataset name (string)\n");
  printf("  -S <start>     Transfer start time as UNIX timestamp (integer)\n");
  printf("  -L <duration>  Duration in seconds (integer)\n");
  printf("  -B <bytes>     Number of bytes transfered (integer)\n");
  printf("  -F <files>     Number of files transfered (integer)\n");
  printf("\n");
  return;
}

int processArgs(int argc, char **argv, struct arglist* args)
{
  int i, j;
  char *argptr;
  char flag;

  /* set to default values */
  args->prefix   = NULL;
  args->username = NULL;
  args->jobname  = NULL;
  args->jobid    = NULL;
  args->start    = 0;

  args->transfer          = 0;
  args->transfer_type     = NULL;
  args->transfer_from     = NULL;
  args->transfer_to       = NULL;
  args->transfer_dset     = NULL;
  args->transfer_name     = NULL;
  args->transfer_start    = NULL;
  args->transfer_secs     = NULL;
  args->transfer_bytes    = NULL;
  args->transfer_files    = NULL;

  for (i=1; i<argc; i++) {
    /* check for options */
    j = 0;
    if (argv[i][j++] == '-') {
      /* flag is the first char following the '-' */
      flag = argv[i][j++];
      argptr = NULL;

      /* handles "-i#" or "-i #" */
      if (argv[i][j] != 0) {
        argptr = &(argv[i][j]);
      } else {
        argptr = argv[i+1];
        i++;
      }

      /* single argument parameters */
      if (strchr("pujisTXYDnSLBF", flag)) {
        switch(flag) {
        case 'p':
          args->prefix = strdup(argptr);
          break;
        case 'u':
          args->username = strdup(argptr);
          break;
        case 'j':
          args->jobname = strdup(argptr);
          break;
        case 'i':
          args->jobid = strdup(argptr);
          break;
        case 's':
          args->start = (time_t) strtoul(argptr, NULL, 0);
          break;

        case 'T':
          args->transfer_type = strdup(argptr);
          break;
        case 'X':
          args->transfer_from = strdup(argptr);
          break;
        case 'Y':
          args->transfer_to = strdup(argptr);
          break;
        case 'D':
          global_dset = (int) atoi(argptr);
          args->transfer_dset = &global_dset;
          break;
        case 'n':
          args->transfer_name = strdup(argptr);
          break;
        case 'S':
          global_start = (time_t) strtoul(argptr, NULL, 0);
          args->transfer_start = &global_start;
          break;
        case 'L':
          global_secs = (double) atoi(argptr);
          args->transfer_secs = &global_secs;
          break;
        case 'B':
          global_bytes = (double) atoi(argptr);
          args->transfer_bytes = &global_bytes;
          break;
        case 'F':
          global_files = atoi(argptr);
          args->transfer_files = &global_files;
          break;
        }
        continue;
      } else {
        scr_err("Invalid flag -%c", flag);
        print_usage();
        return 0;
      }
    } else {
      scr_err("Unknown argument %s", argv[i]);
      print_usage();
      return 0;
    }
  }

  /* require -p prefix option */
  if (args->prefix == NULL) {
    scr_err("-p <prefix> required");
    print_usage();
    return 0;
  }

  return 1;
}

int main (int argc, char *argv[])
{
  int rc = 0;

  /* process command line arguments */
  struct arglist args;
  if (!processArgs(argc, argv, &args)) {
    scr_err("scr_log_transfer: Failed to process args @ %s:%d",
            __FILE__, __LINE__
    );
    return 1;
  }

  const char* value = NULL;

  /* read in job parameters (if user didn't specify them) */
  if (args.username == NULL) {
    if ((value = getenv("USER")) != NULL) {
      args.username = strdup(value);
    }
  }
  if (args.jobname == NULL) {
    if ((value = getenv("SCR_JOB_NAME")) != NULL) {
      args.jobname = strdup(value);
    }
  }
  if (args.start == 0) {
    args.start = scr_log_seconds();
  }

  /* read in log parameters */
  scr_param_init();
  if ((value = scr_param_get("SCR_LOG_ENABLE")) != NULL) {
    scr_log_enable = atoi(value);
  }

  /* we just use the string returned by gethostname */
  char hostname[256] = "nullhost";
  if (gethostname(hostname, sizeof(hostname)) != 0) {
    scr_err("scr_log_transfer: Call to gethostname failed @ %s:%d",
      __FILE__, __LINE__
    );
  }

  if (scr_log_enable) {
    /* init logging */
    if (scr_log_init(args.prefix) == SCR_SUCCESS) {
      /* register job */
      if (args.username != NULL && args.prefix != NULL) {
        if (scr_log_job(args.username, hostname, args.jobid, args.prefix, args.start) != SCR_SUCCESS) {
          scr_err("scr_log_transfer: Failed to register job, disabling logging @ %s:%d",
                  __FILE__, __LINE__
          );
          scr_log_enable = 0;
          rc = 1;
        }
      } else {
        scr_err("scr_log_transfer: Missing username, prefix, or start time, disabling logging @ %s:%d",
                __FILE__, __LINE__
        );
        scr_log_enable = 0;
        rc = 1;
      }
    } else {
      scr_err("scr_log_transfer: Failed to initialize SCR logging, disabling logging @ %s:%d",
              __FILE__, __LINE__
      );
      scr_log_enable = 0;
      rc = 1;
    }
  }

  if (scr_log_enable) {
    if (scr_log_transfer(args.transfer_type, args.transfer_from, args.transfer_to,
          args.transfer_dset, args.transfer_name, args.transfer_start, args.transfer_secs,
          args.transfer_bytes, args.transfer_files) != SCR_SUCCESS)
    {
      rc = 1;
    }
  }

  if (scr_log_enable) {
    scr_log_finalize();
  }

  scr_param_finalize();
  return rc;
}
