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


#include "libsyndicate/ms/file.h"
#include "libsyndicate/ms/url.h"
#include "libsyndicate/ms/getattr.h"
#include "libsyndicate/ms/listdir.h"

#include "path.h"

// free an MS listing
void ms_client_free_listing( struct ms_listing* listing ) {
   if( listing->entries != NULL ) {
      for( unsigned int i = 0; i < listing->entries->size(); i++ ) {
         md_entry_free( &listing->entries->at(i) );
      }
      
      SG_safe_delete( listing->entries );
   }
}

// build a path ent
// NOTE: not all fields are necessary for all operations
// return 0 on success 
// return -ENOMEM if out of memory
int ms_client_make_path_ent( struct ms_path_ent* path_ent, uint64_t volume_id, uint64_t parent_id, uint64_t file_id, int64_t version, int64_t write_nonce,
                             int64_t num_children, int64_t generation, int64_t capacity, char const* name, void* cls ) {
   
   
   if( name != NULL ) {
      if( path_ent->name != NULL ) {
         free( path_ent->name );
      }
      
      path_ent->name = SG_strdup_or_null( name );
      
      if( path_ent->name == NULL ) {
         return -ENOMEM;
      }
   }
   
   // build up the ms_path as we traverse our cached path
   path_ent->volume_id = volume_id;
   path_ent->file_id = file_id;
   path_ent->parent_id = parent_id;
   path_ent->version = version;
   path_ent->write_nonce = write_nonce;
   path_ent->num_children = num_children;
   path_ent->generation = generation;
   path_ent->capacity = capacity;
   
   path_ent->cls = cls;
   return 0;
}
   
// free a path entry, using the given free callback to free the path cls
void ms_client_free_path_ent( struct ms_path_ent* path_ent, void (*free_cls)( void* ) ) {
   
   SG_safe_free( path_ent->name );
   
   if( path_ent->cls != NULL && free_cls ) {
      (*free_cls)( path_ent->cls );
      path_ent->cls = NULL;
   }

   memset( path_ent, 0, sizeof(struct ms_path_ent) );
}

// free a path, using the given free callback to free the path cls
void ms_client_free_path( ms_path_t* path, void (*free_cls)(void*) ) {
   for( unsigned int i = 0; i < path->size(); i++ ) {
      ms_client_free_path_ent( &path->at(i), free_cls );
   }
}


// parse an MS listing, initializing the given ms_listing structure
// return 0 on success
// return -ENOMEM if we're out of memory 
int ms_client_parse_listing( struct ms_listing* dst, ms::ms_reply* reply ) {
   
   const ms::ms_listing& src = reply->listing();
   int rc = 0;
   
   memset( dst, 0, sizeof(struct ms_listing) );
   
   if( src.status() != ms::ms_listing::NONE ) {
      
      dst->status = (src.status() == ms::ms_listing::NEW ? MS_LISTING_NEW : MS_LISTING_NOCHANGE);
   }
   else {
      
      dst->status = MS_LISTING_NONE;
   }

   if( dst->status == MS_LISTING_NEW ) {
      
      dst->type = src.ftype();
      dst->entries = SG_safe_new( vector<struct md_entry>() );
      
      if( dst->entries == NULL ) {
         
         return -ENOMEM;
      }
      
      for( int i = 0; i < src.entries_size(); i++ ) {
         
         struct md_entry ent;
         rc = ms_entry_to_md_entry( src.entries(i), &ent );
         
         if( rc != 0 ) {
            
            // free all 
            for( unsigned int i = 0; i < dst->entries->size(); i++ ) {
               md_entry_free( &dst->entries->at(i) );
            }
            
            SG_safe_delete( dst->entries );
            return -ENOMEM;
         }

         dst->entries->push_back( ent );
      }
   }
   
   dst->error = reply->error();

   return 0;
}

