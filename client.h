/******************************************************************************\
 *  $Id: client.h,v 1.9 2001/05/29 23:45:24 dun Exp $
 *    by Chris Dunlap <cdunlap@llnl.gov>
\******************************************************************************/


#ifndef _CLIENT_H
#define _CLIENT_H


#include <termios.h>
#include "common.h"
#include "list.h"


typedef struct client_conf {
    req_t          *req;		/* client request info                */
    int             escapeChar;		/* char to issue client escape seq    */
    int             enableVerbose;	/* true if to be more verbose to user */
    char           *log;		/* connection logfile name            */
    int             logd;		/* connection logfile descriptor      */
    int             closedByClient;	/* true if socket closed by client    */
    int             errnum;		/* error number from issuing command  */
    char           *errmsg;		/* error message from issuing command */
    struct termios  term;		/* saved "cooked" terminal mode       */
} client_conf_t;


/*******************\
**  client-conf.c  **
\*******************/

client_conf_t * create_client_conf(void);

void destroy_client_conf(client_conf_t *conf);

void process_client_cmd_line(int argc, char *argv[], client_conf_t *conf);

void open_client_log(client_conf_t *conf);

void close_client_log(client_conf_t *conf);


/*******************\
**  client-sock.c  **
\*******************/

void connect_to_server(client_conf_t *conf);

int send_greeting(client_conf_t *conf);

int send_req(client_conf_t *conf);

int recv_rsp(client_conf_t *conf);

void display_error(client_conf_t *conf);

void display_data(client_conf_t *conf, int fd);

void display_consoles(client_conf_t *conf, int fd);


/******************\
**  client-tty.c  **
\******************/

void connect_console(client_conf_t *conf);

char * write_esc_char(char c, char *p);


#endif /* !_CLIENT_H */
