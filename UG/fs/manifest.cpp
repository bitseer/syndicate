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

#include "manifest.h"

// get the pointer at a particular offset into a list of hashes
static inline unsigned char* hash_at( unsigned char* hashes, uint64_t block_offset ) {
   return hashes + (block_offset) * BLOCK_HASH_LEN();
}

// public interface to hash_at 
unsigned char* fs_entry_manifest_block_hash_ref( unsigned char* hashes, uint64_t block_offset ) {
   return hash_at( hashes, block_offset );
}

// default constructor
block_url_set::block_url_set() {
   this->file_id = 0;
   this->start_id = -1;
   this->end_id = -1;
   this->block_versions = NULL;
   this->file_version = -1;
   this->block_hashes = NULL;
}

// value constructor
block_url_set::block_url_set( uint64_t volume_id, uint64_t gateway_id, uint64_t file_id, int64_t file_version, uint64_t start, uint64_t end, int64_t* bv, unsigned char* hashes ) {
   this->init( volume_id, gateway_id, file_id, file_version, start, end, bv, hashes );
}


// copy constructor
block_url_set::block_url_set( block_url_set& bus ) {
   this->init( bus.volume_id, bus.gateway_id, bus.file_id, bus.file_version, bus.start_id, bus.end_id, bus.block_versions, bus.block_hashes );
}


// initialization
void block_url_set::init( uint64_t volume_id, uint64_t gateway_id, uint64_t file_id, int64_t file_version, uint64_t start, uint64_t end, int64_t* bv, unsigned char* hashes ) {
   this->file_id = file_id;
   this->gateway_id = gateway_id;
   this->volume_id = volume_id;
   this->start_id = start;
   this->end_id = end;
   this->file_version = file_version;
   this->block_versions = SG_CALLOC( int64_t, end - start );
   memcpy( this->block_versions, bv, sizeof(int64_t) * (end - start) );
   
   this->block_hashes = SG_CALLOC( unsigned char, (end - start) * BLOCK_HASH_LEN() );
   memcpy( this->block_hashes, hashes, (end - start) * BLOCK_HASH_LEN() );
}


// destructor
block_url_set::~block_url_set() {
   if( this->block_versions ) {
      free( this->block_versions );
      this->block_versions = NULL;
   }
   if( this->block_hashes ) {
      free( this->block_hashes );
      this->block_hashes = NULL;
   }
   this->start_id = -1;
   this->end_id = -1;
}


// look up the version
int64_t block_url_set::lookup_version( uint64_t block_id ) {
   if( this->in_range( block_id ) )
      return this->block_versions[ block_id - this->start_id ];
   else
      return -1;
}

// look up the gateway
uint64_t block_url_set::lookup_gateway( uint64_t block_id ) {
   if( this->in_range( block_id ) )
      return this->gateway_id;
   else
      return 0;
}

// compare a hash 
int block_url_set::hash_cmp( uint64_t block_id, unsigned char* hash ) {
   if( this->in_range( block_id ) ) {
      int rc = memcmp( hash_at( this->block_hashes, block_id - this->start_id ), hash, BLOCK_HASH_LEN() );
      if( rc != 0 ) {
         unsigned char* expected_hash = SG_CALLOC( unsigned char, BLOCK_HASH_LEN() );
         memcpy( expected_hash, hash_at( this->block_hashes, block_id - this->start_id ), BLOCK_HASH_LEN() );
         
         char* expected_hash_printable = BLOCK_HASH_TO_STRING( expected_hash );
         char* got_hash_printable = BLOCK_HASH_TO_STRING( hash );
         
         SG_error("hash mismatch for %" PRIX64 "[%" PRId64 "]: expected %s, got %s\n", this->file_id, block_id, expected_hash_printable, got_hash_printable );
         
         free( expected_hash );
         free( expected_hash_printable );
         free( got_hash_printable );
         
         return 1;
      }
      else {
         return 0;
      }
   }
   else {
      return -ENOENT;
   }
}

// duplicate a hash
unsigned char* block_url_set::hash_dup( uint64_t block_id ) {
   if( this->in_range( block_id ) ) {
      unsigned char* ret = SG_CALLOC( unsigned char, BLOCK_HASH_LEN() );
      memcpy( ret, hash_at( this->block_hashes, block_id - this->start_id ), BLOCK_HASH_LEN() );
      return ret;
   }
   else {
      return NULL;
   }
}

// is a block id in a range?
bool block_url_set::in_range( uint64_t block_id ) { return (block_id >= this->start_id && block_id < this->end_id); }

// is a block appendable?
bool block_url_set::is_appendable( uint64_t vid, uint64_t gid, uint64_t fid, uint64_t block_id ) {
   return this->volume_id == vid && this->gateway_id == gid && this->file_id == fid && block_id == this->end_id;
}

// is a block prependable?
bool block_url_set::is_prependable( uint64_t vid, uint64_t gid, uint64_t fid, uint64_t block_id ) {
   return this->volume_id == vid && this->gateway_id == gid && this->file_id == fid && block_id + 1 == this->start_id;
}

// size of a block range
uint64_t block_url_set::size() { return this->end_id - this->start_id; }

// append a block to a set
bool block_url_set::append( uint64_t vid, uint64_t gid, uint64_t fid, uint64_t block_id, int64_t block_version, unsigned char* hash ) {
   if( this->is_appendable( vid, gid, fid, block_id ) ) {
      this->end_id++;
      
      int64_t* new_versions = (int64_t*)realloc( this->block_versions, (this->end_id - this->start_id) * sizeof(int64_t) );
      new_versions[ this->end_id - 1 - this->start_id ] = block_version;
      
      unsigned char* new_hashes = (unsigned char*)realloc( this->block_hashes, (this->end_id - this->start_id) * BLOCK_HASH_LEN() );
      memcpy( hash_at( new_hashes, this->end_id - 1 - this->start_id ), hash, BLOCK_HASH_LEN() );
      
      this->block_versions = new_versions;
      this->block_hashes = new_hashes;

      return true;
   }
   else {
      return false;
   }
}


