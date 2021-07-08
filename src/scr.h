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

/* enable C++ codes to include this header directly */
#ifdef __cplusplus
extern "C" {
#endif

/* Version information */
#define SCR_MAJOR_VERSION "3"
#define SCR_MINOR_VERSION "0"
#define SCR_PATCH_VERSION "0"
#define SCR_VERSION "v3.0.0"

/* constants returned from SCR functions for success and failure */
#define SCR_SUCCESS (0)
#define SCR_FAILURE (1)

/* maximum characters in a filename returned by SCR */
#define SCR_MAX_FILENAME 1024

/* bit flags to be OR'd in SCR_Start_output */
#define SCR_FLAG_NONE       (0 << 0) /* empty flags */
#define SCR_FLAG_CHECKPOINT (1 << 0) /* means that job can be restarted using this dataset */
#define SCR_FLAG_OUTPUT     (1 << 1) /* means this dataset must be flushed to the file system */

/* see the SCR user manual for full details on these functions */

/*****************
 * Init and finalize routines
 ****************/

/* initialize the SCR library */
int SCR_Init(void);

/* shut down the SCR library */
int SCR_Finalize(void);


/*****************
 * Programmatically change configuration options
 ****************/
/* config: is a string of the form of a line in a configurartion file
 * if it consists only of KEY=VALUE pairs then a value is set (including all
 * parent values)
 * If config ends in "KEY" then a copy of the value of KEY is returned, it must
 * be free()ed.
 * If config ends in "KEY=" then KEY is marked as removed (value of NULL).
 *
 * The return value is the value of the gotten KEY or the last VALUE set, NULL
 * is returned on failure.
 */
const char* SCR_Config(const char* config);

/* Formatted version of SCR_Config, for printf-like formatting */
const char* SCR_Configf(const char* format, ...);

/*****************
 * File registration
 ****************/

/* determine the path and filename to be used to open a file */
int SCR_Route_file(const char* name, char* file);

/*****************
 * Restart routines
 ****************/

/* determine whether SCR has a restart available to read,
 * and get name of restart if one is available */
int SCR_Have_restart(int* flag, char* name);

/* inform library that restart is starting, get name of 
 * restart that is available */
int SCR_Start_restart(char* name);

/* inform library that the current restart is complete */
int SCR_Complete_restart(int valid);

/*****************
 * Checkpoint routines (backwards compatibility)
 ****************/

/* determine whether a checkpoint should be taken at the current time */
int SCR_Need_checkpoint(int* flag);

/* inform library that a new checkpoint is starting */
int SCR_Start_checkpoint(void);

/* inform library that the current checkpoint is complete */
int SCR_Complete_checkpoint(int valid);

/*****************
 * Output routines
 ****************/

/* inform library that a new output dataset is starting */
int SCR_Start_output(const char* name, int flags);

/* inform library that the current dataset is complete */
int SCR_Complete_output(int valid);

/*****************
 * Dataset management routines
 ****************/

/* set named dataset as current in index,
 * and initialize SCR internal counters to assume job
 * has restarted from this checkpoint */
int SCR_Current(const char* name);

/* drop named dataset from index */
int SCR_Drop(const char* name);

/* delete files for named dataset */
int SCR_Delete(const char* name);

/*****************
 * Environment and configuration routines
 ****************/

/* get and return the SCR version */
char* SCR_Get_version(void);

/* query whether it is time to exit */
int SCR_Should_exit(int* flag);

/* enable C++ codes to include this header directly */
#ifdef __cplusplus
} /* extern "C" */
#endif
