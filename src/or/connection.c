/* Copyright 2001,2002 Roger Dingledine, Matej Pfajfar. */
/* See LICENSE for licensing information */
/* $Id$ */

#include "or.h"

/********* START VARIABLES **********/

extern or_options_t options; /* command-line and config-file options */

extern int global_read_bucket;

char *conn_type_to_string[] = {
  "",            /* 0 */
  "OP listener", /* 1 */
  "OP",          /* 2 */
  "OR listener", /* 3 */
  "OR",          /* 4 */
  "Exit",        /* 5 */
  "App listener",/* 6 */
  "App",         /* 7 */
  "Dir listener",/* 8 */
  "Dir",         /* 9 */
  "DNS worker",  /* 10 */
  "CPU worker",  /* 11 */
};

char *conn_state_to_string[][_CONN_TYPE_MAX+1] = {
	{ NULL },         /* no type associated with 0 */
  { "ready" }, /* op listener, 0 */
  { "awaiting keys", /* op, 0 */
    "open",              /* 1 */
    "close",             /* 2 */
    "close_wait" },      /* 3 */
  { "ready" }, /* or listener, 0 */
  { "connect()ing",                 /* 0 */
    "handshaking",                  /* 1 */
    "open" },                       /* 2 */
  { "waiting for dest info",     /* exit, 0 */
    "connecting",                      /* 1 */
    "open" },                          /* 2 */
  { "ready" }, /* app listener, 0 */
  { "", /* 0 */
    "", /* 1 */
    "", /* 2 */
    "awaiting dest info",         /* app, 3 */
    "waiting for OR connection",       /* 4 */
    "open" },                          /* 5 */
  { "ready" }, /* dir listener, 0 */
  { "connecting (fetch)",              /* 0 */
    "connecting (upload)",             /* 1 */
    "client sending fetch",            /* 2 */
    "client sending upload",           /* 3 */
    "client reading fetch",            /* 4 */
    "client reading upload",           /* 5 */
    "awaiting command",                /* 6 */
    "writing" },                       /* 7 */
  { "idle",                /* dns worker, 0 */
    "busy" },                          /* 1 */
  { "idle",                /* cpu worker, 0 */
    "busy with onion",                 /* 1 */
    "busy with handshake" },           /* 2 */
};

/********* END VARIABLES ************/

static int connection_init_accepted_conn(connection_t *conn);
static int connection_tls_continue_handshake(connection_t *conn);
static int connection_tls_finish_handshake(connection_t *conn);

/**************************************************************/

connection_t *connection_new(int type) {
  connection_t *conn;
  struct timeval now;

  my_gettimeofday(&now);

  conn = (connection_t *)tor_malloc(sizeof(connection_t));
  memset(conn,0,sizeof(connection_t)); /* zero it out to start */

  conn->type = type;
  conn->inbuf = buf_new();
  conn->outbuf = buf_new();

  conn->timestamp_created = now.tv_sec;
  conn->timestamp_lastread = now.tv_sec;
  conn->timestamp_lastwritten = now.tv_sec;

  return conn;
}

void connection_free(connection_t *conn) {
  assert(conn);

  buf_free(conn->inbuf);
  buf_free(conn->outbuf);
  if(conn->address)
    free(conn->address);

  if(connection_speaks_cells(conn)) {
    directory_set_dirty();
    if (conn->tls)
      tor_tls_free(conn->tls);
  }

  if (conn->onion_pkey)
    crypto_free_pk_env(conn->onion_pkey);
  if (conn->link_pkey)
    crypto_free_pk_env(conn->link_pkey);
  if (conn->identity_pkey)
    crypto_free_pk_env(conn->identity_pkey);
  if (conn->nickname) 
    free(conn->nickname);

  if(conn->s > 0) {
    log_fn(LOG_INFO,"closing fd %d.",conn->s);
    close(conn->s);
  }
  free(conn);
}