// prepend a block to a set
bool block_url_set::prepend( uint64_t vid, uint64_t gid, uint64_t fid, uint64_t block_id, int64_t block_version, unsigned char* hash ) {
   if( this->is_prependable( vid, gid, fid, block_id ) ) {
      // shift everyone down
      this->start_id--;
      
      int64_t* new_versions = SG_CALLOC( int64_t, this->end_id - this->start_id );
      new_versions[ 0 ] = block_version;
      memcpy( new_versions + 1, this->block_versions, sizeof(int64_t) * (this->end_id - this->start_id - 1) );

      unsigned char* new_hashes = SG_CALLOC( unsigned char, (this->end_id - this->start_id) * BLOCK_HASH_LEN() );
      memcpy( new_hashes, hash, BLOCK_HASH_LEN() );
      memcpy( hash_at( new_hashes, 1 ), this->block_hashes, (this->end_id - this->start_id - 1) * BLOCK_HASH_LEN() );
      
      free( this->block_versions);
      this->block_versions = new_versions;

      free( this->block_hashes );
      this->block_hashes = new_hashes;
      
      return true;
   }
   else {
      return false;
   }
}


// truncate a block set.  return true on success
bool block_url_set::truncate_smaller( uint64_t new_end_id ) {
   if( this->in_range( new_end_id ) ) {
      this->end_id = new_end_id;
      return true;
   }
   else {
      return false;
   }
}

// shrink one unit from the left
bool block_url_set::shrink_left() {
   if( this->start_id + 1 >= this->end_id )
      return false;

   this->start_id++;

   int64_t* new_versions = SG_CALLOC( int64_t, this->end_id - this->start_id );

   memcpy( new_versions, this->block_versions + 1, sizeof(int64_t) * (this->end_id - this->start_id) );
   free( this->block_versions );
   this->block_versions = new_versions;

   unsigned char* new_hashes = SG_CALLOC( unsigned char, (this->end_id - this->start_id) * BLOCK_HASH_LEN() );
   
   memcpy( new_hashes, hash_at( this->block_hashes, 1 ), (this->end_id - this->start_id) * BLOCK_HASH_LEN() );
   free( this->block_hashes );
   this->block_hashes = new_hashes;
   
   return true;
}


// shrink one unit from the right
bool block_url_set::shrink_right() {
   if( this->start_id + 1 >= this->end_id )
      return false;

   // TODO: realloc instead?
   
   this->end_id--;
   int64_t* new_versions = SG_CALLOC( int64_t, this->end_id - this->start_id );

   memcpy( new_versions, this->block_versions, sizeof(int64_t) * (this->end_id - this->start_id) );
   free( this->block_versions );
   this->block_versions = new_versions;

   unsigned char* new_hashes = SG_CALLOC( unsigned char, (this->end_id - this->start_id) * BLOCK_HASH_LEN() );
   
   memcpy( new_hashes, this->block_hashes, (this->end_id - this->start_id) * BLOCK_HASH_LEN() );
   free( this->block_hashes );
   this->block_hashes = new_hashes;
   
   return true;
}


// split to the left
block_url_set* block_url_set::split_left( uint64_t block_id ) {
   block_url_set* ret = new block_url_set( this->volume_id, this->gateway_id, this->file_id, this->file_version, this->start_id, block_id, this->block_versions, this->block_hashes );
   return ret;
}


// split to the right
block_url_set* block_url_set::split_right( uint64_t block_id ) {
   int64_t off = (int64_t)(block_id) - this->start_id + 1;
   block_url_set* ret = new block_url_set( this->volume_id, this->gateway_id, this->file_id, this->file_version, block_id+1, this->end_id, this->block_versions + off, hash_at( this->block_hashes, off ) );
   return ret;
}


// populate a protobuf representation of ourself
void block_url_set::as_protobuf( struct fs_core* core, Serialization::BlockURLSetMsg* busmsg ) {
   busmsg->set_start_id( this->start_id );
   busmsg->set_end_id( this->end_id );
   busmsg->set_gateway_id( this->gateway_id );
   for( uint64_t id = this->start_id; id < this->end_id; id++ ) {
      busmsg->add_block_versions( this->block_versions[id - this->start_id] );
      busmsg->add_block_hashes( string( (char*)hash_at(this->block_hashes, id - this->start_id), BLOCK_HASH_LEN() ) );
   }
}

// default constructor
file_manifest::file_manifest() {
   this->lastmod.tv_sec = 1;
   this->lastmod.tv_nsec = 1;
   this->file_version = -1;
   this->stale = true;
   this->initialized = false;
   pthread_rwlock_init( &this->manifest_lock, NULL );
}

// default constructor
file_manifest::file_manifest( int64_t version ) {
   this->file_version = version;
   this->stale = true;
   this->initialized = false;
   this->lastmod.tv_sec = 1;
   this->lastmod.tv_nsec = 1;
   pthread_rwlock_init( &this->manifest_lock, NULL );
}

// destructor
file_manifest::~file_manifest() {
   pthread_rwlock_wrlock( &this->manifest_lock );
   for( block_map::iterator itr = this->block_urls.begin(); itr != this->block_urls.end(); itr++ ) {
      delete itr->second;
   }
   this->block_urls.clear();
   pthread_rwlock_unlock( &this->manifest_lock );
   pthread_rwlock_destroy( &this->manifest_lock );
}

