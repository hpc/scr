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

/* Reads configuration file and return values in hash of following form:
 *
 * # Comments set off with '#' and blank lines are ok
 * # Note that '#' and '=' are reserved characters;
 * # they cannot be used in key or value strings.
 *
 * # Each entry either contains a single key/value pair separated by '=' sign
 *
 * SCR_VARIABLE=VALUE # Each line can end with a trailing comment
 *
 * # Or an entry contains a parent key/value pair with a series of
 * # one or more child key/value pairs
 *
 * PARENT_KEY=PARENT_VALUE CHILD_KEY1=CHILD_VALUE1 CHILD_KEY2=CHILD_VALUE2 ...
 * */

#include "scr_conf.h"
#include "scr.h"
#include "scr_err.h"
#include "scr_util.h"
#include "scr_io.h"
#include "kvtree.h"
#include "spath.h"
#include "scr_config.h"

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <assert.h>

/* variable length args */
#include <stdarg.h>
#include <errno.h>

/* toupper */
#include <ctype.h>

#define IS_ENVVAR_CHAR(X) ((X >= 'a' && X <= 'z') || (X >= 'A' && X <= 'Z') || (X == '_'))

/* read in whitespace (spaces and tabs) until we hit a non-whitespace character */
static int scr_config_read_whitespace(
  FILE* fs, const char* file, int linenum,
  int* n_external, char* c_external)
{
  /* remove whitespace (spaces and tabs) until we hit a character */
  int  n = *n_external;
  char c = *c_external;
  while (n != EOF && (c == ' ' || c == '\t')) {
    n = fgetc(fs);
    c = (char) n;
  }
  *n_external = n;
  *c_external = c;
  return SCR_SUCCESS;
}

/* read token into buffer of specified max size,
 * stops at whitespace, newline, or '=' character */
static int scr_config_read_token(
  FILE* fs, const char* file, int linenum,
  int* n_external, char* c_external, char* token, int size)
{
  /* remove whitespace (spaces and tabs) until we hit a character */
  int  n = *n_external;
  char c = *c_external;

  /* read bytes of token into buffer */
  int i = 0;
  while (n != EOF && c != ' ' && c != '\t' && c != '\n' && c != '=') {
    /* check that we don't overwrite our token buffer */
    if (i >= size) {
      scr_err("Internal buffer too short (%d bytes) while reading token in configuration file @ %s:%d",
        size, file, linenum
      );
      return SCR_FAILURE;
    }

    /* copy next character into token buffer */
    token[i++] = c;

    n = fgetc(fs);
    c = (char) n;
  }

  /* check that our token is at least one character long */
  if (i == 0) {
    scr_err("Missing token in configuration file @ %s:%d",
      file, linenum
    );
    return SCR_FAILURE;
  }

  /* terminate our token with a null character */
  if (i >= size) {
    scr_err("Internal buffer too short (%d bytes) while reading token in configuration file @ %s:%d",
      size, file, linenum
    );
    return SCR_FAILURE;
  }
  token[i++] = '\0';

  *n_external = n;
  *c_external = c;
  return SCR_SUCCESS;
}

/* expand environment variables in the input and output the result
 * primarily useful for paths in config files*/
static char* scr_expand_value(char* raw_value)
{
  char value[SCR_MAX_FILENAME+1];
  char envvar[SCR_MAX_FILENAME+1];
  int i = 0, j = 0;
  int is_escaped = 0;

  while (raw_value[i] != '\0') {
    if (j >= SCR_MAX_FILENAME) {
      scr_err("Path length %s is too long, the maximum length is %d\n", raw_value, SCR_MAX_FILENAME);
      return NULL;
    }
    if (is_escaped) {
      switch (raw_value[i]) {
      case 'a': value[j] = '\a'; break;
      case 'b': value[j] = '\b'; break;
      case 'f': value[j] = '\f'; break;
      case 'n': value[j] = '\n'; break;
      case 'r': value[j] = '\r'; break;
      case 't': value[j] = '\t'; break;
      case 'v': value[j] = '\v'; break;
      default: value[j] = raw_value[i];
      }
      is_escaped = 0;
      i++;
      j++;
    }
    else if (raw_value[i] == '\\') {
      is_escaped = 1;
      i++;
    }
    else if (raw_value[i] == '$') {
      char* env_start = raw_value + i + 1;
      char* env_end = env_start;
      while (IS_ENVVAR_CHAR(*env_end)) {
        env_end++;
      }
      size_t envvar_len = env_end - env_start;
      if (envvar_len > SCR_MAX_FILENAME) {
	scr_err("The environment variable specified by %s is too long, the maximum length is %d\n", raw_value, SCR_MAX_FILENAME);
	return NULL;
      }
      strncpy(envvar, env_start, envvar_len);
      envvar[envvar_len] = '\0';

      char* env_value = getenv(envvar);
      if (!env_value) {
	scr_err("No environment variable %s is defined, needed to satisfy %s\n", envvar, raw_value);
	return NULL;
      }
      size_t env_value_len = strlen(env_value);
      if (env_value_len + j > SCR_MAX_FILENAME) {
	scr_err("File path %s is too long when expanded with %s replacing %s. The maximum length is %d\n", raw_value, env_value, envvar);
	return NULL;
      }
      strncpy(value + j, env_value, env_value_len);
      i += envvar_len + 1;
      j += env_value_len;
    }
    else {
      value[j] = raw_value[i];
      i++;
      j++;
    }
  }

  value[j] = '\0';
  return strdup(value);
}

