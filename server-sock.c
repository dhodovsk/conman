/******************************************************************************\
 *  server-sock.c  
 *    by Chris Dunlap <cdunlap@llnl.gov>
 *
 *  $Id: server-sock.c,v 1.5 2001/05/14 21:55:27 dun Exp $
\******************************************************************************/


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "conman.h"
#include "errors.h"
#include "lex.h"
#include "server.h"
#include "util.h"


typedef struct request {
    int    sd;				/* client socket descriptor           */
    char  *ip;				/* ip addr string of client           */
    char  *host;			/* name of client host, or NULL       */
    char  *user;			/* name of client user                */
    cmd_t  command;			/* command to perform for client      */
    int    enableBroadcast;		/* true if b-casting to many consoles */
    int    enableForce;			/* true if forcing console connection */
    char  *program;			/* program name for EXECUTE cmd       */
    List   consoles;			/* list of consoles affected by cmd   */
} req_t;


static req_t * create_req(int sd);
static void destroy_req(req_t *req);
static int recv_greeting(req_t *req);
static int parse_greeting(Lex l, req_t *req);
static int recv_req(req_t *req);
static int parse_cmd_opts(Lex l, req_t *req);
static int query_consoles(server_conf_t *conf, req_t *req);
static int validate_req(req_t *req);
static int send_rsp(req_t *req, int errnum, char *errmsg);
static void perform_query_cmd(req_t *req);
static void perform_monitor_cmd(req_t *req, server_conf_t *conf);
static void perform_connect_cmd(req_t *req, server_conf_t *conf);
static void perform_execute_cmd(req_t *req, server_conf_t *conf);


void process_client(server_conf_t *conf)
{
/*  The thread responsible for accepting a client connection
 *    and processing the request.
 *  The QUERY cmd is processed entirely by this thread.
 *  The MONITOR and CONNECT cmds are setup and then placed
 *    in the conf->objs list to be handled by mux_io().
 *  The EXECUTE cmd still puzzles me somewhat...
 */
    int rc;
    int sd;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    char buf[MAX_LINE];
    req_t *req;

    if ((rc = pthread_detach(pthread_self())) != 0)
        err_msg(rc, "pthread_detach() failed");

    /*  The accept() is performed within this thread instead of mux_io()
     *    because this routine needs to have the conf ptr passed in as an
     *    arg in order to perform query_consoles(), etc.
     *  Sure, I could have created a struct in which to stuff both the
     *    conf ptr and the sd of the new connection, but that seemed silly.
     */
    while ((sd = accept(conf->ld, &addr, &addrlen)) < 0) {
        if (errno == EINTR)
            continue;
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
            return;
        if (errno == ECONNABORTED)
            return;
        err_msg(errno, "accept() failed");
    }

    if (!inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf))) {
        err_msg(errno, "inet_ntop() failed");
    }
    if (!(req = create_req(sd))) {
        log_msg(0, "Request from %s terminated: out of memory", buf);
        if (close(sd) < 0)
            err_msg(errno, "close(%d) failed", sd);
        return;
    }
    if (!(req->ip = strdup(buf))) {
        log_msg(0, "Request from %s terminated: out of memory", buf);
        goto err;
    }
    if (get_hostname_via_addr(&addr.sin_addr, buf, sizeof(buf))) {
        if (!(req->host = strdup(buf))) {
            log_msg(0, "Request from %s terminated: out of memory", buf);
            goto err;
        }
    }

    if (recv_greeting(req) < 0)
        goto err;
    if (recv_req(req) < 0)
        goto err;
    if (query_consoles(conf, req) < 0)
        goto err;
    if (validate_req(req) < 0)
        goto err;

    switch(req->command) {
    case CONNECT:
        perform_connect_cmd(req, conf);
        break;
    case EXECUTE:
        perform_execute_cmd(req, conf);
        break;
    case MONITOR:
        perform_monitor_cmd(req, conf);
        break;
    case QUERY:
        perform_query_cmd(req);
        break;
    default:
        /*  This should not happen, as invalid commands
         *    will be detected by validate_req().
         */
        log_msg(0, "INTERNAL ERROR: Received invalid command (%d) at %s:%d",
            req->command, __FILE__, __LINE__);
        break;
    }

    /*  FIX_ME: Is a pthread_exit() needed here?  Check the weird ps behavior.
     */
    destroy_req(req);
    return;

