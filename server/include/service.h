#ifndef _SERVICE_H
#define _SERVICE_H
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

#include <time.h>
#include <spinlock.h>
#include <dcb.h>
#include <server.h>

/**
 * @file service.h
 *
 * The service level definitions within the gateway
 *
 * @verbatim
 * Revision History
 *
 * Date		Who			Description
 * 14/06/13	Mark Riddoch		Initial implementation
 * 18/06/13	Mark Riddoch		Addition of statistics and function
 * 					prototypes
 * 23/06/13	Mark Riddoch		Added service user and users
 * 06/02/14	Massimiliano Pinto	Added service flag for root user access
 *
 * @endverbatim
 */
struct	server;
struct	router;
struct	router_object;
struct	users;

/**
 * The servprotocol structure is used to link a service to the protocols that
 * are used to support that service. It defines the name of the protocol module
 * that should be loaded to support the client connection and the port that the
 * protocol should use to listen for incoming client connections.
 */
typedef struct servprotocol {
	char		*protocol;	/**< Protocol module to load */
	unsigned short	port;		/**< Port to listen on */
	char		*address;	/**< Address to listen with */
	DCB		*listener;	/**< The DCB for the listener */
	struct	servprotocol
			*next;		/**< Next service protocol */
} SERV_PROTOCOL;

/**
 * The service statistics structure
 */
typedef struct {
	time_t		started;	/**< The time when the service was started */
	int		n_sessions;	/**< Number of sessions created on service since start */
	int		n_current;	/**< Current number of sessions */
} SERVICE_STATS;

/**
 * The service user structure holds the information that is needed for this service to
 * allow the gateway to login to the backend database and extact information such as
 * the user table or other database status or configuration data.
 */
typedef struct {
	char		*name;		/**< The user name to use to extract information */
	char		*authdata;	/**< The authentication data requied */
} SERVICE_USER;

/**
 * Defines a service within the gateway.
 *
 * A service is a combination of a set of backend servers, a routing mechanism
 * and a set of client side protocol/port pairs used to listen for new connections
 * to the service.
 */
typedef struct service {
	char		*name;		/**< The service name */
	int		state;		/**< The service state */
	SERV_PROTOCOL	*ports;		/**< Linked list of ports and protocols
					 * that this service will listen on.
					 */
	char		*routerModule;	/**< Name of router module to use */
	char		**routerOptions;/**< Router specific option strings */
	struct router_object
			*router;	/**< The router we are using */
	void		*router_instance;
					/**< The router instance for this service */
	struct server	*databases;	/**< The set of servers in the backend */
	SERVICE_USER	credentials;	/**< The cedentials of the service user */	
	SPINLOCK	spin;		/**< The service spinlock */
	SERVICE_STATS	stats;		/**< The service statistics */
	struct users	*users;		/**< The user data for this service */
	int		enable_root;	/**< Allow root user  access */
	struct service	*next;		/**< The next service in the linked list */
} SERVICE;

#define	SERVICE_STATE_ALLOC	1	/**< The service has been allocated */
#define	SERVICE_STATE_STARTED	2	/**< The service has been started */

extern	SERVICE *service_alloc(char *, char *);
extern	int	service_free(SERVICE *);
extern	SERVICE *service_find(char *);
extern	int	serviceAddProtocol(SERVICE *, char *, char *, unsigned short);
extern	int	serviceHasProtocol(SERVICE *, char *, unsigned short);
extern	void	serviceAddBackend(SERVICE *, SERVER *);
extern	int	serviceHasBackend(SERVICE *, SERVER *);
extern	void	serviceAddRouterOption(SERVICE *, char *);
extern	void	serviceClearRouterOptions(SERVICE *);
extern	int	serviceStart(SERVICE *);
extern	int	serviceStartAll();
extern	void	serviceStartProtocol(SERVICE *, char *, int);
extern	int	serviceStop(SERVICE *);
extern	int	serviceRestart(SERVICE *);
extern	int	serviceSetUser(SERVICE *, char *, char *);
extern	int	serviceGetUser(SERVICE *, char **, char **);
extern	int	serviceEnableRootUser(SERVICE *, int );
extern	void	service_update(SERVICE *, char *, char *, char *);
extern	void	printService(SERVICE *);
extern	void	printAllServices();
extern	void	dprintAllServices(DCB *);
#endif
