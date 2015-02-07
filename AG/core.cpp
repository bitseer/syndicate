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


#include "core.h"
#include "cache.h"
#include "http.h"
#include "driver.h"
#include "events.h"
#include "publish.h"
#include "map-parser-xml.h"
#include "workqueue.h"

static struct AG_opts g_AG_opts;
static struct AG_state global_state;

// ref the global state
struct AG_state* AG_get_state() {
   if( !global_state.referenceable )
      return NULL;
   
   pthread_rwlock_rdlock( &global_state.state_lock );
   return &global_state;
}

// unref the global state
void AG_release_state( struct AG_state* state ) {
   pthread_rwlock_unlock( &state->state_lock );
}

// signal handler to handle dying
void AG_death_signal_handler( int signum ) {
   
   struct AG_state* state = &global_state;
   if( state->referenceable ) {
      // tell the main loop to proceed to shut down
      sem_post( &state->running_sem );
   }
}


// read-lock access to the AG_fs structure within the state
int AG_state_fs_rlock( struct AG_state* state ) {
   int rc = pthread_rwlock_rdlock( &state->fs_lock );
   if( rc != 0 ) {
      SG_error("pthread_rwlock_rdlock(AG_state %p) rc = %d\n", state, rc );
   }
   return rc;
}

// write-lock access to the AG_fs structure within the state
int AG_state_fs_wlock( struct AG_state* state ) {
   int rc = pthread_rwlock_wrlock( &state->fs_lock );
   if( rc != 0 ) {
      SG_error("pthread_rwlock_wrlock(AG_state %p) rc = %d\n", state, rc );
   }
   return rc;
}

// unlock access to the AG_fs structure within the state
int AG_state_fs_unlock( struct AG_state* state ) {
   int rc = pthread_rwlock_unlock( &state->fs_lock );
   if( rc != 0 ) {
      SG_error("pthread_rwlock_unlock(AG_state %p) rc = %d\n", state, rc );
   }
   return rc;
}

// read-lock the AG_config structure within the state
int AG_state_config_rlock( struct AG_state* state ) {
   int rc = pthread_rwlock_rdlock( &state->config_lock );
   if( rc != 0 ) {
      SG_error("pthread_rwlock_rdlock(AG_state %p) rc = %d\n", state, rc );
   }
   return rc;
}

// write-lock the AG_config structure within the state
int AG_state_config_wlock( struct AG_state* state ) {
   int rc = pthread_rwlock_wrlock( &state->config_lock );
   if( rc != 0 ) {
      SG_error("pthread_rwlock_wrlock(AG_state %p) rc = %d\n", state, rc );
   }
   return rc;
}

// unlock the AG_config structure within the state
int AG_state_config_unlock( struct AG_state* state ) {
   int rc = pthread_rwlock_unlock( &state->config_lock );
   if( rc != 0 ) {
      SG_error("pthread_rwlock_unlock(AG_state %p) rc = %d\n", state, rc );
   }
   return rc;
}

// get the specfile from the AG's cert (given by the MS)
int AG_get_spec_file_text_from_MS( struct ms_client* client, char** out_specfile_text, size_t* out_specfile_text_len ) {
   
   // this will be embedded in the AG's driver text as a base64-encoded string.
   int rc = 0;
   char* specfile_text_json = NULL;
   size_t specfile_text_json_len = 0;
   
   char* specfile_text = NULL;
   size_t specfile_text_len = 0;
   
   // get the json
   rc = ms_client_get_closure_text( client, &specfile_text_json, &specfile_text_json_len );
   if( rc != 0 ) {
      SG_error("ms_client_get_closure_text rc = %d\n", rc );
      return rc;
   }
   
   // extract from json 
   rc = md_closure_load_AG_specfile( specfile_text_json, specfile_text_json_len, &specfile_text, &specfile_text_len );
   free( specfile_text_json );
   
   if( rc != 0 ) {
      SG_error("md_closure_load_AG_specfile rc = %d\n", rc );
      
      return rc;
   }
   
   // it came from the MS, so it's definitely compressed 
   char* decompressed_text = NULL;
   size_t decompressed_text_len = AG_MAX_SPECFILE_SIZE;
   
   int zrc = md_inflate( specfile_text, specfile_text_len, &decompressed_text, &decompressed_text_len );
   
   free( specfile_text );
   
   if( zrc != 0 ) {
      SG_error("md_inflate(%zu bytes) rc = %d\n", specfile_text_len, zrc );
      
      return zrc;
   }
   
   *out_specfile_text = decompressed_text;
   *out_specfile_text_len = decompressed_text_len;
   
   return rc;
}

