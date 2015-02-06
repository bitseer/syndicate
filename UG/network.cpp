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

#include "network.h"
#include "manifest.h"
#include "url.h"
#include "replication.h"
#include "driver.h"
#include "syndicate.h"

// download and verify a manifest
// fent must be at least read-locked
int fs_entry_download_manifest( struct fs_core* core, char const* fs_path, struct fs_entry* fent, int64_t manifest_mtime_sec, int32_t manifest_mtime_nsec, char const* manifest_url, Serialization::ManifestMsg* mmsg ) {
   
   // connect to the cache...
   struct driver_connect_cache_cls driver_cls;
   driver_cls.client = core->ms;
   driver_cls.core = core;
   
   // process the manifest 
   struct driver_read_manifest_postdown_cls manifest_cls;
   manifest_cls.core = core;
   manifest_cls.fs_path = fs_path;
   manifest_cls.fent = fent;
   manifest_cls.manifest_mtime_sec = manifest_mtime_sec;
   manifest_cls.manifest_mtime_nsec = manifest_mtime_nsec;
   
   int rc = md_download_manifest( core->conf, &core->state->dl, manifest_url, core->closure, driver_connect_cache, &driver_cls, mmsg, driver_read_manifest_postdown, &manifest_cls );
   if( rc != 0 ) {
      
      SG_error("md_download_manifest(%s) rc = %d\n", manifest_url, rc );
      return rc;
   }
   
   uint64_t origin = mmsg->coordinator_id();
   
   int gateway_type = ms_client_get_gateway_type( core->ms, origin );
   if( gateway_type < 0 ) {
      SG_error("ms_client_get_gateway_type( %" PRIu64 " ) rc = %d\n", origin, gateway_type );
      
      if( gateway_type == -ENOENT ) {
         // schedule a reload of this volume...there seems to be a missing gateway 
         ms_client_start_config_reload( core->ms );
         return -EAGAIN;
      }
      else {
         return -EINVAL;
      }
   }
   
   // verify it
   rc = ms_client_verify_gateway_message< Serialization::ManifestMsg >( core->ms, core->volume, gateway_type, origin, mmsg );
   if( rc != 0 ) {
      SG_error("ms_client_verify_manifest(%s) from Gateway %" PRIu64 " rc = %d\n", manifest_url, origin, rc );
      return -EBADMSG;
   }
   
   // error code? 
   if( mmsg->has_errorcode() ) {
      rc = mmsg->errorcode();
      SG_error("manifest gives error %d\n", rc );
      return rc;
   }
   
   return rc;
}


// download a manifest from an RG.
// fent must be at least read-locked
int fs_entry_download_manifest_replica( struct fs_core* core, char const* fs_path, struct fs_entry* fent, int64_t manifest_mtime_sec, int32_t manifest_mtime_nsec,
                                        Serialization::ManifestMsg* mmsg, uint64_t* successful_RG_id ) {
   
   uint64_t* rg_ids = ms_client_RG_ids( core->ms );
   
   int rc = -ENOTCONN;
   int i = -1;
   uint64_t RG_id = 0;
   
   // try a replica
   if( rg_ids != NULL ) {
      for( i = 0; rg_ids[i] != 0; i++ ) {
         
         rc = 0;
         
         struct timespec ts;
         ts.tv_sec = manifest_mtime_sec;
         ts.tv_nsec = manifest_mtime_nsec;
         
         char* replica_url = md_url_RG_manifest_url( core->ms, rg_ids[i], fent->file_id, fent->version, &ts );
         
         rc = fs_entry_download_manifest( core, fs_path, fent, manifest_mtime_sec, manifest_mtime_nsec, replica_url, mmsg );

         if( rc == 0 ) {
            free( replica_url );
            break;
         }
         else {
            SG_error("fs_entry_download_manifest(%s) rc = %d\n", replica_url, rc );
            
            if( rc != -ENOENT )
               rc = -ENODATA;
         }
         
         free( replica_url );
      }

      if( rc == 0 && i >= 0 ) {
         RG_id = rg_ids[i];
         
         if( successful_RG_id )
            *successful_RG_id = RG_id;
      }
      
      free( rg_ids);
   }
   
   if( rc == 0 ) {
      
      // error code? 
      if( mmsg->has_errorcode() ) {
         rc = mmsg->errorcode();
         SG_error("manifest gives error %d\n", rc );
         return rc;
      }
   }
   
   return rc;
}