// copy-construct a file manifest
file_manifest::file_manifest( file_manifest& fm ) {
   for( block_map::iterator itr = fm.block_urls.begin(); itr != block_urls.end(); itr++ ) {
      this->block_urls[ itr->first ] = itr->second;
   }
   this->lastmod = fm.lastmod;
   this->file_version = fm.file_version;
   this->stale = fm.stale;
   this->initialized = fm.initialized;
   pthread_rwlock_init( &this->manifest_lock, NULL );
}

file_manifest::file_manifest( file_manifest* fm ) {
   for( block_map::iterator itr = fm->block_urls.begin(); itr != fm->block_urls.end(); itr++ ) {
      this->block_urls[ itr->first ] = itr->second;
   }
   this->lastmod = fm->lastmod;
   this->file_version = fm->file_version;
   this->stale = fm->stale;
   this->initialized = fm->initialized;
   pthread_rwlock_init( &this->manifest_lock, NULL );
}

file_manifest::file_manifest( struct fs_core* core, struct fs_entry* fent, Serialization::ManifestMsg* mmsg ) {
   pthread_rwlock_init( &this->manifest_lock, NULL );
   this->file_version = fent->version;
   this->stale = true;
   file_manifest::parse_protobuf( core, fent, this, mmsg );
}


// set the version
void file_manifest::set_file_version(struct fs_core* core, int64_t version) {
   pthread_rwlock_wrlock( &this->manifest_lock );

   for( block_map::iterator itr = this->block_urls.begin(); itr != this->block_urls.end(); itr++ ) {
      // only for blocks we host 
      if( core->gateway == itr->second->gateway_id )
         itr->second->file_version = version;
   }

   this->file_version = version;
   
   pthread_rwlock_unlock( &this->manifest_lock );
   return;
}

// get the number of blocks
uint64_t file_manifest::get_num_blocks() {
   uint64_t num_blocks = 0;
   pthread_rwlock_rdlock( &this->manifest_lock );
   
   block_map::reverse_iterator last = this->block_urls.rbegin();
   if( last != this->block_urls.rend() ) {
      num_blocks = last->second->end_id;
   }
   
   pthread_rwlock_unlock( &this->manifest_lock );
   return num_blocks;
}

// generate a block URL
char* file_manifest::get_block_url( struct fs_core* core, char const* fs_path, struct fs_entry* fent, uint64_t block_id ) {
   pthread_rwlock_rdlock( &this->manifest_lock );
   block_map::iterator itr = this->find_block_set( block_id );
   if( itr == this->block_urls.end() ) {
      // not found
      pthread_rwlock_unlock( &this->manifest_lock );
      return NULL;
   }
   else {
      int64_t block_version = itr->second->lookup_version( block_id );
      bool local = (itr->second->gateway_id == core->gateway);
      
      pthread_rwlock_unlock( &this->manifest_lock );

      if( block_version == 0 ) {
         // no such block
         SG_error("No version for block %" PRIu64 "\n", block_id );
         return NULL;
      }

      if( local ) {
         return md_url_local_block_url( core->conf->data_root, fent->volume, fent->file_id, fent->version, block_id, block_version );
      }
      else {
         // not hosted here
         if( fs_path != NULL ) {
            char* ret = NULL;
            int rc = md_url_make_block_url( core->ms, fs_path, itr->second->gateway_id, fent->file_id, fent->version, block_id, block_version, &ret );
            if( rc != 0 ) {
               SG_error("md_url_make_block_url( %s %" PRIX64 ".%" PRId64 "[%" PRIu64 ".%" PRId64 "] ) rc = %d\n", fs_path, fent->file_id, fent->version, block_id, block_version, rc );
               return NULL;
            }
            
            return ret;
         }
         else {
            SG_error("%s", "No fs_path given\n");
            return NULL;
         }
      }
   }
   return NULL;
}

uint64_t file_manifest::get_block_host( struct fs_core* core, uint64_t block_id ) {
   pthread_rwlock_rdlock( &this->manifest_lock );
   block_map::iterator itr = this->find_block_set( block_id );
   if( itr == this->block_urls.end() ) {
      // not found
      pthread_rwlock_unlock( &this->manifest_lock );
      return 0;
   }
   else {
      uint64_t ret = itr->second->gateway_id;
      pthread_rwlock_unlock( &this->manifest_lock );
      return ret;
   }
}

// look up a block version, given a block ID
int64_t file_manifest::get_block_version( uint64_t block ) {
   pthread_rwlock_rdlock( &this->manifest_lock );
   block_map::iterator urlset = this->find_block_set( block );
   if( urlset == this->block_urls.end() ) {
      pthread_rwlock_unlock( &this->manifest_lock );
      return 0;     // not found
   }
   else {
      int64_t ret = urlset->second->lookup_version( block );
      pthread_rwlock_unlock( &this->manifest_lock );
      return ret;
   }
}


// get the block versions (a copy)
int64_t* file_manifest::get_block_versions( uint64_t start_id, uint64_t end_id ) {
   if( end_id <= start_id )
      return NULL;

   pthread_rwlock_rdlock( &this->manifest_lock );

   int64_t* ret = SG_CALLOC( int64_t, end_id - start_id );

   int i = 0;
   uint64_t curr_id = start_id;

   while( curr_id < end_id ) {
      block_map::iterator itr = this->find_block_set( curr_id );
      if( itr == this->block_urls.end() ) {
         free( ret );
         ret = NULL;
         break;
      }

      for( uint64_t j = 0; j < itr->second->end_id - itr->second->start_id && curr_id < end_id; j++ ) {
         ret[i] = itr->second->block_versions[j];
         i++;
         curr_id++;
      }
   }

   pthread_rwlock_unlock( &this->manifest_lock );
   return ret;
}



