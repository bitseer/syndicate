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

#include "link.h"


// attach an entry as a child directly
// both fs_entry structures must be write-locked
int fs_entry_attach_lowlevel( struct fs_core* core, struct fs_entry* parent, struct fs_entry* fent ) {
   fs_core_fs_rlock( core );
   fent->link_count++;

   struct timespec ts;
   clock_gettime( CLOCK_REALTIME, &ts );
   parent->mtime_sec = ts.tv_sec;
   parent->mtime_nsec = ts.tv_nsec;

   fs_entry_set_insert( parent->children, fent->name, fent );
   fs_core_fs_unlock( core );
   return 0;
}

// attach a file to the filesystem (same as link())
// THIS METHOD ONLY UPDATES THE METADATA; IT DOES NOT TOUCH STABLE STORAGE
int fs_entry_attach( struct fs_core* core, struct fs_entry* fent, char const* path, uint64_t user, uint64_t vol ) {
   // sanity check: path's basename should be fent's name
   char* path_base = md_basename( path, NULL );
   if( strcmp( fent->name, path_base ) != 0 ) {
      free(path_base);
      return -EINVAL;      // invalid entry
   }
   free( path_base );

   int err = 0;
   char* dirname = md_dirname( path, NULL );
   struct fs_entry* parent = fs_entry_resolve_path( core, dirname, user, vol, true, &err );
   free( dirname );
   if( parent == NULL ) {
      return err;
   }

   err = 0;

   fs_entry_wlock( fent );

   if( !IS_DIR_READABLE( parent->mode, parent->owner, parent->volume, user, vol ) ) {
      // directory not searchable
      fs_entry_unlock( fent );
      fs_entry_unlock( parent );
      return -EACCES;
   }
   if( !IS_WRITEABLE( parent->mode, parent->owner, parent->volume, user, vol ) ) {
      // not writable--cannot insert
      fs_entry_unlock( fent );
      fs_entry_unlock( parent );
      return -EACCES;
   }
   if( fs_entry_set_find_name( parent->children, fent->name ) == NULL ) {
      fs_entry_attach_lowlevel( core, parent, fent );
   }
   else {
      err = -EEXIST;
   }

   fs_entry_unlock( fent );

   fs_entry_unlock( parent );

   return err;
}

