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

#include "cache.h"
#include "core.h"
#include "driver.h"
#include "map-info.h"

// generate a file ID from a path 
static uint64_t AG_cache_file_id( char const* path) {
   uint64_t file_id = (uint64_t)md_hash( path );
   return file_id;
}

// fetch a serialized manifest.
int AG_cache_get_manifest( struct AG_state* state, char const* path, int64_t file_version, int64_t mtime_sec, int32_t mtime_nsec, char** serialized_manifest, size_t* serialized_manifest_len ) {
   
   char* manifest_path = md_fullpath( path, "/manifest", NULL );
   if( manifest_path == NULL ) {
      return -ENOMEM;
   }
   
   int rc = AG_cache_get_block( state, manifest_path, file_version, (uint64_t)mtime_sec, (int64_t)mtime_nsec, serialized_manifest, serialized_manifest_len );
   
   free( manifest_path );
   return rc;
}

// promote a serialized manifest
int AG_cache_promote_manifest( struct AG_state* state, char const* path, int64_t file_version, int64_t mtime_sec, int32_t mtime_nsec ) {
   
   char* manifest_path = md_fullpath( path, "/manifest", NULL );
   if( manifest_path == NULL ) {
      return -ENOMEM;
   }
   
   int rc = AG_cache_promote_block( state, manifest_path, file_version, (uint64_t)mtime_sec, (int64_t)mtime_nsec );
   
   free( manifest_path );
   return rc;
}

// put a manifest asynchronously 
// NOTE: the cache takes ownership of the serialized manifest!
int AG_cache_put_manifest_async( struct AG_state* state, char const* path, int64_t file_version, int64_t mtime_sec, int32_t mtime_nsec, char* serialized_manifest, size_t serialized_manifest_len ) {
   
   char* manifest_path = md_fullpath( path, "/manifest", NULL );
   if( manifest_path == NULL ) {
      return -ENOMEM;
   }
   
   int rc = AG_cache_put_block_async( state, manifest_path, file_version, (uint64_t)mtime_sec, (int64_t)mtime_nsec, serialized_manifest, serialized_manifest_len );
   
   free( manifest_path );
   return rc;
}

// evict a manifest 
int AG_cache_evict_manifest( struct AG_state* state, char const* path, int64_t file_version, int64_t mtime_sec, int32_t mtime_nsec ) {
   
   char* manifest_path = md_fullpath( path, "/manifest", NULL );
   if( manifest_path == NULL ) {
      return -ENOMEM;
   }
   
   int rc = AG_cache_evict_block( state, manifest_path, file_version, (uint64_t)mtime_sec, (int64_t)mtime_nsec );
   
   free( manifest_path );
   return rc;
}

// get a block from the cache 
int AG_cache_get_block( struct AG_state* state, char const* path, int64_t file_version, uint64_t block_id, int64_t block_version, char** block, size_t* block_len ) {
   
   // convert path into a file_id 
   uint64_t file_id = AG_cache_file_id( path );

   int fd = md_cache_open_block( state->cache, file_id, file_version, block_id, block_version, O_RDONLY );
   if( fd < 0 ) {
      
      if( fd != -ENOENT ) {
         SG_error("md_cache_open_block(%s.%" PRId64 ".%" PRIu64 ".%" PRId64 ") rc = %d\n", path, file_version, block_id, block_version, fd );
      }
      else {
         SG_debug("CACHE MISS %s.%" PRId64 ".%" PRIu64 ".%" PRId64 "\n", path, file_version, block_id, block_version );
      }
      
      return fd;
   }
   
   ssize_t nr = md_cache_read_block( fd, block );
   
   if( nr < 0 ) {
      
      SG_error("md_cache_read_block(%s.%" PRId64 ".%" PRIu64 ".%" PRId64 ") rc = %d\n", path, file_version, block_id, block_version, fd );
      
      close( fd );
      return (int)nr;
   }
   
   *block_len = (size_t)nr;
   
   close( fd );
   
   SG_debug("CACHE HIT %s.%" PRId64 ".%" PRIu64 ".%" PRId64 "\n", path, file_version, block_id, block_version );
   
   return 0;
}

// promote a block in the cache 
int AG_cache_promote_block( struct AG_state* state, char const* path, int64_t file_version, uint64_t block_id, int64_t block_version ) {
   
   // convert path into a file_id 
   uint64_t file_id = AG_cache_file_id( path );

   int rc = md_cache_promote_block( state->cache, file_id, file_version, block_id, block_version );
   if( rc != 0 ) {
      SG_error("md_cache_promote_block(%s.%" PRId64 ".%" PRIu64 ".%" PRId64 ") rc = %d\n", path, file_version, block_id, block_version, rc );
   }
   
   return rc;
}

