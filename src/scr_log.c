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

/* Implements a logging interface for SCR events and file transfer operations */

#include "scr_conf.h"
#include "scr.h"
#include "scr_err.h"
#include "scr_io.h"
#include "scr_util.h"
#include "scr_param.h"
#include "scr_log.h"

#include "kvtree.h"
#include "kvtree_util.h"

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <errno.h>

/* gettimeofday */
#include <sys/time.h>

/* localtime, asctime */
#include <time.h>

#include <syslog.h>

#ifdef HAVE_LIBMYSQLCLIENT
#include <mysql.h>
#endif

static char* id_username = NULL;
static char* id_hostname = NULL;
static char* id_prefix   = NULL;
static char* id_jobid    = NULL;

static int   txt_enable      = SCR_LOG_TXT_ENABLE; /* whether to log event in text file */
static int   txt_initialized = 0;                  /* flag indicating whether we have opened the log file */
static char* txt_name        = NULL;               /* name of log file */
static int   txt_fd          = -1;                 /* file descriptor of log file */

static int syslog_enable = SCR_LOG_SYSLOG_ENABLE; /* whether to write log messages to syslog */

static int db_enable = 0;    /* whether to log event in SCR log database */
static int db_debug  = 0;    /* database debug level */
static char* db_host = NULL; /* hostname or IP running DB server */
static char* db_user = NULL; /* username to use to connect to DB server */
static char* db_pass = NULL; /* password to use to connect to DB server */
static char* db_name = NULL; /* database name to connect to */

/*
=========================================
MySQL functions
=========================================
*/

#ifdef HAVE_LIBMYSQLCLIENT
static MYSQL scr_mysql; /* caches mysql object */
#endif

static unsigned long scr_db_jobid = 0; /* caches the jobid for the current job */

#ifdef HAVE_LIBMYSQLCLIENT
static kvtree* scr_db_types = NULL; /* caches type string to type id lookups */
#endif