// get the specfile text, either from the MS certificate, or from an opt-defined location on disk.
// this does NOT try to decompress it
int AG_load_spec_file_text( struct AG_state* state, char** specfile_text, size_t* specfile_text_len ) {
   
   int rc = 0;
   
   if( state->ag_opts.spec_file_path != NULL ) {
      
      // read from disk
      off_t txt_len = 0;
      char* txt = md_load_file( state->ag_opts.spec_file_path, &txt_len );
      
      if( txt == NULL ) {
         SG_error("Failed to load spec file text from %s, rc = %d\n", state->ag_opts.spec_file_path, (int)txt_len );
         rc = -ENODATA;
      }
      else {
         SG_debug("Loaded %zu-byte specfile from %s\n", txt_len, state->ag_opts.spec_file_path );
         
         *specfile_text = txt;
         *specfile_text_len = txt_len;
         return 0;
      }
   }
   else {
      // read from MS 
      rc = AG_get_spec_file_text_from_MS( state->ms, specfile_text, specfile_text_len );
      
      if( rc != 0 ) {
         SG_error("AG_get_spec_file_text_from_MS rc = %d\n", rc );
      }
      else {
         SG_debug("Loaded %zu-byte specfile from the MS\n", *specfile_text_len );
      }
   }
   
   if( rc != 0 ) {
      // can't reload--didn't get the text
      SG_error("Failed to get spec file text, rc = %d\n", rc );
   }
   
   return rc;
}


// generate the latest fs_map and config from the specfile 
// NOTE: state only needs .ms and .ag_opts to be initialized for this method to work.
int AG_reload_specfile( struct AG_state* state, AG_fs_map_t** new_fs, AG_config_t** new_config ) {
   
   SG_debug("%s", "Reloading AG spec file...\n");
   
   int rc = 0;
   char* new_specfile_text = NULL;
   size_t new_specfile_text_len = 0;
   
   // get the text 
   rc = AG_load_spec_file_text( state, &new_specfile_text, &new_specfile_text_len );
   if( rc != 0 ) {
      SG_error("AG_load_spec_file_text rc = %d\n", rc );
      return rc;
   }
   
   // try to parse the text 
   rc = AG_parse_spec( state, new_specfile_text, new_specfile_text_len, new_fs, new_config );
   if( rc != 0 ) {
      SG_error("AG_parse_spec rc = %d\n", rc );
   }
   
   free( new_specfile_text );
   
   return rc;
}


// synchronize the MS's copy of the AG's filesystem with what the AG itself has.
// that is, delete from the MS anything that is in old_fs exclusively, and publish to the 
// MS anything that is in new_fs exclusively.  Update entries that are in the intersection.
// neither AG_fs must be locked.
// This does not affect cache validation flags or driver pointers of either FS
int AG_resync( struct AG_state* state, struct AG_fs* old_fs, struct AG_fs* new_fs, AG_map_info_equality_func_t mi_equ, bool force_refresh ) {
   
   int rc = 0;
   
   AG_fs_map_t to_delete;
   AG_fs_map_t to_publish;
   AG_fs_map_t to_update;
   AG_fs_map_t to_remain;
   
   // find the difference between old_fs's contents and new_fs's contents
   AG_fs_rlock( old_fs );
   AG_fs_rlock( new_fs );
   
   rc = AG_fs_map_transforms( old_fs->fs, new_fs->fs, &to_publish, &to_remain, &to_update, &to_delete, mi_equ );
   
   AG_fs_unlock( new_fs );
   AG_fs_unlock( old_fs );
   
   if( rc != 0 ) {
      SG_error("AG_fs_map_transforms rc = %d\n", rc );
      return rc;
   }
   
   // add metadata for the ones to publish
   rc = AG_fs_publish_generate_metadata( &to_publish );
   if( rc != 0 ) {
      SG_error("AG_fs_publish_generate_metadata rc = %d\n", rc );
      return rc;
   }
   
   SG_debug("%s", "To publish:\n");
   AG_dump_fs_map( &to_publish );
   
   SG_debug("%s", "To remain:\n");
   AG_dump_fs_map( &to_remain );
   
   SG_debug("%s", "To update:\n");
   AG_dump_fs_map( &to_update );
   
   SG_debug("%s", "To delete:\n");
   AG_dump_fs_map( &to_delete );
   
   // apply our changes to it.
   // add new entries, and delete old ones.
   int publish_rc = AG_fs_publish_all( new_fs->ms, old_fs->fs, &to_publish );
   
   if( publish_rc != 0 ) {
      SG_error("ERR: AG_fs_publish_all rc = %d\n", publish_rc );
      
      return publish_rc;
   }
   
   int update_rc = AG_fs_update_all( new_fs->ms, old_fs->fs, &to_update );
   if( update_rc != 0 ) {
      SG_error("ERR: AG_fs_update_all rc = %d\n", update_rc );
      
      return update_rc;
   }
   
   if( force_refresh ) {
      
      SG_debug("Forcing refresh of %zu fresh entries\n", to_remain.size() );
      
      // refresh fresh entries 
      int fresh_rc = AG_fs_update_all( new_fs->ms, old_fs->fs, &to_remain );
      
      if( fresh_rc != 0 ) {
         SG_error("ERR: AG_fs_update_all rc = %d\n", fresh_rc );
         
         return fresh_rc;
      }
   }
   
   int delete_rc = AG_fs_delete_all( new_fs->ms, new_fs->fs, &to_delete );
   if( delete_rc != 0 ) {
      SG_error("ERR: AG_fs_delete_all rc = %d\n", delete_rc );
      
      return delete_rc;
   }
   
   return 0;
}