// get one block hash
unsigned char* file_manifest::get_block_hash( uint64_t block_id ) {
   
   pthread_rwlock_rdlock( &this->manifest_lock );

   block_map::iterator itr = this->find_block_set( block_id );
   if( itr == this->block_urls.end() ) {
      pthread_rwlock_unlock( &this->manifest_lock );
      return NULL;
   }
   
   unsigned char* ret = SG_CALLOC( unsigned char, BLOCK_HASH_LEN() );
   memcpy( ret, hash_at( itr->second->block_hashes, block_id - itr->second->start_id ), BLOCK_HASH_LEN() );
   
   pthread_rwlock_unlock( &this->manifest_lock );
   return ret;
}

// get the set of block hashes
unsigned char** file_manifest::get_block_hashes( uint64_t start_id, uint64_t end_id ) {
   if( end_id <= start_id ) {
      return NULL;
   }
   
   pthread_rwlock_rdlock( &this->manifest_lock );

   unsigned char** ret = SG_CALLOC( unsigned char*, end_id - start_id + 1 );

   int i = 0;
   uint64_t curr_id = start_id;

   while( curr_id < end_id ) {
      block_map::iterator itr = this->find_block_set( curr_id );
      if( itr == this->block_urls.end() ) {
         
         SG_FREE_LIST( ret, free );
         ret = NULL;
         break;
      }

      for( uint64_t j = 0; j < itr->second->end_id - itr->second->start_id && curr_id < end_id; j++ ) {
         unsigned char* hash = SG_CALLOC( unsigned char, BLOCK_HASH_LEN() );
         memcpy( hash, hash_at( itr->second->block_hashes, j ), BLOCK_HASH_LEN() );
         
         ret[i] = hash;
         i++;
         curr_id++;
      }
   }

   pthread_rwlock_unlock( &this->manifest_lock );
   return ret;
}


// compare a hash
int file_manifest::hash_cmp( uint64_t block_id, unsigned char* hash ) {
   pthread_rwlock_rdlock( &this->manifest_lock );
   
   block_map::iterator itr = this->find_block_set( block_id );
   if( itr == this->block_urls.end() ) {
      // not found
      pthread_rwlock_unlock( &this->manifest_lock );
      return -ENOENT;
   }
   
   int ret = itr->second->hash_cmp( block_id, hash );

   pthread_rwlock_unlock( &this->manifest_lock );
   return ret;
}


// duplicate a hash
unsigned char* file_manifest::hash_dup( uint64_t block_id ) {
   pthread_rwlock_rdlock( &this->manifest_lock );
   
   block_map::iterator itr = this->find_block_set( block_id );
   if( itr == this->block_urls.end() ) {
      // not found
      pthread_rwlock_unlock( &this->manifest_lock );
      return NULL;
   }
   
   unsigned char* ret = SG_CALLOC( unsigned char, BLOCK_HASH_LEN() );
   memcpy( ret, hash_at( itr->second->block_hashes, block_id - itr->second->start_id ), BLOCK_HASH_LEN() );

   pthread_rwlock_unlock( &this->manifest_lock );
   return ret;
}


// is a block potentially cached locally?
// as in, are we the last known writer of this block?
int file_manifest::is_block_local( struct fs_core* core, uint64_t block_id ) {
   int ret = 0;
   
   pthread_rwlock_rdlock( &this->manifest_lock );

   block_map::iterator itr = this->find_block_set( block_id );
   if( itr != this->block_urls.end() ) {
      ret = (core->gateway == itr->second->gateway_id ? 1 : 0);
   }
   else {
      ret = -ENOENT;
   }
   
   pthread_rwlock_unlock( &this->manifest_lock );
   return ret;
}


// find a block set for a block URL
// NEED TO LOCK FIRST!
block_map::iterator file_manifest::find_block_set( uint64_t block ) {

   block_map::iterator itr = this->block_urls.begin();

   while( itr != this->block_urls.end() ) {
      // does this block fall into the range?
      if( itr->second->in_range( block ) ) {
         // found it!
         break;
      }
      itr++;
   }

   return itr;
}


// find the block set with the maximal block ID 
// need to lock first.
block_map::iterator file_manifest::find_end_block_set() {
   
   block_map::iterator itr = this->block_urls.end();
   itr--;
   
   return itr;
}


// attempt to merge two adjacent block url sets, given the block_id that identiifes the block URL set (i.e. can be found)
// return true on success
bool file_manifest::merge_adjacent( uint64_t block_id ) {
   block_map::iterator itr = this->find_block_set( block_id );
   if( itr == this->block_urls.end() )
      // nothing to do
      return false;

   block_map::iterator itr2 = itr;
   itr2++;
   if( itr2 == this->block_urls.end() )
      // nothing to do
      return false;

   block_url_set* left = itr->second;
   block_url_set* right = itr2->second;

   //printf("// merge %s.%lld to %s.%lld?\n", left->file_url, left->file_version, right->file_url, right->file_version );
   if( left->gateway_id == right->gateway_id &&
       left->volume_id == right->volume_id &&
       left->file_id == right->file_id &&
       left->file_version == right->file_version) {
      
      // these block URL sets refer to blocks on the same host.  merge them
      int64_t *bvec = SG_CALLOC( int64_t, right->end_id - left->start_id );
      unsigned char* hashes = SG_CALLOC( unsigned char, (right->end_id - left->start_id) * BLOCK_HASH_LEN() );

      // TODO: optimize into four memcpy() calls
      
      uint64_t i = 0;
      for( uint64_t j = 0; j < left->end_id - left->start_id; j++ ) {
         bvec[i] = left->block_versions[j];
         memcpy( hashes + i * BLOCK_HASH_LEN(), hash_at( left->block_hashes, j ), BLOCK_HASH_LEN() );
         
         i++;
      }
      for( uint64_t j = 0; j < right->end_id - right->start_id; j++ ) {
         bvec[i] = right->block_versions[j];
         memcpy( hashes + i * BLOCK_HASH_LEN(), hash_at( right->block_hashes, j ), BLOCK_HASH_LEN() );
         
         i++;
      }
      
      block_url_set* merged = new block_url_set( left->volume_id, left->gateway_id, left->file_id, left->file_version, left->start_id, right->end_id, bvec, hashes );

      free( bvec );
      free( hashes );

      this->block_urls.erase( itr );
      this->block_urls.erase( itr2 );

      delete left;
      delete right;

      this->block_urls[ merged->start_id ] = merged;

      return true;
   }
   else {
      return false;
   }
}

