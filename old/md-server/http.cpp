#include "http.h"

static time_t g_last_mod_time = 0;      // time of the last POST

static int num_gets_started = 0;      // number of operations per second
static int num_gets_finished = 0;
static int num_posts = 0;  

static pthread_t bw_reporter_thread;
static bool g_running = false;   // are we running?

static bool g_reload = false;    // reload the users on the next connection

static pthread_mutex_t reload_lock = PTHREAD_MUTEX_INITIALIZER;

static char* passwd_buf = NULL;
static size_t passwd_buf_len = 0;
static struct md_user_entry** g_userlist = NULL;

static char* replica_buf = NULL;
static size_t replica_buf_len = 0;
static char** g_replica_urls = NULL;

// set the last mod time
static void set_last_post_time( time_t time ) {
   g_last_mod_time = time;
}

static time_t get_last_post_time() {
   return g_last_mod_time;
}

/*
static void cache_put( char* path, char* data, size_t len, uid_t uid, mode_t mode ) {
   Cache->put( path, data, len, uid, mode );
}

static void cache_clear( char* path ) {
   Cache->clear( path );
}

static int cache_get( char* path, char** data, size_t* len, uid_t uid ) {
   return Cache->get( path, data, len, uid );
}
*/

void* bw_reporter(void*) {
   struct timespec ts;
   ts.tv_sec = 0;
   ts.tv_nsec = 2500000;
   int prev_gets = 0;
   while( g_running ) {
      if( prev_gets != num_gets_finished )
         printf("OPS: %d-%d, %d\n", num_gets_started, num_gets_finished, num_posts);
      prev_gets = num_gets_finished;
      fflush(stdout);
      nanosleep( &ts, NULL );
   }
   return NULL;
}


// make a last-modified header
void add_last_mod_header( struct md_HTTP_response* resp, time_t t ) {
   
   char hdr_buf[200];
   memset( hdr_buf, 0, 200 );
   
   struct tm utc_time;
   time_t utc_seconds = get_last_post_time();
   gmtime_r( &utc_seconds, &utc_time );
   strftime( hdr_buf, 200, "%a, %d %b %Y %H:%M:%S GMT", &utc_time );

   md_HTTP_add_header( resp, "Last-Modified", hdr_buf );
}


// process function for walking the master copy, given a user.
// don't present any information the user isn't allowed to see!
static bool md_process(char* path, struct md_entry* ent, void* arg) {
   struct HTTP_dir_serialization_data* dir_data = (struct HTTP_dir_serialization_data*)arg;

   // serialize this entry
   char* serialized_ent = NULL;

   size_t len = 0;
   /*
   uid_t uid;
   char* url_path = path + strlen(dir_data->conf->master_copy_root) - 1;

   if( dir_data->user )
      uid = dir_data->user->uid;
   else
      uid = MD_GUEST_UID;

   int rc = cache_get( url_path, &serialized_ent, &len, dir_data->user->uid );
   if( rc == -ENOENT ) {
   */
      // cache miss; need to read from disk 
      bool read_rc = md_walk_mc_dir_process_func( path, ent, arg );
      if( !read_rc ) {
         errorf( "md_walk_mc_dir_process_func rc = %s\n", "false");
         return read_rc;
      }

      serialized_ent = md_serialize_entry( ent, NULL );
      strcat( serialized_ent, "\n" );
      len = strlen(serialized_ent);
     
      // printf( "about to put '%s' --> '%s'\n", url_path, serialized_ent );
      //cache_put( url_path, strdup(serialized_ent), len, uid, ent->mode );
   /*
   }
   else if( rc < 0 ) {
      errorf( "cache_get rc = %d\n", rc );
      return false;
   }
   */
   
   dir_data->rb.push_back( buffer_segment_t( serialized_ent, len ) );
   return true;
}

// connection handler
void* HTTP_connect( struct md_HTTP_connection_data* md_con_data ) {
   struct HTTP_connection_data* con_data = new HTTP_connection_data();
   con_data->user = md_con_data->user;
   con_data->line_offset = 0;
   con_data->line_buf = CALLOC_LIST( char, MD_MAX_LINE_LEN );
   con_data->error = 0;
   memset( &con_data->update, 0, sizeof(con_data->update) );
   return (void*)con_data;
}


// GET handler
struct md_HTTP_response* HTTP_GET_handler( struct md_HTTP_connection_data* md_con_data ) {
   __sync_add_and_fetch( &num_gets_started, 1 );
   
