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

  int     event;
  char*   event_type;
  char*   event_note;
  int*    event_dset;
  char*   event_name;
  time_t* event_start;
  double* event_secs;
};

int scr_log_enable = SCR_LOG_ENABLE;

int global_dset;
time_t global_start;
double global_secs;

void print_usage()
{
  printf("\n");
  printf("scr_log_event -- record an event in the SCR log\n");
  printf("\n");
  printf("Options:\n");
  printf("  -p <prefix>    Prefix directory\n");
  printf("  -u <username>  Username of job owner, reads $USER if not specified\n");
  printf("  -j <jobname>   Job name of job, reads $SCR_JOB_NAME if not specified\n");
  printf("  -i <jobid>     Job id\n");
  printf("  -s <seconds>   Job start time, uses current UNIX timestamp if not specified\n");
  printf("\n");
  printf("  -T <type>      Event type (string)\n");
  printf("  -N <note>      Note (string)\n");
  printf("  -D <id>        Dataset id (integer)\n");
  printf("  -n <name>      Dataset name (string)\n");
  printf("  -S <start>     Event start time as UNIX timestamp (integer)\n");
  printf("  -L <duration>  Duration in seconds (integer)\n");
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

  args->event             = 0;
  args->event_type        = NULL;
  args->event_note        = NULL;
  args->event_dset        = NULL;
  args->event_name        = NULL;
  args->event_start       = NULL;
  args->event_secs        = NULL;

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
      if (strchr("pujisTNDnSL", flag)) {
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
          args->event_type = strdup(argptr);
          break;
        case 'N':
          args->event_note = strdup(argptr);
          break;
        case 'D':
          global_dset = (int) atoi(argptr);
          args->event_dset = &global_dset;
          break;
        case 'n':
          args->event_name = strdup(argptr);
          break;
        case 'S':
          global_start = (time_t) strtoul(argptr, NULL, 0);
          args->event_start = &global_start;
          break;
        case 'L':
          global_secs = (int) atoi(argptr);
          args->event_secs = &global_secs;
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
    scr_err("scr_log_event: Failed to process args @ %s:%d",
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
    scr_err("scr_log_event: Call to gethostname failed @ %s:%d",
      __FILE__, __LINE__
    );
  }

  if (scr_log_enable) {
    /* init logging */
    if (scr_log_init(args.prefix) == SCR_SUCCESS) {
      /* register job */
      if (args.username != NULL && args.prefix != NULL) {
        if (scr_log_job(args.username, hostname, args.jobid, args.prefix, args.start) != SCR_SUCCESS) {
          scr_err("scr_log_event: Failed to register job, disabling logging @ %s:%d",
                  __FILE__, __LINE__
          );
          scr_log_enable = 0;
          rc = 1;
        }
      } else {
        scr_err("scr_log_event: Missing username, prefix, or start time, disabling logging @ %s:%d",
                __FILE__, __LINE__
        );
        scr_log_enable = 0;
        rc = 1;
      }
    } else {
      scr_err("scr_log_event: Failed to initialize SCR logging, disabling logging @ %s:%d",
              __FILE__, __LINE__
      );
      scr_log_enable = 0;
      rc = 1;
    }
  }

  if (scr_log_enable) {
    if (scr_log_event(args.event_type, args.event_note, args.event_dset,
          args.event_name, args.event_start, args.event_secs) != SCR_SUCCESS)
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