// get the range information from a block ID
int file_manifest::get_range( uint64_t block_id, uint64_t* start_id, uint64_t* end_id, uint64_t* gateway_id ) {
   block_map::iterator itr = this->find_block_set( block_id );
   if( itr == this->block_urls.end() ) {
      return -ENOENT;
   }

   if( start_id )
      *start_id = itr->second->start_id;

   if( end_id )
      *end_id = itr->second->end_id;

   if( gateway_id )
      *gateway_id = itr->second->gateway_id;

   return 0;
}


// insert a reference to a block into a file manifest
// this advances the manifest's modtime, and marks it as initialized
// fent must be at least read-locked
int file_manifest::put_block( struct fs_core* core, uint64_t gateway, struct fs_entry* fent, uint64_t block_id, int64_t block_version, unsigned char* block_hash ) {
   pthread_rwlock_wrlock( &this->manifest_lock );

   // sanity check
   if( fent->version != this->file_version ) {
      SG_error("Invalid version (%" PRId64 " != %" PRId64 ")\n", this->file_version, fent->version );
      pthread_rwlock_unlock( &this->manifest_lock );
      return -EINVAL;
   }

   block_map::iterator itr = this->find_block_set( block_id );
   int64_t bvec[] = { block_version };

   if( itr == this->block_urls.end() ) {

      //printf("// no block set contains this block\n");

      if( this->block_urls.size() > 0 ) {
         //printf("// block occurs after the end of the file.  Can we append it?\n");
         block_map::iterator last_range_itr = this->find_end_block_set();
         block_url_set* last_range = last_range_itr->second;

         bool rc = last_range->append( core->volume, gateway, fent->file_id, block_id, block_version, block_hash );
         if( !rc ) {
            // printf("// could not append (%" PRIu64 ", %" PRIu64 ", %" PRIX64 ", %" PRIu64 ") to (%" PRIu64 ", %" PRIu64 ", %" PRIX64 ", [%" PRIu64 "-%" PRIu64 "]) to the last block range, so we'll need to make a new one\n",
            //       core->volume, gateway, fent->file_id, block_id, last_range->volume_id, last_range->gateway_id, last_range->file_id, last_range->start_id, last_range->end_id );
            
            // put a range of write-holes, if we need to 
            if( last_range->end_id < block_id ) {
               
               // blank hashes and versions...
               uint64_t num_holes = block_id - last_range->end_id;
               
               int64_t* versions = SG_CALLOC( int64_t, num_holes );
               unsigned char* hashes = SG_CALLOC( unsigned char, num_holes * BLOCK_HASH_LEN() );
               
               // add the write-holes
               this->block_urls[ last_range->end_id ] = new block_url_set( core->volume, 0, fent->file_id, this->file_version, last_range->end_id, block_id, versions, hashes );
               
               free( versions );
               free( hashes );
            }
            
            this->block_urls[ block_id ] = new block_url_set( core->volume, gateway, fent->file_id, this->file_version, block_id, block_id + 1, bvec, block_hash );
         }
         else {
            //printf("// successfully appended this block to the last range!\n");
         }
      }
      else {
         //printf("// we don't have any blocks yet.  put the first one\n");
         this->block_urls[ block_id ] = new block_url_set( core->volume, gateway, fent->file_id, this->file_version, block_id, block_id + 1, bvec, block_hash );
      }
   }

   else {
      
      block_url_set* existing = itr->second;
      block_map::iterator itr_existing = itr;
      //uint64_t existing_block_id = itr->first;

      //printf("// some block set (start_id = %lld) contains this block.\n", existing_block_id);

      if( existing->volume_id == core->volume &&
          existing->gateway_id == gateway &&
          existing->file_id == fent->file_id &&
          existing->file_version == fent->version) {
         
         //printf("// this block URL belongs to this url set.\n");
         //printf("// insert the version\n");
         existing->block_versions[ block_id - existing->start_id ] = block_version;
         memcpy( hash_at( existing->block_hashes, block_id - existing->start_id ), block_hash, BLOCK_HASH_LEN() );
      }
      else {
         //printf("// this block URL does not belong to this block URL set.\n");
         //printf("// It is possible that it belongs in the previous or next URL sets, if the block ID is on the edge\n");

         if( existing->start_id == block_id ) {
            // need to clear the existing block url set? (i.e. will we need to modify it and re-insert it?)
            bool need_clear = true;

            if( itr != this->block_urls.begin() ) {

               //printf("// see if we can insert this block URL into the previous set.\n");
               //printf("// if not, then shift this set to the right and make a new set\n");
               itr--;
               block_url_set* prev_existing = itr->second;

               bool rc = prev_existing->append( core->volume, gateway, fent->file_id, block_id, block_version, block_hash );
               if( !rc ) {
                  //printf("// could not append to the previous block URL set.\n");
                  //printf("// Make a new block URL set and insert it (replacing existing)\n");
                  
                  block_url_set* bus = new block_url_set( core->volume, gateway, fent->file_id, this->file_version, block_id, block_id + 1, bvec, block_hash );
                  this->block_urls[ block_id ] = bus;
               }
            }
            else {
               //printf("// need to insert this block URL set at the beginning (replacing existing)\n");
               
               block_url_set* bus = new block_url_set( core->volume, gateway, fent->file_id, this->file_version, block_id, block_id + 1, bvec, block_hash );
               this->block_urls[ block_id ] = bus;
               
               need_clear = false;
            }

            //printf("// will need to shift the existing block URL set down a slot\n");
            bool rc = existing->shrink_left();
            if( !rc ) {
               delete existing;
            }
            else {
               this->block_urls[ existing->start_id ] = existing;
            }

            if( need_clear )
               this->block_urls.erase( itr_existing );

            // attempt to merge
            this->merge_adjacent( block_id );
         }

         else if( existing->end_id - 1 == block_id ) {
            if( itr != this->block_urls.end() ) {

               //printf("// see if we can insert this block URL into the next set\n");
               //printf("// If not, then shift this set to the left and make a new set\n");
               itr++;

               bool rc = false;
               block_url_set* next_existing = itr->second;

               if( itr != this->block_urls.end() ) {
                  rc = next_existing->prepend( core->volume, gateway, fent->file_id, block_id, block_version, block_hash );
               }

               if( !rc ) {
                  //printf("// could not prepend to the next block URL set.\n");
                  //printf("// Make a new block URL set and insert it.\n");
                  this->block_urls[ block_id ] = new block_url_set( core->volume, gateway, fent->file_id, this->file_version, block_id, block_id + 1, bvec, block_hash );
               }
               else {
                  //printf("// adjust and shift next_existing into its new place.\n");
                  //printf("// get rid of the old next_existing first.\n");
                  this->block_urls.erase( itr );
                  this->block_urls[ next_existing->start_id ] = next_existing;
               }
            }
            else {
               //printf("// need to insert this block URL at the end\n");
               this->block_urls[ block_id ] = new block_url_set( core->volume, gateway, fent->file_id, this->file_version, block_id, block_id + 1, bvec, block_hash );
            }

            //printf("// will need to shrink existing\n");
            bool rc = existing->shrink_right();
            if( !rc ) {
               //printf("// NOTE: this shouldn't ever happen, since the only way for existing to hold only one block is for existing->start_id + 1 == existing->end_id\n");
               //printf("// (in which case block_id == existing->start_id, meaning this branch won't execute)\n");
               delete existing;
            }

            // attempt to merge
            this->merge_adjacent( block_id );
         }

         else {
            //printf("// split up this URL set\n");
            block_url_set* left = existing->split_left( block_id );
            block_url_set* right = existing->split_right( block_id );
            block_url_set* given = new block_url_set( core->volume, gateway, fent->file_id, this->file_version, block_id, block_id + 1, bvec, block_hash );

            delete existing;

            this->block_urls[ left->start_id ] = left;
            this->block_urls[ given->start_id ] = given;
            this->block_urls[ right->start_id ] = right;
         }
      }
   }
   
   // advance the mod time 
   clock_gettime( CLOCK_REALTIME, &this->lastmod );
   
   // mark as initialized 
   this->initialized = true;
   
   pthread_rwlock_unlock( &this->manifest_lock );
   
   SG_debug("put %" PRIu64 "/%" PRIX64 ".%" PRId64 "[%" PRIu64 ".%" PRId64 "] from %" PRIu64 "\n", core->volume, fent->file_id, fent->version, block_id, block_version, gateway );
   
   /*
   char* data = this->serialize_str();
   SG_debug( "Manifest is now:\n%s\n", data);
   free( data );
   */

   return 0;
}


