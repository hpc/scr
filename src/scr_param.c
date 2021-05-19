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

/* Reads parameters from environment and configuration files */

#include "scr_conf.h"
#include "scr.h"
#include "scr_util.h"
#include "scr_err.h"
#include "scr_io.h"
#include "kvtree.h"
#include "kvtree_util.h"
#include "spath.h"
#include "scr_config.h"
#include "scr_param.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>

/* variable length args */
#include <stdarg.h>
#include <errno.h>

/* since multiple objects may require parameters, we keep track if the parameter
 * database has been initialized and load from disk only once */
static int scr_param_initialized = 0;

/* name of system configuration file we should read */
static char scr_config_file[SCR_MAX_FILENAME] = SCR_CONFIG_FILE;

/* this data structure holds variables names which we can't lookup in
 * environment */
static kvtree* scr_no_user_hash = NULL;

/* this data structure holds variables names which must not be changed by the
 * main code because they are used by scripts that run before the main code */
static kvtree* scr_no_app_hash = NULL;

/* this data structure will hold values read from the user config file */
static kvtree* scr_user_hash = NULL;

/* this data structure will hold values read from the system config file */
static kvtree* scr_system_hash = NULL;

/* caches lookups via getenv, need to do this on some systems because returning
 * pointers to getenv values back too many functions was segfaulting on some
 * systems */
static kvtree* scr_env_hash = NULL;

/* holds param values set through SCR_Config */
kvtree* scr_app_hash = NULL;

/* expand environment variables in parameter value */
static char* expand_env(const char* value)
{
  ssize_t len = (ssize_t)strlen(value) + 1;
  ssize_t rlen = len;
  char* retval = malloc(rlen);
  assert(retval); /* TODO: should I check for running out of memory? */

  int i = 0;
  int j = 0;
  while (i < len) {
    if (value[i] == '$') {
      /* try and extract an environment variable name the '$' */
      int envbegin = -1;
      int envend   = -1;
      if (i+1 < len && value[i+1] == '{') {
        /* look for ${...} construct,
         * extract variable name in {...} */
        envbegin = i + 1;
        envend   = envbegin + 1;
        while (envend < len && value[envend] != '}' &&
              (isalnum(value[envend]) || value[envend] == '_'))
        {
          envend += 1;
        }
      } else if (i+1 < len && (isalnum(value[i+1]) || value[i+1] == '_')) {
        /* look for $... construct,
         * extract variable name in {...} */
        envbegin = i + 1;
        envend   = envbegin + 1;
        while (envend < len &&
              (isalnum(value[envend]) || value[envend] == '_'))
        {
          envend += 1;
        }
      }

      if (envbegin >= 0 && envend >= 0 && envend < len &&
         (value[envbegin] == '{') == (value[envend] == '}'))
      {
        /* found a begin, and an end, end is not past the end of the string,
         * and '{' and '}' are balanced */

        /* extract name */
        const int envnamelen = envend - envbegin;
        char envname[envnamelen + 1];
        assert(envbegin + envnamelen <= len);
        memcpy(envname, &value[envbegin], envnamelen);
        envname[envnamelen] = '\0';

        /* get value, for ${...} we copied the initial '{' into the name, skip
         * it here */
        const char* env = getenv(envname + (value[envbegin]=='{'));
        if (env == NULL) {
          env = "";
        }

        /* resize retval to make room for expanded value */
        const int envlen = strlen(env);
        rlen += envlen - (envnamelen + 1);
        retval = realloc(retval, rlen);
        assert(retval); /* TODO: should I check for running out of memory? */

        /* finally copy value into retval */
        int envi = 0;
        while (envi < envlen) {
          assert(j < rlen);
          assert(envi < envlen);
          retval[j++] = env[envi++];
        }

        i += (envnamelen + 1);

        /* account for any '}' at end */
        if (value[envend] == '}') {
          i += 1;
        }

        assert(i <= len);
      } else {
        /* invalid name of some sort, copy it to output string */
        assert(j < rlen);
        assert(i < len);
        retval[j++] = value[i++];
      }
    } else {
      /* regular character */
      assert(j < rlen);
      assert(i < len);
      retval[j++] = value[i++];
    }
  } /* while i */

  return retval;
}

/* searches for name and returns a character pointer to its value if set,
 * returns NULL if not found */