int connection_create_listener(struct sockaddr_in *bindaddr, int type) {
  connection_t *conn;
  int s;
  int one=1;

  s = socket(PF_INET,SOCK_STREAM,IPPROTO_TCP);
  if (s < 0) { 
    log_fn(LOG_WARNING,"Socket creation failed.");
    return -1;
  }

  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (void*)&one, sizeof(one));

  if(bind(s,(struct sockaddr *)bindaddr,sizeof(*bindaddr)) < 0) {
    perror("bind ");
    log(LOG_WARNING,"Could not bind to port %u.",ntohs(bindaddr->sin_port));
    return -1;
  }

  if(listen(s,SOMAXCONN) < 0) {
    log(LOG_WARNING,"Could not listen on port %u.",ntohs(bindaddr->sin_port));
    return -1;
  }

  set_socket_nonblocking(s);

  conn = connection_new(type);
  conn->s = s;

  if(connection_add(conn) < 0) { /* no space, forget it */
    log_fn(LOG_WARNING,"connection_add failed. Giving up.");
    connection_free(conn);
    return -1;
  }

  log_fn(LOG_DEBUG,"%s listening on port %u.",conn_type_to_string[type], ntohs(bindaddr->sin_port));

  conn->state = LISTENER_STATE_READY;
  connection_start_reading(conn);

  return 0;
}

int connection_handle_listener_read(connection_t *conn, int new_type) {
  int news; /* the new socket */
  connection_t *newconn;
  struct sockaddr_in remote; /* information about the remote peer when connecting to other routers */
  int remotelen = sizeof(struct sockaddr_in); /* length of the remote address */
#ifdef MS_WINDOWS
  int e;
#endif

  news = accept(conn->s,(struct sockaddr *)&remote,&remotelen);
  if (news == -1) { /* accept() error */
    if(ERRNO_EAGAIN(errno)) {
#ifdef MS_WINDOWS
      e = correct_socket_errno(conn->s);
      if (ERRNO_EAGAIN(e))
        return 0;
#else
      return 0; /* he hung up before we could accept(). that's fine. */
#endif
    }
    /* else there was a real error. */
    log_fn(LOG_WARNING,"accept() failed. Closing listener.");
    return -1;
  }
  log(LOG_INFO,"Connection accepted on socket %d (child of fd %d).",news, conn->s);

  set_socket_nonblocking(news);

  newconn = connection_new(new_type);
  newconn->s = news;

  newconn->address = strdup(inet_ntoa(remote.sin_addr)); /* remember the remote address */
  newconn->addr = ntohl(remote.sin_addr.s_addr);
  newconn->port = ntohs(remote.sin_port);

  if(connection_add(newconn) < 0) { /* no space, forget it */
    connection_free(newconn);
    return 0; /* no need to tear down the parent */
  }

  if(connection_init_accepted_conn(newconn) < 0) {
    newconn->marked_for_close = 1;
    return 0;
  }
  return 0;
}

static int connection_init_accepted_conn(connection_t *conn) {

  connection_start_reading(conn);

  switch(conn->type) {
    case CONN_TYPE_OR:
      return connection_tls_start_handshake(conn, 1);
    case CONN_TYPE_AP:
      conn->state = AP_CONN_STATE_SOCKS_WAIT;
      break;
    case CONN_TYPE_DIR:
      conn->state = DIR_CONN_STATE_SERVER_COMMAND_WAIT;
      break;
  }
  return 0;
}

int connection_tls_start_handshake(connection_t *conn, int receiving) {
  conn->state = OR_CONN_STATE_HANDSHAKING;
  conn->tls = tor_tls_new(conn->s, receiving);
  if(!conn->tls) {
    log_fn(LOG_WARNING,"tor_tls_new failed. Closing.");
    return -1;
  }
  connection_start_reading(conn);
  log_fn(LOG_DEBUG,"starting the handshake");
  if(connection_tls_continue_handshake(conn) < 0)
    return -1;
  return 0;
}

static int connection_tls_continue_handshake(connection_t *conn) {
  switch(tor_tls_handshake(conn->tls)) {
    case TOR_TLS_ERROR:
    case TOR_TLS_CLOSE:
      log_fn(LOG_INFO,"tls error. breaking.");
      return -1;
    case TOR_TLS_DONE:
     return connection_tls_finish_handshake(conn);
    case TOR_TLS_WANTWRITE:
      connection_start_writing(conn);
      log_fn(LOG_DEBUG,"wanted write");
      return 0;
    case TOR_TLS_WANTREAD: /* handshaking conns are *always* reading */
      log_fn(LOG_DEBUG,"wanted read");
      return 0;
  }
  return 0;
}