// get the latest specfile, and use it to publish new entries and withdraw now-old entries
int AG_reload( struct AG_state* state ) {
   
   int rc = 0;
   
   AG_fs_map_t* new_fs = NULL;
   AG_config_t* new_config = NULL;
   
   SG_debug("%s", "Begin reload state\n");
   
   // get the new fs data 
   rc = AG_reload_specfile( state, &new_fs, &new_config );
   if( rc != 0 ) {
      SG_error("AG_reload_specfile rc = %d\n", rc );
      return rc;
   }
   
   // verify its integrity 
   rc = AG_validate_map_info( new_fs );
   if( rc != 0 ) {
      SG_error("AG_validate_map_info rc = %d\n", rc );
      
      AG_fs_map_free( new_fs );
      
      delete new_fs;
      delete new_config;
      return rc;
   }
   
   // wrap the new mapping into an AG_fs
   struct AG_fs* fs_clone = SG_CALLOC( struct AG_fs, 1 );
   
   // clone the fs (but prevent another thread from replacing state->ag_fs)
   AG_state_fs_rlock( state );
   AG_fs_rlock( state->ag_fs );
   
   rc = AG_fs_init( fs_clone, new_fs, state->ms );
   
   if( rc == 0 ) {
      // copy all cached data from the current fs to the new fs (from the spec file),
      // since the current fs is coherent but the specfile-loaded mapping is not 
      AG_fs_copy_cached_data( fs_clone, state->ag_fs, AG_map_info_copy_MS_data );
      AG_fs_copy_cached_data( fs_clone, state->ag_fs, AG_map_info_copy_driver_data );
      AG_fs_copy_cached_data( fs_clone, state->ag_fs, AG_map_info_copy_AG_data );
   }
   
   AG_fs_unlock( state->ag_fs );
   AG_state_fs_unlock( state );
   
   if( rc != 0 ) {
      SG_error("AG_fs_map_dup rc = %d\n", rc );
      
      AG_fs_free( fs_clone );   // NOTE: frees new_fs 
      free( fs_clone );
      
      delete new_config;
      
      return rc;
   }
   
   
   // for reloading, an element is fresh if it has the same AG-specific metadata 
   struct AG_fresh_comparator {
      static bool equ( struct AG_map_info* mi1, struct AG_map_info* mi2 ) {
         
         bool streq = true;
         if( mi1->query_string != NULL && mi2->query_string != NULL ) {
            streq = (strcmp( mi1->query_string, mi2->query_string ) == 0);
         }
         
         return (mi1->driver    == mi2->driver    &&
                 mi1->file_perm == mi2->file_perm &&
                 mi1->reval_sec == mi2->reval_sec &&
                 mi1->type      == mi2->type      &&
                 streq );
      }
   };
   
   // Evolve the current AG_fs into the one described by the specfile.
   rc = AG_resync( state, state->ag_fs, fs_clone, AG_fresh_comparator::equ, false );
   if( rc != 0 ) {
      SG_error("WARN: AG_resync rc = %d\n", rc );
      rc = 0;
   }
   
   // make the newly-loaded AG_fs the current AG_fs
   AG_state_fs_wlock( state );
   
   struct AG_fs* old_fs = state->ag_fs;
   
   // stop all other accesses to this old fs
   AG_fs_wlock( old_fs );
   
   
   AG_fs_rlock( fs_clone );
   
   // swap in the new one
   state->ag_fs = fs_clone;
   
   AG_fs_unlock( fs_clone );
   
   AG_state_fs_unlock( state );
   
   // swap the new config in 
   AG_state_config_wlock( state );
   
   AG_config_t* old_config = state->config;
   state->config = new_config;
   
   AG_state_config_unlock( state );
   
   // delete the old fs, all of its entries, and config
   AG_fs_unlock( old_fs );
   AG_fs_free( old_fs );
   free( old_fs );
   
   delete old_config;
   
   SG_debug("%s", "End reload state\n");
   return 0;
}


// view-change reload thread (triggerred in response to volume change)
void* AG_reload_thread_main( void* arg ) {
   
   struct AG_state* state = (struct AG_state*)arg;
   
   SG_debug("%s\n", "Starting specfile reload thread\n");
   
   while( state->specfile_reload_thread_running ) {
      
      // wait to reload...
      sem_wait( &state->specfile_reload_sem );
      
      // were we simply told to exit?
      if( !state->specfile_reload_thread_running ) {
         break;
      }
      
      // do the reload 
      AG_reload( state );
   }
   
   
   SG_debug("%s\n", "Specfile reload thread exit\n");
   return NULL;
}


// view-change callback for the volume.
// just wake up the reload thread 
int AG_config_change_callback( struct ms_client* ms, void* arg ) {
   
   struct AG_state* state = (struct AG_state*)arg;
   
   sem_post( &state->specfile_reload_sem );
   
   return 0;
}