   char* url = md_con_data->url_path;
   struct md_HTTP* http = md_con_data->http;
   struct md_syndicate_conf* conf = md_con_data->conf;
   
   struct md_HTTP_response* resp = CALLOC_LIST(struct md_HTTP_response, 1);
   
   char* path = url + 1;
   
   // if this is a query for replica server information, then simply return that
   if( strcmp(path, "REPLICAS") == 0 ) {
      if( replica_buf != NULL ) {
         // build and cache response
         pthread_mutex_lock( &reload_lock );
         md_create_HTTP_response_ram( resp, "text/plain", 200, replica_buf, replica_buf_len + 1 );
         pthread_mutex_unlock( &reload_lock );
      }
      else {
         md_create_HTTP_response_ram_static( resp, "text/plain", 200, MD_HTTP_NOMSG, strlen(MD_HTTP_NOMSG) + 1);
      }
      __sync_add_and_fetch( &num_gets_finished, 1 );
      return resp;
   }
   
   // if this is a UID query, then simply return that
   if( strcmp(path, "OWNERID") == 0 ) {
      char buf[20];
      memset( buf, 0, 20 );
      if( md_con_data->user ) {
         sprintf(buf, "%u\n", md_con_data->user->uid);
      }
      else {
         sprintf(buf, "UNKNOWN\n");
      }
      
      md_create_HTTP_response_ram( resp, "text/plain", 200, buf, strlen(buf) + 1 );
      __sync_add_and_fetch( &num_gets_finished, 1 );
      return resp;
   }

   // if this is a Volume ID query, then simply return that
   if( strcmp(path, "VOLUMEID") == 0 ) {
      char buf[20];
      memset( buf, 0, 20 );
      sprintf(buf, "%u\n", md_con_data->conf->volume_id);

      md_create_HTTP_response_ram( resp, "text/plain", 200, buf, strlen(buf) + 1 );
      __sync_add_and_fetch( &num_gets_finished, 1 );
      return resp;
   }
   
   // if this is a blocking factor query, then simply return that
   if( strcmp(path, "BLOCKSIZE") == 0 ) {
      char buf[20];
      memset( buf, 0, 20 );
      sprintf(buf, "%lld", md_con_data->conf->blocking_factor );

      md_create_HTTP_response_ram( resp, "text/plain", 200, buf, strlen(buf) + 1 );
      __sync_add_and_fetch( &num_gets_finished, 1 );
      return resp;
   }

   // if this is a query for owner <--> username/password mappings, then simply return that
   if( strcmp( path, "USERS" ) == 0 ) {
      if( passwd_buf ) {
         pthread_mutex_lock( &reload_lock );
         md_create_HTTP_response_ram( resp, "text/plain", 200, passwd_buf, passwd_buf_len + 1 );
         pthread_mutex_unlock( &reload_lock );
      }
      else {
         md_create_HTTP_response_ram_static( resp, "text/plain", 200, MD_HTTP_NOMSG, strlen(MD_HTTP_NOMSG) + 1 );
      }
      __sync_add_and_fetch( &num_gets_finished, 1 );
      return resp;
   }

   // attempt to read metadata state for the path at client_md_path
   char client_md_path[PATH_MAX];
   md_fullpath( conf->master_copy_root, path, client_md_path );
   dbprintf( "get metadata in %s\n", client_md_path );
   

   // check cache

   int rc = 0;
   
   /*
   char* serialized_response = NULL;
   uid_t uid;
   size_t len = 0;

   if( md_con_data->user )
      uid = md_con_data->user->uid;
   else
      uid = MD_GUEST_UID;

   int rc = cache_get( url, &serialized_response, &len, uid );
   if( rc == 0 ) {
      // just serve this
      md_create_HTTP_response_ram_nocopy( resp, "text/plain", 200, serialized_response, len );
      add_last_mod_header( resp, get_last_post_time() );
      __sync_add_and_fetch( &num_gets_finished, 1 );
      return resp;
   }
   */
   
   // cache miss
   struct md_entry parent;
   memset( &parent, 0, sizeof(parent) );
   rc = md_read_entry( conf->master_copy_root, path, &parent );

   if( rc != 0 ) {
      // does not exist
      errorf( "failed to read %s, rc = %d\n", path, rc );
      md_create_HTTP_response_ram_static( resp, "text/plain", 404, MD_HTTP_404_MSG, strlen(MD_HTTP_404_MSG) + 1 );
      __sync_add_and_fetch( &num_gets_finished, 1 );
      return resp;
   }