static int connection_tls_finish_handshake(connection_t *conn) {
  crypto_pk_env_t *pk;
  routerinfo_t *router;

  conn->state = OR_CONN_STATE_OPEN;
  directory_set_dirty();
  connection_watch_events(conn, POLLIN);
  log_fn(LOG_DEBUG,"tls handshake done. verifying.");
  if(options.OnionRouter) { /* I'm an OR */
    if(tor_tls_peer_has_cert(conn->tls)) { /* it's another OR */
      pk = tor_tls_verify(conn->tls);
      if(!pk) {
        log_fn(LOG_WARNING,"Other side has a cert but it's invalid. Closing.");
        return -1;
      }
      router = router_get_by_link_pk(pk);
      if (!router) {
        log_fn(LOG_WARNING,"Unrecognized public key from peer. Closing.");
        crypto_free_pk_env(pk);
        return -1;
      }
      if(conn->link_pkey) { /* I initiated this connection. */
        if(crypto_pk_cmp_keys(conn->link_pkey, pk)) {
          log_fn(LOG_WARNING,"We connected to '%s' but he gave us a different key. Closing.", router->nickname);
          crypto_free_pk_env(pk);
          return -1;
        }
        log_fn(LOG_DEBUG,"The router's pk matches the one we meant to connect to. Good.");
      } else {
        if(connection_exact_get_by_addr_port(router->addr,router->or_port)) {
          log_fn(LOG_INFO,"Router %s is already connected. Dropping.", router->nickname);
          return -1;
        }
        connection_or_init_conn_from_router(conn, router);
      }
      crypto_free_pk_env(pk);
    } else { /* it's an OP */
      conn->receiver_bucket = conn->bandwidth = DEFAULT_BANDWIDTH_OP;
    }
  } else { /* I'm a client */
    if(!tor_tls_peer_has_cert(conn->tls)) { /* it's a client too?! */
      log_fn(LOG_WARNING,"Neither peer sent a cert! Closing.");
      return -1;
    }
    pk = tor_tls_verify(conn->tls);
    if(!pk) {
      log_fn(LOG_WARNING,"Other side has a cert but it's invalid. Closing.");
      return -1;
    }
    router = router_get_by_link_pk(pk);
    if (!router) {
      log_fn(LOG_WARNING,"Unrecognized public key from peer. Closing.");
      crypto_free_pk_env(pk);
      return -1;
    }
    if(crypto_pk_cmp_keys(conn->link_pkey, pk)) {
      log_fn(LOG_WARNING,"We connected to '%s' but he gave us a different key. Closing.", router->nickname);
      crypto_free_pk_env(pk);
      return -1;
    }
    log_fn(LOG_DEBUG,"The router's pk matches the one we meant to connect to. Good.");
    crypto_free_pk_env(pk);
    conn->receiver_bucket = conn->bandwidth = DEFAULT_BANDWIDTH_OP;
    circuit_n_conn_open(conn); /* send the pending create */
  }
  return 0;
}

/* take conn, make a nonblocking socket; try to connect to 
 * addr:port (they arrive in *host order*). If fail, return -1. Else
 * assign s to conn->s: if connected return 1, if eagain return 0.
 * address is used to make the logs useful.
 */
int connection_connect(connection_t *conn, char *address, uint32_t addr, uint16_t port) {
  int s;
  struct sockaddr_in dest_addr;

  s=socket(PF_INET,SOCK_STREAM,IPPROTO_TCP);
  if (s < 0) {
    log_fn(LOG_WARNING,"Error creating network socket.");
    return -1;
  }
  set_socket_nonblocking(s);

  memset((void *)&dest_addr,0,sizeof(dest_addr));
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(port);
  dest_addr.sin_addr.s_addr = htonl(addr);

  log_fn(LOG_DEBUG,"Connecting to %s:%u.",address,port);

  if(connect(s,(struct sockaddr *)&dest_addr,sizeof(dest_addr)) < 0) {
    if(!ERRNO_CONN_EINPROGRESS(errno)) {
      /* yuck. kill it. */
      perror("connect");
      log_fn(LOG_INFO,"Connect() to %s:%u failed.",address,port);
      return -1;
    } else {
      /* it's in progress. set state appropriately and return. */
      conn->s = s;
      log_fn(LOG_DEBUG,"connect in progress, socket %d.",s);
      return 0;
    }
  }

  /* it succeeded. we're connected. */
  log_fn(LOG_INFO,"Connection to %s:%u established.",address,port);
  conn->s = s;
  return 1;
}