// terminate on command--send ourselves a SIGTERM
int AG_event_handler_terminate( char* event_payload, void* unused ) {
   
   SG_debug("%s\n", "EVENT: Terminate\n");
   
   pid_t my_pid = getpid();
   
   // tell ourselves to die 
   return kill( my_pid, SIGTERM );
}

// pass an event to the driver 
int AG_event_handler_driver_ioctl( char* event_payload, void* unused ) {
   
   SG_debug("%s\n", "EVENT: Driver ioctl\n");
   
   struct AG_state* state = AG_get_state();
   if( state == NULL ) {
      // nothing to do 
      return -ENOTCONN;
   }
   
   char* query_type = NULL;
   char* payload = NULL;
   size_t payload_len = 0;
   
   int rc = 0;
   
   // parse the payload 
   rc = AG_parse_driver_ioctl( event_payload, &query_type, &payload, &payload_len );
   if( rc != 0 ) {
      SG_error("AG_parse_driver_ioctl rc = %d\n", rc );
      return rc;
   }
   
   struct AG_driver* driver = AG_lookup_driver( state->drivers, query_type );
   if( driver == NULL ) {
      SG_error("No such driver '%s'\n", query_type );
      return -EPERM;
   }
   
   // call the driver's event handler 
   rc = AG_driver_handle_event( driver, payload, payload_len );
   if( rc != 0 ) {
      SG_error("AG_driver_handle_event( driver = '%s' ) rc = %d\n", query_type, rc );
   }
   
   return rc;
}


// state initialization 
// NOTE: ag_opts will get shallow-copied
// if this method fails, the caller should call AG_state_free on the given state
int AG_state_init( struct AG_state* state, struct md_opts* opts, struct AG_opts* ag_opts, struct md_syndicate_conf* conf, struct ms_client* client ) {
   
   int rc = 0;
   AG_fs_map_t* parsed_map = NULL;
   
   memset( state, 0, sizeof(struct AG_state));
   
   // basic init 
   state->ms = client;
   state->conf = conf;
   
   sem_init( &state->specfile_reload_sem, 0, 0 );
   sem_init( &state->running_sem, 0, 0 );
   
   memcpy( &state->ag_opts, ag_opts, sizeof(struct AG_opts) );
   
   pthread_rwlock_init( &state->fs_lock, NULL );
   pthread_rwlock_init( &state->state_lock, NULL );
   pthread_rwlock_init( &state->config_lock, NULL );
   
   // make the instance nonce
   char* tmp = SG_CALLOC( char, 16 );
   rc = md_read_urandom( tmp, 16 );
   if( rc != 0 ) {
      SG_error("md_read_urandom rc = %d\n", rc );
      free( tmp );
      return rc;
   }
   
   rc = md_base64_encode( tmp, 16, &state->inst_nonce );
   free( tmp );
   
   if( rc != 0 ) {
      SG_error("md_base64_encode rc = %d\n", rc );
      return rc;
   }
   
   SG_debug("Initializing AG instance %s\n", state->inst_nonce );
   
   // initialize drivers 
   state->drivers = new AG_driver_map_t();
   
   rc = AG_load_drivers( state->conf, state->drivers, state->ag_opts.driver_dir );
   if( rc != 0 ) {
      SG_error("AG_load_drivers(%s) rc = %d\n", state->ag_opts.driver_dir, rc );
      return rc;
   }
   
   // initialize the path-mapping 
   state->ag_fs = SG_CALLOC( struct AG_fs, 1 );

   // get the new FS mapping and config 
   rc = AG_reload_specfile( state, &parsed_map, &state->config );
   if( rc != 0 ) {
      SG_error("AG_reload_specfile rc = %d\n", rc );
      return rc;
   }
   
   // verify its integrity 
   rc = AG_validate_map_info( parsed_map );
   if( rc != 0 ) {
      SG_error("AG_validate_map_info rc = %d\n", rc );
      
      AG_fs_map_free( parsed_map );
      delete parsed_map;
      
      return rc;
   }
   
   // pass in the newly-parsed FS map into the AG_fs (which takes ownership)
   rc = AG_fs_init( state->ag_fs, parsed_map, state->ms );
   
   if( rc != 0 ) {
      SG_error("AG_fs_init rc = %d\n", rc );
      
      AG_fs_map_free( parsed_map );
      delete parsed_map;
      
      return rc;
   }
   
   // initialize HTTP 
   state->http = SG_CALLOC( struct md_HTTP, 1 );
   
   rc = AG_http_init( state->http, state->conf );
   if( rc != 0 ) {
      SG_error("AG_http_init rc = %d\n", rc );
      return rc;
   }
   
   // initialize event listener 
   state->event_listener = SG_CALLOC( struct AG_event_listener, 1 );
   
   rc = AG_event_listener_init( state->event_listener, ag_opts );
   if( rc != 0 ) {
      SG_error("AG_event_listener_init rc = %d\n", rc );
      return rc;
   }
   
   // initialize reversioner 
   state->wq = SG_CALLOC( struct md_wq, 1 );
   
   rc = md_wq_init( state->wq, state );
   if( rc != 0 ) {
      SG_error("md_wq_init rc = %d\n", rc );
      return rc;
   }
   
   // set up block cache 
   state->cache = SG_CALLOC( struct md_syndicate_cache, 1 );
   
   if( opts->cache_hard_limit == 0 ) {
      opts->cache_hard_limit = AG_CACHE_DEFAULT_HARD_LIMIT;
   }
   
   if( opts->cache_soft_limit == 0 ) {
      opts->cache_soft_limit = (AG_CACHE_DEFAULT_SOFT_LIMIT < opts->cache_hard_limit ? AG_CACHE_DEFAULT_SOFT_LIMIT : opts->cache_hard_limit );
   }
   
   uint64_t block_size = ms_client_get_volume_blocksize( client );
   rc = md_cache_init( state->cache, conf, opts->cache_soft_limit / block_size, opts->cache_hard_limit / block_size );
   
   if( rc != 0 ) {
      SG_error("md_cache_init rc = %d\n", rc );
      return rc;
   }
                       
   // state can be referenced 
   state->referenceable = true;
   
   // specfile reload thread should be running 
   state->specfile_reload_thread_running = true;
   
   // set up event handlers 
   AG_add_event_handler( state->event_listener, AG_EVENT_TERMINATE_ID, AG_event_handler_terminate, NULL );
   AG_add_event_handler( state->event_listener, AG_EVENT_DRIVER_IOCTL_ID, AG_event_handler_driver_ioctl, NULL );
   
   // set up reload callback 
   ms_client_set_config_change_callback( state->ms, AG_config_change_callback, state );
   
                       
   return 0;
}