err:
    /*  Only close the client's socket connection on error,
     *    since it may be further handled by mux_io().
     */
    if (req->sd > 0) {
        if (close(req->sd) < 0)
            log_msg(0, "Error closing connection from %s: %s",
                req->ip, strerror(errno));
        req->sd = -1;
    }
    destroy_req(req);
    return;
}


static req_t * create_req(int sd)
{
/*  Creates a request struct.
 *  Returns the new struct, or NULL or error.
 */
    req_t *req;

    assert(sd >= 0);

    if (!(req = malloc(sizeof(req_t))))
        return(NULL);
    req->sd = sd;
    req->ip = NULL;
    req->host = NULL;
    req->user = NULL;
    req->command = NONE;
    req->enableBroadcast = 0;
    req->enableForce = 0;
    req->program = NULL;
    /*
     *  The "consoles" list will initially contain strings received while
     *    parsing the client's request.  These strings will then be matched
     *    against the server's conf during query_consoles().
     *  The destroy_string() destructor is used here because the initial
     *    list of strings will be destroyed when query_consoles()
     *    replaces it with a list of console objects.
     */
    if (!(req->consoles = list_create((ListDelF) destroy_string))) {
        free(req);
        return(NULL);
    }
    return(req);
}


static void destroy_req(req_t *req)
{
/*  Destroys a request struct.
 *  The client's socket connection is not shutdown here
 *    since it may be further handled by mux_io().
 */
    DPRINTF("Destroyed request from %s.\n", (req->host ? req->host : req->ip));

    if (req->ip)
        free(req->ip);
    if (req->host)
        free(req->host);
    if (req->user)
        free(req->user);
    if (req->program)
        free(req->program);
    list_destroy(req->consoles);
    free(req);
    return;
}


static int recv_greeting(req_t *req)
{
/*  Performs the initial handshake with the client
 *    (SOMEDAY including authentication & encryption, if needed).
 *  Returns 0 if the greeting is valid, or -1 on error.
 */
    int n;
    char buf[MAX_SOCK_LINE];
    Lex l;
    int done = 0;
    int tok;

    assert(req->sd >= 0);

    /*  Read greeting (ie, first line of request):
     *    HELLO USER='<str>'
     */
    if ((n = read_line(req->sd, buf, sizeof(buf))) < 0) {
        log_msg(0, "Error reading greeting from %s: %s",
            req->ip, strerror(errno));
        return(-1);
    }
    else if (n == 0) {
        log_msg(0, "Connection terminated by %s", req->ip);
        return(-1);
    }

    if (!(l = lex_create(buf, proto_strs))) {
        send_rsp(req, CONMAN_ERR_NO_RESOURCES,
            "Insufficient resources to process request.");
        return(-1);
    }
    while (!done) {
        tok = lex_next(l);
        switch(tok) {
        case CONMAN_TOK_HELLO:
            done = parse_greeting(l, req);
            break;
        case LEX_EOF:
        case LEX_EOL:
            done = 1;
            break;
        default:
            break;
        }
    }
    lex_destroy(l);

    /*  Validate greeting.
     */
    if (done == -1) {
        send_rsp(req, CONMAN_ERR_NO_RESOURCES,
            "Insufficient resources to process request.");
        return(-1);
    }
    if (!req->user) {
        send_rsp(req, CONMAN_ERR_BAD_REQUEST, 
            "Invalid greeting: no user specified");
        return(-1);
    }
#ifdef NDEBUG
    if (strcmp(req->ip, "127.0.0.1")) {
        send_rsp(req, CONMAN_ERR_AUTHENTICATE,
            "Authentication required (but not yet implemented)");
        return(-1);
    }
#endif /* NDEBUG */

    DPRINTF("Received request from <%s@%s>.\n",
        req->user, (req->host ? req->host : req->ip));

    /*  Send response to greeting.
     */
    return(send_rsp(req, CONMAN_ERR_NONE, NULL));
}


