/*
 * This file is distributed as part of the SkySQL Gateway.  It is free
 * software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation,
 * version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright SkySQL Ab 2013
 */

/**
 * @file dbusers.c  - Loading MySQL users from a MySQL backend server, this needs libmysqlclient.so and header files
 *
 * @verbatim
 * Revision History
 *
 * Date		Who			Description
 * 24/06/2013	Massimiliano Pinto	Initial implementation
 * 08/08/2013	Massimiliano Pinto	Fixed bug for invalid memory access in row[1]+1 when row[1] is ""
 * 06/02/2014	Massimiliano Pinto	Mysql user root selected based on configuration flag
 * 07/02/2014	Massimiliano Pinto	Added Mysql user@host authentication
 *
 * @endverbatim
 */

#include <stdio.h>
#include <mysql.h>

#include <dcb.h>
#include <service.h>
#include <users.h>
#include <skygw_utils.h>
#include <log_manager.h>
#include <secrets.h>
#include <dbusers.h>

#define USERS_QUERY_NO_ROOT " AND user NOT IN ('root')"
#define LOAD_MYSQL_USERS_QUERY "SELECT user, host, password FROM mysql.user WHERE user IS NOT NULL AND user <> ''"

extern int lm_enabled_logfiles_bitmask;

static int getUsers(SERVICE *service, struct users *users);
static int uh_cmpfun( void* v1, void* v2);
static void *uh_keydup(void* key);
static void uh_keyfree( void* key);
static int uh_hfun( void* key);
char *mysql_users_fetch(USERS *users, MYSQL_USER_HOST *key);
char *mysql_users_print(void *data);

/**
 * Load the user/passwd form mysql.user table into the service users' hashtable
 * environment.
 *
 * @param service   The current service
 * @return      -1 on any error or the number of users inserted (0 means no users at all)
 */
int 
load_mysql_users(SERVICE *service)
{
	return getUsers(service, service->users);
}

/**
 * Reload the user/passwd form mysql.user table into the service users' hashtable
 * environment.
 *
 * @param service   The current service
 * @return      -1 on any error or the number of users inserted (0 means no users at all)
 */
int 
reload_mysql_users(SERVICE *service)
{
int		i;
struct users	*newusers, *oldusers;

	if ((newusers = mysql_users_alloc()) == NULL)
		return 0;
	i = getUsers(service, newusers);
	spinlock_acquire(&service->spin);
	oldusers = service->users;
	service->users = newusers;
	spinlock_release(&service->spin);
	users_free(oldusers);

	return i;
}

/**
 * Load the user/passwd form mysql.user table into the service users' hashtable
 * environment.
 *
 * @param service	The current service
 * @param users		The users table into which to load the users
 * @return      -1 on any error or the number of users inserted (0 means no users at all)
 */