// put a block into the cache (asynchronously)
// NOTE: the cache takes ownership of the block!
int AG_cache_put_block_async( struct AG_state* state, char const* path, int64_t file_version, uint64_t block_id, int64_t block_version, char* block, size_t block_len ) {
   
   // convert path into a file_id 
   uint64_t file_id = AG_cache_file_id( path );

   int rc = 0;
   
   struct md_cache_block_future* block_fut = md_cache_write_block_async( state->cache, file_id, file_version, block_id, block_version, block, block_len, true, &rc );
   
   if( block_fut == NULL || rc != 0 ) {
      
      SG_error("md_cache_write_block_async(%s.%" PRId64 ".%" PRIu64 ".%" PRId64 ") rc = %d\n", path, file_version, block_id, block_version, rc );
      return rc;
   }
   
   // NOTE: block_fut will get freed internally, since the write is detached from the caller
   return rc;
}

// evict a block
int AG_cache_evict_block( struct AG_state* state, char const* path, int64_t file_version, uint64_t block_id, int64_t block_version ) {
   
   // convert path into a file_id 
   uint64_t file_id = AG_cache_file_id( path );

   int rc = 0;
   
   rc = md_cache_evict_block( state->cache, file_id, file_version, block_id, block_version );
   
   if( rc != 0 ) {
      SG_error("md_cache_evict_block(%s.%" PRId64 ".%" PRIu64 ".%" PRId64 ") rc = %d\n", path, file_version, block_id, block_version, rc );
   }
   
   SG_debug("CACHE EVICT %s.%" PRId64 ".%" PRIu64 ".%" PRId64 " rc = %d\n", path, file_version, block_id, block_version, rc );
   
   return rc;
}


// evict a file's worth of blocks and stats
int AG_cache_evict_file( struct AG_state* state, char const* path, int64_t file_version ) {
   
   int rc = 0;
   
   uint64_t file_id = AG_cache_file_id( path );
   
   // evict all blocks
   rc = md_cache_evict_file( state->cache, file_id, file_version );
   if( rc != 0 ) {
      if( rc != -ENOENT ) {
         SG_error("md_cache_evict_file(%s.%" PRId64 ") rc = %d\n", path, file_version, rc );
      }
   }
   
   // evict the manifest 
   char* manifest_path = md_fullpath( path, "/manifest", NULL );
   uint64_t manifest_id = AG_cache_file_id( manifest_path );
   free( manifest_path );
   
   rc = md_cache_evict_file( state->cache, manifest_id, file_version );
   if( rc != 0 ) {
      SG_error("md_cache_evict_file(%s/manifest.%" PRId64 ") rc = %d\n", path, file_version, rc );
   }
   
   return rc;
}


// load a line of cached metadata.
// The line format is encoded as type:HTTP request, since it largely stores the same information.
// type is either "f" or "d", for file or directory.
static int AG_MS_cache_unserialize_line( char const* buf, char** path, int* type, uint64_t* file_id, int64_t* file_version ) {
   
   int rc = 0;
   struct timespec ts;              // ignored
   uint64_t volume_id = 0;          // ignored
   uint64_t block_id = 0;           // ignored
   int64_t block_version = 0;       // ignored
   
   if( strlen(buf) <= 2 ) {
      // need at least type:
      return -EINVAL;
   }
   
   memset( &ts, 0, sizeof(struct timespec) );
   
   // get the type 
   char typec = buf[0];
   char delim = buf[1];
   
   if( delim != ':' || (typec != 'd' && typec != 'f') ) {
      return -EINVAL;
   }
   
   rc = md_HTTP_parse_url_path( buf + 2, &volume_id, path, file_id, file_version, &block_id, &block_version, &ts );
   if( rc != 0 ) {
      SG_error("md_HTTP_parse_url_path(%s) rc = %d\n", buf, rc );
      return rc;
   }
   
   *type = (typec == 'f' ? MD_ENTRY_FILE : MD_ENTRY_DIR);
   
   return 0;
}


// serialize a line of cached data (put it into *buf)
static int AG_MS_cache_serialize_line( char** buf, char const* path, int type, uint64_t file_id, int64_t file_version ) {
   
   char type_str[3];
   if( type == MD_ENTRY_FILE ) {
      strcpy( type_str, "f:");
   }
   else {
      strcpy( type_str, "d:");
   }
   
   char* ret = md_url_public_block_url( type_str, 0, path, file_id, file_version, 0, 0 );
   if( ret == NULL ) {
      return -ENOMEM;
   }
   
   *buf = ret;
   return 0;
}

