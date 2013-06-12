/*
 * Depricated; will not be used in the future.
 */

#include "master_copy.h"

// create a master copy
MasterCopy::MasterCopy( struct md_syndicate_conf* conf, consume_func cf, void* cls )
   : Threadpool<struct md_entry>( MAX( conf->num_crawlers, 1 ), MASTERCOPY_THREAD_WORKSIZE, true ) {
   
   this->conf = conf;
   this->consumer = cf;
   this->cls = cls;
}


MasterCopy::~MasterCopy() {
}

// begin walking the master copy
int MasterCopy::begin() {
   int rc = 0;
   this->done = false;
   
   // add the root of the master copy to be processed
   struct md_entry* root = CALLOC_LIST( struct md_entry, 1 );
   rc = md_read_entry( this->conf->master_copy_root, "/", root );
   
   if( rc == 0 ) {
      rc = this->add_work( root );
   }
   else {
      errorf("MasterCopy::begin(): could not read master copy at %s\n", this->conf->master_copy_root );
   }
   
   return rc;
}

// wait until we're done walking the master copy
int MasterCopy::wait( int check_interval ) {
   int rc = 0;
   while( !this->done ) {
      usleep( check_interval );
   }
   
   return rc;
}

// do we have more work?
bool MasterCopy::has_more() {
   return !this->done;
}

// process a path, and enqueue more master copy paths
int MasterCopy::process_work( struct md_entry* ent, int thread_no ) {
   if( this->done ) {
      usleep( 50000 );
      return 0;
   }
   
   int rc = 0;
   char fp[PATH_MAX];
   
   if( MD_ENTRY_ISDIR( *ent ) ) {
      
      dbprintf("MasterCopy(%d)::process_work: %s\n", thread_no, ent->path );
      
      // this is a directory
      md_fullpath( this->conf->master_copy_root, ent->path, fp );
      struct md_entry** ents = md_walk_fs_dir( fp, false, true, NULL, NULL, md_walk_mc_dir_process_func, NULL );
      
      if( ents != NULL ) {
         
         for( int i = 0; ents[i] != NULL; i++ ) {
            
            // is this a directory?
            if( MD_ENTRY_ISDIR( *ents[i] ) ) {
               
               rc = this->add_work( md_entry_dup( ents[i] ) );
               if( rc != 0 ) {
                  errorf("MasterCopy(%d)::process_work: add_work(%s) rc = %d\n", thread_no, ents[i]->path, rc );
               }
            }
            
            (*this->consumer)( ents[i], this->cls );
         }
         free( ents );
      }
      else {
         errorf("MasterCopy(%d)::process_work: could not walk %s\n", thread_no, ent->path );
      }
   }

   md_entry_free( ent );
   free( ent );
   
   if( this->work_count() == 0 ) {
      this->done = true;
   }
   
   return rc;
}