   if( MD_ENTRY_ISDIR( parent ) ) {
      // GET on a directory 

      struct HTTP_dir_serialization_data dir_data;
      dir_data.user = md_con_data->user;
      dir_data.conf = http->conf;

      char* parent_txt = md_serialize_entry( &parent, NULL );
      strcat( parent_txt, "\n" );

      dir_data.rb.push_back( buffer_segment_t(parent_txt, strlen(parent_txt)) );
      struct md_entry** ents = md_walk_fs_dir( client_md_path, false, true, NULL, NULL, md_process, &dir_data );

      if( ents == NULL ) {
         md_entry_free( &parent );
         response_buffer_free( &dir_data.rb );
         md_create_HTTP_response_ram_static( resp, "text/plain", 404, MD_HTTP_404_MSG, strlen(MD_HTTP_404_MSG) + 1 );
         __sync_add_and_fetch( &num_gets_finished, 1 );
         return resp;
      }

      dir_data.rb.push_back( buffer_segment_t(strdup("\0"), 1) );
      
      char* all_txt = response_buffer_to_string( &dir_data.rb );
      size_t all_txt_len = response_buffer_size( &dir_data.rb );

      char* reply_txt = CALLOC_LIST( char, all_txt_len );
      memcpy( reply_txt, all_txt, all_txt_len );
      
      // cache this
      // cache_put( url, all_txt, all_txt_len, parent.owner, parent.mode );;

      md_entry_free( &parent );

      md_create_HTTP_response_ram_nocopy( resp, "text/plain", MHD_HTTP_OK, reply_txt, all_txt_len );
      
      response_buffer_free( &dir_data.rb );

      add_last_mod_header( resp, get_last_post_time() );

      for( int i = 0; ents[i] != NULL; i++ ) {
         md_entry_free( ents[i] );
         free( ents[i] );
      }
      free( ents );

      __sync_add_and_fetch( &num_gets_finished, 1 );
      return resp;
   }
   else {
      struct md_entry* parent_list[] = { &parent, NULL };
      char* buf = md_serialize( parent_list, '\n' );
      size_t buf_len = strlen(buf);

      char* reply_txt = CALLOC_LIST( char, buf_len );
      memcpy( reply_txt, buf, buf_len );

      // cache this
      // cache_put( url, buf, buf_len, parent.owner, parent.mode );
      
      md_create_HTTP_response_ram_nocopy( resp, "text/plain", MHD_HTTP_OK, reply_txt, buf_len );

      add_last_mod_header( resp, get_last_post_time() );

      md_entry_free( &parent );
      
      __sync_add_and_fetch( &num_gets_finished, 1 );
      return resp;
   }
}


