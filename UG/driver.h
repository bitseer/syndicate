/*
   Copyright 2014 The Trustees of Princeton University

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

#ifndef _DRIVER_H_
#define _DRIVER_H_

#include "libsyndicate/libsyndicate.h"
#include "libsyndicate/closure.h"
#include "fs_entry.h"

#include <dlfcn.h>

#define DRIVER_NOT_GARBAGE 1

// driver callback signatures
typedef int (*driver_connect_cache_func)( struct fs_core*, struct md_closure*, CURL*, char const*, void* );
typedef int (*driver_write_block_preup_func)( struct fs_core*, struct md_closure*, char const*, struct fs_entry*, uint64_t, int64_t, char const*, size_t, char**, size_t*, void* );
typedef int (*driver_write_manifest_preup_func)( struct fs_core*, struct md_closure*, char const*, struct fs_entry*, int64_t, int32_t, char const*, size_t, char**, size_t*, void* );
typedef ssize_t (*driver_read_block_postdown_func)( struct fs_core*, struct md_closure*, char const*, struct fs_entry*, uint64_t, int64_t, char const*, size_t, char*, size_t, void* );
typedef int (*driver_read_manifest_postdown_func)( struct fs_core*, struct md_closure*, char const*, struct fs_entry*, int64_t, int32_t, char const*, size_t, char**, size_t*, void* );
typedef int (*driver_chcoord_begin_func)( struct fs_core*, struct md_closure*, char const*, struct fs_entry*, int64_t, void* );
typedef int (*driver_chcoord_end_func)( struct fs_core*, struct md_closure*, char const*, struct fs_entry*, int64_t, int, void* );
typedef int (*driver_garbage_collect_func)( struct fs_core*, struct md_closure*, char const*, struct replica_snapshot*, uint64_t*, int64_t*, size_t );
typedef int (*driver_create_file_func)( struct fs_core*, struct md_closure*, char const*, struct fs_entry* );
typedef int (*driver_delete_file_func)( struct fs_core*, struct md_closure*, char const*, struct fs_entry* );
typedef char* (*driver_get_name_func)( void );

// for connecting to the cache providers
struct driver_connect_cache_cls {
   struct fs_core* core;
   struct ms_client* client;
};

// for reading a manifest 
struct driver_read_manifest_postdown_cls {
   struct fs_core* core;
   char const* fs_path;
   struct fs_entry* fent;
   int64_t manifest_mtime_sec;
   int32_t manifest_mtime_nsec;
};

extern "C" {
   
// driver control API
int driver_init( struct fs_core* core, struct md_closure** closure );
int driver_reload( struct fs_core* core, struct md_closure* closure );
int driver_shutdown( struct md_closure* closure );

// UG calls these methods to access the driver...

// called by libsyndicate (md_download_*()), so they can't take fs_core as an argument
int driver_connect_cache( struct md_closure* closure, CURL* curl, char const* url, void* cls );

int driver_read_manifest_postdown( struct md_closure* closure, char const* in_manifest_data, size_t in_manifest_data_len, char** out_manifest_data, size_t* out_manifest_data_len, void* user_cls );

// called by creat(), mknod(), or open() with O_CREAT
int driver_create_file( struct fs_core* core, struct md_closure* closure, char const* fs_path, struct fs_entry* fent );

// called by unlink()
int driver_delete_file( struct fs_core* core, struct md_closure* closure, char const* fs_path, struct fs_entry* fent );

// called by read(), write(), and trunc()
int driver_write_block_preup( struct fs_core* core, struct md_closure* closure, char const* fs_path, struct fs_entry* fent, uint64_t block_id, int64_t block_version,
                              char const* in_block_data, size_t in_block_data_len, char** out_block_data, size_t* out_block_data_len );
int driver_write_manifest_preup( struct fs_core* core, struct md_closure* closure, char const* fs_path, struct fs_entry* fent, int64_t manifest_mtime_sec, int32_t manifest_mtime_nsec,
                                 char const* in_manifest_data, size_t in_manifest_data_len, char** out_manifest_data, size_t* out_manifest_data_len );
ssize_t driver_read_block_postdown( struct fs_core* core, struct md_closure* closure, char const* fs_path, struct fs_entry* fent, uint64_t block_id, int64_t block_version,
                                    char const* in_block_data, size_t in_block_data_len, char* out_block_data, size_t out_block_data_len );

// called by chcoord()
int driver_chcoord_begin( struct fs_core*, struct md_closure* closure, char const* fs_path, struct fs_entry* fent, int64_t replica_version );
int driver_chcoord_end( struct fs_core*, struct md_closure* closure, char const* fs_path, struct fs_entry* fent, int64_t replica_version, int chcoord_status );

// called by garbage_collect()
int driver_garbage_collect( struct fs_core* core, struct md_closure* closure, char const* fs_path, struct replica_snapshot* fent_snapshot, uint64_t* block_ids, int64_t* block_versions, size_t num_blocks );

// called for logging 
char* driver_get_name( struct fs_core* core, struct md_closure* closure );

extern struct md_closure_callback_entry UG_CLOSURE_PROTOTYPE[];

}

#endif