static int parse_greeting(Lex l, req_t *req)
{
/*  Parses the "HELLO" command from the client:
 *    HELLO USER='<str>'
 *  Returns 0 on success, or -1 on error (ie, out of memory).
 */
    int done = 0;
    int tok;

    while (!done) {
        tok = lex_next(l);
        switch(tok) {
        case CONMAN_TOK_USER:
            if ((lex_next(l) == '=') && (lex_next(l) == LEX_STR)
              && (*lex_text(l) != '\0')) {
                if (req->user)
                    free(req->user);
                if (!(req->user = lex_decode(strdup(lex_text(l)))))
                    return(-1);
            }
            break;
        case LEX_EOF:
        case LEX_EOL:
            done = 1;
            break;
        default:
            break;
        }
    }
    return(0);
}


static int recv_req(req_t *req)
{
/*  Receives the request from the client after the greeting has completed.
 *  Returns 0 if the request is read OK, or -1 on error.
 */
    int n;
    char buf[MAX_SOCK_LINE];
    Lex l;
    int done = 0;
    int tok;

    assert(req->sd >= 0);

    if ((n = read_line(req->sd, buf, sizeof(buf))) < 0) {
        log_msg(0, "Error reading request from %s: %s",
            req->ip, strerror(errno));
        return(-1);
    }
    else if (n == 0) {
        log_msg(0, "Connection terminated by %s", req->ip);
        return(-1);
    }

    if (!(l = lex_create(buf, proto_strs))) {
        send_rsp(req, CONMAN_ERR_NO_RESOURCES,
            "Insufficient resources to process request.");
        return(-1);
    }
    while (!done) {
        tok = lex_next(l);
        switch(tok) {
        case CONMAN_TOK_CONNECT:
            req->command = CONNECT;
            done = parse_cmd_opts(l, req);
            break;
        case CONMAN_TOK_EXECUTE:
            req->command = EXECUTE;
            done = parse_cmd_opts(l, req);
            break;
        case CONMAN_TOK_MONITOR:
            req->command = MONITOR;
            done = parse_cmd_opts(l, req);
            break;
        case CONMAN_TOK_QUERY:
            req->command = QUERY;
            done = parse_cmd_opts(l, req);
            break;
        case LEX_EOF:
        case LEX_EOL:
            done = 1;
            break;
        default:
            break;
        }
    }
    lex_destroy(l);

    if (done == -1) {
        send_rsp(req, CONMAN_ERR_NO_RESOURCES,
            "Insufficient resources to process request.");
        return(-1);
    }
    return(0);
}


static int parse_cmd_opts(Lex l, req_t *req)
{
/*  Parses the command options for the given request.
 *  Returns 0 on success, or -1 on error (ie, out of memory).
 */
    int done = 0;
    int tok;
    char *str;

    while (!done) {
        tok = lex_next(l);
        switch(tok) {
        case CONMAN_TOK_CONSOLE:
            if ((lex_next(l) == '=') && (lex_next(l) == LEX_STR)
              && (*lex_text(l) != '\0')) {
                if (!(str = lex_decode(strdup(lex_text(l)))))
                    return(-1);
                if (!list_append(req->consoles, str)) {
                    free(str);
                    return(-1);
                }
            }
            break;
        case CONMAN_TOK_OPTION:
            if (lex_next(l) == '=') {
                if (lex_next(l) == CONMAN_TOK_FORCE)
                    req->enableForce = 1;
                else if (lex_prev(l) == CONMAN_TOK_BROADCAST)
                    req->enableBroadcast = 1;
            }
            break;
        case CONMAN_TOK_PROGRAM:
            if ((lex_next(l) == '=') && (lex_next(l) == LEX_STR)
              && (*lex_text(l) != '\0')) {
                if (req->program)
                    free(req->program);
                if (!(req->program = lex_decode(strdup(lex_text(l)))))
                    return(-1);
            }
            break;
        case LEX_EOF:
        case LEX_EOL:
            done = 1;
            break;
        default:
            break;
        }
    }
    return(0);
}