// get a manifest, either from the coordinator, or from an RG.  If we got it from an RG, remember which one.
// if we're getting a manifest from an AG, don't try getting it from the RGs (they won't have it)
// fent must be at least read-locked.
// return -EAGAIN if the caller should try again (i.e. we were working off of inconsistent/stale data)
// return -ENODATA if we did not have sufficient data to fetch the manifest 
// return -EBADMSG if the manifest was malformed
// return -ESTALE if the path metadata was or became stale, and should be revalidated.  The caller should revalidate the path and try this method again.
int fs_entry_get_manifest( struct fs_core* core, char const* fs_path, struct fs_entry* fent, int64_t manifest_mtime_sec, int32_t manifest_mtime_nsec,
                           Serialization::ManifestMsg* manifest_msg, uint64_t* successful_gateway_id ) {
   
   char* manifest_url = NULL;
   int rc = 0;
   
   struct timespec modtime;
   modtime.tv_sec = manifest_mtime_sec;
   modtime.tv_nsec = manifest_mtime_nsec;
   
   int gateway_type = ms_client_get_gateway_type( core->ms, fent->coordinator );
   
   if( !FS_ENTRY_LOCAL( core, fent ) ) {
      // check the MS-listed coordinator first, instead of asking the RGs directly
      
      if( gateway_type < 0 ) {
         // unknown gateway...try refreshing the Volume
         SG_error("Unknown Gateway %" PRIu64 "\n", fent->coordinator );
         ms_client_start_config_reload( core->ms );
         return -EAGAIN;
      }
      
      rc = md_url_make_manifest_url( core->ms, fs_path, fent->coordinator, fent->file_id, fent->version, &modtime, &manifest_url );
      
      if( rc != 0 ) {
         // failed to produce the url
         SG_error("md_url_make_manifest_url rc = %d\n", rc );
         
         if( rc == -ENOENT ) {
            // gateway not found.  try refreshing our certs
            ms_client_start_config_reload( core->ms );
            return -EAGAIN;
         }
         else {
            return -ENODATA;
         }
      }
      
      SG_debug("Download manifest from Gateway %" PRIu64 " at %s\n", fent->coordinator, manifest_url );
  
      rc = fs_entry_download_manifest( core, fs_path, fent, manifest_mtime_sec, manifest_mtime_nsec, manifest_url, manifest_msg );
      
      if( rc == 0 && successful_gateway_id ) {
         // got it from the coordinator
         *successful_gateway_id = fent->coordinator;
      }
      else if( rc != 0 ) {
         SG_error("fs_entry_download_manifest(%s) rc = %d\n", manifest_url, rc );
      }
   }
   
   if( gateway_type != SYNDICATE_AG && (rc != 0 || FS_ENTRY_LOCAL( core, fent )) ) {
      // either we couldn't get it from the remote UG, or its local and we don't have a copy ourselves
      
      // try the RGs
      uint64_t rg_id = 0;
      
      rc = fs_entry_download_manifest_replica( core, fs_path, fent, manifest_mtime_sec, manifest_mtime_nsec, manifest_msg, &rg_id );
      
      if( rc != 0 ) {
         // error
         SG_error("Failed to read /%" PRIu64 "/%" PRIX64 ".%" PRId64 "/manifest.%" PRId64 ".%d from RGs\n", fent->volume, fent->file_id, fent->version, manifest_mtime_sec, manifest_mtime_nsec );
      }
      else {
         // success!
         SG_debug("Read /%" PRIu64 "/%" PRIX64 ".%" PRId64 "/manifest.%" PRId64 ".%d from RG %" PRIu64 "\n", fent->volume, fent->file_id, fent->version, manifest_mtime_sec, manifest_mtime_nsec, rg_id );
         rc = 0;
         
         if( successful_gateway_id ) {
            // got it from this RG
            *successful_gateway_id = rg_id;
         }
      }
   }

   free( manifest_url );

   if( rc != 0 ) {
      return rc;
   }
   
   // is this an error code?
   if( manifest_msg->has_errorcode() ) {
      // remote gateway indicates error
      SG_error("manifest error %d\n", manifest_msg->errorcode() );
      return manifest_msg->errorcode();
   }
   
   rc = 0;
   
   // verify that the manifest matches the timestamp
   if( manifest_msg->mtime_sec() != manifest_mtime_sec || manifest_msg->mtime_nsec() != manifest_mtime_nsec ) {
      // invalid manifest--probably due to a timestamp mismatch.  Try again 
      SG_error("timestamp mismatch: got %" PRId64 ".%d, expected %" PRId64 ".%d\n", manifest_msg->mtime_sec(), manifest_msg->mtime_nsec(), manifest_mtime_sec, manifest_mtime_nsec );
      fent->manifest->mark_stale();
      fent->read_stale = false;
      rc = -ESTALE;
   }
   
   // verify that the manifest matches the volume and file id and version 
   if( manifest_msg->volume_id() != fent->volume || manifest_msg->file_id() != fent->file_id || manifest_msg->file_version() != fent->version ) {
      // wrong manifest 
      SG_error("Invalid manifest: got /%" PRIu64 "/%" PRIX64 ".%" PRId64 ", expected /%" PRIu64 "/%" PRIX64 ".%" PRId64 "\n", 
             manifest_msg->volume_id(), manifest_msg->file_id(), manifest_msg->file_version(),
             fent->volume, fent->file_id, fent->version );
      
      // if it's a version mismatch, we're probably stale 
      if( manifest_msg->volume_id() == fent->volume && manifest_msg->file_id() == fent->file_id ) {
         fent->manifest->mark_stale();
         fent->read_stale = false;
         return -ESTALE;
      }
      else {
         // wrong file ID or volume ID
         return -EBADMSG;
      }
   }
   
   return rc;
}