/* connects to the SCR log database */
int scr_mysql_connect(
  const char* host,
  const char* user,
  const char* pass,
  const char* name)
{
#ifdef HAVE_LIBMYSQLCLIENT
  /* create our type-string-to-id cache */
  scr_db_types = kvtree_new();
  if (scr_db_types == NULL) {
    scr_err("Failed to create a hash to cache type string to id lookups @ %s:%d",
            __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* initialize our database structure */
  mysql_init(&scr_mysql);

  /* connect to the database */
  if (! mysql_real_connect(&scr_mysql, host, user, pass, name, 0, NULL, 0)) {
    scr_err("Failed to connect to SCR log database %s on host %s for user %s @ %s:%d",
            name, host, user, __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

#endif
  return SCR_SUCCESS;
}

/* disconnects from SCR log database */
int scr_mysql_disconnect()
{
#ifdef HAVE_LIBMYSQLCLIENT
  /* free our type string to id cache */
  kvtree_delete(&scr_db_types);

  mysql_close(&scr_mysql);
#endif
  return SCR_SUCCESS;
}

/* allocate a new string, having escaped all internal quotes
 * using mysql_real_escape_string, escaping is needed
 * in case values to be inserted have quotes,
 * caller is responsible for freeing string */
char* scr_mysql_quote_string(const char* value)
{
#ifdef HAVE_LIBMYSQLCLIENT
  if (value != NULL) {
    /* start with a leading single quote, escape internal quotes
     * by adding a leading backslash (could double length of input
     * string), then end with trailing single quote and
     * terminating NUL */
    int n = strlen(value);
    char* q = (char*) malloc(2*n+1+2);
    if (q != NULL) {
      q[0] = '\'';
      mysql_real_escape_string(&scr_mysql, &q[1], value, n);
      n = strlen(q);
      q[n] = '\'';
      q[n+1] = '\0';
      return q;
    } else {
      scr_err("Failed to allocate buffer space to encode string %s @ %s:%d",
              value, __FILE__, __LINE__
      );
      return NULL;
    }
  } else {
    return strdup("NULL");
  }
#else
  return NULL;
#endif
}

/* given a number of seconds since the epoch, fill in a string for mysql datetime */
char* scr_mysql_quote_seconds(const time_t* value)
{
#ifdef HAVE_LIBMYSQLCLIENT
  if (value != NULL) {
    char buf[30];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(value));
    return scr_mysql_quote_string(buf);
  } else {
    return scr_mysql_quote_string((char*) value);
  }
#else
  return NULL;
#endif
}

/* allocate string of quoted integer value */
char* scr_mysql_quote_int(const int* value)
{
#ifdef HAVE_LIBMYSQLCLIENT
  if (value != NULL) {
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "%d", *value);

    /* check that we were able to print the value ok */
    if (n >= sizeof(buf)) {
      scr_err("Insufficient buffer space (%lu bytes) to encode value (%lu needed) @ %s:%d",
              sizeof(buf), n, __FILE__, __LINE__
      );
      return NULL;
    }

    return scr_mysql_quote_string(buf);
  } else {
    return scr_mysql_quote_string((char*) value);
  }
#else
  return NULL;
#endif
}

/* allocate string of quoted double value */
char* scr_mysql_quote_double(const double* value)
{
#ifdef HAVE_LIBMYSQLCLIENT
  if (value != NULL) {
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "%f", *value);

    /* check that we were able to print the value ok */
    if (n >= sizeof(buf)) {
      scr_err("Insufficient buffer space (%lu bytes) to encode value (%lu needed) @ %s:%d",
              sizeof(buf), n, __FILE__, __LINE__
      );
      return NULL;
    }

    return scr_mysql_quote_string(buf);
  } else {
    return scr_mysql_quote_string((char*) value);
  }
#else
  return NULL;
#endif
}

/* lookup name in table and return id if found,
 * returns SCR_FAILURE on error or if name is not found */
int scr_mysql_read_id(const char* table, const char* name, unsigned long* id)
{
#ifdef HAVE_LIBMYSQLCLIENT
  /* escape parameter */
  char* qname = scr_mysql_quote_string(name);

  /* check that we got valid strings for each of our parameters */
  if (qname == NULL) {
    scr_err("Failed to escape and quote one or more arguments @ %s:%d",
            __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* construct the query */
  char query[1024];
  int n = snprintf(query, sizeof(query),
    "SELECT * FROM `%s` WHERE `name` = %s ;",
    table, qname
  );

  /* free the strings as they are now encoded into the query */
  scr_free(&qname);

  /* check that we were able to construct the query ok */
  if (n >= sizeof(query)) {
    scr_err("Insufficient buffer space (%lu bytes) to build query (%lu bytes) @ %s:%d",
            sizeof(query), n, __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* execute the query */
  if (db_debug >= 1) {
    scr_dbg(0, "%s", query);
  }
  if (mysql_real_query(&scr_mysql, query, (unsigned int) strlen(query))) {
    scr_err("Select failed, query = (%s), error = (%s) @ %s:%d",
            query, mysql_error(&scr_mysql), __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* prepare the result set to be used */
  MYSQL_RES* res = mysql_store_result(&scr_mysql);
  if (res == NULL) {
    scr_err("Result failed, query = (%s), error = (%s) @ %s:%d",
            query, mysql_error(&scr_mysql), __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* get the number of rows in the result set */
  my_ulonglong nrows = mysql_num_rows(res);
  if (nrows != 1) {
    mysql_free_result(res);
    return SCR_FAILURE;
  }

  /* finally, lookup our id */
  MYSQL_ROW row = mysql_fetch_row(res);
  if (row == NULL) {
    scr_err("Row fetch failed, query = (%s), error = (%s) @ %s:%d",
            query, mysql_error(&scr_mysql), __FILE__, __LINE__
    );
    mysql_free_result(res);
    return SCR_FAILURE;
  }
  *id = strtoul(row[0], NULL, 0);

  /* free the result set */
  mysql_free_result(res);

#endif
  return SCR_SUCCESS;
}

/* lookup name in table, insert if it doesn't exist, and return id */
int scr_mysql_read_write_id(const char* table, const char* name, unsigned long* id)
{
  int rc = SCR_SUCCESS;

#ifdef HAVE_LIBMYSQLCLIENT
  /* if the value is already in the database, return its id */
  rc = scr_mysql_read_id(table, name, id);
  if (rc == SCR_SUCCESS) {
    return SCR_SUCCESS;
  }

  /* didn't find the value in the db, so let's add it */

  /* escape parameter */
  char* qname = scr_mysql_quote_string(name);

  /* check that we got valid strings for each of our parameters */
  if (qname == NULL) {
    scr_err("Failed to escape and quote one or more arguments @ %s:%d",
            __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* construct the query */
  char query[1024];
  int n = snprintf(query, sizeof(query),
    "INSERT IGNORE INTO `%s` (`id`,`name`) VALUES (NULL, %s) ;",
    table, qname
  );

  /* free the strings as they are now encoded into the query */
  scr_free(&qname);

  /* check that we were able to construct the query ok */
  if (n >= sizeof(query)) {
    scr_err("Insufficient buffer space (%lu bytes) to build query (%lu bytes) @ %s:%d",
            sizeof(query), n, __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* execute the query */
  if (db_debug >= 1) {
    scr_dbg(0, "%s", query);
  }
  if (mysql_real_query(&scr_mysql, query, (unsigned int) strlen(query))) {
    scr_err("Insert failed, query = (%s), error = (%s) @ %s:%d",
            query, mysql_error(&scr_mysql), __FILE__, __LINE__
    );
    /* don't return failure, since another process may have just beat us to the punch */
    /*return SCR_FAILURE;*/
  }

  /* alright, now we should be able to read the id */
  rc = scr_mysql_read_id(table, name, id);

#endif
  return rc;
}

/* lookups a type string and returns its id, 
 * inserts string into types table if not found,
 * caches lookups to avoid database reading more than once */
int scr_mysql_type_id(const char* type, int* id)
{
#ifdef HAVE_LIBMYSQLCLIENT
  /* check that we don't have a NULL string */
  if (type == NULL) {
    scr_err("Type string is NULL @ %s:%d",
            __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* first check the hash in case we can avoid reading from the database */
  unsigned long tmp_id;
  if (kvtree_util_get_unsigned_long(scr_db_types, type, &tmp_id) == KVTREE_SUCCESS) {
    /* found our id from the hash, convert to an int and return */
    *id = (int) tmp_id;
    return SCR_SUCCESS;
  }

  /* failed to find our id in the hash, lookup the id for our jobname */
  if (scr_mysql_read_write_id("types", type, &tmp_id) != SCR_SUCCESS) {
    scr_err("Failed to find type_id for %s @ %s:%d",
            type, __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* got our id, now cache the lookup */
  kvtree_util_set_unsigned_long(scr_db_types, type, tmp_id);

  /* cast the id down to an int */
  *id = (int) tmp_id;
#endif
  return SCR_SUCCESS;
}

/* records an SCR event in the SCR log database */
int scr_mysql_log_event(
  const char* type,
  const char* note,
  const int* dset,
  const char* name,
  const time_t* start,
  const double* secs)
{
#ifdef HAVE_LIBMYSQLCLIENT
  /* lookup the id for the type string */
  int type_id = -1;
  if (scr_mysql_type_id(type, &type_id) == SCR_FAILURE) {
    scr_err("Failed to lookup id for type string %s @ %s:%d",
            type, __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  char* qnote  = scr_mysql_quote_string(note);
  char* qdset  = scr_mysql_quote_int(dset);
  char* qname  = scr_mysql_quote_string(name);
  char* qstart = scr_mysql_quote_seconds(start);
  char* qsecs  = scr_mysql_quote_double(secs);

  /* check that we got valid strings for each of our parameters */
  if (qnote  == NULL ||
      qdset  == NULL ||
      qname  == NULL ||
      qstart == NULL ||
      qsecs  == NULL)
  {
    scr_err("Failed to escape and quote one or more arguments @ %s:%d",
            __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* construct the query */
  char query[4096];
  int n = snprintf(query, sizeof(query),
    "INSERT"
    " INTO `events`"
    " (`id`,`job_id`,`type_id`,`dset_id`,`dset_name`,`start`,`secs`,`note`)"
    " VALUES"
    " (NULL, %lu, %d, %s, %s, %s, %s, %s)"
    " ;",
    scr_db_jobid, type_id, qdset, qname, qstart, qsecs, qnote
  );

  /* free the strings as they are now encoded into the query */
  scr_free(&qnote);
  scr_free(&qdset);
  scr_free(&qname);
  scr_free(&qstart);
  scr_free(&qsecs);

  /* check that we were able to construct the query ok */
  if (n >= sizeof(query)) {
    scr_err("Insufficient buffer space (%lu bytes) to build query (%lu bytes) @ %s:%d",
            sizeof(query), n, __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* execute the query */
  if (db_debug >= 1) {
    scr_dbg(0, "%s", query);
  }
  if (mysql_real_query(&scr_mysql, query, (unsigned int) strlen(query))) {
    scr_err("Insert failed, query = (%s), error = (%s) @ %s:%d",
            query, mysql_error(&scr_mysql), __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

#endif
  return SCR_SUCCESS;
}

/* records an SCR file transfer (copy/fetch/flush/drain) in the SCR log database */
int scr_mysql_log_transfer(
  const char* type,
  const char* from,
  const char* to,
  const int* dset,
  const char* name,
  const time_t* start,
  const double* secs,
  const double* bytes,
  const int* files)
{
#ifdef HAVE_LIBMYSQLCLIENT
  /* lookup the id for the type string */
  int type_id = -1;
  if (scr_mysql_type_id(type, &type_id) == SCR_FAILURE) {
    scr_err("Failed to lookup id for type string %s @ %s:%d",
            type, __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* compute end epoch, using trucation here */
  time_t* end = NULL;
  time_t end_val;
  if (start != NULL && secs != NULL) {
    end_val = *start + (time_t) *secs;
    end = &end_val;
  }

  /* compute the number of seconds and the bandwidth of the operation */
  double* bw = NULL;
  double bw_val;
  if (bytes != NULL && secs != NULL && *secs > 0.0) {
    bw_val = *bytes / *secs;
    bw = &bw_val;
  }
  
  /* convert seconds since epoch to mysql datetime strings */
  char* qfrom  = scr_mysql_quote_string(from);
  char* qto    = scr_mysql_quote_string(to);
  char* qdset  = scr_mysql_quote_int(dset);
  char* qname  = scr_mysql_quote_string(name);
  char* qstart = scr_mysql_quote_seconds(start);
  char* qend   = scr_mysql_quote_seconds(end);
  char* qsecs  = scr_mysql_quote_double(secs);
  char* qbytes = scr_mysql_quote_double(bytes);
  char* qbw    = scr_mysql_quote_double(bw);
  char* qfiles = scr_mysql_quote_int(files);

  /* check that we got valid strings for each of our parameters */
  if (qfrom  == NULL ||
      qto    == NULL ||
      qdset  == NULL ||
      qname  == NULL ||
      qstart == NULL ||
      qend   == NULL ||
      qsecs  == NULL ||
      qbytes == NULL ||
      qbw    == NULL ||
      qfiles == NULL)
  {
    scr_err("Failed to escape and quote one or more arguments @ %s:%d",
            __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* construct the query */
  char query[4096];
  int n = snprintf(query, sizeof(query),
    "INSERT"
    " INTO `transfers`"
    " (`id`,`job_id`,`type_id`,`dset_id`,`dset_name`,`start`,`end`,`secs`,`bytes`,`bw`,`files`,`from`,`to`)"
    " VALUES"
    " (NULL, %lu, %d, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)"
    " ;",
    scr_db_jobid, type_id, qdset, qname, qstart, qend, qsecs, qbytes, qbw, qfiles, qfrom, qto
  );

  /* free the strings as they are now encoded into the query */
  scr_free(&qfrom);
  scr_free(&qto);
  scr_free(&qdset);
  scr_free(&qname);
  scr_free(&qstart);
  scr_free(&qend);
  scr_free(&qsecs);
  scr_free(&qbytes);
  scr_free(&qbw);
  scr_free(&qfiles);

  /* check that we were able to construct the query ok */
  if (n >= sizeof(query)) {
    scr_err("Insufficient buffer space (%lu bytes) to build query (%lu bytes) @ %s:%d",
            sizeof(query), n, __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* execute the query */
  if (db_debug >= 1) {
    scr_dbg(0, "%s", query);
  }
  if (mysql_real_query(&scr_mysql, query, (unsigned int) strlen(query))) {
    scr_err("Insert failed, query = (%s), error = (%s) @ %s:%d",
            query, mysql_error(&scr_mysql), __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

#endif
  return SCR_SUCCESS;
}

int scr_mysql_read_job(unsigned long username_id, unsigned long jobname_id, unsigned long* id)
{
#ifdef HAVE_LIBMYSQLCLIENT
  /* TODO: need to escape parameters */
  /* construct the query */
  char query[1024];
  int n = snprintf(query, sizeof(query),
    "SELECT * FROM `jobs` WHERE `username_id` = '%lu' AND `jobname_id` = '%lu' ;",
    username_id, jobname_id
  );

  /* check that we were able to construct the query ok */
  if (n >= sizeof(query)) {
    scr_err("Insufficient buffer space (%lu bytes) to build query (%lu bytes) @ %s:%d",
            sizeof(query), n, __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* execute the query */
  if (db_debug >= 1) {
    scr_dbg(0, "%s", query);
  }
  if (mysql_real_query(&scr_mysql, query, (unsigned int) strlen(query))) {
    scr_err("Select failed, query = (%s), error = (%s) @ %s:%d",
            query, mysql_error(&scr_mysql), __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* prepare the result set to be used */
  MYSQL_RES* res = mysql_store_result(&scr_mysql);
  if (res == NULL) {
    scr_err("Result failed, query = (%s), error = (%s) @ %s:%d",
            query, mysql_error(&scr_mysql), __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* get the number of rows in the result set */
  my_ulonglong nrows = mysql_num_rows(res);
  if (nrows != 1) {
    mysql_free_result(res);
    return SCR_FAILURE;
  }

  /* finally, lookup our id */
  MYSQL_ROW row = mysql_fetch_row(res);
  if (row == NULL) {
    scr_err("Row fetch failed, query = (%s), error = (%s) @ %s:%d",
            query, mysql_error(&scr_mysql), __FILE__, __LINE__
    );
    mysql_free_result(res);
    return SCR_FAILURE;
  }
  *id = strtoul(row[0], NULL, 0);

  /* free the result set */
  mysql_free_result(res);

#endif
  return SCR_SUCCESS;
}

int scr_mysql_register_job(
  const char* username,
  const char* jobname,
  unsigned long start,
  unsigned long* jobid)
{
  int rc = SCR_SUCCESS;

#ifdef HAVE_LIBMYSQLCLIENT
  /* lookup the id for our username */
  unsigned long username_id;
  rc = scr_mysql_read_write_id("usernames", username, &username_id);
  if (rc != SCR_SUCCESS) {
    scr_err("Failed to find username_id for %s @ %s:%d",
            username, __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* lookup the id for our jobname */
  unsigned long jobname_id;
  rc = scr_mysql_read_write_id("jobnames", jobname, &jobname_id);
  if (rc != SCR_SUCCESS) {
    scr_err("Failed to find jobname_id for %s @ %s:%d",
            jobname, __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* if this job already has a db id, return it */
  rc = scr_mysql_read_job(username_id, jobname_id, jobid);
  if (rc == SCR_SUCCESS) {
    return SCR_SUCCESS;
  }

  /* didn't find the job, so we need to insert a new record into the db */

  /* translate unix seconds since epoch into mysql datetime field */
  time_t start_time_t = (time_t) start;
  char* qsecs = scr_mysql_quote_seconds(&start_time_t);

  /* check that we got valid strings for each of our parameters */
  if (qsecs == NULL) {
    scr_err("Failed to escape and quote one or more arguments @ %s:%d",
            __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* construct the query */
  char query[1024];
  int n = snprintf(query, sizeof(query),
    "INSERT IGNORE"
    " INTO `jobs`"
    " (`id`,`username_id`,`jobname_id`,`start`)"
    " VALUES"
    " (NULL, %lu, %lu, %s)"
    " ;",
    username_id, jobname_id, qsecs
  );

  /* free the strings as they are now encoded into the query */
  scr_free(&qsecs);

  /* check that we were able to construct the query ok */
  if (n >= sizeof(query)) {
    scr_err("Insufficient buffer space (%lu bytes) to build query (%lu bytes) @ %s:%d",
            sizeof(query), n, __FILE__, __LINE__
    );
    return SCR_FAILURE;
  }

  /* execute the query */
  if (db_debug >= 1) {
    scr_dbg(0, "%s", query);
  }
  if (mysql_real_query(&scr_mysql, query, (unsigned int) strlen(query))) {
    scr_err("Insert failed, query = (%s), error = (%s) @ %s:%d",
            query, mysql_error(&scr_mysql), __FILE__, __LINE__
    );
    /* don't return failure, since another process may have just beat us to the punch */
    /*return SCR_FAILURE;*/
  }

  /* now the job should be in the db, so read again to get its id */
  rc = scr_mysql_read_job(username_id, jobname_id, jobid);

#endif
  return rc;
}

/*
=========================================
Log functions
=========================================
*/

/* returns the current linux timestamp */
time_t scr_log_seconds()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  time_t now = tv.tv_sec;
  return now;
}

/* initialize text file logging in prefix directory */
int scr_log_init_txt(const char* prefix)
{
  int rc = SCR_SUCCESS;

  txt_enable = 1;

  if (! txt_initialized) {
    /* build path to log file */
    char logname[SCR_MAX_FILENAME];
    snprintf(logname, sizeof(logname), "%s/.scr/log", prefix);
    txt_name = strdup(logname);

    /* open log file */
    txt_fd = scr_open(txt_name, O_WRONLY | O_CREAT | O_APPEND, S_IWUSR | S_IRUSR);
    if (txt_fd < 0) {
      scr_err("Failed to open log file: `%s' errno=%d (%s) @ %s:%d",
        txt_name, errno, strerror(errno), __FILE__, __LINE__
      );
      txt_enable = 0;
      scr_free(&txt_name);
      return SCR_FAILURE;
    }

    txt_initialized = 1;
  }

  return rc; 
}

/* initialize syslog logging */
int scr_log_init_syslog(void)
{
  int rc = SCR_SUCCESS;

  syslog_enable = 1;

  /* open connection to syslog if we're using it,
   * file messages under "SCR" */
  openlog(SCR_LOG_SYSLOG_PREFIX, LOG_ODELAY, SCR_LOG_SYSLOG_FACILITY);

  return rc; 
}

/* initialize the mysql database logging */
int scr_log_init_db(
  int debug,
  const char* host,
  const char* user,
  const char* pass,
  const char* name)
{
  int rc = SCR_SUCCESS;

  db_enable = 1;

  /* read in the debug level for database log messages */
  db_debug = debug;

  /* connect to the database, if enabled */
  if (scr_mysql_connect(host, user, pass, name) != SCR_SUCCESS) {
    scr_err("Failed to connect to SCR logging database, disabling database logging @ %s:%d",
            __FILE__, __LINE__
    );
    db_enable = 0;
    rc = SCR_FAILURE;
  }

  return rc; 
}

/* initialize the logging */
int scr_log_init(const char* prefix)
{
  int tmp_rc;

  int rc = SCR_SUCCESS;

  /* read in parameters */
  const char* value = NULL;

  scr_param_init();

  /* check whether SCR logging DB is enabled */
  if ((value = scr_param_get("SCR_LOG_TXT_ENABLE")) != NULL) {
    txt_enable = atoi(value);
  }

  /* check whether SCR logging DB is enabled */
  if ((value = scr_param_get("SCR_LOG_SYSLOG_ENABLE")) != NULL) {
    syslog_enable = atoi(value);
  }

  /* check whether SCR logging DB is enabled */
  if ((value = scr_param_get("SCR_LOG_DB_ENABLE")) != NULL) {
    db_enable = atoi(value);
  }

  /* read in the debug level for database log messages */
  if ((value = scr_param_get("SCR_LOG_DB_DEBUG")) != NULL) {
    db_debug = atoi(value);
  }

  /* SCR log DB connection parameters */
  if ((value = scr_param_get("SCR_LOG_DB_HOST")) != NULL) {
    db_host = strdup(value);
  }
  if ((value = scr_param_get("SCR_LOG_DB_USER")) != NULL) {
    db_user = strdup(value);
  }
  if ((value = scr_param_get("SCR_LOG_DB_PASS")) != NULL) {
    db_pass = strdup(value);
  }
  if ((value = scr_param_get("SCR_LOG_DB_NAME")) != NULL) {
    db_name = strdup(value);
  }

  /* open log file if enabled */
  if (txt_enable) {
    tmp_rc = scr_log_init_txt(prefix);
    if (tmp_rc != SCR_SUCCESS) {
      rc = tmp_rc;
    }
  }

  /* open connection to syslog if we're using it,
   * file messages under "SCR" */
  if (syslog_enable) {
    tmp_rc = scr_log_init_syslog();
    if (tmp_rc != SCR_SUCCESS) {
      rc = tmp_rc;
    }
  }

  /* connect to the database, if enabled */
  if (db_enable) {
    tmp_rc = scr_log_init_db(db_debug, db_host, db_user, db_pass, db_name);
    if (tmp_rc != SCR_SUCCESS) {
      rc = tmp_rc;
    }
  }

  return rc; 
}

/* shut down the logging */
int scr_log_finalize()
{
  /* close log file if we opened one */
  if (txt_enable) {
    if (txt_fd >= 0) {
      scr_close(txt_name, txt_fd);
      txt_fd = -1;
    }
    scr_free(&txt_name);
  }

  /* close syslog if we're using it */
  if (syslog_enable) {
    closelog();
  }

  /* disconnect from database */
  if (db_enable) {
    scr_mysql_disconnect();
  }

  /* free memory */
  scr_free(&db_host);
  scr_free(&db_user);
  scr_free(&db_pass);
  scr_free(&db_name);

  scr_free(&id_username);
  scr_free(&id_hostname);
  scr_free(&id_prefix);
  scr_free(&id_jobid);

  return SCR_SUCCESS;
}

/* given a username, a jobname, and a start time, lookup (or create) the id for this job */
int scr_log_job(
  const char* username,
  const char* hostname,
  const char* jobid,
  const char* prefix,
  time_t start)
{
  int rc = SCR_SUCCESS;

  /* TODO: rather than jobname, which most people won't define,
   * we could capture the prefix directory instead, and maybe
   * hash that to hide details */

  /* copy user and job name to use in other log entries */
  id_username = strdup(username);
  id_hostname = strdup(hostname);
  id_jobid    = strdup(jobid);
  id_prefix   = strdup(prefix);

  if (db_enable) {
    if (username != NULL && prefix != NULL) {
      int rc = scr_mysql_register_job(username, prefix, start, &scr_db_jobid);
      if (rc != SCR_SUCCESS) {
        scr_err("Failed to register job for username %s and prefix %s, disabling database logging @ %s:%d",
                username, prefix, __FILE__, __LINE__
        );
        db_enable = 0;
        rc = SCR_FAILURE;
      }
    } else {
      scr_err("Failed to read username or prefix from environment, disabling database logging @ %s:%d",
              __FILE__, __LINE__
      );
      db_enable = 0;
      rc = SCR_FAILURE;
    }
  }

  return rc;
}

/* log start time of current run */
int scr_log_run(time_t start, int procs, int nodes)
{
  int rc = SCR_SUCCESS;

  struct tm* timeinfo = localtime(&start);
  char timestr[100];
  strftime(timestr, sizeof(timestr), "%s", timeinfo);
  char timestamp[32];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", timeinfo);

  if (txt_enable) {
    char buf[1024];
    size_t remaining = sizeof(buf);
    size_t nwritten = snprintf(buf, remaining,
      "%s: host=%s, jobid=%s, event=%s, procs=%d, nodes=%d",
      timestamp, id_hostname, id_jobid, "START", procs, nodes
    );
    remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    nwritten += snprintf(buf + nwritten, remaining, "\n");
    remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    if (nwritten >= sizeof(buf)) {
        buf[sizeof(buf)-2] = '\n';
        buf[sizeof(buf)-1] = '\0';
    }
    scr_write(txt_name, txt_fd, buf, strlen(buf));
    //fsync(txt_fd);
  }

  if (syslog_enable) {
    char buf[1024];
    size_t remaining = sizeof(buf);
    size_t nwritten = snprintf(buf, remaining,
      "user=%s, jobid=%s, prefix=%s, event=%s, procs=%d, nodes=%d",
      id_username, id_jobid, id_prefix, "START", procs, nodes
    );
    remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    nwritten += snprintf(buf + nwritten, remaining, "\n");
    remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    if (nwritten >= sizeof(buf)) {
        buf[sizeof(buf)-2] = '\n';
        buf[sizeof(buf)-1] = '\0';
    }
    syslog(SCR_LOG_SYSLOG_LEVEL, buf);
  }

  if (db_enable) {
    rc = scr_mysql_log_event("START", NULL, NULL, NULL, &start, NULL);
  }

  return rc;
}

/* log reason and time for halting current run */
int scr_log_halt(const char* reason)
{
  int rc = SCR_SUCCESS;

  time_t now = scr_log_seconds();
  struct tm* timeinfo = localtime(&now);
  char timestr[100];
  strftime(timestr, sizeof(timestr), "%s", timeinfo);
  char timestamp[32];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", timeinfo);

  if (txt_enable) {
    char buf[1024];
    size_t remaining = sizeof(buf);
    size_t nwritten = snprintf(buf, remaining,
      "%s: host=%s, jobid=%s, event=%s",
      timestamp, id_hostname, id_jobid, "HALT"
    );
    remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    if (reason != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", note=\"%s\"", reason);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    nwritten += snprintf(buf + nwritten, remaining, "\n");
    remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    if (nwritten >= sizeof(buf)) {
        buf[sizeof(buf)-2] = '\n';
        buf[sizeof(buf)-1] = '\0';
    }
    scr_write(txt_name, txt_fd, buf, strlen(buf));
    //fsync(txt_fd);
  }

  if (syslog_enable) {
    char buf[1024];
    size_t remaining = sizeof(buf);
    size_t nwritten = snprintf(buf, remaining,
      "user=%s, jobid=%s, prefix=%s, event=%s",
      id_username, id_jobid, id_prefix, "HALT"
    );
    remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    if (reason != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", note=\"%s\"", reason);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    nwritten += snprintf(buf + nwritten, remaining, "\n");
    remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    if (nwritten >= sizeof(buf)) {
        buf[sizeof(buf)-2] = '\n';
        buf[sizeof(buf)-1] = '\0';
    }
    syslog(LOG_INFO, buf);
  }

  if (db_enable) {
    rc = scr_mysql_log_event("HALT", reason, NULL, NULL, &now, NULL);
  }

  return rc;
}

/* log an event */
int scr_log_event(
  const char* type,
  const char* note,
  const int* dset,
  const char* name,
  const time_t* start,
  const double* secs)
{
  int rc = SCR_SUCCESS;

  int    dset_val  = (dset  != NULL) ? *dset  : -1;
  double secs_val  = (secs  != NULL) ? *secs  : 0.0;
  time_t start_val = (start != NULL) ? *start : scr_log_seconds();

  struct tm* timeinfo = localtime(&start_val);
  char timestr[100];
  strftime(timestr, sizeof(timestr), "%s", timeinfo);
  char timestamp[32];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", timeinfo);

  if (txt_enable) {
    char buf[1024];
    size_t remaining = sizeof(buf);
    size_t nwritten = snprintf(buf, remaining,
      "%s: host=%s, jobid=%s, event=%s",
      timestamp, id_hostname, id_jobid, type
    );
    remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    if (note != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", note=\"%s\"", note);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    if (dset != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", dset=%d", dset_val);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    if (name != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", name=\"%s\"", name);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    if (secs != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", secs=%f", secs_val);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    nwritten += snprintf(buf + nwritten, remaining, "\n");
    remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    if (nwritten >= sizeof(buf)) {
        buf[sizeof(buf)-2] = '\n';
        buf[sizeof(buf)-1] = '\0';
    }
    scr_write(txt_name, txt_fd, buf, strlen(buf));
    //fsync(txt_fd);
  }

  if (syslog_enable) {
    char buf[1024];
    size_t remaining = sizeof(buf);
    size_t nwritten = snprintf(buf, remaining,
      "user=%s, jobid=%s, prefix=%s, event=%s",
      id_username, id_jobid, id_prefix, type
    );
    remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    if (note != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", note=\"%s\"", note);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    if (dset != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", dset=%d", dset_val);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    if (name != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", name=\"%s\"", name);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    if (secs != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", secs=%f", secs_val);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    nwritten += snprintf(buf + nwritten, remaining, "\n");
    remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    if (nwritten >= sizeof(buf)) {
        buf[sizeof(buf)-2] = '\n';
        buf[sizeof(buf)-1] = '\0';
    }
    syslog(LOG_INFO, buf);
  }

  if (db_enable) {
    rc = scr_mysql_log_event(type, note, dset, name, &start_val, secs);
  }

  return rc;
}

/* log a transfer: copy / checkpoint / fetch / flush */
int scr_log_transfer(
  const char* type,
  const char* from,
  const char* to,
  const int* dset,
  const char* name,
  const time_t* start,
  const double* secs,
  const double* bytes,
  const int* files)
{
  int rc = SCR_SUCCESS;

  struct tm* timeinfo = localtime(start);
  char timestr[100];
  strftime(timestr, sizeof(timestr), "%s", timeinfo);
  char timestamp[32];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", timeinfo);

  int    dset_val  = (dset != NULL)  ? *dset  : -1;
  double secs_val  = (secs != NULL)  ? *secs  : 0.0;
  double bytes_val = (bytes != NULL) ? *bytes : 0.0;
  int    files_val = (files != NULL) ? *files : 0;

  if (txt_enable) {
    char buf[1024];
    size_t remaining = sizeof(buf);
    size_t nwritten = snprintf(buf, remaining,
      "%s: host=%s, jobid=%s, xfer=%s",
      timestamp, id_hostname, id_jobid, type
    );
    remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    if (from != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", from=%s", from);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    if (to != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", to=%s", to);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    if (dset != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", dset=%d", dset_val);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    if (name != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", name=\"%s\"", name);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    if (secs != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", secs=%f", secs_val);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    if (bytes != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", bytes=%f", bytes_val);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    if (files != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", files=%d", files_val);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    nwritten += snprintf(buf + nwritten, remaining, "\n");
    remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    if (nwritten >= sizeof(buf)) {
        buf[sizeof(buf)-2] = '\n';
        buf[sizeof(buf)-1] = '\0';
    }
    scr_write(txt_name, txt_fd, buf, strlen(buf));
    //fsync(txt_fd);
  }

  if (syslog_enable) {
    char buf[1024];
    size_t remaining = sizeof(buf);
    size_t nwritten = snprintf(buf, remaining,
      "user=%s, jobid=%s, prefix=%s, xfer=%s",
      id_username, id_jobid, id_prefix, type
    );
    remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    if (from != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", from=%s", from);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    if (to != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", to=%s", to);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    if (dset != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", dset=%d", dset_val);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    if (name != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", name=\"%s\"", name);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    if (secs != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", secs=%f", secs_val);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    if (bytes != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", bytes=%f", bytes_val);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    if (files != NULL) {
      nwritten += snprintf(buf + nwritten, remaining, ", files=%d", files_val);
      remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    }
    nwritten += snprintf(buf + nwritten, remaining, "\n");
    remaining = (sizeof(buf) > nwritten) ? sizeof(buf) - nwritten : 0;
    if (nwritten >= sizeof(buf)) {
        buf[sizeof(buf)-2] = '\n';
        buf[sizeof(buf)-1] = '\0';
    }
    syslog(LOG_INFO, buf);
  }

  if (db_enable) {
    rc = scr_mysql_log_transfer(type, from, to, dset, name, start, secs, bytes, files);
  }

  return rc;
}