static int query_consoles(server_conf_t *conf, req_t *req)
{
/*  Queries the server's conf to resolve the console names
 *    specified in the client's request.
 *  The req->consoles list initially contains strings constructed
 *    while parsing the client's request in parse_command().
 *    These strings are combined into a regex pattern and then
 *    matched against the console objs in the conf->objs list.
 *  Returns 0 on success, or -1 on error.
 *    Upon a successful return, the req->consoles list is replaced
 *    with a list of console obj_t's.
 */
    ListIterator i;
    char buf[MAX_BUF_SIZE];
    char *p;
    int rc;
    regex_t rex;
    List matches;
    obj_t *obj;

    /*  An empty list for the QUERY command matches all consoles.
     */
    if (list_is_empty(req->consoles)) {
        if (req->command != QUERY)
            return(0);
        if (!(p = strdup(".*")))
            goto no_mem;
        if (!list_append(req->consoles, p)) {
            free(p);
            goto no_mem;
        }
    }

    /*  Combine console patterns via alternation to create single regex.
     */
    if (!(i = list_iterator_create(req->consoles)))
        goto no_mem;
    strlcpy(buf, list_next(i), sizeof(buf));
    while ((p = list_next(i))) {
        strlcat(buf, "|", sizeof(buf));
        strlcat(buf, p, sizeof(buf));
    }
    list_iterator_destroy(i);

    /*  Compile regex for searching server's console objs.
     */
    rc = regcomp(&rex, buf, REG_EXTENDED | REG_ICASE | REG_NOSUB | REG_NEWLINE);
    if (rc != 0) {
        if (regerror(rc, &rex, buf, sizeof(buf)) > sizeof(buf))
            log_msg(10, "Buffer overflow during regerror()");
        regfree(&rex);
        send_rsp(req, CONMAN_ERR_BAD_REGEX, lex_encode(buf));
        return(-1);
    }

    /*  Search objs for consoles matching the regex.
     *  The NULL destructor is used here because the matches list will
     *    only contain copies of objs contained in the conf->objs list.
     *    These objs will be destroyed when the conf->objs list is destroyed.
     */
    if (!(matches = list_create(NULL))) {
        regfree(&rex);
        goto no_mem;
    }
    if (!(i = list_iterator_create(conf->objs))) {
        list_destroy(matches);
        regfree(&rex);
        goto no_mem;
    }
    while ((obj = list_next(i))) {
        if (obj->type != CONSOLE)
            continue;
        if (!regexec(&rex, obj->name, 0, NULL, 0)) {
            if (!list_append(matches, obj)) {
                list_iterator_destroy(i);
                list_destroy(matches);
                regfree(&rex);
                goto no_mem;
            }
        }
    }
    list_iterator_destroy(i);
    regfree(&rex);

    /*  Replace original consoles-string list with regex-matched obj_t list.
     */
    list_destroy(req->consoles);
    req->consoles = matches;

    return(0);

no_mem:
    send_rsp(req, CONMAN_ERR_NO_RESOURCES,
        "Insufficient resources to process request.");
    return(-1);
}


static int validate_req(req_t *req)
{
    /*  FIX_ME: NOT_IMPLEMENTED_YET
     */
    if (list_is_empty(req->consoles)) {
        send_rsp(req, CONMAN_ERR_NO_CONSOLES, "Found no matching consoles.");
        return(-1);
    }
    return(0);
}