// set up a write message
int fs_entry_init_write_message( Serialization::WriteMsg* writeMsg, struct fs_core* core, Serialization::WriteMsg_MsgType type ) {
   struct ms_client* client = core->ms;
   
   writeMsg->set_type( type );
   writeMsg->set_volume_version( ms_client_volume_version( client ) );
   writeMsg->set_cert_version( ms_client_cert_version( client ) );
   writeMsg->set_user_id( core->conf->owner );
   writeMsg->set_volume_id( core->volume );
   writeMsg->set_gateway_id( core->conf->gateway );
   return 0;
}


// set up a WRITE message, with the blocks we're gonna send
int fs_entry_prepare_write_message( Serialization::WriteMsg* writeMsg, struct fs_core* core, char const* fs_path, struct replica_snapshot* fent_snapshot, int64_t write_nonce, modification_map* dirty_blocks ) {
   
   fs_entry_init_write_message( writeMsg, core, Serialization::WriteMsg::WRITE );
   
   Serialization::FileMetadata* file_md = writeMsg->mutable_metadata();

   file_md->set_fs_path( string(fs_path) );
   file_md->set_volume_id( core->volume );
   file_md->set_file_id( fent_snapshot->file_id );
   file_md->set_file_version( fent_snapshot->file_version );
   file_md->set_size( fent_snapshot->size );
   file_md->set_write_nonce( write_nonce );
   file_md->set_coordinator_id( fent_snapshot->coordinator_id );
   
   for( modification_map::iterator itr = dirty_blocks->begin(); itr != dirty_blocks->end(); itr++ ) {
      
      uint64_t block_id = itr->first;
      struct fs_entry_block_info* binfo = &itr->second;
      
      Serialization::BlockInfo* msg_binfo = writeMsg->add_blocks();
      
      msg_binfo->set_block_id( block_id );
      msg_binfo->set_block_version( binfo->version );
      msg_binfo->set_hash( string( (char const*)binfo->hash, BLOCK_HASH_LEN()) );
   }
   
   return 0;
}


// make a truncate message
// fent must be at least read-locked
int fs_entry_prepare_truncate_message( Serialization::WriteMsg* truncate_msg, char const* fs_path, struct fs_entry* fent, uint64_t new_max_block ) {
   Serialization::TruncateRequest* truncate_req = truncate_msg->mutable_truncate();
   
   truncate_req->set_volume_id( fent->volume );
   truncate_req->set_coordinator_id( fent->coordinator );
   truncate_req->set_file_id( fent->file_id );
   truncate_req->set_fs_path( fs_path );
   truncate_req->set_file_version( fent->version );
   truncate_req->set_size( fent->size );

   for( uint64_t i = 0; i < new_max_block; i++ ) {
      
      Serialization::BlockInfo* msg_binfo = truncate_msg->add_blocks();
      
      int64_t block_version = fent->manifest->get_block_version( i );
      unsigned char* block_hash = fent->manifest->get_block_hash( i );
      
      msg_binfo->set_block_id( i );
      msg_binfo->set_block_version( block_version );
      msg_binfo->set_hash( string( (char const*)block_hash, BLOCK_HASH_LEN()) );
      
      free( block_hash );
   }
   
   return 0;
}