// put a write hole 
int file_manifest::put_hole( struct fs_core* core, struct fs_entry* fent, uint64_t block_id ) {
   unsigned char* dead_hash = (unsigned char*)alloca( BLOCK_HASH_LEN() );
   memset( dead_hash, 0, BLOCK_HASH_LEN() );
   
   int rc = this->put_block( core, 0, fent, block_id, 0, dead_hash );
   return rc;
}

// truncate a manifest to a smaller size
// this advances the manifest's modtime
void file_manifest::truncate_smaller( uint64_t new_end_id ) {
   pthread_rwlock_wrlock( &this->manifest_lock );

   block_map::iterator itr = this->find_block_set( new_end_id );

   if( itr != this->block_urls.end() ) {
      // truncate this one
      itr->second->truncate_smaller( new_end_id );

      // remove the rest of the block URLs
      if( itr->second->size() > 0 )
         itr++;      // preserve this blocK-url-set

      if( itr != this->block_urls.end() ) {
         for( block_map::iterator itr2 = itr; itr2 != this->block_urls.end(); itr2++ ) {
            delete itr2->second;
            itr2->second = NULL;
         }
         this->block_urls.erase( itr, this->block_urls.end() );
      }
   }

   // advance the mod time 
   clock_gettime( CLOCK_REALTIME, &this->lastmod );
   
   pthread_rwlock_unlock( &this->manifest_lock );

   /*
   char* data = this->serialize_str();
   SG_debug( "Manifest is now:\n%s\n", data);
   free( data );
   */
}


// is a block part of a write hole?
bool file_manifest::is_hole( uint64_t block_id ) {
   
   bool ret = false;
   
   pthread_rwlock_wrlock( &this->manifest_lock );

   block_map::iterator itr = this->find_block_set( block_id );

   if( itr != this->block_urls.end() ) {
      if( itr->second->is_hole() ) {
         ret = true;
      }
   }

   pthread_rwlock_unlock( &this->manifest_lock );

   return ret;
}