/* start all connections that should be up but aren't */
int retry_all_connections(uint16_t or_listenport, uint16_t ap_listenport, uint16_t dir_listenport) {
  struct sockaddr_in bindaddr; /* where to bind */

  if(or_listenport) {
    router_retry_connections();
  }

  memset(&bindaddr,0,sizeof(struct sockaddr_in));
  bindaddr.sin_family = AF_INET;
  bindaddr.sin_addr.s_addr = htonl(INADDR_ANY); /* anyone can connect */

  if(or_listenport) {
    bindaddr.sin_port = htons(or_listenport);
    if(!connection_get_by_type(CONN_TYPE_OR_LISTENER)) {
      connection_create_listener(&bindaddr, CONN_TYPE_OR_LISTENER);
    }
  }

  if(dir_listenport) {
    bindaddr.sin_port = htons(dir_listenport);
    if(!connection_get_by_type(CONN_TYPE_DIR_LISTENER)) {
      connection_create_listener(&bindaddr, CONN_TYPE_DIR_LISTENER);
    }
  }
 
  if(ap_listenport) {
    bindaddr.sin_port = htons(ap_listenport);
    bindaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* the AP listens only on localhost! */
    if(!connection_get_by_type(CONN_TYPE_AP_LISTENER)) {
      connection_create_listener(&bindaddr, CONN_TYPE_AP_LISTENER);
    }
  }

  return 0;
}

int connection_handle_read(connection_t *conn) {
  struct timeval now;

  my_gettimeofday(&now);
  conn->timestamp_lastread = now.tv_sec;

  switch(conn->type) {
    case CONN_TYPE_OR_LISTENER:
      return connection_handle_listener_read(conn, CONN_TYPE_OR);
    case CONN_TYPE_AP_LISTENER:
      return connection_handle_listener_read(conn, CONN_TYPE_AP);
    case CONN_TYPE_DIR_LISTENER:
      return connection_handle_listener_read(conn, CONN_TYPE_DIR);
  }

  if(connection_read_to_buf(conn) < 0) {
    if(conn->type == CONN_TYPE_DIR && 
      (conn->state == DIR_CONN_STATE_CONNECTING_FETCH ||
       conn->state == DIR_CONN_STATE_CONNECTING_UPLOAD)) {
       /* it's a directory server and connecting failed: forget about this router */
       /* XXX I suspect pollerr may make Windows not get to this point. :( */
       router_forget_router(conn->addr,conn->port); 
         /* FIXME i don't think router_forget_router works. */
    }
    return -1;
  }
  if(connection_process_inbuf(conn) < 0) {
    //log_fn(LOG_DEBUG,"connection_process_inbuf returned %d.",retval);
    return -1;
  }
  return 0;
}