static int
getUsers(SERVICE *service, struct users *users)
{
	MYSQL      *con = NULL;
	MYSQL_ROW  row;
	MYSQL_RES  *result = NULL;
	int        num_fields = 0;
	char       *service_user = NULL;
	char       *service_passwd = NULL;
	char	   *dpwd;
	int        total_users = 0;
	SERVER	   *server;
	struct sockaddr_in serv_addr;
	MYSQL_USER_HOST key;
	char *users_query;

	/* enable_root for MySQL protocol module means load the root user credentials from backend databases */
	if(service->enable_root) {
		users_query = LOAD_MYSQL_USERS_QUERY " ORDER BY HOST DESC";
	} else {
		users_query = LOAD_MYSQL_USERS_QUERY USERS_QUERY_NO_ROOT " ORDER BY HOST DESC";
	}

	serviceGetUser(service, &service_user, &service_passwd);

	/** multi-thread environment requires that thread init succeeds. */
	if (mysql_thread_init()) {
		LOGIF(LE, (skygw_log_write_flush(
                        LOGFILE_ERROR,
                        "Error : mysql_thread_init failed.")));
		return -1;
	}
    
	con = mysql_init(NULL);

 	if (con == NULL) {
		LOGIF(LE, (skygw_log_write_flush(
                        LOGFILE_ERROR,
                        "Error : mysql_init: %s",
                        mysql_error(con))));
		return -1;
	}

	if (mysql_options(con, MYSQL_OPT_USE_REMOTE_CONNECTION, NULL)) {
		LOGIF(LE, (skygw_log_write_flush(
                        LOGFILE_ERROR,
                        "Error : failed to set external connection. "
                        "It is needed for backend server connections. "
                        "Exiting.")));
		return -1;
	}
	/*
	 * Attempt to connect to each database in the service in turn until
	 * we find one that we can connect to or until we run out of databases
	 * to try
	 */
	server = service->databases;
	dpwd = decryptPassword(service_passwd);
	while (server != NULL && mysql_real_connect(con,
                                                    server->name,
                                                    service_user,
                                                    dpwd,
                                                    NULL,
                                                    server->port,
                                                    NULL,
                                                    0) == NULL)
	{
                server = server->nextdb;
	}
	free(dpwd);

	if (server == NULL)
	{
		LOGIF(LE, (skygw_log_write_flush(
                        LOGFILE_ERROR,
                        "Error : Unable to get user data from backend database "
                        "for service %s. Missing server information.",
                        service->name)));
		mysql_close(con);
		return -1;
	}

	if (mysql_query(con, users_query)) {
		LOGIF(LE, (skygw_log_write_flush(
                        LOGFILE_ERROR,
                        "Error : Loading users for service %s encountered "
                        "error: %s.",
                        service->name,
                        mysql_error(con))));
		mysql_close(con);
		return -1;
	}
	result = mysql_store_result(con);
  
	if (result == NULL) {
		LOGIF(LE, (skygw_log_write_flush(
                        LOGFILE_ERROR,
                        "Error : Loading users for service %s encountered "
                        "error: %s.",
                        service->name,
                        mysql_error(con))));
		mysql_close(con);
		return -1;
	}
	num_fields = mysql_num_fields(result);
 
	while ((row = mysql_fetch_row(result))) {
		char ret_ip[INET_ADDRSTRLEN]="";
		/**
                 * Two fields should be returned.
                 * user and passwd+1 (escaping the first byte that is '*') are
                 * added to hashtable.
                 */
		
		/* prepare the user@host data struct */
		memset(&serv_addr, 0, sizeof(serv_addr));
		memset(&key, 0, sizeof(key));

		/* if host == %, 0 is passed */
		if (setipaddress(&serv_addr.sin_addr, strcmp(row[1], "%") ? row[1] : "0.0.0.0")) {

			key.user = strdup(row[0]);

			memcpy(&key.ipv4, &serv_addr, sizeof(serv_addr));

			inet_ntop(AF_INET, &(serv_addr).sin_addr, ret_ip, INET_ADDRSTRLEN);

			LOGIF(LD, (skygw_log_write_flush(
				LOGFILE_ERROR,
				"%lu [mysql_users_add()] Added user %s@%s(%s)\n",
				pthread_self(),
				row[0],
				row[1],
				ret_ip == NULL ? "NULL" : ret_ip)));

			//fprintf(stderr, "Address [%s]: %u.%u.%u.%u\n", row[1], serv_addr.sin_addr.s_addr&0xFF, (serv_addr.sin_addr.s_addr&0xFF00), (serv_addr.sin_addr.s_addr&0xFF0000), (serv_addr.sin_addr.s_addr & 0xFF000000));

			/* add user@host as key and passwd as value in the MySQL users hash table */
			mysql_users_add(users, &key, strlen(row[2]) ? row[2]+1 : row[2]);

			total_users++;
		} else {
			/* setipaddress() failed, skip user add and log this*/
			LOGIF(LE, (skygw_log_write_flush(
				LOGFILE_ERROR,
				"%lu [mysql_users_add()] setipaddress failed: user NOT added %s@%s(%s)\n",
				pthread_self(),
				row[0],
				row[1],
				ret_ip == NULL ? "NULL" : ret_ip)));
		}
	}
	mysql_free_result(result);
	mysql_close(con);
	mysql_thread_end();
	return total_users;
}

/**
 * Allocate a new MySQL users table for mysql specific users@host as key
 *
 *  @return The users table
 */
USERS *
mysql_users_alloc()
{
USERS	*rval;

	if ((rval = calloc(1, sizeof(USERS))) == NULL)
		return NULL;

	if ((rval->data = hashtable_alloc(52, uh_hfun, uh_cmpfun)) == NULL) {
		free(rval);
		return NULL;
	}

	/* set the MySQL user@host print routine for the debug interface */
	rval->usersCustomUserPrint = mysql_users_print;

	/* the key is handled by uh_keydup/uh_keyfree.
	* the value is a (char *): it's handled by strdup/free
	*/
	hashtable_memory_fns(rval->data, (HASHMEMORYFN)uh_keydup, (HASHMEMORYFN) strdup, (HASHMEMORYFN)uh_keyfree, (HASHMEMORYFN)free);

	return rval;
}