// start the AG 
int AG_start( struct AG_state* state ) {
   
   int rc = 0;
   
   // start event listener before reloading--the driver might need it
   SG_debug("%s", "Starting event listener\n");
   
   rc = AG_event_listener_start( state->event_listener );
   if( rc != 0 ) {
      SG_error("AG_event_listener_start rc = %d\n", rc );
      
      return rc;
   }
   
   // start up the block cache 
   SG_debug("%s", "Starting block cache\n");
   
   rc = md_cache_start( state->cache );
   if( rc != 0) {
      SG_error("md_cache_start rc = %d\n", rc );
      
      return rc;
   }
   
   SG_debug("%s", "(Re)synchronizing dataset\n");
   
   // get the list of entries that are already on the MS 
   AG_fs_map_t* on_MS = new AG_fs_map_t();
   
   // wrap on_MS into an AG_fs 
   // NOTE: we don't care about drivers for this map; only consistency information
   struct AG_fs on_MS_fs;
   rc = AG_fs_init( &on_MS_fs, on_MS, state->ms );
   
   if( rc != 0 ) {
      SG_error("AG_fs_init(on_MS) rc = %d\n", rc );
      
      AG_fs_map_free( on_MS );
      delete on_MS;
      
      return rc;
   }
   
   if( state->ag_opts.cached_metadata_path != NULL ) {
      
      // populate on_MS with cached data 
      rc = AG_MS_cache_load( state->ag_opts.cached_metadata_path, on_MS_fs.fs );
      if( rc != 0 ) {
         SG_error("WARN: AG_MS_cache_load(%s) rc = %d\n", state->ag_opts.cached_metadata_path, rc );
         rc = 0;
      }
      
      // copy cached MS metadata into the specfile-generated fs map (so we can tell which metadata to download next)
      AG_fs_copy_cached_data( state->ag_fs, &on_MS_fs, AG_map_info_copy_MS_data );
   }
   
   
   if( !state->ag_opts.quickstart ) {
         
      AG_fs_wlock( state->ag_fs );
      
      // fetched non-cached entries
      rc = AG_download_MS_fs_map( state->ms, state->ag_fs->fs, on_MS_fs.fs );
      
      AG_fs_unlock( state->ag_fs );
      
      if( rc != 0 ) {
         SG_error("AG_download_existing_fs_map rc = %d\n", rc );
         
         AG_fs_free( &on_MS_fs );
         
         return rc;
      }
      
      // copy all downloaded MS metadata into the specfile-generated fs map.
      AG_fs_copy_cached_data( state->ag_fs, &on_MS_fs, AG_map_info_copy_MS_data );
      
      // get all driver metadata for the specfile-generated fs map 
      rc = AG_get_publish_info_all( state, state->ag_fs->fs );
      if( rc != 0 ) { 
         SG_error("AG_get_publish_info_all(specfile) rc = %d\n", rc );
         
         AG_fs_free( &on_MS_fs );
         return rc;
      }
      
      // for initializing, an entry is fresh if the specfile matches the MS (update if not)
      struct AG_fresh_comparator {
         static bool equ( struct AG_map_info* mi1, struct AG_map_info* mi2 ) {
            return (mi1->file_perm == mi2->file_perm &&
                    mi1->reval_sec == mi2->reval_sec &&
                    mi1->type      == mi2->type );
         }
      };
      
      // the AG_fs on the MS is the "old" mapping,
      // and the one we loaded from the spec file is
      // the "new" mapping.  Evolve the old into the new on the MS.
      // force refresh if asked via the command-line
      rc = AG_resync( state, &on_MS_fs, state->ag_fs, AG_fresh_comparator::equ, state->ag_opts.reversion_on_startup );
      
      if( rc != 0 ) {
         SG_error("ERR: AG_resync rc = %d\n", rc );
         AG_fs_free( &on_MS_fs );
         return rc;
      }
   }
   
   AG_fs_free( &on_MS_fs );
   
   if( state->ag_opts.cached_metadata_path != NULL ) {
      
      AG_fs_rlock( state->ag_fs );
      
      // save the cached data 
      rc = AG_MS_cache_store( state->ag_opts.cached_metadata_path, state->ag_fs->fs );
      if( rc != 0 ) {
         SG_error("WARN: AG_MS_cache_store(%s) rc = %d\n", state->ag_opts.cached_metadata_path, rc );
         rc = 0;
      }
      
      AG_fs_unlock( state->ag_fs );
   }
   
   // start HTTP 
   SG_debug("Starting HTTP server (%d threads)\n", state->conf->num_http_threads );
   
   rc = md_start_HTTP( state->http, state->conf->portnum, state->conf );
   if( rc != 0 ) {
      SG_error("ERR: md_start_HTTP rc = %d\n", rc );
      return rc;
   }
   
   // start performing invalidations
   AG_state_fs_rlock( state );
   AG_fs_rlock( state->ag_fs );
   
   // SG_debug("%s", "Starting with the following FS map:\n");
   // AG_dump_fs_map( state->ag_fs->fs );
   
   rc = 0;
   
   AG_fs_unlock( state->ag_fs );
   AG_state_fs_unlock( state );
   
   // start the work queue 
   SG_debug("%s", "Starting workqueue\n");
   
   rc = md_wq_start( state->wq );
   if( rc != 0 ) {
      SG_error("AG_reversioner_start rc = %d\n", rc );
      return rc;
   }
   
   // start the reloader 
   SG_debug("%s", "Starting specfile reload thread\n");
   
   state->specfile_reload_thread = md_start_thread( AG_reload_thread_main, state, false );
   
   if( state->specfile_reload_thread < 0 ) {
      SG_error("ERR: md_start_thread rc = %d\n", (int)state->specfile_reload_thread );
      return (int)state->specfile_reload_thread;
   }

   state->running = true;
   
   return 0;
}