const char* scr_param_get(const char* name)
{
  char* value = NULL;

  /* see if this parameter is one which is restricted from user */
  kvtree* no_user = kvtree_get(scr_no_user_hash, name);

  /* if parameter is set in environment, return that value */
  if (no_user == NULL && getenv(name) != NULL) {
    /* we don't just return the getenv value directly because that causes
     * segfaults on some systems, so instead we add it to a hash and return
     * the pointer into the hash */

    /* try to lookup the value for this name in case we've already cached it */
    if (kvtree_util_get_str(scr_env_hash, name, &value) != KVTREE_SUCCESS) {
      /* it's not in the hash yet, so add it */
      char* tmp_value = expand_env(getenv(name));
      kvtree_util_set_str(scr_env_hash, name, tmp_value);
      scr_free(&tmp_value);

      /* now issue our lookup again */
      if (kvtree_util_get_str(scr_env_hash, name, &value) != KVTREE_SUCCESS) {
        /* it's an error if we don't find it this time */
        scr_abort(-1, "Failed to find value for %s in env hash @ %s:%d",
          name, __FILE__, __LINE__
        );
      }
    }

    return value;
  }

  /* otherwise, if parameter is set in user configuration file,
   * return that value */
  value = kvtree_elem_get_first_val(scr_user_hash, name);
  if (no_user == NULL && value != NULL) {
    /* evaluate environment variables */
    if (strchr(value, '$')) {
      value = expand_env(value);
    }
    return value;
  }

  /* otherwise, if this parameter is one which has been set by the application
   * return that value */
  value = kvtree_elem_get_first_val(scr_app_hash, name);
  if (value != NULL) {
    /* evaluate environment variables */
    if (strchr(value, '$')) {
      value = expand_env(value);
    }
    return value;
  }

  /* otherwise, if parameter is set in system configuration file,
   * return that value */
  value = kvtree_elem_get_first_val(scr_system_hash, name);
  if (value != NULL) {
    /* evaluate environment variables */
    if (strchr(value, '$')) {
      value = expand_env(value);
    }
    return value;
  }

  /* parameter not found, return NULL */
  return NULL;
}

/* searchs for name and returns a newly allocated hash of its value if set,
 * returns NULL if not found */
const kvtree* scr_param_get_hash(const char* name)
{
  kvtree* hash = NULL;
  kvtree* value_hash = NULL;

  /* see if this parameter is one which is restricted from user */
  kvtree* no_user = kvtree_get(scr_no_user_hash, name);

  /* if parameter is set in environment, return that value */
  if (no_user == NULL && getenv(name) != NULL) {
    /* TODO: need to strdup here to be safe? */
    hash = kvtree_new();
    char* tmp_value = expand_env(getenv(name));
    kvtree_set(hash, tmp_value, kvtree_new());
    scr_free(&tmp_value);
    return hash;
  }

  /* otherwise, if parameter is set in user configuration file,
   * return that value */
  value_hash = kvtree_get(scr_user_hash, name);
  if (no_user == NULL && value_hash != NULL) {
    /* walk value_hash and evaluate environment variables */
    kvtree_elem* elem;
    for (elem = kvtree_elem_first(value_hash);
	 elem != NULL;
	 elem = kvtree_elem_next(elem))
    {
      char* value = kvtree_elem_key(elem);
      if (strchr(value, '$')) {
        value = expand_env(value);
	elem->key = value;
      }
    }

    /* return the resulting hash */
    hash = kvtree_new();
    kvtree_merge(hash, value_hash);
    return hash;
  }

  /* otherwise, if this parameter is one which has been set by the application
   * return that value */
  value_hash = kvtree_get(scr_app_hash, name);
  if (value_hash != NULL) {
    /* walk value_hash and evaluate environment variables */
    kvtree_elem* elem;
    for (elem = kvtree_elem_first(value_hash);
	 elem != NULL;
	 elem = kvtree_elem_next(elem))
    {
      char* value = kvtree_elem_key(elem);
      assert(value);
      if (strchr(value, '$')) {
        value = expand_env(value);
	elem->key = value;
      }
    }

    /* return the resulting hash */
    hash = kvtree_new();
    kvtree_merge(hash, value_hash);
    return hash;
  }

  /* otherwise, if parameter is set in system configuration file,
   * return that value */
  value_hash = kvtree_get(scr_system_hash, name);
  if (value_hash != NULL) {
    /* walk value_hash and evaluate environment variables */
    kvtree_elem* elem;
    for (elem = kvtree_elem_first(value_hash);
	 elem != NULL;
	 elem = kvtree_elem_next(elem))
    {
      char* value = kvtree_elem_key(elem);
      if (strchr(value, '$')) {
        value = expand_env(value);
	elem->key = value;
      }
    }

    /* return the resulting hash */
    hash = kvtree_new();
    kvtree_merge(hash, value_hash);
    return hash;
  }

  /* parameter not found, return NULL */
  return NULL;
}

