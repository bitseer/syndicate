/*
   Copyright 2015 The Trustees of Princeton University

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

#include "fs.h"
#include "consistency.h"
#include "read.h"
#include "write.h"
#include "client.h"
#include "replication.h"
#include "inode.h"
#include "sync.h"
#include "vacuumer.h"

// export an fskit_entry to an md_entry, i.e. to create it on the MS.  Use the given gateway to get the coordinator, volume, and read/write freshness values.
// only set fields in dest that can be filled in from src
// return 0 on success
// return -ENOMEM on OOM 
// NOTE: src must be read-locked
static int UG_fs_export( struct md_entry* dest, struct fskit_entry* src, uint64_t parent_id, char const* parent_name, struct SG_gateway* gateway ) {
   
   struct ms_client* ms = SG_gateway_ms( gateway );
   struct md_syndicate_conf* conf = SG_gateway_conf( gateway );
   
   struct UG_inode* inode = NULL;
   
   // get type
   int type = fskit_entry_get_type( src );
   
   if( type == FSKIT_ENTRY_TYPE_FILE ) {
      dest->type = MD_ENTRY_FILE;
   }
   else if( type == FSKIT_ENTRY_TYPE_DIR ) {
      dest->type = MD_ENTRY_DIR;
   }
   else {
      // invalid 
      return -EINVAL;
   }
   
   char* name = fskit_entry_get_name( src );
   if( name == NULL ) {
      
      return -ENOMEM;
   }
   
   char* parent_name_dup = SG_strdup_or_null( parent_name );
   if( parent_name_dup == NULL && parent_name != NULL ) {
      
      SG_safe_free( name );
      return -ENOMEM;
   }
   
   inode = (struct UG_inode*)fskit_entry_get_user_data( src );
   
   dest->type = type;
   dest->name = name;
   dest->file_id = fskit_entry_get_file_id( src );
   
   fskit_entry_get_ctime( src, &dest->ctime_sec, &dest->ctime_nsec );
   fskit_entry_get_mtime( src, &dest->mtime_sec, &dest->mtime_nsec );
   
   SG_manifest_get_modtime( UG_inode_manifest( *inode ), &dest->manifest_mtime_sec, &dest->manifest_mtime_nsec );
   
   dest->owner = fskit_entry_get_owner( src );
   dest->mode = fskit_entry_get_mode( src );
   dest->size = fskit_entry_get_size( src );
   dest->parent_id = parent_id;
   dest->parent_name = parent_name_dup;
   
   dest->max_read_freshness = conf->default_read_freshness;
   dest->max_write_freshness = conf->default_write_freshness;
   dest->coordinator = SG_gateway_id( gateway );
   dest->volume = ms_client_get_volume_id( ms );
   
   return 0;
}


// create or mkdir
// return 0 on success
// return -errno on failure (i.e. it exists, we don't have permission, we get a network error, etc.)
// NOTE: fent will be write-locked by fskit
static int UG_fs_create_or_mkdir( struct fskit_core* fs, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, mode_t mode, struct UG_inode** ret_inode_data, bool is_mkdir ) {
   
   int rc = 0;
   struct SG_gateway* gateway = (struct SG_gateway*)fskit_core_get_user_data( fs );
   struct ms_client* ms = SG_gateway_ms( gateway );
   
   char const* method_name = NULL;
   
   // inode data 
   struct UG_inode* inode = NULL;
   
   uint64_t file_id = 0;
   int64_t write_nonce = 0;
   int64_t file_version = md_random64();
   
   struct md_entry inode_data;
   struct fskit_entry* parent = fskit_route_metadata_parent( route_metadata );
   char* parent_name = fskit_entry_get_name( parent );
   
   if( parent_name == NULL ) {
      
      return -ENOMEM;
   }
   
   // generate the request
   rc = UG_fs_export( &inode_data, fent, fskit_entry_get_file_id( parent ), parent_name, gateway );
   
   SG_safe_free( parent_name );
   
   if( rc != 0 ) {
      
      return rc;
   }
   
   // propagate the caller and Syndicate-specific fields...
   inode_data.mode = mode;
   inode_data.version = file_version;
   inode_data.owner = SG_gateway_user_id( gateway );
   
   // make the request
   if( is_mkdir ) {
      
      method_name = "ms_client_mkdir";
      rc = ms_client_mkdir( ms, &file_id, &write_nonce, &inode_data );   
   }
   else {
      
      method_name = "ms_client_create";
      rc = ms_client_create( ms, &file_id, &write_nonce, &inode_data );
   }
   
   md_entry_free( &inode_data );
   
   if( rc != 0 ) {
      
      SG_error("%s('%s') rc = %d\n", method_name, fskit_route_metadata_path( route_metadata ), rc );
      
      return rc;
   }
   
   // update the child with the new inode number 
   fskit_entry_set_file_id( fent, file_id );
   
   // success!  create the inode data 
   inode = SG_CALLOC( struct UG_inode, 1 );
   if( inode == NULL ) {
      
      return -ENOMEM;
   }
   
   rc = UG_inode_init( inode, fent, ms_client_get_volume_id( ms ), SG_gateway_id( gateway ), file_version );
   if( rc != 0 ) {
      
      SG_error("UG_inode_init('%s') rc = %d\n", fskit_route_metadata_path( route_metadata ), rc );
      
      SG_safe_free( inode );
      return rc;
   }
   
   // success!
   *ret_inode_data = inode;
   
   return 0;
}


// fskit create callback: try to create the entry on the MS.
// return 0 on success, and create the file on the MS
// return negative on failure (i.e. it exists, we don't have permission, we get a network error, etc.)
// NOTE: fent will be write-locked by fskit
static int UG_fs_create( struct fskit_core* fs, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, mode_t mode, void** ret_inode_data, void** ret_handle_data ) {
   
   int rc = 0;
   
   // inode data 
   struct UG_inode* inode = NULL;
   
   // handle data 
   struct UG_file_handle* handle = NULL;
   
   rc = UG_fs_create_or_mkdir( fs, route_metadata, fent, mode, &inode, false );
   if( rc != 0 ) {
      
      return rc;
   }
   
   // success!
   // create the handle 
   handle = SG_CALLOC( struct UG_file_handle, 1 );
   if( handle == NULL ) {
      
      UG_inode_free( inode );
      SG_safe_free( inode );
      return -ENOMEM;
   }
   
   rc = UG_file_handle_init( handle, inode, O_CREAT | O_WRONLY | O_TRUNC );
   if( rc != 0 ) {
      
      SG_safe_free( handle );
      UG_inode_free( inode );
      SG_safe_free( inode );
      return -ENOMEM;
   }
   
   // success!
   *ret_inode_data = inode;
   *ret_handle_data = handle;
   
   return 0;
}


// fskit mkdir callback 
// return 0 on success, and create the dir on the MS
// return negative on failure (i.e. it exists, we don't have permission, we got a network error, etc.)
// NOTE: fent will be write-locked by fskit
static int UG_fs_mkdir( struct fskit_core* fs, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, mode_t mode, void** ret_inode_data ) {
   
   int rc = 0;
   
   // inode data 
   struct UG_inode* inode = NULL;
   
   rc = UG_fs_create_or_mkdir( fs, route_metadata, fent, mode, &inode, true );
   if( rc != 0 ) {
      
      return rc;
   }
   
   // success!
   *ret_inode_data = inode;
   
   return 0;
}


// fskit open/opendir callback
// refresh path information for the fent 
// return 0 on success
// return negative on failure (i.e. network error, OOM)
// NOTE: fent must *not* be locked (the consistency discipline must not alter its lock state)
static int UG_fs_open( struct fskit_core* fs, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, int flags, void** handle_data ) {
   
   int rc = 0;
   struct UG_file_handle* handle = NULL;
   struct SG_gateway* gateway = (struct SG_gateway*)fskit_core_get_user_data( fs );
   struct UG_inode* inode = NULL;
   
   // refresh path 
   rc = UG_consistency_path_ensure_fresh( gateway, fskit_route_metadata_path( route_metadata ) );
   if( rc != 0 ) {
      
      SG_error( "UG_consistency_path_ensure_fresh('%s') rc = %d\n", fskit_route_metadata_path( route_metadata ), rc );
      return rc;
   }
   
   // if this is a directory, go reload the children 
   if( fskit_entry_get_type( fent ) == FSKIT_ENTRY_TYPE_DIR ) {
      
      // ensure the listing is fresh 
      rc = UG_consistency_dir_ensure_fresh( gateway, fskit_route_metadata_path( route_metadata ) );
      if( rc != 0 ) {
         
         SG_error("UG_consistency_dir_ensure_fresh('%s') rc = %d\n", fskit_route_metadata_path( route_metadata ), rc );
         return rc;
      }
      
      // no handle structure is necessary
   }
   else {
      
      // generate a file handle 
      handle = SG_CALLOC( struct UG_file_handle, 1 );
      if( handle == NULL ) {
         
         // OOM 
         return -ENOMEM;
      }
      
      // get inode 
      fskit_entry_rlock( fent );
      
      inode = (struct UG_inode*)fskit_entry_get_user_data( fent );
      
      if( UG_inode_deleting( *inode ) ) {
         
         rc = -ENOENT;
      }
      else {
         
         rc = UG_file_handle_init( handle, inode, flags );
      }
      
      fskit_entry_unlock( fent );
      
      if( rc != 0 ) {
         
         // OOM 
         SG_safe_free( handle );
         return rc;
      }
      
      *handle_data = handle;
   }
   
   return rc;
}


// fskit close/closedir callback--free up the handle 
// return 0 on success (always succeeds)
// fent is not even accessed--no locking is needed
static int UG_fs_close( struct fskit_core* fs, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, void* handle_data ) {
   
   struct UG_file_handle* handle = (struct UG_file_handle*)handle_data;
   
   // free up
   if( handle != NULL ) {
      
      UG_file_handle_free( handle );
      SG_safe_free( handle );
   }
   
   return 0;
}


// fskit stat callback.
// go refresh the path, and pull in any immediate children if it's a directory.
// NOTE: fent must *not* be locked (the consistency discipline must not alter its lock state)
static int UG_fs_stat( struct fskit_core* fs, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, struct stat* sb ) {
   
   int rc = 0;
   
   struct UG_inode* inode = NULL;
   
   fskit_entry_rlock( fent );
   
   // check deleting...
   inode = (struct UG_inode*)fskit_entry_get_user_data( fent );
   
   if( UG_inode_deleting( *inode ) ) {
      
      rc = -ENOENT;
   }
   
   fskit_entry_unlock( fent );
   
   return rc;
}


// truncate locally--ask the MS to update the size and version, vacuum now-removed blocks, and replicate the new manifest.
// return 0 on success
// return -ENOMEM on OOM 
// return -EISDIR if the inode is a directory
// return -errno on network error
// NOTE: inode->entry must be write-locked (e.g. by fskit)
static int UG_fs_trunc_local( struct SG_gateway* gateway, char const* fs_path, struct UG_inode* inode, off_t new_size ) {
   
   int rc = 0;
   struct md_entry inode_data;
   struct UG_state* ug = (struct UG_state*)SG_gateway_cls( gateway );
   struct ms_client* ms = SG_gateway_ms( gateway );
   struct UG_vacuumer* vacuumer = UG_state_vacuumer( ug );
   
   int64_t new_write_nonce = 0;
   
   struct SG_manifest new_manifest;     // manifest after truncate
   struct SG_manifest removed;
   
   struct UG_replica_context rctx;
   struct UG_vacuum_context vctx;
   
   struct timespec old_manifest_timestamp = UG_inode_old_manifest_modtime( *inode );
   struct timespec new_manifest_timestamp;
   
   int64_t new_manifest_timestamp_sec = 0;
   int32_t new_manifest_timestamp_nsec = 0;
   
   uint64_t volume_blocksize = ms_client_get_volume_blocksize( ms );
   uint64_t new_max_block = new_size / volume_blocksize;
   
   if( new_size % volume_blocksize > 0 ) {
      new_max_block++;
   }
   
   // if deleting, deny further I/O 
   if( UG_inode_deleting( *inode ) ) {
      
      return -ENOENT;
   }
   
   // can't truncate a directory 
   if( fskit_entry_get_type( inode->entry ) == FSKIT_ENTRY_TYPE_DIR ) {
      
      return -EISDIR;
   }
   
   rc = UG_inode_export( &inode_data, inode, 0, NULL );
   if( rc != 0 ) {
      
      return rc;
   }
   
   rc = SG_manifest_init( &removed, ms_client_get_volume_id( ms ), SG_gateway_id( gateway ), UG_inode_file_id( *inode ), UG_inode_file_version( *inode ) );
   if( rc != 0 ) {
      
      md_entry_free( &inode_data );
      return rc;
   }
   
   rc = SG_manifest_dup( &new_manifest, UG_inode_manifest( *inode ) );
   if( rc != 0 ) {
      
      // OOM
      md_entry_free( &inode_data );
      SG_manifest_free( &removed );
      return rc;
   }
   
   // find removed blocks 
   rc = UG_inode_truncate_find_removed( gateway, inode, new_size, &removed );
   if( rc != 0 ) {
      
      // OOM 
      SG_manifest_free( &removed );
      SG_manifest_free( &new_manifest );
      md_entry_free( &inode_data );
      return rc;
   }
   
   // prepare the vacuum request 
   memset( &vctx, 0, sizeof(struct UG_vacuum_context) );
   rc = UG_vacuum_context_init( &vctx, ug, fs_path, inode, &removed );
   
   SG_manifest_free( &removed );
   
   if( rc != 0 ) {
      
      // OOM 
      SG_manifest_free( &new_manifest );
      md_entry_free( &inode_data );
      return rc;
   }
   
   // replicate truncated manifest...
   SG_manifest_truncate( &new_manifest, new_max_block );
   
   rc = UG_replica_context_init( &rctx, ug, fs_path, inode, &new_manifest, &old_manifest_timestamp, NULL );
   if( rc != 0 ) {
      
      // OOM 
      SG_manifest_free( &new_manifest );
      UG_vacuum_context_free( &vctx );
      md_entry_free( &inode_data );
      return rc;
   }
   
   // preserve now-replicated manifest modtime
   SG_manifest_get_modtime( &new_manifest, &new_manifest_timestamp_sec, &new_manifest_timestamp_nsec );
   SG_manifest_free( &new_manifest );
   
   // update on the MS
   inode_data.size = new_size;
   inode_data.version = md_random64();  // next version...
   inode_data.manifest_mtime_sec = new_manifest_timestamp_sec;          // preserve modtime of manifest we replicated
   inode_data.manifest_mtime_nsec = new_manifest_timestamp_nsec;
   
   new_manifest_timestamp.tv_sec = new_manifest_timestamp_sec;
   new_manifest_timestamp.tv_nsec = new_manifest_timestamp_nsec;
   
   // update size and version remotely
   rc = ms_client_update( ms, &new_write_nonce, &inode_data );
   
   md_entry_free( &inode_data );
   
   if( rc != 0 ) {
      
      UG_vacuum_context_free( &vctx );
      
      SG_error("ms_client_update('%s', size=%jd) rc = %d\n", fs_path, new_size, rc );
      return rc;
   }
   
   // replicate truncated manifest to all RGs
   rc = UG_replicate( gateway, &rctx );
   
   if( rc != 0 ) {
      
      // replication error...
      SG_error("UG_replicate('%s') rc = %d\n", fs_path, rc );
      
      UG_vacuum_context_free( &vctx );
      return rc;
   }
   
   // propagate write nonce and old manifest timestamp...
   UG_inode_set_write_nonce( inode, new_write_nonce );
   UG_inode_set_old_manifest_modtime( inode, &new_manifest_timestamp );
   
   // truncate locally 
   UG_inode_truncate( gateway, inode, new_size, inode_data.version );
   
   // preserve the timestamp from the replicated manifest, so they match on the RG and locally
   SG_manifest_set_modtime( UG_inode_manifest( *inode ), new_manifest_timestamp_sec, new_manifest_timestamp_nsec );
   
   // garbate-collect 
   while( 1 ) {
      
      rc = UG_vacuum_run( vacuumer, &vctx );
      if( rc != 0 ) {
         
         SG_error("UG_vacuum_run('%s') rc = %d, retrying...\n", fs_path, rc );
         continue;
      }
      
      break;
   }
      
   UG_vacuum_context_free( &vctx );
   
   return rc;
}


// ask another gateway to truncate a file for us.
// return 0 on success
// return -ENOMEM on OOM 
// return -EISDIR if the entry is a directory.
// NOTE: inode->entry should be write-locked
static int UG_fs_trunc_remote( struct SG_gateway* gateway, char const* fs_path, struct UG_inode* inode, off_t new_size ) {
   
   int rc = 0;
   
   SG_messages::Request req;
   SG_messages::Reply reply;
   struct SG_request_data reqdat;
   
   int64_t manifest_mtime_sec = 0;
   int32_t manifest_mtime_nsec = 0;
   
   // if deleting, deny further I/O 
   if( UG_inode_deleting( *inode ) ) {
      
      return -ENOENT;
   }
   
   if( fskit_entry_get_type( inode->entry ) != FSKIT_ENTRY_TYPE_DIR ) {
      
      return -EISDIR;
   }
   
   SG_manifest_get_modtime( UG_inode_manifest( *inode ), &manifest_mtime_sec, &manifest_mtime_nsec );
   
   rc = SG_request_data_init_manifest( gateway, fs_path, UG_inode_file_id( *inode ), UG_inode_file_version( *inode ), manifest_mtime_sec, manifest_mtime_nsec, &reqdat );
   if( rc != 0 ) {
      
      // OOM
      return rc;
   }
   
   rc = SG_client_request_TRUNCATE_setup( gateway, &req, &reqdat, new_size );
   if( rc != 0 ) {
      
      // OOM 
      SG_error("SG_client_request_TRUNCATE_setup('%s') rc = %d\n", fs_path, rc );
      SG_request_data_free( &reqdat );
      return rc;
   }
   
   SG_request_data_free( &reqdat );
   
   rc = SG_client_request_send( gateway, UG_inode_coordinator_id( *inode ), &req, NULL, &reply );
   if( rc != 0 ) {
      
      // network error 
      SG_error("SG_client_request_send(TRUNC '%s' %jd) rc = %d\n", fs_path, new_size, rc );
      return rc;
   }
   
   if( reply.error_code() != 0 ) {
      
      // failed to process
      SG_error("SG_client_request_send(TRUNC '%s' %jd) reply error = %d\n", fs_path, new_size, rc );
      return rc;
   }
   
   // truncate locally 
   UG_inode_truncate( gateway, inode, new_size, 0 );
   
   // reload inode on next access
   inode->read_stale = true;
   
   return rc;
}


// fskit route for truncating files.
// In the UG, this simply tells the MS that the size has changed.
// return 0 on success
// return -ENOMEM on OOM 
// return -errno on failure to connect to the MS
// NOTE: fent will be write-locked by fskit
static int UG_fs_trunc( struct fskit_core* fs, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, off_t new_size, void* inode_cls ) {
   
   int rc = 0;
   
   struct SG_gateway* gateway = (struct SG_gateway*)fskit_core_get_user_data( fs );
   struct UG_inode* inode = (struct UG_inode*)fskit_entry_get_user_data( fent );
   char* path = fskit_route_metadata_path( route_metadata );
   
   UG_try_or_coordinate( gateway, path, UG_inode_coordinator_id( *inode ),
                         UG_fs_trunc_local( gateway, path, inode, new_size ),
                         UG_fs_trunc_remote( gateway, path, inode, new_size ), &rc );
   
   return rc;
}


// ask the MS to detach a file or directory.  If we succeed, clear any cached state.
// return 0 on success
// return -ENOMEM on OOM 
// return -errno on network error 
// NOTE: inode->entry must be write-locked
static int UG_fs_detach_local( struct SG_gateway* gateway, char const* fs_path, struct UG_inode* inode ) {
   
   int rc = 0;
   struct ms_client* ms = SG_gateway_ms( gateway );
   struct md_syndicate_cache* cache = SG_gateway_cache( gateway );
   struct UG_state* ug = (struct UG_state*)SG_gateway_cls( gateway );
   
   struct md_entry inode_data;
   
   struct UG_vacuum_context vctx;
   
   if( UG_inode_deleting( *inode ) ) {
      
      return -ENOENT;
   }
   
   // deny subsequent I/O operations 
   inode->deleting = true;
   
   // export...
   rc = UG_inode_export( &inode_data, inode, 0, NULL );
   if( rc != 0 ) {
      
      inode->deleting = false;
      return rc;
   }
   
   // if this is a file, and we're the coordinator, vacuum it 
   if( UG_inode_coordinator_id( *inode ) == SG_gateway_id( gateway ) && fskit_entry_get_file_id( inode->entry ) == FSKIT_ENTRY_TYPE_FILE ) {
         
      memset( &vctx, 0, sizeof(struct UG_vacuum_context) );
      
      rc = UG_vacuum_context_init( &vctx, ug, fs_path, inode, UG_inode_replaced_blocks( *inode ) );
      if( rc != 0 ) {
         
         SG_error("UG_vacuum_context_init('%s') rc = %d\n", fs_path, rc );
         
         md_entry_free( &inode_data );
         inode->deleting = false;
         return rc;
      }
      
      while( 1 ) {
         
         // vacuum until we succeed
         rc = UG_vacuum_run( UG_state_vacuumer( ug ), &vctx );
         
         if( rc != 0 ) {
            
            SG_error("UG_vacuum_run('%s') rc = %d; retrying...\n", fs_path, rc );
            continue;
         }
         
         break;
      }
   }
   
   UG_vacuum_context_free( &vctx );
   
   // delete on the MS
   rc = ms_client_delete( ms, &inode_data );
   md_entry_free( &inode_data );
   
   if( rc != 0 ) {
      
      inode->deleting = false;
      SG_error("ms_client_delete('%s') rc = %d\n", fs_path, rc );
      return rc;
   }
   
   // blow away local cached state, if this is a file 
   if( fskit_entry_get_file_id( inode->entry ) == FSKIT_ENTRY_TYPE_FILE ) {
      
      md_cache_evict_file( cache, UG_inode_file_id( *inode ), UG_inode_file_version( *inode ) );
   }
   
   return rc;
}


// ask a remote gateway to detach an inode for us, if the inode is a file.
// if the inode is a directory, ask the MS directly.
// return 0 on success
// return -ENOMEM on OOM 
// return -errno on network error 
static int UG_fs_detach_remote( struct SG_gateway* gateway, char const* fs_path, struct UG_inode* inode ) {
   
   int rc = 0;
   struct md_syndicate_cache* cache = SG_gateway_cache( gateway );
   
   SG_messages::Request req;
   SG_messages::Reply reply;
   struct SG_request_data reqdat;
   
   int64_t manifest_mtime_sec = 0;
   int32_t manifest_mtime_nsec = 0;
   
   SG_manifest_get_modtime( UG_inode_manifest( *inode ), &manifest_mtime_sec, &manifest_mtime_nsec );
   
   rc = SG_request_data_init_manifest( gateway, fs_path, UG_inode_file_id( *inode ), UG_inode_file_version( *inode ), manifest_mtime_sec, manifest_mtime_nsec, &reqdat );
   if( rc != 0 ) {
      
      // OOM
      return rc;
   }
   
   // NOTE: no vacuum ticket; the receiving gateway can verify the write-permission with the certificate
   rc = SG_client_request_DETACH_setup( gateway, &req, &reqdat, NULL );
   if( rc != 0 ) {
      
      // OOM 
      SG_error("SG_client_request_DETACH_setup('%s') rc = %d\n", fs_path, rc );
      SG_request_data_free( &reqdat );
      return rc;
   }
   
   SG_request_data_free( &reqdat );
   
   rc = SG_client_request_send( gateway, UG_inode_coordinator_id( *inode ), &req, NULL, &reply );
   if( rc != 0 ) {
      
      // network error 
      SG_error("SG_client_request_send(DETACH '%s') rc = %d\n", fs_path, rc );
      return rc;
   }
   
   if( reply.error_code() != 0 ) {
      
      // failed to process
      SG_error("SG_client_request_send(DETACH '%s') reply error = %d\n", fs_path, rc );
      return rc;
   }
   
   // blow away local cached state 
   md_cache_evict_file( cache, UG_inode_file_id( *inode ), UG_inode_file_version( *inode ) );
   
   return rc;
}


// fskit route for detaching a file or directory.
// In the UG, this simply tells the MS to delete the entry.
// if we're the coordinator, and this is a file, then garbage-collect all of its blocks
// return 0 on success 
// return -ENOMEM on OOM 
// return -EAGAIN if the caller should try detaching again
// return -errno on failure to connect to the MS 
// NOTE: fent should not be locked at all (it will be unreferenceable)
static int UG_fs_detach( struct fskit_core* fs, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, void* inode_cls ) {
   
   int rc = 0;
   struct UG_inode* inode = (struct UG_inode*)inode_cls;
   struct SG_gateway* gateway = (struct SG_gateway*)fskit_core_get_user_data( fs );
   char* path = fskit_route_metadata_path( route_metadata );
   
   UG_try_or_coordinate( gateway, path, UG_inode_coordinator_id( *inode ),
                         UG_fs_detach_local( gateway, fskit_route_metadata_path( route_metadata ), inode ),
                         UG_fs_detach_remote( gateway, fskit_route_metadata_path( route_metadata ), inode ), &rc );
   
   if( rc == 0 ) {
      
      // success!
      UG_inode_free( inode );
      SG_safe_free( inode );
   }
   
   return rc;
}


// ask the MS to rename an inode for us.
// old_parent and new_parent must be at least read-locked
// return 0 on success
// return -ENOMEM on OOM
// return negative on network error
// NOTE: old_inode->entry should be write-locked by fskit
static int UG_fs_rename_local( struct SG_gateway* gateway, struct fskit_entry* old_parent, char const* old_path, struct UG_inode* old_inode, struct fskit_entry* new_parent, char const* new_path, struct UG_inode* new_inode ) {
   
   int rc = 0;
   
   struct md_entry old_fent_metadata;
   struct md_entry new_fent_metadata;
   
   uint64_t old_parent_id = fskit_entry_get_file_id( old_parent );
   uint64_t new_parent_id = fskit_entry_get_file_id( new_parent );
   
   char* old_parent_name = fskit_entry_get_name( old_parent );
   char* new_parent_name = fskit_entry_get_name( new_parent );
   
   int64_t new_write_nonce = 0;
   
   struct ms_client* ms = SG_gateway_ms( gateway );
   
   if( old_parent_name == NULL || new_parent_name == NULL ) {
      
      SG_safe_free( old_parent_name );
      SG_safe_free( new_parent_name );
      
      return -ENOMEM;
   }
   
   rc = UG_inode_export( &old_fent_metadata, old_inode, old_parent_id, old_parent_name );
   if( rc != 0 ) {
      
      return rc;
   }
   
   if( new_inode != NULL ) {
      
      rc = UG_inode_export( &new_fent_metadata, new_inode, new_parent_id, new_parent_name );
      if( rc != 0 ) {
         
         md_entry_free( &old_fent_metadata );
         return rc;
      }
   }
   else {
      
      // will rename into a new path entirely  
      rc = md_entry_dup2( &old_fent_metadata, &new_fent_metadata );
      if( rc != 0 ) {
         
         md_entry_free( &old_fent_metadata );
         return rc;
      }
      
      // indicates we don't know the new inode's IDs
      new_fent_metadata.file_id = 0;
   }
   
   // do the rename on the MS
   rc = ms_client_rename( ms, &new_write_nonce, &old_fent_metadata, &new_fent_metadata );
   
   md_entry_free( &old_fent_metadata );
   md_entry_free( &new_fent_metadata );
   
   if( rc != 0 ) {
      
      // failed to rename
      SG_error("ms_client_rename( '%s', '%s' ) rc = %d\n", old_path, new_path, rc );
      return rc;
   }
   
   return rc;
}


// ask another gateway to rename an inode, if the inode is a file.
// if the inode is a directory, just ask the MS directly.
// return 0 on success 
// return -ENOMEM on OOM 
// return -errno if we had a network error 
// NOTE: inode->entry should be write-locked by fskit
static int UG_fs_rename_remote( struct SG_gateway* gateway, struct fskit_entry* old_parent, char const* fs_path, struct UG_inode* inode, struct fskit_entry* new_parent, char const* new_path, struct UG_inode* new_inode ) {
   
   int rc = 0;
   
   SG_messages::Request req;
   SG_messages::Reply reply;
   struct SG_request_data reqdat;
   
   int64_t manifest_mtime_sec = 0;
   int32_t manifest_mtime_nsec = 0;
   
   // if this is a directory, then this is a "local" rename--i.e. we have the ability to ask the MS directly, since the MS is the coordinator of all directories.
   if( fskit_entry_get_type( inode->entry ) == FSKIT_ENTRY_TYPE_DIR ) {
      
      return UG_fs_rename_local( gateway, old_parent, fs_path, inode, new_parent, new_path, new_inode );
   }
   
   SG_manifest_get_modtime( UG_inode_manifest( *inode ), &manifest_mtime_sec, &manifest_mtime_nsec );
   
   rc = SG_request_data_init_manifest( gateway, fs_path, UG_inode_file_id( *inode ), UG_inode_file_version( *inode ), manifest_mtime_sec, manifest_mtime_nsec, &reqdat );
   if( rc != 0 ) {
      
      // OOM
      return rc;
   }
   
   rc = SG_client_request_RENAME_setup( gateway, &req, &reqdat, new_path );
   if( rc != 0 ) {
      
      // OOM 
      SG_error("SG_client_request_RENAME_setup('%s') rc = %d\n", fs_path, rc );
      SG_request_data_free( &reqdat );
      return rc;
   }
   
   SG_request_data_free( &reqdat );
   
   rc = SG_client_request_send( gateway, UG_inode_coordinator_id( *inode ), &req, NULL, &reply );
   if( rc != 0 ) {
      
      // network error 
      SG_error("SG_client_request_send(RENAME '%s' to '%s') rc = %d\n", fs_path, new_path, rc );
      return rc;
   }
   
   if( reply.error_code() != 0 ) {
      
      // failed to process
      SG_error("SG_client_request_send(DETACH '%s' to '%s') reply error = %d\n", fs_path, new_path, rc );
      return rc;
   }
   
   return rc;
}


// fskit route for renaming a file or directory.
// In the UG, this simply tells the MS to rename the entry if we're the coordinator, or tell the coordinator to do so if we're not.
// return 0 on success
// return -ENOMEM on OOM 
// return -errno if we had a network error 
static int UG_fs_rename( struct fskit_core* fs, struct fskit_route_metadata* route_metadata, struct fskit_entry* fent, char const* new_path, struct fskit_entry* dest ) {
   
   int rc = 0;
   struct UG_inode* inode = (struct UG_inode*)fskit_entry_get_user_data( fent );
   struct UG_inode* new_inode = NULL;
   struct SG_gateway* gateway = (struct SG_gateway*)fskit_core_get_user_data( fs );
   char* path = fskit_route_metadata_path( route_metadata );
   
   if( dest != NULL ) {
      
      new_inode = (struct UG_inode*)fskit_entry_get_user_data( dest );
   }
   
   UG_try_or_coordinate( gateway, path, UG_inode_coordinator_id( *inode ),
                         UG_fs_rename_remote( gateway, fskit_route_metadata_parent( route_metadata ), path, inode, fskit_route_metadata_new_parent( route_metadata ), new_path, new_inode ),
                         UG_fs_rename_local( gateway, fskit_route_metadata_parent( route_metadata ), path, inode, fskit_route_metadata_new_parent( route_metadata ), new_path, new_inode ),
                         &rc );
   
   return rc;
}

// insert fskit entries into the fskit core 
// return 0 on success.
// return -ENOMEM on OOM 
int UG_fs_install_methods( struct fskit_core* core ) {
   
   int rh = 0;
   
   rh = fskit_route_stat( core, FSKIT_ROUTE_ANY, UG_fs_stat, FSKIT_CONCURRENT );
   if( rh < 0 ) {
      
      SG_error("fskit_route_stat(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      return rh;
   }
   
   rh = fskit_route_mkdir( core, FSKIT_ROUTE_ANY, UG_fs_mkdir, FSKIT_INODE_SEQUENTIAL );
   if( rh < 0 ) {
      
      SG_error("fskit_route_mkdir(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      return rh;
   }
   
   rh = fskit_route_create( core, FSKIT_ROUTE_ANY, UG_fs_create, FSKIT_INODE_SEQUENTIAL );
   if( rh < 0 ) {
      
      SG_error("fskit_route_create(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      return rh;
   }
   
   rh = fskit_route_open( core, FSKIT_ROUTE_ANY, UG_fs_open, FSKIT_CONCURRENT );
   if( rh < 0 ) {
      
      SG_error("fskit_route_open(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      return rh;
   }
   
   rh = fskit_route_read( core, FSKIT_ROUTE_ANY, UG_read, FSKIT_CONCURRENT );
   if( rh < 0 ) {
      
      SG_error("fskit_route_read(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      return rh;
   }
   
   rh = fskit_route_write( core, FSKIT_ROUTE_ANY, UG_write, FSKIT_CONCURRENT );
   if( rh < 0 ) {
      
      SG_error("fskit_route_write(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      return rh;
   }
   
   rh = fskit_route_trunc( core, FSKIT_ROUTE_ANY, UG_fs_trunc, FSKIT_INODE_SEQUENTIAL );
   if( rh < 0 ) {
      
      SG_error("fskit_route_trunc(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      return rh;
   }
      
   rh = fskit_route_close( core, FSKIT_ROUTE_ANY, UG_fs_close, FSKIT_CONCURRENT );
   if( rh < 0 ) {
      
      SG_error("fskit_route_close(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      return rh;
   }
   
   rh = fskit_route_sync( core, FSKIT_ROUTE_ANY, UG_fsync, FSKIT_CONCURRENT );
   if( rh < 0 ) {
      
      SG_error("fskit_route_sync(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      return rh;
   }
   
   rh = fskit_route_detach( core, FSKIT_ROUTE_ANY, UG_fs_detach, FSKIT_CONCURRENT );
   if( rh < 0 ) {
      
      SG_error("fskit_route_detach(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      return rh;
   }
   
   rh = fskit_route_rename( core, FSKIT_ROUTE_ANY, UG_fs_rename, FSKIT_INODE_SEQUENTIAL );
   if( rh < 0 ) {
      
      SG_error("fskit_route_rename(%s) rc = %d\n", FSKIT_ROUTE_ANY, rh );
      return rh;
   }
   
   return 0;
}


// remove all fskit methods 
int UG_fs_uninstall_methods( struct fskit_core* fs ) {
   
   return fskit_unroute_all( fs );
}