// shut down the AG
int AG_stop( struct AG_state* state ) {
   
   SG_debug("%s", "Shutting down specfile reloader\n");
   
   // wake up the reload callback and tell it to exit
   state->specfile_reload_thread_running = false;
   AG_config_change_callback( state->ms, state );
   
   // de-register the viewchange callback
   ms_client_set_config_change_callback( state->ms, NULL, NULL );
   
   // join with the reload thread
   pthread_cancel( state->specfile_reload_thread );
   pthread_join( state->specfile_reload_thread, NULL );
   
   SG_debug("%s", "Shutting down HTTP server\n");
   md_stop_HTTP( state->http );
   
   SG_debug("%s", "Shutting down event listener\n");
   AG_event_listener_stop( state->event_listener );
   
   SG_debug("%s", "Shutting down workqueue\n");
   md_wq_stop( state->wq );
   
   SG_debug("%s", "Shutting down block cache\n");
   md_cache_stop( state->cache );
   
   state->running = false;
   
   return 0;
}


// free state 
int AG_state_free( struct AG_state* state ) {
   
   if( state->running || state->specfile_reload_thread_running ) {
      // need to stop first 
      return -EINVAL;
   }
   
   SG_debug("Freeing AG instance %s\n", state->inst_nonce );
   
   // state can no longer be referenced 
   state->referenceable = false;
   
   // wait for all other threads to release state 
   pthread_rwlock_wrlock( &state->state_lock );
   
   if( state->http != NULL ) {
      md_free_HTTP( state->http );
      free( state->http );
      state->http = NULL;
   }
   
   if( state->event_listener != NULL ) {
      AG_event_listener_free( state->event_listener );
      free( state->event_listener );
      state->event_listener = NULL;
   }
   
   if( state->wq != NULL ) {
      md_wq_free( state->wq, NULL );
      free( state->wq );
      state->wq = NULL;
   }
   
   if( state->drivers != NULL ) {
      AG_shutdown_drivers( state->drivers );
      delete state->drivers;
      state->drivers = NULL;
   }
   
   if( state->ag_fs != NULL ) {
      AG_fs_free( state->ag_fs );
      free( state->ag_fs );
      state->ag_fs = NULL;
   }
   
   if( state->config != NULL ) {
      delete state->config;
      state->config = NULL;
   }
   
   if( state->cache != NULL ) {
      md_cache_destroy( state->cache );
      free( state->cache );
      state->cache = NULL;
   }
   
   sem_destroy( &state->specfile_reload_sem );
   sem_destroy( &state->running_sem );
   
   // opts to free 
   char* opts_to_free[] = {
      state->ag_opts.sock_path,
      state->ag_opts.logfile_path,
      state->ag_opts.driver_dir,
      state->ag_opts.spec_file_path,
      state->ag_opts.cached_metadata_path,
      NULL
   };
   
   for( int i = 0; opts_to_free[i] != NULL; i++ ) {
      if( opts_to_free[i] != NULL ) {
         free( opts_to_free[i] );
      }
   }
   
   if( state->inst_nonce ) {
      free( state->inst_nonce );
      state->inst_nonce = NULL;
   }
   
   pthread_rwlock_unlock( &state->state_lock );
   
   pthread_rwlock_destroy( &state->fs_lock );
   pthread_rwlock_destroy( &state->config_lock );
   pthread_rwlock_destroy( &state->state_lock );
   
   memset( &state->ag_opts, 0, sizeof(struct AG_opts) );
   memset( state, 0, sizeof(struct AG_state) );
   return 0;
}