// make a rename message
int fs_entry_prepare_rename_message( Serialization::WriteMsg* rename_msg, char const* old_path, char const* new_path, struct fs_entry* fent_old, int64_t version ) {

   Serialization::RenameMsg* rename_info = rename_msg->mutable_rename();
   rename_info->set_volume_id( fent_old->volume );
   rename_info->set_coordinator_id( fent_old->coordinator );
   rename_info->set_file_id( fent_old->file_id );
   rename_info->set_file_version( version );
   rename_info->set_old_fs_path( string(old_path) );
   rename_info->set_new_fs_path( string(new_path) );
   
   return 0;
}

// make an unlink message 
// fent must be read-locked
int fs_entry_prepare_detach_message( Serialization::WriteMsg* detach_msg, char const* fs_path, struct fs_entry* fent, int64_t version ) {
   
   Serialization::DetachRequest* detach = detach_msg->mutable_detach();
   
   detach->set_volume_id( fent->volume );
   detach->set_coordinator_id( fent->coordinator );
   detach->set_file_id( fent->file_id );
   detach->set_fs_path( string(fs_path) );
   detach->set_file_version( version );
   
   return 0;
}


// CURL method to quickly buffer up a response
static size_t fs_entry_response_buffer( char* ptr, size_t size, size_t nmemb, void* userdata ) {
   md_response_buffer_t* respBuf = (md_response_buffer_t*)userdata;

   size_t tot = size * nmemb;

   char* ptr_dup = SG_CALLOC( char, tot );
   memcpy( ptr_dup, ptr, tot );

   respBuf->push_back( md_buffer_segment_t( ptr_dup, tot ) );

   return tot;
}


// send off a write message, and get back the received write message
int fs_entry_post_write( Serialization::WriteMsg* recvMsg, struct fs_core* core, uint64_t gateway_id, Serialization::WriteMsg* sendMsg ) {
   CURL* curl_h = curl_easy_init();
   md_response_buffer_t buf;

   char* content_url = ms_client_get_UG_content_url( core->ms, gateway_id );
   if( content_url == NULL ) {
      SG_error("No such Gateway %" PRIu64 "\n", gateway_id );
      curl_easy_cleanup( curl_h );
      return -EINVAL;
   }

   md_init_curl_handle( core->conf, curl_h, content_url, core->conf->connect_timeout );
   curl_easy_setopt( curl_h, CURLOPT_POST, 1L );
   curl_easy_setopt( curl_h, CURLOPT_WRITEFUNCTION, fs_entry_response_buffer );
   curl_easy_setopt( curl_h, CURLOPT_WRITEDATA, &buf );
   curl_easy_setopt( curl_h, CURLOPT_SSL_VERIFYPEER, (core->conf->verify_peer ? 1L : 0L) );
   curl_easy_setopt( curl_h, CURLOPT_SSL_VERIFYHOST, 2L );
   
   // sign this
   int rc = md_sign<Serialization::WriteMsg>( core->ms->gateway_key, sendMsg );
   if( rc != 0 ) {
      SG_error("md_sign rc = %d\n", rc );
      curl_easy_cleanup( curl_h );
      return rc;
   }
   
   char* writemsg_buf = NULL;
   size_t writemsg_len = 0;
   
   rc = md_serialize<Serialization::WriteMsg>( sendMsg, &writemsg_buf, &writemsg_len );
   if( rc != 0 ) {
      SG_error("md_serialize rc = %d\n", rc );
      curl_easy_cleanup( curl_h );
      return rc;
   }
   
   struct curl_httppost *post = NULL, *last = NULL;
   curl_formadd( &post, &last, CURLFORM_PTRNAME, "WriteMsg", CURLFORM_PTRCONTENTS, writemsg_buf, CURLFORM_CONTENTSLENGTH, writemsg_len, CURLFORM_END );

   curl_easy_setopt( curl_h, CURLOPT_HTTPPOST, post );

   struct timespec ts, ts2;

   SG_BEGIN_TIMING_DATA( ts );
   
   SG_debug( "send WriteMsg type %d length %zu\n", sendMsg->type(), writemsg_len );
   rc = curl_easy_perform( curl_h );

   SG_END_TIMING_DATA( ts, ts2, "Remote write" );
   
   // free memory
   curl_easy_setopt( curl_h, CURLOPT_HTTPPOST, NULL );
   curl_formfree( post );
   
   free( writemsg_buf );
   
   if( rc != 0 ) {
      // could not perform
      // OS error?
      
      long err = 0;
      curl_easy_getinfo( curl_h, CURLINFO_OS_ERRNO, &err );
      
      SG_debug("curl_easy_perform(%s) rc = %d, err = %ld\n", content_url, rc, err );
      
      md_response_buffer_free( &buf );
      curl_easy_cleanup( curl_h );
      free( content_url );
      
      return -ENODATA;
   }
   else {
      long http_status = 0;
      curl_easy_getinfo( curl_h, CURLINFO_RESPONSE_CODE, &http_status );

      if( http_status != 200 ) {
         SG_error( "remote HTTP response %ld\n", http_status );

         md_response_buffer_free( &buf );
         curl_easy_cleanup( curl_h );
         free( content_url );

         return -EREMOTEIO;
      }

      // got back a message--parse it
      char* msg_buf = md_response_buffer_to_string( &buf );
      size_t msg_len = md_response_buffer_size( &buf );

      // extract the messsage
      rc = md_parse< Serialization::WriteMsg >( recvMsg, msg_buf, msg_len );
      
      free( msg_buf );
      md_response_buffer_free( &buf );
      curl_easy_cleanup( curl_h );
      
      if( rc != 0 ) {
         SG_error("Failed to parse response from %s\n", content_url );
         rc = -EBADMSG;
      }
      else {
         // verify authenticity
         rc = ms_client_verify_gateway_message< Serialization::WriteMsg >( core->ms, core->volume, SYNDICATE_UG, gateway_id, recvMsg );
         if( rc != 0 ) {
            SG_error("Failed to verify the authenticity of Gateway %" PRIu64 "'s response, rc = %d\n", gateway_id, rc );
            rc = -EBADMSG;
         }
         else {
            // check for error codes
            if( recvMsg->has_errorcode() ) {
               SG_error("WriteMsg error %d\n", recvMsg->errorcode() );
               rc = recvMsg->errorcode();
            }
            
            else {
               // send the MS-related header to our client
               ms_client_process_header( core->ms, core->volume, recvMsg->volume_version(), recvMsg->cert_version() );
            }
         }
      }
   }

   free( content_url );

   return rc;
}