/* read in a key value pair and insert into hash,
 * return hash under value in hash2 pointer,
 * (key)(\s*)(=)(\s*)(value) */
static int scr_config_read_kv(
  FILE* fs, const char* file, int linenum,
  int* n_external, char* c_external, kvtree* hash, kvtree** hash2)
{
  int  n = *n_external;
  char c = *c_external;

  /* read in the key token */
  char key[SCR_MAX_FILENAME];
  scr_config_read_token(fs, file, linenum, &n, &c, key, sizeof(key));

  /* optional white space between key and '=' sign */
  scr_config_read_whitespace(fs, file, linenum, &n, &c);

  /* should be sitting on the '=' that splits the key and value at this point */
  if (c != '=') {
    scr_err("Ill-formed key value pair detected in configuration file @ %s:%d",
      file, linenum
    );
    return SCR_FAILURE;
  }
  n = fgetc(fs);
  c = (char) n;

  /* optional white space between '=' sign and value */
  scr_config_read_whitespace(fs, file, linenum, &n, &c);

  /* read in the value token */
  char raw_value[SCR_MAX_FILENAME];
  scr_config_read_token(fs, file, linenum, &n, &c, raw_value, sizeof(raw_value));
  char* value = scr_expand_value(raw_value);

  /* convert key to upper case */
  int i = 0;
  for (i=0; i < strlen(key); i++) {
    key[i] = (char) toupper(key[i]);
  }

  /* insert key/value into hash */
  kvtree* tmp_hash = kvtree_set_kv(hash, key, value);
  scr_free(&value);

  *n_external = n;
  *c_external = c;
  *hash2 = tmp_hash;

  return SCR_SUCCESS;
}

/* found a comment, strip out everything until we hit a newline or end-of-file */
static int scr_config_read_comment(
  FILE* fs, const char* file, int linenum,
  int* n_external, char* c_external)
{
  /* inside a comment, remove the rest of the line */
  int  n = *n_external;
  char c = *c_external;
  while (n != EOF && c != '\n') {
    n = fgetc(fs);
    c = (char) n;
  }
  *n_external = n;
  *c_external = c;
  return SCR_SUCCESS;
}

/* process all items found on the current line
 * and record any entries in hash */
static int scr_config_read_line(
  FILE* fs, const char* file, int linenum,
  int* n_external, char* c_external, kvtree* hash)
{
  int  n = *n_external;
  char c = *c_external;

  /* go until we hit a newline of end-of-file */
  kvtree* target_hash = hash;
  int set_root = 0;
  while (n != EOF && c != '\n') {
    /* remove whitespace (spaces and tabs) until we hit a character */
    scr_config_read_whitespace(fs, file, linenum, &n, &c);

    /* if the line didn't end, we must have found a comment or another key/value pair */
    if (n != EOF && c != '\n') {
      if (c == '#') {
        /* found the start of a comment, strip it out */
        scr_config_read_comment(fs, file, linenum, &n, &c);
      } else {
        /* otherwise, must have a key value chain, so read them in */
        kvtree* tmp_hash;
        scr_config_read_kv(fs, file, linenum, &n, &c, target_hash, &tmp_hash);

        /* we set the first key/value pair as the root,
         * if there are other key/value pairs on the same line,
         * we store all of those as children under the parent */
        if (set_root == 0) {
          target_hash = tmp_hash;
          set_root = 1;
        }
      }
    }
  }

  *n_external = n;
  *c_external = c;
  return SCR_SUCCESS;
}