/**
 * Add a new MySQL user to the user table. The user name must be unique
 *
 * @param users		The users table
 * @param user		The user name
 * @param auth		The authentication data
 * @return		The number of users added to the table
 */
int
mysql_users_add(USERS *users, MYSQL_USER_HOST *key, char *auth)
{
int     add;

        atomic_add(&users->stats.n_adds, 1);
        add = hashtable_add(users->data, key, auth);
        atomic_add(&users->stats.n_entries, add);

        return add;
}

/**
 * Fetch the authentication data for a particular user from the users table
 *
 * @param users The MySQL users table
 * @param key	The key with user@host
 * @return	The authentication data or NULL on error
 */
char *mysql_users_fetch(USERS *users, MYSQL_USER_HOST *key) {
        atomic_add(&users->stats.n_fetches, 1);
        return hashtable_fetch(users->data, key);
}

/**
 * The hash function we use for storing MySQL users as: users@hosts.
 *
 * @param key	The key value, i.e. username@host ip4/ip6 data
 * @return	The hash key
 */

static int uh_hfun( void* key) {
        MYSQL_USER_HOST *hu = (MYSQL_USER_HOST *) key;

	if (key == NULL || hu == NULL) {
		return 0;
	} else {
        	return (*hu->user + *(hu->user + 1) + (unsigned int) (hu->ipv4.sin_addr.s_addr & 0xFF000000 / (256 * 256 * 256)));
	}
}

/**
 * The compare function we use for compare MySQL users as: users@hosts.
 *
 * @param key1	The key value, i.e. username@host ip4/ip6 data
 * @param key1	The key value, i.e. username@host ip4/ip6 data
 * @return	The compare value
 */

static int uh_cmpfun( void* v1, void* v2) {
	MYSQL_USER_HOST *hu1 = (MYSQL_USER_HOST *) v1;
	MYSQL_USER_HOST *hu2 = (MYSQL_USER_HOST *) v2;

	if (strcmp(hu1->user, hu2->user) == 0 && (hu1->ipv4.sin_addr.s_addr == hu2->ipv4.sin_addr.s_addr)) {
		return 0;
	} else {
		return 1;
	}
}

/**
 *The key dup function we use for duplicate the users@hosts.
 *
 * @param key	The key value, i.e. username@host ip4/ip6 data
 */

static void *uh_keydup(void* key) {
	MYSQL_USER_HOST *rval = (MYSQL_USER_HOST *) calloc(1, sizeof(MYSQL_USER_HOST));
	MYSQL_USER_HOST *current_key = (MYSQL_USER_HOST *)key;

	rval->user = strdup(current_key->user);
	memcpy(&rval->ipv4, &current_key->ipv4, sizeof(struct sockaddr_in));

	return (void *) rval;
}

/**
 * The key free function we use for freeing the users@hosts data
 *
 * @param key	The key value, i.e. username@host ip4 data
 */
static void uh_keyfree( void* key) {
	MYSQL_USER_HOST *current_key = (MYSQL_USER_HOST *)key;

	free(current_key->user);
	free(key);
}

/**
 * Print details of the mysql_users storage mechanism to a DCB
 *
 *  @param data		Input data
 *  @return 		the MySQL user@host
 */
char *mysql_users_print(void *data)
{
	MYSQL_USER_HOST *entry;
	char *mysql_user;
	/* the returned user string is "USER@HOST" */
	int mysql_user_len = 128 + 1 + INET_ADDRSTRLEN + 1;

	if (data == NULL)
		return NULL;
	
        entry = (MYSQL_USER_HOST *) data;

	if (entry == NULL)
		return NULL;

	mysql_user = (char *) calloc(mysql_user_len, sizeof(char));

	if (mysql_user == NULL)
		return NULL;
	
	if (entry->ipv4.sin_addr.s_addr == INADDR_ANY) {
		snprintf(mysql_user, mysql_user_len, "%s@%%", entry->user);
	} else {
		snprintf(mysql_user, 128, entry->user);
		strcat(mysql_user, "@");
		inet_ntop(AF_INET, &(entry->ipv4).sin_addr, mysql_user+strlen(mysql_user), INET_ADDRSTRLEN);
	}

        return mysql_user;
}