// send a write message for a file to a remote gateway, or become the coordinator of the file.
// this does not affect the manifest in any way.
// fent must be write-locked!
// return 0 if the send was successful.
// return 1 if we're now the coordinator.
// return negative on error.
int fs_entry_send_write_or_coordinate( struct fs_core* core, char const* fs_path, struct fs_entry* fent, Serialization::WriteMsg* write_msg, Serialization::WriteMsg* write_ack ) {

   int ret = 0;
   
   // try to send to the coordinator, or become the coordinator
   bool sent = false;
   bool local = false;
   
   while( !sent ) {
      int rc = fs_entry_post_write( write_ack, core, fent->coordinator, write_msg );

      // process the write message acknowledgement--hopefully it's an ACCEPTED
      if( rc != 0 ) {
         // failed to post
         SG_error( "fs_entry_post_write(%s /%" PRIu64 "/%" PRIX64 " (%s)) to %" PRIu64 " rc = %d\n", fs_path, fent->volume, fent->file_id, fent->name, fent->coordinator, rc );
         
         if( rc == -ENODATA ) {
            // curl couldn't connect--this is maybe a partition.
            // Try to become the coordinator.
            rc = fs_entry_coordinate( core, fs_path, fent );
            
            if( rc == 0 ) {
               // we're now the coordinator, and the MS has the latest metadata.
               local = true;
               
               SG_debug("Now coordinator for %" PRIu64 " (%s)\n", fent->file_id, fent->name );
               
               break;
            }
            else if( rc == -EAGAIN ) {
               // the coordinator changed to someone else.  Try again.
               // fent->coordinator refers to the current coordinator.
               SG_debug("coordinator of %s /%" PRIu64 "/%" PRIX64 " is now %" PRIu64 "\n", fs_path, fent->volume, fent->file_id, fent->coordinator );
               rc = 0;
               continue;
            }
            else {
               // some other (fatal) error
               SG_error("fs_entry_coordinate(%s /%" PRIu64 "/%" PRIX64 ") rc = %d\n", fs_path, fent->volume, fent->file_id, rc );
               break;
            }
         }
         else {
            // some other (fatal) error
            break;
         }
      }
      else {
         // success!
         sent = true;
      }
      
      if( rc != 0 ) {
         ret = rc;
      }
   }
   
   // are we the coordinator?
   if( ret == 0 && local )
      ret = 1;

   return ret;
}