/* return -1 if we want to break conn, else return 0 */
int connection_read_to_buf(connection_t *conn) {
  int result;
  int at_most;

  if(options.LinkPadding) {
    at_most = global_read_bucket;
  } else {
    /* do a rudimentary round-robin so one connection can't hog a thickpipe */
    if(connection_speaks_cells(conn)) {
      at_most = 10*(CELL_NETWORK_SIZE);
    } else {
      at_most = 10*(CELL_PAYLOAD_SIZE - RELAY_HEADER_SIZE);
    }

    at_most = 103; /* an unusual number, to force bugs into the open */

    if(at_most > global_read_bucket)
      at_most = global_read_bucket;
  }

  if(connection_speaks_cells(conn) && conn->state != OR_CONN_STATE_CONNECTING) {
    if(conn->state == OR_CONN_STATE_HANDSHAKING)
      return connection_tls_continue_handshake(conn);

    /* else open, or closing */
    if(at_most > conn->receiver_bucket)
      at_most = conn->receiver_bucket;
    result = read_to_buf_tls(conn->tls, at_most, conn->inbuf);

    switch(result) {
      case TOR_TLS_ERROR:
      case TOR_TLS_CLOSE:
        log_fn(LOG_INFO,"tls error. breaking.");
        return -1; /* XXX deal with close better */
      case TOR_TLS_WANTWRITE:
        connection_start_writing(conn);
        return 0;
      case TOR_TLS_WANTREAD: /* we're already reading */
      case TOR_TLS_DONE: /* no data read, so nothing to process */
        return 0;
    }
  } else {
    result = read_to_buf(conn->s, at_most, conn->inbuf,
                         &conn->inbuf_reached_eof);

//  log(LOG_DEBUG,"connection_read_to_buf(): read_to_buf returned %d.",read_result);

    if(result < 0)
      return -1;
  }

  global_read_bucket -= result; assert(global_read_bucket >= 0);
  if(global_read_bucket == 0) {
    log_fn(LOG_DEBUG,"global bucket exhausted. Pausing.");
    conn->wants_to_read = 1;
    connection_stop_reading(conn);
    return 0;
  }
  if(connection_speaks_cells(conn) && conn->state == OR_CONN_STATE_OPEN) {
    conn->receiver_bucket -= result; assert(conn->receiver_bucket >= 0);
    if(conn->receiver_bucket == 0) {
      log_fn(LOG_DEBUG,"receiver bucket exhausted. Pausing.");
      conn->wants_to_read = 1;
      connection_stop_reading(conn);
      return 0;
    }
  }
  return 0;
}

int connection_fetch_from_buf(char *string, int len, connection_t *conn) {
  return fetch_from_buf(string, len, conn->inbuf);
}

int connection_find_on_inbuf(char *string, int len, connection_t *conn) {
  return find_on_inbuf(string, len, conn->inbuf);
}

int connection_wants_to_flush(connection_t *conn) {
  return conn->outbuf_flushlen;
}

int connection_outbuf_too_full(connection_t *conn) {
  return (conn->outbuf_flushlen > 10*CELL_PAYLOAD_SIZE);
}

int connection_flush_buf(connection_t *conn) {
  return flush_buf(conn->s, conn->outbuf, &conn->outbuf_flushlen);
}

/* return -1 if you want to break the conn, else return 0 */
int connection_handle_write(connection_t *conn) {
  struct timeval now;

  if(connection_is_listener(conn)) {
    log_fn(LOG_WARNING,"Got a listener socket. Can't happen!");
    return -1;
  }

  my_gettimeofday(&now);
  conn->timestamp_lastwritten = now.tv_sec;

  if(connection_speaks_cells(conn) && conn->state != OR_CONN_STATE_CONNECTING) {
    if(conn->state == OR_CONN_STATE_HANDSHAKING) {
      connection_stop_writing(conn);
      return connection_tls_continue_handshake(conn);
    }

    /* else open, or closing */
    switch(flush_buf_tls(conn->tls, conn->outbuf, &conn->outbuf_flushlen)) {
      case TOR_TLS_ERROR:
      case TOR_TLS_CLOSE:
        log_fn(LOG_INFO,"tls error. breaking.");
        return -1; /* XXX deal with close better */
      case TOR_TLS_WANTWRITE:
        log_fn(LOG_DEBUG,"wanted write.");
        /* we're already writing */
        return 0;
      case TOR_TLS_WANTREAD:
        /* Make sure to avoid a loop if the receive buckets are empty. */
        log_fn(LOG_DEBUG,"wanted read.");
        if(!connection_is_reading(conn)) {
          connection_stop_writing(conn);
          conn->wants_to_write = 1;
          /* we'll start reading again when the next second arrives,
           * and then also start writing again.
           */
        }
        /* else no problem, we're already reading */
        return 0;
      /* case TOR_TLS_DONE:
       * for TOR_TLS_DONE, fall through to check if the flushlen
       * is empty, so we can stop writing.
       */  
    }
  } else {
    if(flush_buf(conn->s, conn->outbuf, &conn->outbuf_flushlen) < 0)
      return -1;
      /* conns in CONNECTING state will fall through... */
  }

  if(!connection_wants_to_flush(conn)) /* it's done flushing */
    if(connection_finished_flushing(conn) < 0) /* ...and get handled here. */
      return -1;

  return 0;
}