// validate posted metadata.
// if this function returns successfully, it will be acceptable to add ent to the master copy
static int validate_md_update( struct md_HTTP_connection_data* md_con_data, struct md_update* e, struct md_user_entry* uent ) {
   
   // enforce UID
   e->ent.owner = uent->uid;
   
   // is this a user of this metadata server?
   bool is_member = (md_find_user_entry2( uent->uid, g_userlist ) != NULL);
   
   // read the MC entry that may be overwritten
   struct md_entry existing;
   memset( &existing, 0, sizeof(existing) );
   int rc = md_read_entry( md_con_data->http->conf->master_copy_root, e->ent.path, &existing );
   if( rc == 0 ) {
      // existing data!
      // make sure we're allowed to overwrite it
      if( e->ent.owner != uent->uid && !((is_member && (existing.mode & S_IWGRP)) || (!is_member && (existing.mode & S_IWOTH))) ) {
         // can't write here
         errorf( " insufficient permissions to overwrite %s\n", e->ent.path);
         rc = -EACCES;
      }
      
      // make sure we're not changing permissions if we don't own the file
      if( rc == 0 && existing.owner != uent->uid && e->ent.mode != e->ent.mode ) {
         // can't change access permissions unless we own the file
         errorf( "insufficient permissions to change mode on %s\n", e->ent.path);
         rc = -EPERM;
      }
      
      // ownership can't change
      if( rc == 0 && e->ent.owner != uent->uid && e->ent.owner != existing.owner ) {
         errorf( "cannot change owner from %d to %d for %s\n", existing.owner, e->ent.owner, e->ent.path);
         rc = -EACCES;
      }
      
      // if the version of this data is before the version of existing data, then ignore it.
      if( rc == 0 && existing.version > e->ent.version ) {
         errorf( "current record for %s is more recent\n", e->ent.path);
         rc = -EAGAIN;
      }

      // if we're updating and the timestamp is too old, then reject
      if( rc == 0 && e->op == MD_OP_UP && (existing.mtime_sec > e->ent.mtime_sec || (existing.mtime_sec == e->ent.mtime_sec && existing.mtime_nsec > e->ent.mtime_nsec)) ) {
         errorf( "current record has a newer timestamp (%lld.%lld > %lld.%lld)\n", existing.mtime_sec, existing.mtime_nsec, e->ent.mtime_sec, e->ent.mtime_nsec );
         rc = -ESTALE;
      }

      // if all we're doing is updating/verifying an entry, the primary URL must be the same
      if( rc == 0 && (e->op == MD_OP_UP || e->op == MD_OP_VER) && strcmp( e->ent.url, existing.url ) != 0 ) {
         int nrc = 0;
         char* normal_url = md_normalize_url( existing.url, &nrc );
         if( nrc != 0 ) {
            errorf("md_normalize_url(%s) rc = %d\n", existing.url, nrc );
            rc = -EIO;
         }

         else {
            char* new_normal_url = md_normalize_url( e->ent.url, &nrc );
            if( nrc != 0 ) {
               errorf("md_normalize_url(%s) rc = %d\n", e->ent.url, nrc );
               rc = -EINVAL;
               
               free( normal_url );
            }
            else {
               if( strcmp(normal_url, new_normal_url) != 0 ) {
                  errorf( "invalid primary URL for %s (current URL is %s, given URL is %s)\n", e->ent.path, existing.url, e->ent.url );
                  rc = -EINVAL;
               }

               free( normal_url );
               free( new_normal_url );
            }
         }
      }

      if( rc == 0 && e->op == MD_OP_VER ) {
         dbprintf( "validated permissions for %s\n", e->ent.path );
         rc = 1;
      }
      
      // valid!
      md_entry_free( &existing );
   }
   else {
      // data is new
      errorf( "failed to read %s, rc = %d.  It must be new\n", e->ent.path, rc );
      rc = 0;
   }
   
   return rc;
}


// commit a metadata update to disk
static int commit_md_update( struct md_syndicate_conf* conf, struct md_update* e ) {
   int rc = 0;
   switch( e->op ) {
      case MD_OP_ADD: {
         // add/overwrite a metadata entry
         rc = md_add_mc_entry( conf->master_copy_root, &e->ent );
         
         if( rc != 0 ) {
            errorf( "failed to add/replace %s, rc = %d\n", e->ent.path, rc );
         }
         else {
            // cache_clear( e->ent.path );
            dbprintf( "added/replaced %s\n", e->ent.path );
         }
         
         break;
      }
      
      case MD_OP_RM: {
         // remove a metadata entry
         rc = md_remove_mc_entry( conf->master_copy_root, &e->ent );
         
         if( rc != 0 ) {
            errorf( "failed to remove %s\n", e->ent.path );
         }
         else {
            // cache_clear( e->ent.path );
            dbprintf( "removed %s\n", e->ent.path );
         }
         
         break;
      }
      
      case MD_OP_UP: {
         // update an existing metadata entry
         
         // prevent another concurrent update on this path
         md_global_lock_path( conf->master_copy_root, e->ent.path );
         
         struct md_entry curr;
         memset( &curr, 0, sizeof(curr) );
         
         FILE* fent = md_open_entry( conf->master_copy_root, e->ent.path, "r+" );
         if( fent == NULL ) {
            errorf( "failed to update %s, rc = %d\n", e->ent.path, -errno );
         }
         else {
            rc = md_read_entry3( fent, &curr );
            if( rc != 0 ) {
               errorf( "failed to read %s, rc = %d\n", e->ent.path, rc );
            }
            else {
               // merge the replica URLs
               size_t num_replicas = 0;
               SIZE_LIST( &num_replicas, e->ent.url_replicas );
               
               char** new_replicas = CALLOC_LIST( char*, num_replicas + 1 );
               rc = md_normalize_urls( e->ent.url_replicas, new_replicas );

               if( rc != 0 ) {
                  errorf("md_normalize_urls(%s) rc = %d\n", e->ent.path, rc );
               }
               else {

                  char* new_url = md_normalize_url( e->ent.url, &rc );

                  if( rc != 0 ) {
                     errorf("md_normalize_url(%s) rc = %d\n", e->ent.path, rc );
                     FREE_LIST( new_replicas );
                  }

                  else {
                     // commit the changes
                     memcpy( &curr, &e->ent, sizeof(struct md_entry) );

                     free( curr.url );
                     FREE_LIST( curr.url_replicas );
                     
                     curr.url = new_url;
                     curr.url_replicas = new_replicas;
                     
                     fseek( fent, 0L, SEEK_SET );
                     ftruncate( fileno(fent), 0 );
                     rc = md_write_entry( fent, &curr );
                     if( rc != 0 ) {
                        errorf( "could not commit %s, rc = %d\n", e->ent.path, rc );
                     }
                  }
               }
               
               md_entry_free( &curr );
            }
            
            fclose( fent );
         }

         // cache_clear( e->ent.path );
 
         // unlock this path
         md_global_unlock_path( conf->master_copy_root, e->ent.path );
         break;
      }
      
      case MD_OP_VER: {
         // nothing to do
         break;
      }
      
      default: {
         dbprintf( "will do nothing for %s\n", e->ent.path );
         rc = -EINVAL;
         break;
      }
   }
   
   return rc;
}


