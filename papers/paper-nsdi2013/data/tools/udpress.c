/*
 * Copyright (c) 2011-2012 Yaroslav Stavnichiy <yarosla@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <assert.h>
#include <malloc.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <netdb.h>
#include <errno.h>
#include <math.h>

#include <ev.h>

#define VERSION "1.1"

#if (__SIZEOF_POINTER__==8)
typedef uint64_t int_to_ptr;
#else
typedef uint32_t int_to_ptr;
#endif

#define MEM_GUARD 128

struct config {
  int num_connections;
  int num_requests;
  int num_threads;
  int progress_step;
  struct addrinfo *saddr;
  const char* uri_path;
  const char* uri_host;
  char* request_data;
  int request_length;

  int keep_alive:1;
  int secure:1;

  char _padding1[MEM_GUARD]; // guard from false sharing
  volatile int request_counter;
  char _padding2[MEM_GUARD];
};

static struct config config;

enum connection_state {C_CONNECTING, C_WRITING, C_READING};

#define CONN_BUF_SIZE 32768

typedef struct connection {
  struct ev_loop* loop;
  struct thread_config* tdata;
  int fd;
  ev_io watch_read;
  ev_io watch_write;
  ev_tstamp last_activity;

  int write_pos;
  int read_pos;
  int bytes_to_read;
  int bytes_received;
  int alive_count;
  int success_count;
  int time_index;

  int keep_alive:1;
  int done:1;

  char buf[CONN_BUF_SIZE];
  char* body_ptr;

  int id;
  enum connection_state state;
} connection;


typedef struct read_time {
   double delta;
   int wrote;
} read_time;

typedef struct thread_config {
  pthread_t tid;
  connection *conns;
  int id;
  int num_conn;
  struct ev_loop* loop;
  ev_tstamp start_time;
  ev_timer watch_heartbeat;

  int shutdown_in_progress;

  int num_success;
  int num_fail;
  long num_bytes_received;
  long num_overhead_received;
  int num_connect;
  ev_tstamp avg_req_time;

  read_time* read_times;
  int num_times;

} thread_config;

void nxweb_die(const char* fmt, ...) {
  va_list ap;
  fprintf(stderr, "FATAL: ");
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(EXIT_FAILURE);
}

static inline const char* get_current_time(char* buf, int max_buf_size) {
  time_t t;
  struct tm tm;
  time(&t);
  localtime_r(&t, &tm);
  strftime(buf, max_buf_size, "%F %T", &tm); // %F=%Y-%m-%d %T=%H:%M:%S
  return buf;
}

void nxweb_log_error(const char* fmt, ...) {
  char cur_time[32];
  va_list ap;

  get_current_time(cur_time, sizeof(cur_time));
  flockfile(stderr);
  fprintf(stderr, "%s [%u:%p]: ", cur_time, getpid(), (void*)pthread_self());
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
  funlockfile(stderr);
}

static inline int setup_socket(int fd) {
  int flags=fcntl(fd, F_GETFL);
  if (flags<0) return flags;
  if (fcntl(fd, F_SETFL, flags|=O_NONBLOCK)<0) return -1;

  struct timeval timeout;
  timeout.tv_sec = 15;
  timeout.tv_usec = 0;

  if( setsockopt( fd, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout) ) < 0 )
     return -1;
  
  if( setsockopt( fd, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout) ) < 0 )
     return -1;

  return 0;
}

static inline void _nxweb_close_good_socket(int fd) {
  close(fd);
}

static inline void _nxweb_close_bad_socket(int fd) {
  struct linger linger;
  linger.l_onoff=1;
  linger.l_linger=0; // timeout for completing writes
  setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
  close(fd);
}

static inline void sleep_ms(int ms) {
  struct timespec req;
  time_t sec=ms/1000;
  ms%=1000;
  req.tv_sec=sec;
  req.tv_nsec=ms*1000000L;
  while(nanosleep(&req, &req)==-1) continue;
}

#define CONN_SUCCESS 1
#define CONN_FAILURE 2

static inline void time_start( connection* conn ) {
  struct timespec ts;
  clock_gettime (CLOCK_REALTIME, &ts);

  conn->time_index = __sync_add_and_fetch( &conn->tdata->num_times, 1 );

  //printf("[%p] open %d\n", conn->tdata, conn->tdata->num_connect);
  conn->tdata->read_times[ conn->time_index ].delta = (double)ts.tv_sec + ((double)ts.tv_nsec * 1e-9);
  conn->tdata->read_times[ conn->time_index ].wrote = 0;
}


static inline void time_end( connection* conn, int status ) {
   
   if( conn->tdata->read_times[ conn->time_index ].wrote == 0 ) {
      conn->tdata->read_times[ conn->time_index ].wrote = status;

      struct timespec ts;
      clock_gettime (CLOCK_REALTIME, &ts);

      conn->tdata->read_times[ conn->time_index ].delta = (double)ts.tv_sec + ((double)ts.tv_nsec * 1e-9) - conn->tdata->read_times[ conn->time_index ].delta;
      //printf( "[%p] delta[%d] = %ld\n", conn->tdata, conn->time_index, conn->tdata->read_times[ conn->time_index ].delta );
   }
}


static inline void inc_success(connection* conn) {
  conn->success_count++;
  conn->tdata->num_success++;
  conn->tdata->num_bytes_received+=conn->bytes_received;
  conn->tdata->num_overhead_received+=(conn->body_ptr-conn->buf);

  time_end( conn, CONN_SUCCESS );
}

static inline void inc_fail(connection* conn) {
  conn->tdata->num_fail++;

  time_end( conn, CONN_FAILURE );
}

static inline void inc_connect(connection* conn) {
  conn->tdata->num_connect++;
  time_start( conn );
}

enum {ERR_AGAIN=-2, ERR_ERROR=-1, ERR_RDCLOSED=-3};

static inline ssize_t conn_read(connection* conn, void* buf, size_t size) {
   ssize_t ret=read(conn->fd, buf, size);
   if (ret>0) return ret;
   if (ret==0) return ERR_RDCLOSED;
   if (errno==EAGAIN) return ERR_AGAIN;
   return ERR_ERROR;
}

static inline ssize_t conn_write(connection* conn, void* buf, size_t size) {
   ssize_t ret=write(conn->fd, buf, size);
   if (ret>=0) return ret;
   if (errno==EAGAIN) return ERR_AGAIN;
   return ERR_ERROR;
}

static inline void conn_close(connection* conn, int good) {
  if (good) _nxweb_close_good_socket(conn->fd);
  else _nxweb_close_bad_socket(conn->fd);
}

static int open_socket(connection* conn);
static void rearm_socket(connection* conn);

static void write_cb(struct ev_loop *loop, ev_io *w, int revents) {
  connection *conn=((connection*)(((char*)w)-offsetof(connection, watch_write)));

  if (conn->state==C_CONNECTING) {
    conn->last_activity=ev_now(loop);
    conn->state=C_WRITING;
  }

  if (conn->state==C_WRITING) {
    int bytes_avail, bytes_sent;
    do {
      bytes_avail=config.request_length-conn->write_pos;
      if (!bytes_avail) {
        conn->state=C_READING;
        conn->read_pos=0;
        ev_io_stop(conn->loop, &conn->watch_write);
        //ev_io_set(&conn->watch_read, conn->fd, EV_READ);
        ev_io_start(conn->loop, &conn->watch_read);
        ev_feed_event(conn->loop, &conn->watch_read, EV_READ);
        return;
      }
      bytes_sent=conn_write(conn, config.request_data+conn->write_pos, bytes_avail);
      if (bytes_sent<0) {
        if (bytes_sent!=ERR_AGAIN) {
          strerror_r(errno, conn->buf, CONN_BUF_SIZE);
          nxweb_log_error("[%d] conn_write() returned %d: %d %s; sent %d of %d bytes total", conn->id, bytes_sent, errno, conn->buf, conn->write_pos, config.request_length);
          conn_close(conn, 0);
          inc_fail(conn);
          open_socket(conn);
          return;
        }
        return;
      }
      if (bytes_sent) conn->last_activity=ev_now(loop);
      conn->write_pos+=bytes_sent;
    } while (bytes_sent==bytes_avail);
    return;
  }
}


static void read_cb(struct ev_loop *loop, ev_io *w, int revents) {
  connection *conn=((connection*)(((char*)w)-offsetof(connection, watch_read)));

  if (conn->state==C_READING) {
    int room_avail, bytes_received;
    
    do {
      room_avail=CONN_BUF_SIZE;
      if (conn->bytes_to_read>0) {
        int bytes_left=conn->bytes_to_read - conn->bytes_received;
        if (bytes_left<room_avail) room_avail=bytes_left;
      }
      bytes_received=conn_read(conn, conn->buf, room_avail);
      if (bytes_received<=0) {
        if (bytes_received==ERR_AGAIN) return;
        if (bytes_received==ERR_RDCLOSED) {
          nxweb_log_error("body [%d] read connection closed", conn->alive_count);
          conn_close(conn, 0);
          inc_fail(conn);
          open_socket(conn);
          return;
        }
        strerror_r(errno, conn->buf, CONN_BUF_SIZE);
        nxweb_log_error("body [%d] conn_read() returned %d error: %d %s", conn->alive_count, bytes_received, errno, conn->buf);
        conn_close(conn, 0);
        inc_fail(conn);
        open_socket(conn);
        return;
      }

      conn->last_activity=ev_now(loop);
      conn->bytes_received+=bytes_received;
      if (conn->bytes_received>=conn->bytes_to_read) {
         // read all
         rearm_socket(conn);
         return;
      }

    } while (bytes_received==room_avail);
    return;
  }
}

static void shutdown_thread(thread_config* tdata) {
  int i;
  connection* conn;
  ev_tstamp now=ev_now(tdata->loop);
  ev_tstamp time_limit=5.;
  
  //fprintf(stderr, "[%.6lf]", time_limit);
  for (i=0; i<tdata->num_conn; i++) {
    conn=&tdata->conns[i];
    if (!conn->done) {
      if (ev_is_active(&conn->watch_read) || ev_is_active(&conn->watch_write)) {
         
        if ( (now - conn->last_activity) > time_limit) {
          // kill this connection
          if (ev_is_active(&conn->watch_write)) ev_io_stop(conn->loop, &conn->watch_write);
          if (ev_is_active(&conn->watch_read)) ev_io_stop(conn->loop, &conn->watch_read);
          conn_close(conn, 0);
          nxweb_log_error("forcibly shutting down [%p] : %d : %d", tdata, conn->id, conn->fd);
          inc_fail(conn);
          conn->done=1;
          //fprintf(stderr, "*");
        }
        else {
          // don't kill this yet, but wake it up
          if (ev_is_active(&conn->watch_read)) {
            ev_feed_event(tdata->loop, &conn->watch_read, EV_READ);
          }
          if (ev_is_active(&conn->watch_write)) {
            ev_feed_event(tdata->loop, &conn->watch_write, EV_WRITE);
          }
          //fprintf(stderr, ".");
        }
      }
      else {
         conn->done = 1;
      }
    }
  }
}

static int more_requests_to_run() {
  int rc=__sync_add_and_fetch(&config.request_counter, 1);
  if (rc>config.num_requests) {
    return 0;
  }
  if (config.progress_step>=10 && (rc%config.progress_step==0 || rc==config.num_requests)) {
    printf("%d requests launched\n", rc);
  }
  return rc;
}

static void heartbeat_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  if (config.request_counter>config.num_requests) {
    thread_config *tdata=((thread_config*)(((char*)w)-offsetof(thread_config, watch_heartbeat)));
    if (!tdata->shutdown_in_progress) {
      ev_tstamp now=ev_now(tdata->loop);
      tdata->avg_req_time=tdata->num_success? (now-tdata->start_time) * tdata->num_conn / tdata->num_success : 0.1;
      if (tdata->avg_req_time>1.) tdata->avg_req_time=1.;
      tdata->shutdown_in_progress=1;
      printf("shutdown thread\n");
    }
    shutdown_thread(tdata);
  }
}

static void rearm_socket(connection* conn) {
  if (ev_is_active(&conn->watch_write)) ev_io_stop(conn->loop, &conn->watch_write);
  if (ev_is_active(&conn->watch_read)) ev_io_stop(conn->loop, &conn->watch_read);

  inc_success(conn);

  if (!config.keep_alive || !conn->keep_alive) {
    conn_close(conn, 1);
    open_socket(conn);
  }
  else {
    int nc = more_requests_to_run();
    if (!nc) {
      conn_close(conn, 1);
      conn->done=1;
      ev_feed_event(conn->tdata->loop, &conn->tdata->watch_heartbeat, EV_TIMER);
      return;
    }
    conn->alive_count++;
    conn->state=C_WRITING;
    conn->write_pos=0;
    conn->id = nc;
    time_start(conn);
    ev_io_start(conn->loop, &conn->watch_write);
    ev_feed_event(conn->loop, &conn->watch_write, EV_WRITE);
  }
}

static int open_socket(connection* conn) {

  if (ev_is_active(&conn->watch_write)) ev_io_stop(conn->loop, &conn->watch_write);
  if (ev_is_active(&conn->watch_read)) ev_io_stop(conn->loop, &conn->watch_read);

  int nc = more_requests_to_run();
  if (!nc) {
    conn->done=1;
    ev_feed_event(conn->tdata->loop, &conn->tdata->watch_heartbeat, EV_TIMER);
    return 1;
  }

  inc_connect(conn);
  conn->id = nc;
  conn->fd=socket(config.saddr->ai_family, config.saddr->ai_socktype, config.saddr->ai_protocol);
  if (conn->fd==-1) {
    strerror_r(errno, conn->buf, CONN_BUF_SIZE);
    nxweb_log_error("can't open socket [%d] %s", errno, conn->buf);
    return -1;
  }
  if (setup_socket(conn->fd)) {
    nxweb_log_error("can't setup socket");
    return -1;
  }
  if (connect(conn->fd, config.saddr->ai_addr, config.saddr->ai_addrlen)) {
    if (errno!=EINPROGRESS && errno!=EALREADY && errno!=EISCONN) {
      nxweb_log_error("can't connect %d", errno);
      return -1;
    }
  }

  conn->state=C_CONNECTING;
  conn->write_pos=0;
  conn->alive_count=0;
  conn->done=0;
  ev_io_set(&conn->watch_write, conn->fd, EV_WRITE);
  ev_io_set(&conn->watch_read, conn->fd, EV_READ);
  ev_io_start(conn->loop, &conn->watch_write);
  ev_feed_event(conn->loop, &conn->watch_write, EV_WRITE);
  return 0;
}

static void* thread_main(void* pdata) {

  thread_config* tdata=(thread_config*)pdata;

  ev_timer_init(&tdata->watch_heartbeat, heartbeat_cb, 0.1, 0.1);
  ev_timer_start(tdata->loop, &tdata->watch_heartbeat);
  ev_unref(tdata->loop); // don't keep loop running just for heartbeat
  ev_run(tdata->loop, 0);

  ev_loop_destroy(tdata->loop);

  if (config.num_threads>1) {
    printf("thread %d: %d connect, %d requests, %d success, %d fail, %ld bytes, %ld overhead\n",
         tdata->id, tdata->num_connect, tdata->num_success+tdata->num_fail,
         tdata->num_success, tdata->num_fail, tdata->num_bytes_received,
         tdata->num_overhead_received);
  }

  return 0;
}


static int resolve_host(struct addrinfo** saddr, const char *host_and_port) {
  char* host=strdup(host_and_port);
  char* port=strchr(host, ':');
  if (port) *port++='\0';
  else port=config.secure? "443":"80";

	struct addrinfo hints, *res, *res_first, *res_last;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family=PF_UNSPEC;
	hints.ai_socktype=SOCK_DGRAM;

	if (getaddrinfo(host, port, &hints, &res_first)) goto ERR1;

	// search for an ipv4 address, no ipv6 yet
	res_last=0;
	for (res=res_first; res; res=res->ai_next) {
		if (res->ai_family==AF_INET) break;
		res_last=res;
	}

	if (!res) goto ERR2;
	if (res!=res_first) {
		// unlink from list and free rest
		res_last->ai_next = res->ai_next;
		freeaddrinfo(res_first);
		res->ai_next=0;
	}

  free(host);
  *saddr=res;
	return 0;

ERR2:
	freeaddrinfo(res_first);
ERR1:
  free(host);
  return -1;
}


static void show_help(void) {
	printf( "httpress <options> <url>\n"
          "  -n num         number of requests     (default: 1)\n"
          "  -t num         number of threads      (default: 1)\n"
          "  -c num         concurrent connections (default: 1)\n"
          "  -k             keep socket open       (default: no)\n"
          "  -h             show this help\n"
          "  -R file        File containing raw information to send\n"
          //"  -v       show version\n"
          "\n"
          "example: httpress -n 10000 -c 100 -t 4 -k localhost:8080\n\n");
}

static char host_buf[1024];


int time_compar( const void* a1, const void* a2 ) {
   if( ((read_time*)a1)->delta < ((read_time*)a2)->delta )
      return -1;
   if( ((read_time*)a1)->delta > ((read_time*)a2)->delta )
      return 1;
   return 0;
}

static int parse_uri(const char* uri) {
  const char* p=strchr(uri, '/');
  if (!p) {
    config.uri_host=uri;
    config.uri_path="/";
    return 0;
  }
  if ((p-uri)>sizeof(host_buf)-1) return -1;
  strncpy(host_buf, uri, (p-uri));
  host_buf[(p-uri)]='\0';
  config.uri_host=host_buf;
  config.uri_path=p;
  return 0;
}

int main(int argc, char* argv[]) {

  config.num_connections=1;
  config.num_requests=1;
  config.num_threads=1;
  config.keep_alive=0;
  config.uri_path=0;
  config.uri_host=0;
  config.request_counter=0;

  char const* raw_file = NULL;

  int c;
  while ((c=getopt(argc, argv, ":hvkn:t:c:z:R:"))!=-1) {
    switch (c) {
      case 'h':
        show_help();
        return 0;
      case 'v':
        printf("version:    " VERSION "\n");
        printf("build-date: " __DATE__ " " __TIME__ "\n\n");
        return 0;
      case 'k':
        config.keep_alive=1;
        break;
      case 'n':
        config.num_requests=atoi(optarg);
        break;
      case 't':
        config.num_threads=atoi(optarg);
        break;
      case 'c':
        config.num_connections=atoi(optarg);
        break;
      case 'R':
        raw_file=optarg;
        break;
      case '?':
        fprintf(stderr, "unkown option: -%c\n\n", optopt);
        show_help();
        return EXIT_FAILURE;
    }
  }

  if ((argc-optind)<1) {
    fprintf(stderr, "missing url argument\n\n");
    show_help();
    return EXIT_FAILURE;
  }
  else if ((argc-optind)>1) {
    fprintf(stderr, "too many arguments\n\n");
    show_help();
    return EXIT_FAILURE;
  }

  if (config.num_requests<1 || config.num_requests>1000000000) nxweb_die("wrong number of requests");
  if (config.num_connections<1 || config.num_connections>1000000 || config.num_connections>config.num_requests) nxweb_die("wrong number of connections");
  if (config.num_threads<1 || config.num_threads>100000 || config.num_threads>config.num_connections) nxweb_die("wrong number of threads");

  config.progress_step=config.num_requests/4;
  if (config.progress_step>50000) config.progress_step=50000;

  if (parse_uri(argv[optind])) nxweb_die("can't parse url: %s", argv[optind]);

  // Block signals for all threads
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGPIPE);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGQUIT);
  sigaddset(&set, SIGHUP);
  if (pthread_sigmask(SIG_BLOCK, &set, NULL)) {
    nxweb_log_error("can't set pthread_sigmask");
    exit(EXIT_FAILURE);
  }

  if (resolve_host(&config.saddr, config.uri_host)) {
    nxweb_log_error("can't resolve host %s", config.uri_host);
    exit(EXIT_FAILURE);
  }
  
  if( raw_file ) {
      FILE* raw_fh = fopen( raw_file, "r" );
      if( !raw_fh ) {
         int errsv = errno;
         nxweb_log_error("Could not open %s, errno = %d\n", raw_file, errsv );
         exit(1);
      }

      struct stat sb;
      int rc = fstat( fileno(raw_fh), &sb );
      if( rc != 0 ) {
         int errsv = errno;
         nxweb_log_error("Could not stat %s, errno = %d\n", raw_file, errsv );
         exit(1);
      }

      config.request_data = calloc( sb.st_size, 1 );
      fread( config.request_data, 1, sb.st_size, raw_fh );
      config.request_length = sb.st_size;
      fclose( raw_fh );
  }
  else {
     config.request_data = calloc( 8192, 1 );
     config.request_length=1;
  }

  printf("<<<<<<<<<<<<<< %d <<<<<<<<<<<<<<\n%s\n<<<<<<<<<<<<<< %d <<<<<<<<<<<<<<\n", config.request_length, config.request_data, config.request_length );

  thread_config** threads=calloc(config.num_threads, sizeof(thread_config*));
  if (!threads) nxweb_die("can't allocate thread pool");

  ev_tstamp ts_start=ev_time();
  int i, j;
  int conns_allocated=0;
  thread_config* tdata;
  for (i=0; i<config.num_threads; i++) {
    threads[i]=
    tdata=memalign(MEM_GUARD, sizeof(thread_config)+MEM_GUARD);
    if (!tdata) nxweb_die("can't allocate thread data");
    memset(tdata, 0, sizeof(thread_config));
    tdata->id=i+1;
    tdata->start_time=ts_start;
    tdata->num_conn=(config.num_connections-conns_allocated)/(config.num_threads-i);
    conns_allocated+=tdata->num_conn;
    tdata->conns=memalign(MEM_GUARD, tdata->num_conn*sizeof(connection)+MEM_GUARD);
    if (!tdata->conns) nxweb_die("can't allocate thread connection pool");
    memset(tdata->conns, 0, tdata->num_conn*sizeof(connection));

    tdata->read_times=memalign(MEM_GUARD, config.num_requests*sizeof(read_time)+MEM_GUARD);
    if (!tdata->read_times) nxweb_die("can't allocate thread read times");
    memset(tdata->read_times, 0, config.num_requests*sizeof(read_time));
    
    tdata->loop=ev_loop_new(0);

    connection* conn;
    for (j=0; j<tdata->num_conn; j++) {
      conn=&tdata->conns[j];
      conn->tdata=tdata;
      conn->loop=tdata->loop;
      ev_io_init(&conn->watch_write, write_cb, -1, EV_WRITE);
      ev_io_init(&conn->watch_read, read_cb, -1, EV_READ);
      open_socket(conn);
    }

    pthread_create(&tdata->tid, 0, thread_main, tdata);
    //sleep_ms(10);
  }

  // Unblock signals for the main thread;
  // other threads have inherited sigmask we set earlier
  sigdelset(&set, SIGPIPE); // except SIGPIPE
  if (pthread_sigmask(SIG_UNBLOCK, &set, NULL)) {
    nxweb_log_error("can't unset pthread_sigmask");
    exit(EXIT_FAILURE);
  }

  int total_success=0;
  int total_fail=0;
  long total_bytes=0;
  long total_overhead=0;
  int total_connect=0;

  for (i=0; i<config.num_threads; i++) {
    tdata=threads[i];
    pthread_join(threads[i]->tid, 0);
    total_success+=tdata->num_success;
    //total_success+=tdata->num_connect;
    total_fail+=tdata->num_fail;
    total_bytes+=tdata->num_bytes_received;
    total_overhead+=tdata->num_overhead_received;
    total_connect+=tdata->num_connect;
  }

  int real_concurrency=0;
  int real_concurrency1=0;
  int real_concurrency1_threshold=config.num_requests/config.num_connections/10;
  if (real_concurrency1_threshold<2) real_concurrency1_threshold=2;
  for (i=0; i<config.num_threads; i++) {
    tdata=threads[i];
    for (j=0; j<tdata->num_conn; j++) {
      connection* conn=&tdata->conns[j];
      if (conn->success_count) real_concurrency++;
      if (conn->success_count>=real_concurrency1_threshold) real_concurrency1++;
    }
  }

  ev_tstamp ts_end=ev_time();
  if (ts_end<=ts_start) ts_end=ts_start+0.00001;
  ev_tstamp duration=ts_end-ts_start;
  int sec=duration;
  duration=(duration-sec)*1000;
  int millisec=duration;
  duration=(duration-millisec)*1000;
  //int microsec=duration;
  int rps=total_success/(ts_end-ts_start);
  int kbps=(total_bytes+total_overhead) / (ts_end-ts_start) / 1024;
  ev_tstamp avg_req_time=total_success? (ts_end-ts_start) * config.num_connections / total_success : 0;

  printf("\nTOTALS:  %d connect, %d requests, %d success, %d fail, %d (%d) real concurrency\n",
         total_connect, total_success+total_fail, total_success, total_fail, real_concurrency, real_concurrency1);
  printf("TRAFFIC: %ld avg bytes, %ld avg overhead, %ld bytes, %ld overhead\n",
         total_success?total_bytes/total_success:0L, total_success?total_overhead/total_success:0L, total_bytes, total_overhead);
  printf("TIMING:  %d.%03d seconds, %d rps, %d kbps, %.1f ms avg req time\n",
         sec, millisec, /*microsec,*/ rps, kbps, (float)(avg_req_time*1000));

  size_t num_xfer = total_success + total_fail;
  printf(" total transfers = %ld\n", num_xfer );
  
  read_time* all_times = calloc( num_xfer * sizeof(read_time), 1 );
  int k = 0;
  for( i = 0; i < config.num_threads; i++ ) {
     for( j = 0; j < threads[i]->num_times; j++ ) {
        all_times[k] = threads[i]->read_times[j];
        k++;
     }
  }

  double total = 0;
  qsort( all_times, num_xfer, sizeof(read_time), time_compar );

  FILE* f = fopen( "all-time.txt", "w" );
  for( i = 0; i < num_xfer; i++ ) {
     total += all_times[i].delta;

     char c = 'U';
     if( all_times[i].wrote == CONN_SUCCESS )
        c = 'S';
     else if( all_times[i].wrote == CONN_FAILURE )
        c = 'F';
     
     fprintf( f, "%c %lf\n", c, all_times[i].delta );
  }
  fclose( f );
  
  total /= num_xfer;

  printf("All-time average (ms):   %lf\n", total * 1000 );
  printf("All-time median (ms):    %lf\n", all_times[ num_xfer / 2 ].delta * 1000 );
  printf("All-time 90th time (ms): %lf\n", all_times[ num_xfer * 9 / 10 ].delta * 1000 );
  printf("All-time 99th time (ms): %lf\n", all_times[ num_xfer * 99 / 100 ].delta * 1000 );

     
  freeaddrinfo(config.saddr);
  for (i=0; i<config.num_threads; i++) {
    tdata=threads[i];
    free(tdata->conns);
    free(tdata);
  }
  free(threads);


  return EXIT_SUCCESS;
}