// is a block present?
bool file_manifest::is_block_present( uint64_t block_id ) {
   
   bool ret = false;
   
   pthread_rwlock_wrlock( &this->manifest_lock );

   block_map::iterator itr = this->find_block_set( block_id );

   if( itr != this->block_urls.end() ) {
      ret = true;
   }

   pthread_rwlock_unlock( &this->manifest_lock );

   return ret;
}


// snapshot the manifest into a modification map 
void file_manifest::snapshot( modification_map* m ) {
   
   pthread_rwlock_rdlock( &this->manifest_lock );
   
   for( block_map::iterator itr = this->block_urls.begin(); itr != this->block_urls.end(); itr++ ) {
      
      block_url_set* urlset = itr->second;
      
      for( uint64_t id = urlset->start_id; id < urlset->end_id; id++ ) {
         
         // generate a garbage block info for this block
         struct fs_entry_block_info binfo;
         memset( &binfo, 0, sizeof(struct fs_entry_block_info) );
         
         uint64_t idx = id - urlset->start_id;
         
         unsigned char* hash = urlset->hash_dup( id );
         
         fs_entry_block_info_garbage_init( &binfo, urlset->block_versions[idx], hash, BLOCK_HASH_LEN(), urlset->gateway_id );
         
         (*m)[ id ] = binfo;
      }
   }
      
   pthread_rwlock_unlock( &this->manifest_lock );
}


// calculate the difference between a manifest snapshot and the current manifest.
// that is, calculate the set of blocks that have NOT been added to the manifest, since the snapshot was taken.
// NOTE: old_blocks will contain deep copies of fs_entry_block_info copies from snapshot, so caller must free them
void file_manifest::copy_old_blocks( modification_map* snapshot, modification_map* old_blocks ) {
   
   pthread_rwlock_rdlock( &this->manifest_lock );
   
   for( block_map::iterator itr = this->block_urls.begin(); itr != this->block_urls.end(); itr++ ) {
      
      block_url_set* urlset = itr->second;
      
      for( uint64_t id = urlset->start_id; id < urlset->end_id; id++ ) {
         
         int64_t manifest_block_version = urlset->block_versions[ id - urlset->start_id ];
         
         // in the snapshot?
         modification_map::iterator mitr = snapshot->find( id );
         if( mitr != snapshot->end() ) {
            
            // in the snapshot.
            // if the version didn't change, then the block is still new 
            int64_t snapshot_block_version = mitr->second.version;
            if( snapshot_block_version == manifest_block_version ) {
               continue;
            }
            
            // in the snapshot, and differring versions.  Add to old_blocks
            struct fs_entry_block_info binfo;
            memset( &binfo, 0, sizeof(struct fs_entry_block_info) );
         
            struct fs_entry_block_info* old_binfo = &mitr->second;
            
            // NOTE: need to duplicate this
            unsigned char* hash_copy = SG_CALLOC( unsigned char, old_binfo->hash_len );
            memcpy( hash_copy, old_binfo->hash, old_binfo->hash_len );
            
            fs_entry_block_info_garbage_init( &binfo, old_binfo->version, hash_copy, old_binfo->hash_len, old_binfo->gateway_id );
            
            (*old_blocks)[ id ] = binfo;
         }
      }
   }
   
   pthread_rwlock_unlock( &this->manifest_lock );
}



// serialize the manifest to a string
char* file_manifest::serialize_str_locked( bool locked ) {
   stringstream sts;

   if( !locked )
      pthread_rwlock_rdlock( &this->manifest_lock );
   
   for( block_map::iterator itr = this->block_urls.begin(); itr != this->block_urls.end(); itr++ ) {
      sts << "IDs: [" << (long)itr->second->start_id << "-" << (long)itr->second->end_id << "] versions=[";

      for( uint64_t i = 0; i < itr->second->end_id - itr->second->start_id - 1; i++ ) {
         sts << itr->second->block_versions[i] << ", ";
      }
      
      sts << itr->second->block_versions[itr->second->end_id - itr->second->start_id - 1] << "] ";
      
      sts << "hashes=[";
      for( uint64_t i = 0; i < itr->second->end_id - itr->second->start_id - 1; i++ ) {
         char* printable_hash = BLOCK_HASH_TO_STRING( hash_at( itr->second->block_hashes, i ) );
         sts << printable_hash << ", ";
         free( printable_hash);
      }
      
      char* printable_hash = BLOCK_HASH_TO_STRING( hash_at( itr->second->block_hashes, itr->second->end_id - itr->second->start_id - 1 ) );
      sts << printable_hash << "] ";
      free( printable_hash);

      char buf[50];
      sprintf(buf, "%" PRIX64, itr->second->file_id);
      
      sts << string("volume=") << itr->second->volume_id << " gateway=" << itr->second->gateway_id << " file_id=" << buf << " version=" << itr->second->file_version << "\n";
   }
   
   if( !locked )
      pthread_rwlock_unlock( &this->manifest_lock );
   
   return strdup( sts.str().c_str() );
}


// serialize the manifest to a string 
char* file_manifest::serialize_str() {
   return this->serialize_str_locked( false );
}