static int send_rsp(req_t *req, int errnum, char *errmsg)
{
/*  Sends a response to the given request (req).
 *  If the request is valid and there are no errors,
 *    errnum = CONMAN_ERR_NONE and an OK response is sent.
 *  Otherwise, (errnum) identifies the err_type enumeration (in conman.h)
 *    and errmsg is a string describing the error in more detail.
 *  Note that errmsg may not contain single-quote chars.
 *  Returns 0 if the response is sent OK, or -1 on error.
 */
    char buf[MAX_SOCK_LINE];
    int n;

    assert(req->sd >= 0);
    assert(errnum >= 0);
    assert(errmsg == NULL || strchr(errmsg, '\'') == NULL);

    /*  Create response message.
     */
    if (errnum == CONMAN_ERR_NONE) {
        n = snprintf(buf, sizeof(buf), "%s\n",
            proto_strs[LEX_UNTOK(CONMAN_TOK_OK)]);
    }
    else {
    /*
     *  FIX_ME? Should all errors be logged?
     *
     *  Note that errmsg cannot be lex_encode()'d here
     *    because it may be a string constant.
     */
        n = snprintf(buf, sizeof(buf), "%s %s=%d %s='%s'\n",
            proto_strs[LEX_UNTOK(CONMAN_TOK_ERROR)],
            proto_strs[LEX_UNTOK(CONMAN_TOK_CODE)], errnum,
            proto_strs[LEX_UNTOK(CONMAN_TOK_MESSAGE)],
            (errmsg ? errmsg : "Doh!"));
    }

    /*  Ensure response is properly terminated.
     *  If the buffer was overrun, it should be due to an insanely-long
     *    errmsg string.  As such, we'll need to add the closing quote.
     */
    if (n < 0 || n >= sizeof(buf)) {
        log_msg(10, "Buffer overflow during send_rsp() for %s", req->ip);
        buf[MAX_SOCK_LINE-3] = '"';
        buf[MAX_SOCK_LINE-2] = '\n';
        buf[MAX_SOCK_LINE-1] = '\0';
    }


    /*  Write response to socket.
     */
    if (write_n(req->sd, buf, strlen(buf)) < 0) {
        log_msg(0, "Error writing to %s: %s", req->ip, strerror(errno));
        return(-1);
    }

    return(0);
}


static void perform_query_cmd(req_t *req)
{
/*  Performs the QUERY command, returning a list of consoles that
 *    matches the console patterns given in the client's request.
 *  Since this cmd is processed entirely by this thread,
 *    the client socket connection is closed once it is finished.
 */
    ListIterator i;
    obj_t *obj;
    char buf[MAX_BUF_SIZE];

    assert(req->sd >= 0);
    assert(req->command == QUERY);

    list_sort(req->consoles, (ListCmpF) compare_objs);

    if (!(i = list_iterator_create(req->consoles))) {
        send_rsp(req, CONMAN_ERR_NO_RESOURCES,
            "Insufficient resources to process request.");
        return;
    }
    send_rsp(req, CONMAN_ERR_NONE, NULL);

    while ((obj = list_next(i))) {
        strlcpy(buf, obj->name, sizeof(buf));
        strlcat(buf, "\n", sizeof(buf));
        if (write_n(req->sd, buf, strlen(buf)) < 0) {
            log_msg(0, "Error writing to %s: %s", req->ip, strerror(errno));
            break;
        }
    }

    list_iterator_destroy(i);

    if (close(req->sd) < 0)
        err_msg(errno, "close(%d) failed", req->sd);
    req->sd = -1;

    return;
}


static void perform_monitor_cmd(req_t *req, server_conf_t *conf)
{
    assert(req->sd >= 0);
    assert(req->command == MONITOR);

    return;
}


static void perform_connect_cmd(req_t *req, server_conf_t *conf)
{
    obj_t *socket;
    obj_t *console;

    assert(req->sd >= 0);
    assert(req->command == CONNECT);

    send_rsp(req, CONMAN_ERR_NONE, NULL);

    console = list_pop(req->consoles);
    socket = create_socket_obj(conf->objs, req->user, req->host, req->sd);
    if (!socket)
        return;
    if ((link_objs(socket, console) < 0) || (link_objs(console, socket) < 0)) {
        destroy_obj(socket);
        return;
    }
    return;
}


static void perform_execute_cmd(req_t *req, server_conf_t *conf)
{
    /*  FIX_ME: NOT_IMPLEMENTED_YET
     */

    assert(req->sd >= 0);
    assert(req->command == EXECUTE);

    return;
}