/* read parameters from config file and fill in hash */
int scr_config_read_common(const char* file, kvtree* hash)
{
  /* check whether we can read the file */
  if (scr_file_is_readable(file) != SCR_SUCCESS) {
    return SCR_FAILURE;
  }

  /* open the file */
  FILE* fs = fopen(file, "r");
  if (fs == NULL) {
    scr_err("Opening configuration file for read: fopen(%s, \"r\") errno=%d %s @ %s:%d",
            file, errno, strerror(errno), __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* read the file until we hit an end-of-file */
  int n;
  char c;
  int linenum = 0;
  do {
    /* keep track of our line number for more useful error messages */
    linenum++;

    /* read in the first character of the line to prime our n and c values */
    n = fgetc(fs);
    c = (char) n;

    /* now process the line */
    scr_config_read_line(fs, file, linenum, &n, &c, hash);
  } while (n != EOF);

  /* close the file */
  fclose(fs);

  return SCR_SUCCESS;
}

int scr_config_write_common(const char* file, const kvtree* hash)
{
  int rc = SCR_SUCCESS;

  /* no need to write out a file for a NULL hash,
   * but let's delete any existing file */
  if (hash == NULL) {
    scr_file_unlink(file);
    return rc;
  }

  /* create directory to hold app config file */
  spath* dirpath = spath_from_str(file);
  assert(dirpath != NULL);
  int dirrc = spath_dirname(dirpath);
  assert(dirrc == SPATH_SUCCESS);
  const char* dirname = spath_strdup(dirpath);
  spath_delete(&dirpath);
  /* create the directory */
  mode_t mode_dir = scr_getmode(1, 1, 1);
  if (scr_mkdir(dirname, mode_dir) != SCR_SUCCESS) {
    scr_abort(-1, "Failed to create directory %s @ %s:%d",
      dirname, __FILE__, __LINE__
    );
  }
  scr_free(&dirname);

  FILE* fh = fopen(file, "w");
  if (fh != NULL) {
    int success = 1;
    kvtree_elem* topkey;
    for (topkey = kvtree_elem_first(hash);
         topkey != NULL && success;
         topkey = kvtree_elem_next(topkey))
    {
      kvtree_elem* topval;
      for (topval = kvtree_elem_first(kvtree_elem_hash(topkey));
           topval != NULL && success;
           topval = kvtree_elem_next(topval))
      {
        /* NULL values mark deleted entries */
        if (topval == NULL) {
          continue;
        }

        if (fprintf(fh, "%s=%s",
            kvtree_elem_key(topkey), kvtree_elem_key(topval)) < 0)
        {
          success = 0;
          break;
        }

        kvtree_elem* key;
        for (key = kvtree_elem_first(kvtree_elem_hash(topval));
             key != NULL;
             key = kvtree_elem_next(key))
        {
          kvtree_elem *val = kvtree_elem_first(kvtree_elem_hash(key));

          /* NULL values mark deleted entries */
          if (val == NULL) {
            continue;
          }

          if (fprintf(fh, " %s=%s",
              kvtree_elem_key(key), kvtree_elem_key(val)) < 0)
          {
            success = 0;
            break;
          }

          /* assert that app hash is at most a 2-level deep nesting */
          assert(kvtree_elem_first(kvtree_elem_hash(val)) == NULL);
        } /* for key */

        if (fputc('\n', fh) == EOF) {
          success = 0;
          break;
        }
      } /* for topval */
    } /* for topkey */

    if (! success) {
      scr_err("Failed to write to config file: '%s' %s @ %s:%d",
        file, strerror(errno), __FILE__, __LINE__
      );
      rc = SCR_FAILURE;
    }

    if (fclose(fh) != 0) {
      scr_err("Failed to close config file after writing: '%s' %s @ %s:%d",
        file, strerror(errno), __FILE__, __LINE__
      );
      rc = SCR_FAILURE;
    }
  } else {
    scr_err("Failed to open config file for writing: '%s' %s @ %s:%d",
      file, strerror(errno), __FILE__, __LINE__
    );
    rc = SCR_FAILURE;
  }

  return rc;
}