// serialize the manifest to a protobuf
void file_manifest::as_protobuf( struct fs_core* core, struct fs_entry* fent, Serialization::ManifestMsg* mmsg ) {
   pthread_rwlock_rdlock( &this->manifest_lock );

   for( block_map::iterator itr = this->block_urls.begin(); itr != this->block_urls.end(); itr++ ) {
      Serialization::BlockURLSetMsg* busmsg = mmsg->add_block_url_set();
      itr->second->as_protobuf( core, busmsg );
   }

   mmsg->set_volume_id( core->volume );
   mmsg->set_coordinator_id( fent->coordinator );
   mmsg->set_owner_id( fent->owner );
   mmsg->set_file_id( fent->file_id );
   mmsg->set_file_version( fent->version );
   mmsg->set_size( fent->size );
   mmsg->set_mtime_sec( this->lastmod.tv_sec );
   mmsg->set_mtime_nsec( this->lastmod.tv_nsec );
   mmsg->set_fent_mtime_sec( fent->mtime_sec );
   mmsg->set_fent_mtime_nsec( fent->mtime_nsec );

   pthread_rwlock_unlock( &this->manifest_lock );
}

// reload/generate a manifest from a manifest message
void file_manifest::reload( struct fs_core* core, struct fs_entry* fent, Serialization::ManifestMsg* mmsg ) {
   pthread_rwlock_wrlock( &this->manifest_lock );

   for( block_map::iterator itr = this->block_urls.begin(); itr != this->block_urls.end(); itr++ ) {
      delete itr->second;
   }
   this->block_urls.clear();

   file_manifest::parse_protobuf( core, fent, this, mmsg );

   this->stale = false;

   pthread_rwlock_unlock( &this->manifest_lock );
}


// populate a manifest from a protobuf
// must lock the manifest first,
// and must lock fent first
int file_manifest::parse_protobuf( struct fs_core* core, struct fs_entry* fent, file_manifest* m, Serialization::ManifestMsg* mmsg ) {

   int rc = 0;

   bool is_AG = ms_client_is_AG( core->ms, fent->coordinator );
   
   // validate
   if( mmsg->volume_id() != core->volume ) {
      SG_error("Invalid Manifest: manifest belongs to Volume %" PRIu64 ", but this Gateway is attached to %" PRIu64 "\n", mmsg->volume_id(), core->volume );
      return -EINVAL;
   }
   
   if( mmsg->size() < 0 && !is_AG ) {
      SG_error("Invalid Manifest: coorinator is not an AG, but size is %" PRId64 "\n", mmsg->size() );
      return -EINVAL;
   }
   
   for( int i = 0; i < mmsg->block_url_set_size(); i++ ) {
      Serialization::BlockURLSetMsg busmsg = mmsg->block_url_set( i );

      // make sure version and hash lengths match up
      if( !is_AG && busmsg.block_versions_size() != busmsg.block_hashes_size() ) {
         SG_error("Manifest message len(block_versions) == %u differs from len(block_hashes) == %u\n", busmsg.block_versions_size(), busmsg.block_hashes_size() );
         return -EINVAL;
      }
      
      if( mmsg->size() >= 0 ) {
         int64_t* block_versions = SG_CALLOC( int64_t, busmsg.end_id() - busmsg.start_id() );
         unsigned char* block_hashes = SG_CALLOC( unsigned char, (busmsg.end_id() - busmsg.start_id()) * BLOCK_HASH_LEN() );

         for( int j = 0; j < busmsg.block_versions_size(); j++ ) {
            
            // get block hashes, if we're not an AG
            if( !is_AG ) {
               if( busmsg.block_hashes(j).size() != BLOCK_HASH_LEN() ) {
                  SG_error("Block URL set hash length for block %" PRIu64 " is %zu, which differs from expected %zu\n", (uint64_t)(busmsg.start_id() + j), busmsg.block_hashes(j).size(), BLOCK_HASH_LEN() );
                  free( block_versions );
                  free( block_hashes );
                  return -EINVAL;
               }
               
               memcpy( hash_at( block_hashes, j ), busmsg.block_hashes(j).data(), BLOCK_HASH_LEN() );
            }
            
            block_versions[j] = busmsg.block_versions(j);
         }

         uint64_t gateway_id = busmsg.gateway_id();

         m->block_urls[ busmsg.start_id() ] = new block_url_set( core->volume, gateway_id, fent->file_id, mmsg->file_version(), busmsg.start_id(), busmsg.end_id(), block_versions, block_hashes );

         free( block_versions );
         free( block_hashes );
      }
   }

   if( rc == 0 ) {
      m->file_version = mmsg->file_version();
      m->lastmod.tv_sec = mmsg->mtime_sec();
      m->lastmod.tv_nsec = mmsg->mtime_nsec();
      m->initialized = true;
   }

   return rc;
}

// put a single block into a manifest
// fent must be write-locked
int fs_entry_manifest_put_block( struct fs_core* core, uint64_t gateway_id, struct fs_entry* fent, uint64_t block_id, int64_t block_version, unsigned char* block_hash ) {
   
   int rc = fent->manifest->put_block( core, gateway_id, fent, block_id, block_version, block_hash );
   if( rc != 0 ) {
      SG_error("manifest::put_block(%" PRId64 ".%" PRIu64 ") rc = %d\n", block_id, block_version, rc );
      return rc;
   }
   
   return 0;
}


// generate a manifest that indicates an error message
// all required fields will be filled with random numbers (for cryptographic padding)
int fs_entry_manifest_error( Serialization::ManifestMsg* mmsg, int error, char const* errormsg ) {
   
   mmsg->set_volume_id( md_random64() );
   mmsg->set_coordinator_id( md_random64() );
   mmsg->set_file_id( md_random64() );
   mmsg->set_file_version( (int64_t)md_random64() );
   mmsg->set_size( (int64_t)md_random64() );
   mmsg->set_mtime_sec( (int64_t)md_random64() );
   mmsg->set_mtime_nsec( (int64_t)md_random64() );
   mmsg->set_fent_mtime_sec( (int64_t)md_random64() );
   mmsg->set_fent_mtime_nsec( (int64_t)md_random64() );

   mmsg->set_errorcode( error );
   mmsg->set_errortxt( string(errormsg) );
   
   return 0;
}