// POST metadata handler
// entries posted are expected to have metadata timestamps and last-modification times in UTC
int HTTP_POST_iterator(void *coninfo_cls, enum MHD_ValueKind kind, 
                       const char *key,
                       const char *filename, const char *content_type,
                       const char *transfer_encoding, const char *data, 
                       uint64_t off, size_t size) {
   

   struct md_HTTP_connection_data* md_con_data = (struct md_HTTP_connection_data*)coninfo_cls;
   struct HTTP_connection_data *con_data = (struct HTTP_connection_data*)md_con_data->cls;
   
   struct md_user_entry *user = con_data->user;
   
   dbprintf( "POST size = %d, off = %lld, data = '%s'\n", size, off, data );
   
   int rc = 0;
   
   if( size > 0 && md_con_data->status > 0 ) {
      
      int line_cnt = 0;
      size_t i = 0;
      char* line = (char*)data;
      
      while( i < size ) {
         bool has_line = false;
         
         // copy data into the line buffer
         while( i < size && line[i] != '\n' && con_data->line_offset < MD_MAX_LINE_LEN ) {
            con_data->line_buf[con_data->line_offset] = line[i];
            i++;
            con_data->line_offset++;
         }
         
         if( i >= size ) {
            // end of data
            // did we get a full line?
            if( i > 0 && con_data->line_buf[i-1] == '\n' ) {
               // this is a full line
               has_line = true;
            }
         }
         else if( con_data->line_offset >= MD_MAX_LINE_LEN ) {
            // overflow?
            if( i > 0 && con_data->line_buf[i-1] == '\n' ) {
               // the line just barely fit
               has_line = true;
            }
            else {
               // the data is malformed
               dbprintf( "metadata overflow at line %d\n", line_cnt );
               md_con_data->status = -1;
               con_data->error = -EOVERFLOW;
               break;
            }
         }
         else {
            // reached new-line, so we need to process our buffered data
            has_line = true;
            i++;
         }
         
         if( has_line ) {
            // got our data; stop processing
            md_con_data->status = -1;

            // parse into entry
            rc = md_extract_update( con_data->line_buf, &con_data->update );
            
            if( rc != 0 ) {
               errorf( "md_extract_update rc = %d (buf = '%s')\n", rc, con_data->line_buf);
               md_con_data->status = -1;
               con_data->error = rc;
               return -1;  // stop processing
            }
            
            // do validation
            struct timeval t;
            gettimeofday(&t, NULL);
            dbprintf( "%lf: post\n", (double)t.tv_sec + (double)t.tv_usec / (double)1000000.0);
            
            rc = validate_md_update( md_con_data, &con_data->update, user );
            if( rc < 0 ) {
               // invalid
               con_data->error = rc;
               md_update_free( &con_data->update );
            }
         }
      }
   }

   return MHD_YES;
}