/* allocates a new string (to be freed with scr_free)
 * that is path to user config file */
static char* user_config_path(void)
{
  char* file = NULL;

  /* first, use SCR_CONF_FILE if it's set */
  const char* value = scr_param_get("SCR_CONF_FILE");
  if (value != NULL) {
    file = strdup(value);
    return file;
  }

  /* otherwise, look in the prefix directory */
  value = scr_param_get("SCR_PREFIX");
  spath* prefix_path = scr_get_prefix(value);

  /* tack file name on to directory */
  spath_append_str(prefix_path, SCR_CONFIG_FILE_USER);
  file = spath_strdup(prefix_path);
  spath_delete(&prefix_path);

  return file;
}

/* allocates a new string (to be freed with scr_free)
 * that is path to application config file */
static char* app_config_path(void)
{
  char* file = NULL;

  /* get the prefix directory */
  const char* value = scr_param_get("SCR_PREFIX");
  spath* prefix_path = scr_get_prefix(value);

  /* tack file name on to directory */
  spath_append_str(prefix_path, ".scr");
  spath_append_str(prefix_path, SCR_CONFIG_FILE_APP);
  file = spath_strdup(prefix_path);
  spath_delete(&prefix_path);

  return file;
}

/* read config files and store contents */
int scr_param_init(void)
{
  /* allocate storage and read in config files if we haven't already */
  if (!scr_param_initialized) {
    /* all parameters used by scripts that run before main code */
    static const char* no_app_params[] = {
      "CACHE",
      "CACHEDIR",
      "CNTLDIR",
      "SCR_CACHE_BASE",
      "SCR_CACHE_SIZE",
      "SCR_CNTL_BASE",
      "SCR_EXCLUDE_NODES",
      "SCR_JOB_NAME",
      "SCR_LOG_ENABLE",
      "SCR_LOG_DB_DEBUG",
      "SCR_LOG_DB_ENABLE",
      "SCR_LOG_DB_HOST",
      "SCR_LOG_DB_NAME",
      "SCR_LOG_DB_PASS",
      "SCR_LOG_DB_USER",
      "SCR_LOG_SYSLOG_ENABLE",
      "SCR_LOG_TXT_ENABLE",
      "SCR_WATCHDOG_TIMEOUT",
      "SCR_WATCHDOG_TIMEOUT_PFS",
    };

#if 0
    /* parameters used by scripts running after the main code */
    static const char* post_script_params[] = {
      "SCR_CRC_ON_FLUSH", "SCR_FILE_BUF_SIZE", "SCR_HALT_SECONDS",
    };
#endif

    assert(scr_no_user_hash == NULL);
    assert(scr_env_hash == NULL);
    assert(scr_user_hash == NULL);
    /* assert(scr_app_hash == NULL); possible already initialized by SCR_Config */
    assert(scr_no_app_hash == NULL);
    assert(scr_system_hash == NULL);

    /* allocate hash object to hold names we cannot read from the
     * environment */
    scr_no_user_hash = kvtree_new();

    /* initialize our hash to cache lookups to getenv */
    scr_env_hash = kvtree_new();

    /* initialize our hash for user configuration file */
    scr_user_hash = kvtree_new();

    /* initialize our hash for SCR_Config values, might be already
     * initialized if SCR_Config has been called */
    if (scr_app_hash == NULL) {
      scr_app_hash = kvtree_new();
    }

    /* allocate hash object to store values that must not be changed */
    scr_no_app_hash = kvtree_new();

    /* initialize our hash for values from system config file */
    scr_system_hash = kvtree_new();

    /* safe to call scr_param_get after this */
    scr_param_initialized = 1;

    /* allocate hash object to store values from user config file,
     * if specified */
    char* user_file = user_config_path();
    scr_config_read(user_file, scr_user_hash);
    scr_free(&user_file);

    /* load list of params used in script into the hash listing
     * params users cannot set with SCR_Config */
    int i;
    for (i = 0; i < sizeof(no_app_params)/sizeof(no_app_params[0]); i++) {
      kvtree_set(scr_no_app_hash, no_app_params[i], kvtree_new());
    }

    /* read in app config file which records any parameters set
     * by application through calls to SCR_Config */
    char* app_file = app_config_path();
    scr_config_read(app_file, scr_app_hash);
    scr_free(&app_file);

    /* allocate hash object to store values from system config file */
    scr_config_read(scr_config_file, scr_system_hash);

    /* warn user if they set any parameters in their environment or user
     * config file which aren't permitted */
    kvtree_elem* elem;
    for (elem = kvtree_elem_first(scr_no_user_hash);
         elem != NULL;
         elem = kvtree_elem_next(elem))
    {
      /* get the parameter name */
      char* key = kvtree_elem_key(elem);

      char* env_val = getenv(key);
      kvtree* env_hash = kvtree_get(scr_user_hash, key);

      /* check whether this is set in the environment */
      if (env_val != NULL || env_hash != NULL) {
        scr_err("%s cannot be set in the environment or user configuration file, ignoring setting",
          key
        );
      }
    }
  }

  return SCR_SUCCESS;
}