// dump config to stdout 
void AG_dump_config( AG_config_t* config ) {
   
   SG_debug("Begin dump config %p\n", config );
   
   for( AG_config_t::iterator itr = config->begin(); itr != config->end(); itr++ ) {
      SG_debug("'%s' = '%s'\n", itr->first.c_str(), itr->second.c_str() );
   }
   
   SG_debug("End dump config %p\n", config );
}

// get a config variable 
char* AG_get_config_var( struct AG_state* state, char const* varname ) {
   
   char* ret = NULL;
   
   string varname_s(varname);
   
   AG_state_config_rlock( state );
   
   AG_config_t::iterator itr = state->config->find( varname_s );
   if( itr != state->config->end() ) {
      ret = strdup( itr->second.c_str() );
   }
   
   AG_state_config_unlock( state );
   
   return ret;
}


// AG-specific usage
static void AG_usage(void) {
   fprintf(stderr, "\n\
AG-specific options:\n\
   -e PATH\n\
            Path to a UNIX domain socket over which to send/receive events.\n\
            \n\
   -i PATH\n\
            Path to which to log runtime information, if not running\n\
            in the foreground.\n\
            \n\
   -D DIR\n\
            Path to the directory that contains the storage drivers.\n\
            \n\
   -s PATH\n\
            Path to an on-disk hierarchy spec file to be used to populate\n\
            this AG's volume.  If not supplied, the MS-served hierarchy spec\n\
            file will be used instead (the default).\n\
            \n\
   -q\n\
            Quick start: take no action on the specfile; just cache the entries\n\
            from the volume and begin serving requests.\n\
            \n\
   -M PATH\n\
            Use cached data from the MS.  If PATH does not exist, then download and\n\
            cache metadata to PATH.  If PATH exists, then load metadata from PATH, and\n\
            append to it any metadata that had to be fetched from the MS.\n\
            \n\
            No consistency checks will be performed if you load cached data from\n\
            the MS.  This is meant for AGs that expose so much static data that re-syncing\n\
            it with the MS on every start-up is infeasible.\n\
            \n\
   -n\n\
            Reversion all files, even if they appear to fresh according to the MS.\n\
            This updates the consistency information for each file on the MS, and invokes\n\
            each dataset driver's reversion method.\n\
\n" );
}

// clear global AG opts buffer
int AG_opts_init() {
   memset( &g_AG_opts, 0, sizeof(struct AG_opts));
   return 0;
}


// add default options
int AG_opts_add_defaults( struct md_syndicate_conf* conf, struct AG_opts* ag_opts ) {
   
   char* storage_root = conf->storage_root;
   
   // default values 
   if( ag_opts->sock_path == NULL ) {
      ag_opts->sock_path = md_fullpath( storage_root, "AG.socket", NULL );
   }
   
   if( ag_opts->logfile_path == NULL ) {
      ag_opts->logfile_path = md_fullpath( storage_root, "AG.log", NULL );
   }
   
   if( ag_opts->driver_dir == NULL ) {
      ag_opts->driver_dir = getcwd( NULL, 0 );     // look locally by default
   }
   
   return 0;
}

// duplicate AG global opts buffer 
int AG_opts_get( struct AG_opts* opts ) {
   memcpy( opts, &g_AG_opts, sizeof(struct AG_opts) );
   
   // deep-copy dynamically-allocatd fields 
   opts->sock_path = SG_strdup_or_null( g_AG_opts.sock_path );
   opts->logfile_path = SG_strdup_or_null( g_AG_opts.logfile_path );
   opts->driver_dir = SG_strdup_or_null( g_AG_opts.driver_dir );
   
   return 0;
}