// extract multiple entries from the listing 
// return 0 on success, and set *ents to the entries and *num_ents to the number of entries.
// return negative on error (-ENOMEM for OOM, -EBADMSG for invalid listing status)
// if non-zero, the listing's error code is placed in listing_error.  It will be negative.
// otherwise, the listing's status will be placed in listing_error.  It will be positive.
int ms_client_listing_read_entries( struct ms_client* client, struct md_download_context* dlctx, struct md_entry** ents, size_t* num_ents, int* listing_error ) {
   
   int rc = 0;
   char* dlbuf = NULL;
   off_t dlbuf_len;
   ms::ms_reply reply;
   struct ms_listing listing;
   
   memset( &listing, 0, sizeof(struct ms_listing) );
   
   rc = md_download_context_get_buffer( dlctx, &dlbuf, &dlbuf_len );
   if( rc != 0 ) {
      
      SG_error("md_download_context_get_buffer(%p) rc = %d\n", dlctx, rc );
      return rc;  
   }
   
   // unserialize
   rc = ms_client_parse_reply( client, &reply, dlbuf, dlbuf_len, true );
   free( dlbuf );
   
   if( rc != 0 ) {
      
      SG_error("ms_client_parse_reply(%p) rc = %d\n", dlctx, rc );
      return rc;
   }
   
   // get listing data 
   rc = ms_client_parse_listing( &listing, &reply );
   if( rc != 0 ) {
      
      SG_error("ms_client_parse_listing(%p) rc = %d\n", dlctx, rc );
      return rc;
   }
   
   // check error status 
   if( listing.error != 0 ) {
      
      SG_error("listing of %p: error == %d\n", dlctx, listing.error );
      ms_client_free_listing( &listing );
      
      *listing_error = listing.error;
      return -ENODATA;
   }
   
   // check existence 
   if( listing.status == MS_LISTING_NONE ) {
      
      // no such file or directory 
      ms_client_free_listing( &listing );
      *listing_error = MS_LISTING_NONE;
      
      return 0;
   }
   
   // if not modified, then nothing to do 
   else if( listing.status == MS_LISTING_NOCHANGE ) {
      
      // nothing to do 
      ms_client_free_listing( &listing );
      
      *ents = NULL;
      *num_ents = 0;
      
      *listing_error = MS_LISTING_NOCHANGE;
      
      return 0;
   }
   
   // have data?
   else if( listing.status == MS_LISTING_NEW ) {
      
      if( listing.entries->size() > 0 ) {
         // success!
         struct md_entry* tmp = SG_CALLOC( struct md_entry, listing.entries->size() );
         if( tmp == NULL ) {
            
            ms_client_free_listing( &listing );
            return -ENOMEM;
         }
         
         // NOTE: vectors are contiguous in memory 
         memcpy( tmp, &listing.entries->at(0), sizeof(struct md_entry) * listing.entries->size() );
         
         *ents = tmp;
         *num_ents = listing.entries->size();
         *listing_error = 0;
         
         // NOTE: don't free md_entry fields; just free the buffer holding them
         listing.entries->clear();
      }
      else {
         
         // EOF
         *ents = NULL;
         *num_ents = 0;
         *listing_error = 0;
      }
      
      ms_client_free_listing( &listing );
      
      *listing_error = MS_LISTING_NEW;
      
      return 0;
   }
   
   else {
      
      // invalid status 
      SG_error("download %p: Invalid listing status %d\n", dlctx, listing.status );
      ms_client_free_listing( &listing );
      return -EBADMSG;
   }
}

// read one entry from the listing 
// assert that there is only one entry if we got any data back at all, and put it into *ent on success
// return 0 on success
// return negative on error 
// * If the MS indicates that there is no change in the requested data, then *ent is zero'ed
int ms_client_listing_read_entry( struct ms_client* client, struct md_download_context* dlctx, struct md_entry* ent, int* listing_error ) {
   
   size_t num_ents = 0;
   int rc = 0;
   struct md_entry* tmp = NULL;
   
   rc = ms_client_listing_read_entries( client, dlctx, &tmp, &num_ents, listing_error );
   
   if( rc != 0 ) {
      return rc;
   }
   
   if( num_ents != 1 && tmp != NULL ) {
      
      // too many entries 
      for( unsigned int i = 0; i < num_ents; i++ ) {
         md_entry_free( &tmp[i] );
      }
      
      SG_safe_free( tmp );
      
      return -EBADMSG;
   }
   
   if( tmp != NULL ) {
      
      *ent = *tmp;
      SG_safe_free( tmp );
   }
   else {
      // no data given (i.e. NOCHANGE)
      memset( ent, 0, sizeof(struct md_entry) );
   }
   
   return 0;
}


