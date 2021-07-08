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

/* This file compiles a normalized interface for Fortran in which:
 *   - Fortran link names are in lower case
 *   - Fortran link names have a single trailing underscore
 *   - boolean true is expected to be 1
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "scr.h"

/* TODO: enable SCR_Fint to be configured to be different type */
typedef int SCR_Fint;

#ifdef USE_FORT_STDCALL
# define FORT_CALL __stdcall
#elif defined (USE_FORT_CDECL)
# define FORT_CALL __cdecl
#else
# define FORT_CALL
#endif

#ifdef USE_FORT_MIXED_STR_LEN
# define FORT_MIXED_LEN_DECL   , SCR_Fint
# define FORT_END_LEN_DECL
# define FORT_MIXED_LEN(a)     , SCR_Fint a
# define FORT_END_LEN(a)
#else
# define FORT_MIXED_LEN_DECL
# define FORT_END_LEN_DECL     , SCR_Fint
# define FORT_MIXED_LEN(a)
# define FORT_END_LEN(a)       , SCR_Fint a
#endif

#ifdef HAVE_FORTRAN_API
# ifdef FORTRAN_EXPORTS
#  define FORTRAN_API __declspec(dllexport)
# else
#  define FORTRAN_API __declspec(dllimport)
# endif
#else
# define FORTRAN_API
#endif

/* convert a Fortran string to a C string, removing any trailing spaces and terminating with a NULL */
static int scr_fstr2cstr(const char* fstr, int flen, char* cstr, int clen)
{
  int rc = 0;

  /* check that our pointers aren't NULL */
  if (fstr == NULL || cstr == NULL) {
    return 1;
  }

  /* determine length of Fortran string after subtracting any trailing spaces */
  while (flen > 0 && fstr[flen-1] == ' ') {
    flen--;
  }
  
  /* assume we can copy the whole string */
  int len = flen;
  if (flen > clen - 1) {
    /* Fortran string is longer than C buffer, copy what we can and truncate */
    len = clen - 1;
    rc = 1;
  }

  /* copy the Fortran string to the C string */
  if (len > 0) {
    strncpy(cstr, fstr, len);
  }

  /* null-terminate the C string */
  if (len >= 0) {
    cstr[len] = '\0';
  }

  return rc;
}

/* convert a C string to a Fortran string, adding trailing spaces if necessary */
static int scr_cstr2fstr(const char* cstr, char* fstr, int flen)
{
  int rc = 0;

  /* check that our pointers aren't NULL */
  if (cstr == NULL || fstr == NULL) {
    return 1;
  }

  /* determine length of C string */
  int clen = strlen(cstr);
  
  /* copy the characters from the Fortran string to the C string */
  if (clen <= flen) {
    /* C string will fit within our Fortran buffer, copy it over */
    if (clen > 0) {
      strncpy(fstr, cstr, clen);
    }

    /* fill in trailing spaces */
    while (clen < flen) {
      fstr[clen] = ' ';
      clen++;
    }
  } else {
    /* C string is longer than our Fortran buffer, copy what we can then truncate */
    strncpy(fstr, cstr, flen);
    rc = 1;
  }

  return rc;
}

/*================================================
 * Init, Finalize, Exit
 *================================================*/

FORTRAN_API void FORT_CALL scr_init_(int* ierror)
{
  *ierror = SCR_Init();
  return;
}

FORTRAN_API void FORT_CALL scr_finalize_(int* ierror)
{
  *ierror = SCR_Finalize();
  return;
}

FORTRAN_API void FORT_CALL scr_should_exit_(int* flag, int* ierror)
{
  *ierror = SCR_Should_exit(flag);
  return;
}

/*================================================
 * Programmatically change configuration options
 *================================================*/
FORTRAN_API void FORT_CALL scr_config_(char* cfg FORT_MIXED_LEN(cfg_len),
                                      char* val FORT_MIXED_LEN(val_len),
                                      int* ierror FORT_END_LEN(cfg_len) FORT_END_LEN(val_len))
{
  *ierror = SCR_SUCCESS;

  /* convert name from a Fortran string to C string */
  char cfg_tmp[SCR_MAX_FILENAME];
  if (scr_fstr2cstr(cfg, cfg_len, cfg_tmp, sizeof(cfg_tmp)) != 0) {
    *ierror = SCR_FAILURE;
    return;
  }

  const char *val_tmp = SCR_Config(cfg_tmp);
  if (!val_tmp) {
    *ierror = SCR_FAILURE;
    return;
  }
  /* TODO: there is a memory leak here. scr_config, when called to get a value
   * scr_config("FOO") returns a copy of the value that needs to be free()'ed
   * while SCR_Config("FOO=1") returns a pointer to FOO itself that must *not*
   * be free'ed. I cannot distinguish what needs to be done here without
   * parsing the string.
   * The best fix would be to make SCR_Config never return a copy, which may
   * require changes to scr_param_get.
   */
  /* free((void*)val_tmp); */

  /* convert val to Fortran string */
  if (scr_cstr2fstr(val_tmp, val, val_len) != 0) {
    *ierror = SCR_FAILURE;
    return;
  }

  return;
}

/*================================================
 * Restart functions
 *================================================*/