// opts handler 
int AG_handle_opt( int opt_c, char* opt_s ) {
   
   int rc = 0;
   
   switch( opt_c ) {
      
      case 'e': {
         
         if( g_AG_opts.sock_path != NULL ) {
            free( g_AG_opts.sock_path );
         }
         
         g_AG_opts.sock_path = strdup( opt_s );
         
         break;
      }
      case 'i': {
         
         if( g_AG_opts.logfile_path != NULL ) {
            free( g_AG_opts.logfile_path );
         }
         
         g_AG_opts.logfile_path = strdup( opt_s );
         
         break;
      }
      case 'D': {
         
         if( g_AG_opts.driver_dir != NULL ) {
            free( g_AG_opts.driver_dir );
         }
         
         g_AG_opts.driver_dir = strdup( opt_s );
         
         break;
      }
      case 's': {
         
         if( g_AG_opts.spec_file_path != NULL ) {
            free( g_AG_opts.spec_file_path );
         }
         
         g_AG_opts.spec_file_path = strdup( opt_s );
         
         break;
      }
      case 'n': {
         
         g_AG_opts.reversion_on_startup = true;
         break;
      }
      case 'q': {
         
         g_AG_opts.quickstart = true;
         break;
      }
      case 'M': {
         
         if( g_AG_opts.cached_metadata_path != NULL ) {
            free( g_AG_opts.cached_metadata_path );
         }
         
         g_AG_opts.cached_metadata_path = strdup( opt_s );
         break;
      }
      default: {
         SG_error("Unrecognized option '%c'\n", opt_c );
         rc = -1;
         break;
      }
   }
   return rc;
}

// main method
int AG_main( int argc, char** argv ) {
   int rc = 0;
   
   // register our death handlers 
   signal( SIGQUIT, AG_death_signal_handler );
   signal( SIGINT,  AG_death_signal_handler );
   signal( SIGTERM, AG_death_signal_handler );
   
   // get state 
   struct AG_state* state = &global_state;
   memset( state, 0, sizeof(struct AG_state) );
   
   // syndicate config and MS client
   struct md_syndicate_conf* conf = SG_CALLOC( struct md_syndicate_conf, 1 );
   struct ms_client* ms = SG_CALLOC( struct ms_client, 1 );
   
   // parse options
   struct md_opts opts;
   AG_opts_init();
   
   memset( &opts, 0, sizeof(struct md_opts));
   
   // get options
   rc = md_opts_parse( &opts, argc, argv, NULL, "e:i:D:s:nqM:", AG_handle_opt );
   if( rc != 0 ) {
      md_common_usage( argv[0] );
      AG_usage();
      exit(1);
   }
   
   // enable debugging 
   md_debug( conf, opts.debug_level );
   
   // load config file
   md_default_conf( conf, SYNDICATE_AG );
   
   // read the config file
   if( opts.config_file != NULL ) {
      rc = md_read_conf( opts.config_file, conf );
      if( rc != 0 ) {
         SG_error("ERR: md_read_conf(%s) rc = %d\n", opts.config_file, rc );
         exit(1);
      }
   }
   
   // initialize libsyndicate
   rc = md_init( conf, ms, &opts );
   
   if( rc != 0 ) {
      SG_error("md_init rc = %d\n", rc );
      exit(1);
   }
   
   // get back AG opts 
   struct AG_opts ag_opts;
   AG_opts_get( &ag_opts );
   
   // load default AG options, if we're missing some
   AG_opts_add_defaults( conf, &ag_opts );
   
   // initialize AG signal handling 
   rc = AG_signal_listener_init();
   
   if( rc != 0 ) {
      SG_error("AG_signal_listener_init rc = %d\n", rc );
      exit(1);
   }
   
   // initialize AG state 
   rc = AG_state_init( state, &opts, &ag_opts, conf, ms );
   
   if( rc != 0 ) {
      SG_error("AG_state_init rc = %d\n", rc );
      exit(1);
   }
   
   // start signal handlers 
   rc = AG_signal_listener_start();
   
   if( rc != 0 ) {
      SG_error("AG_signal_listener_start rc = %d\n", rc );
      exit(1);
   }
   
   // start running 
   rc = AG_start( state );
   
   if( rc != 0 ) {
      SG_error("AG_start rc = %d\n", rc );
   }
   
   else {
      // wait to die 
      while( true ) {
         rc = sem_wait( &state->running_sem );
         if( rc != 0 ) {
            
            rc = -errno;
            
            // ignore interruptions 
            if( rc == -EINTR ) {
               continue;
            }
         }
         else {
            // got woken up by a signal handler
            break;
         }
      }
   }
   
   // stop running 
   rc = AG_stop( state );
   
   if( rc != 0 ) {
      SG_error("WARN: AG_stop rc = %d\n", rc );
   }
   
   // stop signal handlers and restore old ones 
   rc = AG_signal_listener_stop();
   if( rc != 0 ) {
      SG_error("WARN: AG_signal_listener_stop rc = %d\n", rc );
   }
   
   // shut down AG
   rc = AG_state_free( state );
   
   if( rc != 0 ) {
      SG_error("WARN: AG_state_free rc = %d\n", rc );
   }
   
   // shut down signal handlers 
   rc = AG_signal_listener_free();
   if( rc != 0 ) {
      SG_error("WARN: AG_signal_listener_free rc = %d\n", rc );
   }
   
   // shutdown libsyndicate
   
   md_free_conf( conf );
   ms_client_destroy( ms );
   
   free( ms );
   free( conf );
   
   md_shutdown();
   
   return 0;
}


