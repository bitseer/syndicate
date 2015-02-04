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

#include "driver.h"
#include "proc-handler.h"

// set up the driver 
int driver_init( void** ret_driver_state ) {
   
   if( shell_driver_get_state() != NULL ) {
      // already initialized
      return 0;
   }
   
   struct shell_driver_state* state = SG_CALLOC( struct shell_driver_state, 1 );
   
   // initialize
   int rc = shell_driver_state_init( state );
   if( rc != 0 ) {
      SG_error("shell_driver_state_init rc = %d\n", rc );
      return rc;
   }
   
   // register our signal handler 
   rc = AG_driver_set_signal_handler( SIGCHLD, proc_sigchld_handler );
   if( rc != 0 ) {
      SG_error("AG_driver_set_signal_handler(SIGCHLD) rc = %d\n", rc );
      return rc;
   }
   
   // start up 
   rc = shell_driver_state_start( state );
   if( rc != 0 ) {
      SG_error("shell_driver_state_start rc = %d\n", rc );
      shell_driver_state_free( state );
      return rc;
   }
   
   *ret_driver_state = state;
   
   shell_driver_set_state( state );
   
   return 0;
}


// shut down the driver 
int driver_shutdown( void* driver_state ) {
   
   struct shell_driver_state* state = (struct shell_driver_state*)driver_state;
   
   if( state != NULL ) {
      // shut down 
      int rc = shell_driver_state_stop( state );
      if( rc != 0 ) {
         SG_error("shell_driver_state_stop rc = %d\n", rc );
         return rc;
      }
      
      // free 
      rc = shell_driver_state_free( state );
      if( rc != 0 ) {
         SG_error("shell_driver_state_free rc = %d\n", rc );
         return rc;
      }
   
      free( state );
   }
   
   return 0;
}


// read a block 
ssize_t get_dataset_block( struct AG_connection_context* ag_ctx, uint64_t block_id, char* block_buf, size_t size, void* driver_conn_state ) {
   
   struct proc_connection_context* pctx = (struct proc_connection_context*)driver_conn_state;
   struct shell_driver_state* state = pctx->state;
   
   char* request_path = pctx->request_path;
   
   ssize_t rc = 0;
   
   // make sure we're at least in the process of getting data 
   rc = proc_ensure_has_data( state, pctx );
   if( rc != 0 ) {
      SG_error("proc_ensure_has_data(%s) rc = %zd\n", request_path, rc );
      return rc;
   }
   
   // try to get the block
   rc = proc_read_block_data( state, request_path, block_id, block_buf, size );
   if( rc < 0 ) {
      SG_error("proc_read_block_data(%s) rc = %zd\n", request_path, rc );
   }
   else {
      rc = size;
   }
   
   return rc;
}


// set up a connection to get a block 
int connect_dataset_block( struct AG_connection_context* ag_ctx, void* driver_state, void** driver_conn_state ) {
   
   struct shell_driver_state* state = (struct shell_driver_state*)driver_state;
   struct proc_connection_context* pctx = SG_CALLOC( struct proc_connection_context, 1 );
   
   // fill in the connection context
   pctx->state = state;
 
   pctx->request_path = AG_driver_get_request_path( ag_ctx );
   pctx->shell_cmd = AG_driver_get_query_string( ag_ctx );
   
   *driver_conn_state = pctx;
   return 0;
}

// clean up a connection 
int close_dataset_block( void* driver_conn_state) {
   
   struct proc_connection_context* pctx = (struct proc_connection_context*)driver_conn_state;
   
   if( pctx != NULL ) {
      if( pctx->request_path != NULL ) {
         free( pctx->request_path );
         pctx->request_path = NULL;
      }
      
      if( pctx->shell_cmd != NULL ) {
         free( pctx->shell_cmd );
         pctx->shell_cmd = NULL;
      }
      
      free( pctx );
   }
   
   return 0;
}


// fill in dataset publishing information 
int stat_dataset( char const* path, struct AG_map_info* mi, struct AG_driver_publish_info* pubinfo, void* driver_state ) {
   
   struct shell_driver_state* state = (struct shell_driver_state*)driver_state;
   
   ssize_t rc = 0;
   
   // we can give back publish information for at least partial results
   struct stat sb;
   rc = proc_stat_data( state, path, &sb );
   if( rc != 0 ) {
      
      if( rc == -ENOENT ) {
         
         // the job hasn't been run yet
         struct timespec ts;
         clock_gettime( CLOCK_REALTIME, &ts );
         
         // we know nothing 
         pubinfo->size = -1;
         pubinfo->mtime_sec = ts.tv_sec;
         pubinfo->mtime_nsec = ts.tv_nsec;
         
         return 0;
      }
      else {
         
         // some other error
         SG_error("proc_stat_data(%s) rc = %zd\n", path, rc );
         return rc;
      }
   }
   else {
      
      // fill in what we know, even if it's partial
      pubinfo->size = sb.st_size;
      pubinfo->mtime_sec = sb.st_mtime;
      pubinfo->mtime_nsec = 0;
   }
   
   return 0;
}

// handle a dataset reversioning 
int reversion_dataset( char const* path, struct AG_map_info* mi, void* driver_state ) {
   struct shell_driver_state* state = (struct shell_driver_state*)driver_state;
   
   // invalidate cached data
   proc_evict_cache( state, path );
   return 0;
}

// query type 
char* get_query_type(void) {
   return strdup("shell");
}