int connection_write_to_buf(const char *string, int len, connection_t *conn) {

  if(!len)
    return 0;

  if(conn->marked_for_close)
    return 0;

  if( (!connection_speaks_cells(conn)) ||
      (!connection_state_is_open(conn)) ||
      (options.LinkPadding == 0) ) {
    /* connection types other than or, or or not in 'open' state, should flush immediately */
    /* also flush immediately if we're not doing LinkPadding, since otherwise it will never flush */
    connection_start_writing(conn);
    conn->outbuf_flushlen += len;
  }

  return write_to_buf(string, len, conn->outbuf);
}

int connection_receiver_bucket_should_increase(connection_t *conn) {
  assert(conn);

  if(!connection_speaks_cells(conn))
    return 0; /* edge connections don't use receiver_buckets */
  if(conn->state != OR_CONN_STATE_OPEN)
    return 0; /* only open connections play the rate limiting game */  

  assert(conn->bandwidth > 0);
  if(conn->receiver_bucket > 9*conn->bandwidth)
    return 0;

  return 1;
}

int connection_is_listener(connection_t *conn) {
  if(conn->type == CONN_TYPE_OR_LISTENER ||
     conn->type == CONN_TYPE_AP_LISTENER ||
     conn->type == CONN_TYPE_DIR_LISTENER)
    return 1;
  return 0;
}

int connection_state_is_open(connection_t *conn) {
  assert(conn);

  if((conn->type == CONN_TYPE_OR && conn->state == OR_CONN_STATE_OPEN) ||
     (conn->type == CONN_TYPE_AP && conn->state == AP_CONN_STATE_OPEN) ||
     (conn->type == CONN_TYPE_EXIT && conn->state == EXIT_CONN_STATE_OPEN))
    return 1;

  return 0;
}

int connection_send_destroy(aci_t aci, connection_t *conn) {
  cell_t cell;

  assert(conn);

  if(!connection_speaks_cells(conn)) {
     log_fn(LOG_INFO,"Aci %d: At an edge. Marking connection for close.", aci);
/*ENDCLOSE*/ conn->marked_for_close = 1;
     return 0;
  }

  memset(&cell, 0, sizeof(cell_t));
  cell.aci = aci;
  cell.command = CELL_DESTROY;
  log_fn(LOG_INFO,"Sending destroy (aci %d).",aci);
  return connection_write_cell_to_buf(&cell, conn);
}

int connection_process_inbuf(connection_t *conn) {

  assert(conn);

  switch(conn->type) {
    case CONN_TYPE_OR:
      return connection_or_process_inbuf(conn);
    case CONN_TYPE_EXIT:
    case CONN_TYPE_AP:
      return connection_edge_process_inbuf(conn);
    case CONN_TYPE_DIR:
      return connection_dir_process_inbuf(conn);
    case CONN_TYPE_DNSWORKER:
      return connection_dns_process_inbuf(conn); 
    case CONN_TYPE_CPUWORKER:
      return connection_cpu_process_inbuf(conn); 
    default:
      log_fn(LOG_WARNING,"got unexpected conn->type %d.", conn->type);
      return -1;
  }
}

int connection_finished_flushing(connection_t *conn) {

  assert(conn);

//  log_fn(LOG_DEBUG,"entered. Socket %u.", conn->s);

  switch(conn->type) {
    case CONN_TYPE_OR:
      return connection_or_finished_flushing(conn);
    case CONN_TYPE_AP:
    case CONN_TYPE_EXIT:
      return connection_edge_finished_flushing(conn);
    case CONN_TYPE_DIR:
      return connection_dir_finished_flushing(conn);
    case CONN_TYPE_DNSWORKER:
      return connection_dns_finished_flushing(conn);
    case CONN_TYPE_CPUWORKER:
      return connection_cpu_finished_flushing(conn);
    default:
      log_fn(LOG_WARNING,"got unexpected conn->type %d.", conn->type);
      return -1;
  }
}