/* free contents from config files */
int scr_param_finalize()
{
  /* if the reference count is zero, free the data structures */
  if (scr_param_initialized) {
    /* NOTE: we do not free scr_app_hash here,
     * since that is allocated by SCR_Config not scr_param_init,
     * it will be freed in SCR_Finalize instead */
    /* write out scr_app_hash and free it */
    
    /* store parameters set by app code for use by post-run scripts */
    char* app_file = app_config_path();
    if (app_file != NULL) {
      scr_config_write(app_file, scr_app_hash);
    }
    scr_free(&app_file);
    kvtree_delete(&scr_app_hash);

    /* free the hash listing parameters user cannot set through SCR_Config */
    kvtree_delete(&scr_no_app_hash);

    /* free our parameter hash */
    kvtree_delete(&scr_user_hash);

    /* free our parameter hash */
    kvtree_delete(&scr_system_hash);

    /* free our env hash */
    kvtree_delete(&scr_env_hash);

    /* free the hash listing parameters user cannot set */
    kvtree_delete(&scr_no_user_hash);

    scr_param_initialized = 0;
  }

  return SCR_SUCCESS;
}

/* sets (top level) a parameter to a new value, returning the subkey hash */
kvtree* scr_param_set(const char* name, const char* value)
{
  /* cannot set parameters that are used by scripts */
  if (kvtree_get(scr_no_app_hash, name)) {
    scr_dbg(1, "when using SCR scripts, %s should not be changed at runtime",
      name
    );
  }

  kvtree* k = kvtree_new();
  kvtree* v = kvtree_set(k, value, kvtree_new());
  assert(k && v);
  kvtree_set(scr_app_hash, name, k);
  return v;
}

/* sets a parameter to a new value, returning the hash
 * hash_value should be the return from scr_param_get_hash() if the top level
 * value needs to be preserved */
kvtree* scr_param_set_hash(const char* name, kvtree* hash_value)
{
  /* cannot set parameters that are used by scripts */
  if (kvtree_get(scr_no_app_hash, name)) {
    scr_dbg(1, "%s should not be changed at runtime if also using SCR scripts",
      name
    );
  }

  return kvtree_set(scr_app_hash, name, hash_value);
}

/* unsets a parameter, returning 0 if the parameter did not exist */
int scr_param_unset(const char* name)
{
  /* cannot set parameters that are used by scripts */
  if (kvtree_get(scr_no_app_hash, name)) {
    scr_dbg(1, "%s should not be changed at runtime if also using SCR scripts",
      name
    );
  }

  int rc = kvtree_unset(scr_app_hash, name);
  return (rc == KVTREE_SUCCESS) ? SCR_SUCCESS : SCR_FAILURE;
}