// load cached metadata obtained from the MS 
// if successful, add the cached data to ms_cache (creating map_info entries if needed)
// ms_cache should be empty when this method is called.
int AG_MS_cache_load( char const* file_path, AG_fs_map_t* ms_cache ) {
   
   SG_debug("Load MS cache from %s\n", file_path);
   
   int rc = 0;
   FILE* f = NULL;
   char* line_buf = NULL;
   int num_read = 0;
   
   f = fopen( file_path, "r" );
   if( f == NULL ) {
      
      rc = -errno;
      SG_error("fopen(%s) rc = %d\n", file_path, rc );
      return rc;
   }
   
   // read each line and parse it 
   // make sure there are no duplicates
   
   while( true ) {
      
      errno = 0;
      
      size_t n = 0;
      ssize_t nr = getline( &line_buf, &n, f );
      
      if( nr < 0 ) {
         
         rc = -errno;
         if( rc < 0 ) {
            SG_error("getline(%s) rc = %d\n", file_path, rc );
         }
         
         free( line_buf );
         
         // otherwise EOF
         break;
      }
      
      // clear \n at the end 
      line_buf[n-1] = 0;
      
      // parse 
      char* path = NULL;
      uint64_t file_id = 0;
      int64_t file_version = 0;
      int type = 0;
      
      rc = AG_MS_cache_unserialize_line( line_buf, &path, &type, &file_id, &file_version );
      
      if( rc != 0 ) {
         SG_error("AG_MS_cache_unserialize_line(%s) rc = %d\n", line_buf, rc );
         
         free( line_buf );
         break;
      }
      
      string path_s( path );
      
      AG_fs_map_t::iterator itr = ms_cache->find( path_s );
      if( itr != ms_cache->end() ) {
         
         // duplicate 
         SG_error("Duplicate path entry '%s'\n", path );
         rc = -EEXIST;
         
         free( path );
         free( line_buf );
         break;
      }
      
      // fill in requisite data 
      struct AG_map_info* mi = SG_CALLOC( struct AG_map_info, 1 );
      if( mi == NULL ) {
         rc = -ENOMEM;
         
         free( path );
         free( line_buf );
         break;
      }
      
      AG_map_info_init( mi, type, NULL, 0, 0, NULL );
      
      mi->file_id = file_id;
      mi->file_version = file_version;
      mi->cache_valid = true;
      
      (*ms_cache)[ path_s ] = mi;
      
      free( path );
      free( line_buf );
      
      num_read++;
   }
   
   fclose( f );
   
   // validate, even if we couldn't parse everything
   rc = AG_validate_map_info( ms_cache );
   if( rc != 0 ) {
      SG_error("AG_validate_map_info rc = %d\n", rc);
      
      // deallocate--this isn't valid 
      AG_fs_map_free( ms_cache );
   }
   else {
      SG_debug("Loaded %d lines from %s\n", num_read, file_path );
   }
   
   return rc;
}


// store cached data to a given file 
int AG_MS_cache_store( char const* file_path, AG_fs_map_t* ms_cache ) {
   
   int rc = 0;
   FILE* f = NULL;
   char* line_buf = NULL;
   size_t nw = 0;
   size_t len = 0;
   int num_lines = 0;
   
   f = fopen( file_path, "w" );
   if( f == NULL ) {
      
      rc = -errno;
      SG_error( "fopen(%s) errno = %d\n", file_path, rc );
      return rc;
   }
   
   for( AG_fs_map_t::iterator itr = ms_cache->begin(); itr != ms_cache->end(); itr++ ) {
      
      if( !itr->second->cache_valid ) {
         continue;
      }
      
      rc = AG_MS_cache_serialize_line( &line_buf, itr->first.c_str(), itr->second->type, itr->second->file_id, itr->second->file_version );
      if( rc != 0 ) {
         SG_error("AG_MS_cache_serialize_line(%s) rc = %d\n", itr->first.c_str(), rc );
         break;
      }
      
      len = strlen(line_buf);
      
      nw = fwrite( line_buf, 1, len, f );
      if( nw != len ) {
         
         rc = -errno;
         SG_error("fwrite(%s) errno = %d, ferror = %d\n", itr->first.c_str(), rc, ferror(f) );
         
         free( line_buf );
         break;
      }
      
      nw = fwrite( "\n", 1, 1, f );
      if( nw != 1 ) {
         
         rc = -errno;
         SG_error("fwrite(\n) errno = %d, ferror = %d\n", rc, ferror(f) );
         
         free( line_buf );
         break;
      }
      
      free( line_buf );
      num_lines++;
   }
   
   fflush( f );
   fclose( f );
   
   SG_debug("Wrote %d entries to %s\n", num_lines, file_path );
   
   return rc;
}
