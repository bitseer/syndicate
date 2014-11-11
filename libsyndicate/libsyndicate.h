/*
   Copyright 2013 The Trustees of Princeton University

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#ifndef _LIBSYNDICATE_H_
#define _LIBSYNDICATE_H_

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <cstring>
#include <memory>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <vector>
#include <map>
#include <dirent.h>
#include <utime.h>
#include <fstream>
#include <string>
#include <iostream>
#include <sys/socket.h>
#include <pthread.h>
#include <pwd.h>
#include <math.h>
#include <locale>
#include <signal.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <inttypes.h>
#include <sys/resource.h>
#include <stdexcept>

#ifndef _SYNDICATE_NACL_
#include <execinfo.h>
#endif

using namespace std;

#include <curl/curl.h>

#include "ms.pb.h"
#include "serialization.pb.h"

// if we're building a Google Native Client plugin, we have to define these...
#ifndef EREMOTEIO
#define EREMOTEIO 121
#endif

#ifndef MIN
#define MIN(x,y) ((x) < (y) ? (x) : (y))
#endif

#define MD_ENTRY_FILE ms::ms_entry::MS_ENTRY_TYPE_FILE
#define MD_ENTRY_DIR  ms::ms_entry::MS_ENTRY_TYPE_DIR

// metadata entry (represents a file or a directory)
struct md_entry {
   int type;            // file or directory?
   char* name;          // name of this entry
   uint64_t file_id;    // id of this file 
   int64_t ctime_sec;   // creation time (seconds)
   int32_t ctime_nsec;  // creation time (nanoseconds)
   int64_t mtime_sec;   // last-modified time (seconds)
   int32_t mtime_nsec;  // last-modified time (nanoseconds)
   int64_t manifest_mtime_sec;  // manifest last-mod time (actual last-write time, regardless of utime) (seconds)
   int32_t manifest_mtime_nsec; // manifest last-mod time (actual last-write time, regardless of utime) (nanoseconds)
   int64_t write_nonce; // last-write nonce 
   int64_t xattr_nonce; // xattr write nonce
   int64_t version;     // file version
   int32_t max_read_freshness;      // how long is this entry fresh until it needs revalidation?
   int32_t max_write_freshness;     // how long can we delay publishing this entry?
   uint64_t owner;         // ID of the User that owns this File
   uint64_t coordinator;  // ID of the Gateway that coordinatates writes on this File
   uint64_t volume;        // ID of the Volume
   mode_t mode;         // file permission bits
   off_t size;          // size of the file
   int32_t error;       // error information with this md_entry
   int64_t generation;  // n, as in, the nth item to ever be created in the parent directory
   int64_t num_children; // number of children this entry has (if it's a directory)
   uint64_t parent_id;  // id of this file's parent directory
   char* parent_name;   // name of this file's parent directory
};

typedef list<struct md_entry*> md_entry_list;


// metadata update
struct md_update {
   char op;               // update operation
   struct md_entry ent;
   struct md_entry dest;  // only used by RENAME
   int error;             // error information
   int flags;             // op-specific flags
   char* xattr_name;      // xattr name (setxattr, removexattr)
   char* xattr_value;     // xattr value (setxattr)
   size_t xattr_value_len;
   uint64_t xattr_owner;  // xattr owner (chownxattr)
   mode_t xattr_mode;     // xattr mode (chmodxattr)
   
   uint64_t* affected_blocks;   // blocks affected by a write 
   size_t num_affected_blocks;  
};

// metadata update operations
#define MD_OP_ADD    'A'      // add/replace an entry
#define MD_OP_RM     'R'      // remove an entry
#define MD_OP_UP     'U'      // update an existing entry
#define MD_OP_ERR    'E'      // (response only) error encountered in processing the entry
#define MD_OP_VER    'V'      // write verification request
#define MD_OP_USR    'S'      // special (context-specific) state
#define MD_OP_NEWBLK 'B'      // new block written
#define MD_OP_CHOWN  'C'

// upload buffer
struct md_upload_buf {
   char const* text;
   int offset;
   int len;
};


// merge command-line options with the config....
#define MD_SYNDICATE_CONF_OPT( conf, optname, value ) \
   do { \
     if( (value) ) { \
        if( (conf).optname ) { \
           free( (conf).optname ); \
        } \
        \
        (conf).optname = strdup( value ); \
      } \
   } while( 0 );


// server configuration
struct md_syndicate_conf {
   
   // UG fields
   int64_t default_read_freshness;                    // default number of milliseconds a file can age before needing refresh for reads
   int64_t default_write_freshness;                   // default number of milliseconds a file can age before needing refresh for writes
   char* logfile_path;                                // path to the logfile
   bool gather_stats;                                 // gather statistics or not?
   char* content_url;                                 // what is the URL under which local data can be accessed publicly?
   char* storage_root;                                // toplevel directory that stores local syndicate state (blocks, manifests, logs, etc)
   char* volume_name;                                 // name of the volume we're connected to
   char* volume_pubkey_path;                          // path on disk to find Volume metadata public key
   int max_read_retry;                                // maximum number of times to retry a read (i.e. fetching a block or manifest) before considering it failed 
   int max_write_retry;                               // maximum number of times to retry a write (i.e. replicating a block or manifest) before considering it failed
   int max_metadata_read_retry;                       // maximum number of times to retry a metadata read before considering it failed 
   int max_metadata_write_retry;                      // maximum number of times to retry a metadata write before considering it failed
   int retry_delay_ms;                                // number of milliseconds to wait between retries
   
   // RG/AG servers
   unsigned int num_http_threads;                     // how many HTTP threads to create
   char* server_key_path;                             // path to PEM-encoded TLS public/private key for this gateway server
   char* server_cert_path;                            // path to PEM-encoded TLS certificate for this gateway server
   char* local_sd_dir;                                // directory containing local storage drivers (AG only)
   
   // debug
   int debug_lock;                                    // print verbose information on locks

   // common
   char* gateway_name;                                // name of this gateway
   int portnum;                                       // Syndicate-side port number
   int connect_timeout;                               // number of seconds to wait to connect for data
   int transfer_timeout;                              // how long a manifest/block transfer is allowed to take (in seconds)
   bool verify_peer;                                  // whether or not to verify the gateway server's SSL certificate with peers (if using HTTPS to talk to them)
   char* gateway_key_path;                            // path to PEM-encoded user-given public/private key for this gateway
   int replica_connect_timeout;                       // number of seconds to wait to connect to an RG
   
   // MS-related fields
   char* metadata_url;                                // MS url
   char* ms_username;                                 // MS username for this SyndicateUser
   char* ms_password;                                 // MS password for this SyndicateUser
   uint64_t owner;                                    // what is our user ID in Syndicate?  Files created in this UG will assume this UID as their owner
   uint64_t gateway;                                  // what is our gateway ID?
   uint64_t volume;                                   // what is our volume ID?
   uint64_t view_reload_freq;                         // how often do we check for new metadata from the MS?
   char* syndicate_pubkey_path;                       // location on disk where the MS's public key can be found.

   // security fields (loaded at runtime).
   // private keys are all mlock'ed
   char* gateway_key;                                 // gateway private key (PEM format)
   size_t gateway_key_len;
   char* server_key;                                  // TLS private key (PEM format)
   size_t server_key_len;
   char* server_cert;                                 // TLS certificate (PEM format)
   size_t server_cert_len;
   char* volume_pubkey;                               // volume metadata public key (PEM format)
   size_t volume_pubkey_len;
   char* syndicate_pubkey;                            // Syndicate-specific public key
   size_t syndicate_pubkey_len;
   char* user_pkey;                                   // SyndicateUser private key 
   size_t user_pkey_len;

   // set at runtime
   bool need_storage;                                 // set to true if we need to automatically setup local storage
   bool need_networking;                              // set to true if we need to auto-discover networking settings (like our hostname)
   char* data_root;                                   // root of the path where we store local file blocks (a subdirectory of storage_root)
   mode_t usermask;                                   // umask of the user running this program
   char* hostname;                                    // what's our hostname?
   
   // misc
   int gateway_type;                                  // type of gateway 
   bool is_client;                                    // if true for a UG, always fetch data from RGs
};

#define USER_ANON               (uint64_t)0xFFFFFFFFFFFFFFFFLL
#define GATEWAY_ANON            (uint64_t)0xFFFFFFFFFFFFFFFFLL

#define COMMENT_KEY                 '#'

#define DEBUG_KEY                   "DEBUG_LEVEL"

// config elements 
#define DEFAULT_READ_FRESHNESS_KEY  "DEFAULT_READ_FRESHNESS"
#define DEFAULT_WRITE_FRESHNESS_KEY "DEFAULT_WRITE_FRESHNESS"
#define METADATA_URL_KEY            "METADATA_URL"
#define LOGFILE_PATH_KEY            "LOGFILE"
#define GATHER_STATS_KEY            "GATHER_STATISTICS"
#define MS_USERNAME_KEY             "USERNAME"
#define MS_PASSWORD_KEY             "PASSWORD"
#define STORAGE_ROOT_KEY            "STORAGE_ROOT"

#define CONNECT_TIMEOUT_KEY         "CONNECT_TIMEOUT"
#define TRANSFER_TIMEOUT_KEY        "TRANSFER_TIMEOUT"
#define NUM_HTTP_THREADS_KEY        "HTTP_THREADPOOL_SIZE"

#define PORTNUM_KEY                 "PORTNUM"
#define SSL_PKEY_KEY                "TLS_PKEY"
#define SSL_CERT_KEY                "TLS_CERT"
#define GATEWAY_KEY_KEY             "GATEWAY_KEY"
#define VOLUME_PUBKEY_KEY           "VOLUME_PUBKEY"
#define SYNDICATE_PUBKEY_KEY        "SYNDICATE_PUBKEY"
#define VOLUME_NAME_KEY             "VOLUME_NAME"
#define GATEWAY_NAME_KEY            "GATEWAY_NAME"

#define LOCAL_STORAGE_DRIVERS_KEY   "LOCAL_STORAGE_DRIVERS"

// misc
#define VERIFY_PEER_KEY             "SSL_VERIFY_PEER"
#define CONTENT_URL_KEY             "PUBLIC_URL"
#define VIEW_RELOAD_FREQ_KEY        "VIEW_RELOAD_FREQ"

#define SYNDICATEFS_XATTR_URL          "user.syndicate_url"
#define CLIENT_DEFAULT_CONFIG          "/usr/etc/syndicate/syndicate-UG.conf"
#define AG_GATEWAY_DRIVER_KEY	    "AG_GATEWAY_DRIVER"

// URL protocol prefix for local files
#define SYNDICATEFS_LOCAL_PROTO     "file://"

#define SYNDICATE_DATA_PREFIX "SYNDICATE-DATA"

// check to see if a URL refers to local data
#define URL_LOCAL( url ) (strlen(url) > strlen(SYNDICATEFS_LOCAL_PROTO) && strncmp( (url), SYNDICATEFS_LOCAL_PROTO, strlen(SYNDICATEFS_LOCAL_PROTO) ) == 0)

// extract the absolute, underlying path from a local url
#define GET_PATH( url ) ((char*)(url) + strlen(SYNDICATEFS_LOCAL_PROTO))

// extract the filesystem path from a local url
#define GET_FS_PATH( root, url ) ((char*)(url) + strlen(SYNDICATEFS_LOCAL_PROTO) + strlen(root) - 1)

extern "C" {

// library config 
int md_debug( struct md_syndicate_conf* conf, int level );
int md_error( struct md_syndicate_conf* conf, int level );
int md_signals( int use_signals );

// configuration parser
int md_read_conf( char const* conf_path, struct md_syndicate_conf* conf );
int md_read_conf_line( char* line, char** key, char*** values );
int md_free_conf( struct md_syndicate_conf* conf );

// md_entry
struct md_entry* md_entry_dup( struct md_entry* src );
void md_entry_dup2( struct md_entry* src, struct md_entry* ret );
void md_entry_free( struct md_entry* ent );

// serialization
int md_path_version_offset( char const* path );
ssize_t md_metadata_update_text( struct md_syndicate_conf* conf, char **buf, struct md_update** updates );
ssize_t md_metadata_update_text2( struct md_syndicate_conf* conf, char **buf, vector<struct md_update>* updates );
ssize_t md_metadata_update_text3( struct md_syndicate_conf* conf, char **buf, struct md_update* (*iterator)( void* ), void* arg );
char* md_clear_version( char* path );
void md_update_free( struct md_update* update );
void md_update_dup2( struct md_update* src, struct md_update* dest );

// path manipulation
char* md_fullpath( char const* root, char const* path, char* dest );
char* md_dirname( char const* path, char* dest );
char* md_basename( char const* path, char* dest );
int md_depth( char const* path );
int md_basename_begin( char const* path );
int md_dirname_end( char const* path );
char* md_prepend( char const* prefix, char const* str, char* output );
long md_hash( char const* path );
int md_path_split( char const* path, vector<char*>* result );
void md_sanitize_path( char* path );

// serialization
int md_entry_to_ms_entry( ms::ms_entry* msent, struct md_entry* ent );
int ms_entry_to_md_entry( const ms::ms_entry& msent, struct md_entry* ent );

// threading
pthread_t md_start_thread( void* (*thread_func)(void*), void* args, bool detach );

// upload callback for CURL
size_t md_default_upload_callback(void *ptr, size_t size, size_t nmemb, void *userp);

// URL parsing
char** md_parse_cgi_args( char* query_string );
char* md_url_hostname( char const* url );
char* md_url_scheme( char const* url );
char* md_path_from_url( char const* url );
char* md_fs_path_from_url( char const* url );
char* md_url_strip_path( char const* url );
int md_portnum_from_url( char const* url );
char* md_strip_protocol( char const* url );
char* md_flatten_path( char const* path );

int md_split_url_qs( char const* url, char** url_and_path, char** qs );

// header parsing
off_t md_header_value_offset( char* header_buf, size_t header_len, char const* header_name );
uint64_t md_parse_header_uint64( char* hdr, off_t offset, size_t size );
uint64_t* md_parse_header_uint64v( char* hdr, off_t offset, size_t size, size_t* ret_len );

// networking 
int md_set_hostname( struct md_syndicate_conf* conf, char const* hostname );

// top-level initialization
int md_init( struct md_syndicate_conf* conf, struct ms_client* client, struct md_opts* opts );

// initialize syndicate as a client only
int md_init_client( struct md_syndicate_conf* conf, struct ms_client* client, struct md_opts* opts );

int md_shutdown(void);
int md_default_conf( struct md_syndicate_conf* conf, int gateway_type );
int md_check_conf( struct md_syndicate_conf* conf );

}


// protobuf serializer
// have to put this here, since C++ forbids separating the declaration and definition of template functions across multiple files???
template <class T> int md_serialize( T* protobuf, char** bits, size_t* bits_len ) {
   string msgbits;
   try {
      protobuf->SerializeToString( &msgbits );
   }
   catch( exception& e ) {
      errorf("SerializeToString exception: %s\n", e.what() );
      return -EINVAL;
   }
   
   char* ret = CALLOC_LIST( char, msgbits.size() );
   if( ret == NULL ) {
      return -ENOMEM;
   }
   
   memcpy( ret, msgbits.data(), msgbits.size() );
   
   *bits = ret;
   *bits_len = msgbits.size();
   
   return 0;
}


// protobuf parser
// have to put this here, since C++ forbids separating the declaration and definition of template functions across multiple files???
template <class T> int md_parse( T* protobuf, char const* bits, size_t bits_len ) {
   bool valid = false;
   try {
      valid = protobuf->ParseFromString( string(bits, bits_len) );
   }
   catch( exception& e ) {
      errorf("ParseFromString exception: %s\n", e.what() );
      return -EINVAL;
   }
   
   if( !valid ) {
      errorf("ParseFromString rc = %d (missing %s)\n", valid, protobuf->InitializationErrorString().c_str() );
      return -EINVAL;
   }
   
   return 0;
}


// system UID
#define SYS_USER 0

#define INVALID_BLOCK_ID (uint64_t)(-1)
#define INVALID_GATEWAY_ID INVALID_BLOCK_ID
#define INVALID_VOLUME_ID INVALID_BLOCK_ID
#define INVALID_FILE_ID INVALID_BLOCK_ID

// gateway types for md_init
#define SYNDICATE_UG       ms::ms_gateway_cert::USER_GATEWAY
#define SYNDICATE_AG       ms::ms_gateway_cert::ACQUISITION_GATEWAY
#define SYNDICATE_RG       ms::ms_gateway_cert::REPLICA_GATEWAY
#define VALID_GATEWAY_TYPE( type ) ((type) == SYNDICATE_UG || (type) == SYNDICATE_RG || (type) == SYNDICATE_AG)

// gateway HTTP error codes (used by the AG and UG)
#define MD_HTTP_TRYAGAIN    503

#define GATEWAY_CAP_READ_DATA  1
#define GATEWAY_CAP_WRITE_DATA  2
#define GATEWAY_CAP_READ_METADATA  4
#define GATEWAY_CAP_WRITE_METADATA  8
#define GATEWAY_CAP_COORDINATE  16

#define RSA_KEY_SIZE 4096


// limits
#define SYNDICATE_MAX_MANIFEST_LEN              100000000         // 100MB

#endif