FORTRAN_API void FORT_CALL scr_have_restart_(int* flag, char* name FORT_MIXED_LEN(name_len), int* ierror FORT_END_LEN(name_len))
{
  /* check whether a checkpoint is loaded */
  char name_tmp[SCR_MAX_FILENAME];
  *ierror = SCR_Have_restart(flag, name_tmp);

  /* if have a checkpoint to restart from, need to convert name string */
  if (*flag) {
    /* convert name to Fortran string */
    if (scr_cstr2fstr(name_tmp, name, name_len) != 0) {
      *ierror = SCR_FAIULRE;
      return;
    }
  }

  return;
}

FORTRAN_API void FORT_CALL scr_start_restart_(char* name FORT_MIXED_LEN(name_len), int* ierror FORT_END_LEN(name_len))
{
  /* start restart and get name */
  char name_tmp[SCR_MAX_FILENAME];
  *ierror = SCR_Start_restart(name_tmp);

  /* convert name to Fortran string */
  if (scr_cstr2fstr(name_tmp, name, name_len) != 0) {
    *ierror = SCR_FAIULRE;
    return;
  }

  return;
}

FORTRAN_API void FORT_CALL scr_complete_restart_(int* valid, int* ierror)
{
  int valid_tmp = *valid;
  *ierror = SCR_Complete_restart(valid_tmp);
  return;
}

/*================================================
 * Checkpoint functions
 *================================================*/

FORTRAN_API void FORT_CALL scr_need_checkpoint_(int* flag, int* ierror)
{
  *ierror = SCR_Need_checkpoint(flag);
  return;
}

FORTRAN_API void FORT_CALL scr_start_checkpoint_(int* ierror)
{
  *ierror = SCR_Start_checkpoint();
  return;
}

FORTRAN_API void FORT_CALL scr_complete_checkpoint_(int* valid, int* ierror)
{
  int valid_tmp = *valid;
  *ierror = SCR_Complete_checkpoint(valid_tmp);
  return;
}

/*================================================
 * Output functions
 *================================================*/

FORTRAN_API void FORT_CALL scr_start_output_(char* name FORT_MIXED_LEN(name_len), int* flags, int* ierror FORT_END_LEN(name_len))
{
  /* convert name from a Fortran string to C string */
  char name_tmp[SCR_MAX_FILENAME];
  if (scr_fstr2cstr(name, name_len, name_tmp, sizeof(name_tmp)) != 0) {
    *ierror = SCR_FAIULRE;
    return;
  }

  int flags_tmp = *flags;
  *ierror = SCR_Start_output(name_tmp, flags_tmp);

  return;
}

FORTRAN_API void FORT_CALL scr_complete_output_(int* valid, int* ierror)
{
  int valid_tmp = *valid;
  *ierror = SCR_Complete_output(valid_tmp);
  return;
}

/*================================================
 * Route file
 *================================================*/

FORTRAN_API void FORT_CALL scr_route_file_(char* name FORT_MIXED_LEN(name_len),
                                           char* file FORT_MIXED_LEN(file_len),
                                           int* ierror FORT_END_LEN(name_len) FORT_END_LEN(file_len))
{
  /* convert filename from a Fortran string to C string */
  char name_tmp[SCR_MAX_FILENAME];
  if (scr_fstr2cstr(name, name_len, name_tmp, sizeof(name_tmp)) != 0) {
    *ierror = SCR_FAIULRE;
    return;
  }

  /* get the filename to use */
  char file_tmp[SCR_MAX_FILENAME];
  *ierror = SCR_Route_file(name_tmp, file_tmp);

  /* convert filename from C to Fortran string */
  if (scr_cstr2fstr(file_tmp, file, file_len) != 0) {
    *ierror = SCR_FAIULRE;
    return;
  }

  return;
}

/*================================================
 * Dataset management
 *================================================*/

FORTRAN_API void FORT_CALL scr_current_(char* name FORT_MIXED_LEN(name_len), int* ierror FORT_END_LEN(name_len))
{
  /* convert name from a Fortran string to C string */
  char name_tmp[SCR_MAX_FILENAME];
  if (scr_fstr2cstr(name, name_len, name_tmp, sizeof(name_tmp)) != 0) {
    *ierror = SCR_FAIULRE;
    return;
  }

  *ierror = SCR_Current(name_tmp);

  return;
}

FORTRAN_API void FORT_CALL scr_drop_(char* name FORT_MIXED_LEN(name_len), int* ierror FORT_END_LEN(name_len))
{
  /* convert name from a Fortran string to C string */
  char name_tmp[SCR_MAX_FILENAME];
  if (scr_fstr2cstr(name, name_len, name_tmp, sizeof(name_tmp)) != 0) {
    *ierror = SCR_FAIULRE;
    return;
  }

  *ierror = SCR_Drop(name_tmp);

  return;
}

FORTRAN_API void FORT_CALL scr_delete_(char* name FORT_MIXED_LEN(name_len), int* ierror FORT_END_LEN(name_len))
{
  /* convert name from a Fortran string to C string */
  char name_tmp[SCR_MAX_FILENAME];
  if (scr_fstr2cstr(name, name_len, name_tmp, sizeof(name_tmp)) != 0) {
    *ierror = SCR_FAIULRE;
    return;
  }

  *ierror = SCR_Delete(name_tmp);

  return;
}