void assert_connection_ok(connection_t *conn, time_t now)
{
  return;
  assert(conn);
  assert(conn->type >= _CONN_TYPE_MIN);
  assert(conn->type <= _CONN_TYPE_MAX);

  /* XXX check: wants_to_read, wants_to_write, s, poll_index,
   * marked_for_close. */
  
  /* buffers */
  assert(conn->inbuf);
  assert(conn->outbuf);

  assert(!now || conn->timestamp_lastread <= now);
  assert(!now || conn->timestamp_lastwritten <= now);
  assert(conn->timestamp_created <= conn->timestamp_lastread);
  assert(conn->timestamp_created <= conn->timestamp_lastwritten);
  
  /* XXX Fix this; no longer so.*/
#if 0
  if(conn->type != CONN_TYPE_OR && conn->type != CONN_TYPE_DIR)
    assert(!conn->pkey);
  /* pkey is set if we're a dir client, or if we're an OR in state OPEN
   * connected to another OR.
   */
#endif

  if (conn->type != CONN_TYPE_OR) {
    assert(!conn->tls);
  } else {
    if(conn->state == OR_CONN_STATE_OPEN) {
      assert(conn->bandwidth > 0);
      assert(conn->receiver_bucket >= 0);
      assert(conn->receiver_bucket <= 10*conn->bandwidth);
    }
    assert(conn->addr && conn->port);
    assert(conn->address);
    if (conn->state != OR_CONN_STATE_CONNECTING)
      assert(conn->tls);
  }
  
  if (conn->type != CONN_TYPE_EXIT && conn->type != CONN_TYPE_AP) {
    assert(!conn->stream_id[0]);
    assert(!conn->next_stream);
    assert(!conn->cpath_layer);
    assert(!conn->package_window);
    assert(!conn->deliver_window);
    assert(!conn->done_sending);
    assert(!conn->done_receiving);
  } else {
    assert(!conn->next_stream || 
           conn->next_stream->type == CONN_TYPE_EXIT ||
           conn->next_stream->type == CONN_TYPE_AP);
    if(conn->type == CONN_TYPE_AP && conn->state == AP_CONN_STATE_OPEN)
      assert(conn->cpath_layer);
    if(conn->cpath_layer)
      assert_cpath_layer_ok(conn->cpath_layer);
    /* XXX unchecked, package window, deliver window. */
  }

  switch(conn->type) 
    {
    case CONN_TYPE_OR_LISTENER:
    case CONN_TYPE_AP_LISTENER:
    case CONN_TYPE_DIR_LISTENER:
      assert(conn->state == LISTENER_STATE_READY);
      break;
    case CONN_TYPE_OR:
      assert(conn->state >= _OR_CONN_STATE_MIN &&
             conn->state <= _OR_CONN_STATE_MAX);
      break;
    case CONN_TYPE_EXIT:
      assert(conn->state >= _EXIT_CONN_STATE_MIN &&
             conn->state <= _EXIT_CONN_STATE_MAX);
      break;
    case CONN_TYPE_AP:
      assert(conn->state >= _AP_CONN_STATE_MIN &&
             conn->state <= _AP_CONN_STATE_MAX);
      break;
    case CONN_TYPE_DIR:
      assert(conn->state >= _DIR_CONN_STATE_MIN &&
             conn->state <= _DIR_CONN_STATE_MAX);
      break;
    case CONN_TYPE_DNSWORKER:
      assert(conn->state == DNSWORKER_STATE_IDLE ||
             conn->state == DNSWORKER_STATE_BUSY);
    case CONN_TYPE_CPUWORKER:
      assert(conn->state >= _CPUWORKER_STATE_MIN &&
             conn->state <= _CPUWORKER_STATE_MAX);
      break;
    default:
      assert(0);
  }
}

/*
  Local Variables:
  mode:c
  indent-tabs-mode:nil
  c-basic-offset:2
  End:
*/