// finish posting--commit changes to disk
void HTTP_POST_finish( struct md_HTTP_connection_data* md_con_data ) {
   num_posts++;

   md_con_data->resp = CALLOC_LIST( struct md_HTTP_response, 1 );
   
   // metadata updates are valid; commit them!
   struct HTTP_connection_data* con_data = (struct HTTP_connection_data*)md_con_data->cls;

   if( con_data->error == 0 ) {
      // successfully got an update
      int rc = commit_md_update( md_con_data->conf, &con_data->update );
      if( rc != 0 ) {
         errorf( "failed to write %s, rc = %d\n", con_data->update.ent.path, rc );
         con_data->error = rc;
      }
   }

   if( con_data->error != 0 ) {
      // return this error
      char buf[10];
      sprintf(buf, "%d", con_data->error );
      md_create_HTTP_response_ram( md_con_data->resp, "text/plain", 202, buf, strlen(buf) + 1 );
   }
   else {
      // return success
      md_create_HTTP_response_ram_static( md_con_data->resp, "text/plain", 200, MD_HTTP_200_MSG, strlen(MD_HTTP_200_MSG) + 1 );
   }
   

   set_last_post_time( currentTimeSeconds() );
}

// clean up
void HTTP_cleanup( struct MHD_Connection *connection, void *user_cls, enum MHD_RequestTerminationCode term) {

   struct HTTP_connection_data *con_data = (struct HTTP_connection_data*)(user_cls);
   if( con_data == NULL )
      return;
   
   // free memory
   if( con_data->line_buf ) {
      free( con_data->line_buf );
   }

   md_update_free( &con_data->update );
   delete con_data;
}


// cache the user secrets
static int cache_user_secrets( struct md_user_entry** users ) {
   // cache user secrets list
   size_t len = 0;
   for( int i = 0; users[i] != NULL; i++ ) {
      len += 12 + strlen(users[i]->username) + 1 + strlen(users[i]->password_hash) + 1;
   }

   passwd_buf = CALLOC_LIST( char, len + 1 );
   uint64_t off = 0;

   for( int i = 0; users[i] != NULL; i++ ) {
      off += sprintf(passwd_buf + off, "%d:%s:%s\n", users[i]->uid, users[i]->username, users[i]->password_hash );
   }

   passwd_buf_len = strlen(passwd_buf);
   return 0;
}


// cache the replica servers
static int cache_replica_urls( char** replica_urls ) {
   // build and cache response
   size_t sz = 0;
   if( replica_urls != NULL ) {
      for( int i = 0; replica_urls[i] != NULL; i++ ) {
         sz += strlen(replica_urls[i]) + 2;
      }

      replica_buf = CALLOC_LIST( char, sz );
      for( int i = 0; replica_urls[i] != NULL; i++ ) {
         strcat( replica_buf, replica_urls[i] );
         strcat( replica_buf, "\n");
      }
   }
   else {
      replica_buf = strdup("\n");
   }
   replica_buf_len = strlen(replica_buf);
   
   return 0;
}


// initialize the HTTP module
int http_init( struct md_HTTP* http, struct md_syndicate_conf* conf, struct md_user_entry** users ) {

   // cache_init();
   
   md_HTTP_init( http, MHD_USE_SELECT_INTERNALLY | MHD_USE_POLL | MHD_USE_DEBUG, conf, users );
   
   md_HTTP_auth_mode( *http, conf->http_authentication_mode );
   md_HTTP_connect( *http, HTTP_connect );
   md_HTTP_GET( *http, HTTP_GET_handler );
   md_HTTP_HEAD( *http, NULL );
   md_HTTP_POST_iterator( *http, HTTP_POST_iterator );
   md_HTTP_POST_finish( *http, HTTP_POST_finish );
   md_HTTP_close( *http, HTTP_cleanup );

   // load in the users and replica servers
   http_reload( http->users, conf->replica_urls );

   g_userlist = http->users;
   g_replica_urls = conf->replica_urls;
      

   int rc = md_start_HTTP( http, conf->portnum );
   if( rc != 0 ) {
      errorf("ERR: rc = %d when starting HTTP thread\n", rc );
   }

   else {

      g_running = true;
      dbprintf("%s", "Start up HTTP\n");
   }
   
   //bw_reporter_thread = init_thread( bw_reporter );
   return rc;
}


// shut down the HTTP module
int http_shutdown( struct md_HTTP* http ) {
   md_stop_HTTP( http );
   g_running = false;
   // cache_shutdown();
   //pthread_cancel( bw_reporter_thread );
   dbprintf("%s", "Shut down HTTP\n");
   return 0;
}


// re-cache user list 
void http_reload( struct md_user_entry** users, char** replica_urls ) {
   pthread_mutex_lock( &reload_lock );

   free( passwd_buf );
   free( replica_buf );
   
   cache_user_secrets( users );
   cache_replica_urls( replica_urls );
   
   pthread_mutex_unlock( &reload_lock );
}
