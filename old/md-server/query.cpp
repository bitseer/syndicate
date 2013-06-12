/*
   Copyright 2012 Jude Nelson

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

#include "query.h"

struct md_query_server qs;

struct query_thread* query_processor_threads;
int num_query_processor_threads;


void* query_processor( void* arg ) {
   struct query_thread* tdata = (struct query_thread*)arg;
   struct md_query_server* qsrv = tdata->qsrv;
   
   //int num_replies = 0;
   dbprintf("%s", " starting\n");
   while( qsrv->running ) {

      // get the packet
      struct query_entry qent;
      qent.pkt.Clear();
      qent.addrlen = sizeof(qent.remote_addr);

      int rc = md_query_recv_packet( qsrv->soc, &qent.pkt, (struct sockaddr*)(&qent.remote_addr), &qent.addrlen );
      if( rc < 0 ) {
         errorf( "could not recv packet from %d, rc = %d\n", qsrv->soc, rc );
         continue;
      }
      
      md_query::md_packet reply;

      string fs_path_s = qent.pkt.fs_path();
      char const* fs_path = fs_path_s.c_str();

      struct md_entry ent;
      memset( &ent, 0, sizeof(ent) );

      dbprintf( "query on %s\n", fs_path );
      
      // process this request
      switch( qent.pkt.type() ) {

         // request for data
         case (md_query::md_packet::REQUEST): {

            struct query_cache_entry qcent;
            size_t qcent_len = 0;
            memset( &qcent, 0, sizeof(qcent) );
            bool needs_freeing = false;

            char* qcent_cptr = NULL;
            struct query_cache_entry* qcent_ptr = NULL;
            rc = tdata->cache->get( fs_path, &qcent_cptr, &qcent_len, (uid_t)qent.pkt.owner() );
            if( rc == -ENOENT ) {
               // cache miss
               rc = md_read_entry( qsrv->conf->master_copy_root, fs_path, &ent );
               if( rc != 0 ) {
                  // error
                  errorf( "md_read_entry rc = %d\n", rc );
                  md_query_nack_packet( &reply, fs_path, rc );
               }
               else {
                  struct query_cache_entry* qcdup = CALLOC_LIST( struct query_cache_entry, 1 );
                  
                  qcdup->version = ent.version;
                  qcdup->mtime_sec = ent.mtime_sec;
                  qcdup->mtime_nsec = ent.mtime_nsec;
                  qcdup->owner = ent.owner;
                  qcdup->volume = ent.volume;
                  qcdup->size = ent.size;

                  printf( "put %s (version %lld), mtime = %lld.%d\n", fs_path, qcdup->version, qcdup->mtime_sec, qcdup->mtime_nsec );
                  tdata->cache->put( fs_path, (char*)qcdup, sizeof( struct query_cache_entry ), qent.pkt.owner(), 0777 );

                  qcent_ptr = qcdup;
               }
            }
            else if( rc < 0 ) {
               // error
               errorf( "cache read rc = %d\n", rc );
               md_query_nack_packet( &reply, fs_path, rc );
            }
            else {
               qcent_ptr = (struct query_cache_entry*)qcent_cptr;
               needs_freeing = true;
            }
            
            if( rc == 0 ) {
               // success
               printf( "reply %s (version %lld), mtime = %lld.%d\n", fs_path, qcent_ptr->version, qcent_ptr->mtime_sec, qcent_ptr->mtime_nsec );
               md_query_reply_packet( &reply, fs_path, qcent_ptr->version, qcent_ptr->mtime_sec, qcent_ptr->mtime_nsec, qcent_ptr->owner, qcent_ptr->volume, qcent_ptr->mode, qcent_ptr->size );
               md_entry_free( &ent );
            }

            if( needs_freeing && qcent_cptr ) {
               free( qcent_cptr );
            }
            
            break;
         }

         default: {
            errorf( "invalid packet type %d\n", qent.pkt.type() );
            break;
         }
      }

      // send the reply
      rc = md_query_send_packet( qsrv->soc, &reply, (struct sockaddr*)(&qent.remote_addr), qent.addrlen );
      if( rc < 0 ) {
         errorf( "could not send packet, rc = %d\n", rc );
      }

      rc = 0;
   }

   dbprintf("%s", "exit\n");
   return NULL;
}


// invalidate the caches of our query threads
int query_invalidate_caches( char* fs_path ) {
   for( int i = 0; i < num_query_processor_threads; i++ ) {
      query_processor_threads[i].cache->clear( fs_path );
   }
   return 0;
}


int query_init( struct md_syndicate_conf* conf ) {
   memset( &qs, 0, sizeof(qs) );
   int rc = md_query_server_init( &qs, conf->query_portnum );
   if( rc != 0 )
      return rc;

   qs.running = true;
   num_query_processor_threads = conf->num_query_threads;
   query_processor_threads = CALLOC_LIST( struct query_thread, conf->num_query_threads );
   for( unsigned int i = 0; i < conf->num_query_threads; i++ ) {
      query_processor_threads[i].cache = new md_cache( 5000 );
      query_processor_threads[i].qsrv = &qs;
      query_processor_threads[i].thread = md_start_thread( query_processor, &query_processor_threads[i], false );
      if( query_processor_threads[i].thread < 0 ) {
         errorf( "failed to start query thread, rc = %ld\n", query_processor_threads[i].thread );
         return -1;
      }
   }
   return rc;
}


int query_shutdown() {
   
   md_query_server_shutdown( &qs );
   for( int i = 0; i < num_query_processor_threads; i++ ) {
      if( query_processor_threads[i].thread > 0 ) {
         pthread_cancel( query_processor_threads[i].thread );
         pthread_join( query_processor_threads[i].thread, NULL );
      }
      if( query_processor_threads[i].cache != NULL ) {
         delete query_processor_threads[i].cache;
         query_processor_threads[i].cache = NULL;
      }
   }

   free( query_processor_threads );
   query_processor_threads = NULL;

   return 0;
}