// Walk down a path on the MS, filling in the given path with information.  This method iteratively calls getchild() until it 
// reaches the end of the path, or encounters an error.  If it encounters a path-related error from the MS, *error is set to it and *error_idx is set to
// the index of the path entry for which the error was encountered.
// Each time an entry is successfully obtained, download_cb is called with the filled-in data.
// The given path entries must contain:
// * volume_id
// * name
// Also, the first path entry must contain:
// * name
// * volume_id
// * file_id 
// * parent_id
// NOTE: this method will modify path (filling in data it obtained from the MS)
// return 0 on success, indicating no communication error with the MS (but possibly path-related errors, like security check failures or non-existence)
int ms_client_path_download( struct ms_client* client, ms_path_t* path, ms_path_ent_download_cb download_cb, void* download_cls, int* error, int* error_idx ) {
   
   int rc = 0;
   struct ms_client_multi_result result;
   
   // sanity check 
   if( path->size() == 0 ) {
      
      // nothing to do 
      return 0;
   }
   
   // sanity check 
   for( unsigned int i = 0; i < path->size(); i++ ) {
      
      if( path->at(i).name == NULL ) {
         return -EINVAL;
      }
   }
   
   for( unsigned int i = 0; i < path->size(); i++ ) {
      
      // get the next child 
      memset( &result, 0, sizeof(struct ms_client_multi_result) );
      
      rc = ms_client_getchild( client, &path->at(i), &result );
      
      if( rc != 0 ) {
         
         SG_error("ms_client_getchild(%" PRIX64 ".%s) rc = %d\n", path->at(i).parent_id, path->at(i).name, rc );
         break;
      }
      
      if( result.reply_error != 0 ) {
         
         SG_error("MS replied %d for GETCHILD(%" PRIX64 ", %s)\n", result.reply_error, path->at(i).parent_id, path->at(i).name );
         
         *error = result.reply_error;
         *error_idx = i;
         break;
      }
      
      // fill in the information 
      path->at(i).file_id = result.ents[0].file_id;
      path->at(i).version = result.ents[0].version;
      path->at(i).write_nonce = result.ents[0].write_nonce;
      path->at(i).num_children = result.ents[0].num_children;
      
      // provide parent if we can 
      if( i > 0 ) {
         
         path->at(i).parent_id = path->at(i-1).file_id;
      }
      
      // let the caller know 
      if( download_cb != NULL ) {
         
         rc = (*download_cb)( &path->at(i), download_cls );
         
         if( rc != 0 ) {
            
            SG_error("download_cb(%" PRIX64 ", %s) rc = %d\n", path->at(i).parent_id, path->at(i).name, rc );
            break;
         }
      }
   }
   
   return rc;
}


// convert each entry in an ms path to a string, up to max_index (use -1 for all)
// return NULL if ms_path is empty, or we're out of memory
char* ms_path_to_string( ms_path_t* ms_path, int max_index ) {
   
   int i = 0;
   size_t num_chars = 0;
   
   // sanity check
   if( ms_path->size() == 0 || max_index == 0 ) {
      return NULL;
   }
   
   if( max_index < 0 ) {
      max_index = ms_path->size();
   }
   
   // find length of path
   for( i = 0; i < max_index; i++ ) {
      num_chars += strlen(ms_path->at(i).name) + 2;
   }
   
   // build up the path 
   char* ret = SG_CALLOC( char, num_chars + 1 );
   if( ret == NULL ) {
      
      return NULL;
   }
   
   // this is root
   strcat( ret, ms_path->at(0).name );
   
   // done yet?
   if( max_index == 1 ) {
      return ret;
   }
   else if( max_index > 1 ) {
      strcat( ret, ms_path->at(1).name );
   }
   
   // do the rest
   for( i = 2; i < max_index; i++ ) {
      strcat( ret, "/" );
      strcat( ret, ms_path->at(i).name );
   }
   
   return ret;
}
