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

#include "server.h"
#include "write.h"

// connection initialization handler for embedded HTTP server
void* SG_server_HTTP_connect( struct md_HTTP_connection_data* md_con_data ) {
   struct syndicate_connection* syncon = SG_CALLOC( struct syndicate_connection, 1 );
   syncon->state = syndicate_get_state();
   return syncon;
}

// HTTP authentication callback (does nothing, since we verify the message signature)
uint64_t SG_server_HTTP_authenticate( struct md_HTTP_connection_data* md_con_data, char* username, char* password ) {
   return 0;
}

// process the beginning of a HEAD or POST
static int syndicate_begin_read_request( struct md_HTTP_connection_data* md_con_data, struct md_HTTP_response* resp, struct md_gateway_request_data* reqdat, struct stat* sb ) {

   char* url = md_con_data->url_path;
   struct md_HTTP* http = md_con_data->http;
   struct syndicate_connection* syncon = (struct syndicate_connection*)md_con_data->cls;
   struct syndicate_state* state = syncon->state;

   SG_debug( "read on %s\n", url);
   
   int rc = http_parse_request( http, resp, reqdat, url );
   if( rc < 0 ) {
      SG_debug("http_parse_request(%s) rc = %d\n", url, rc );
      // error, but handled
      return 0;
   }

   // handle 302, 400, and 404
   rc = http_handle_redirect( state, resp, sb, reqdat );
   if( rc <= 0 ) {
      // handled!
      SG_debug("http_handle_redirect(%s) rc = %d\n", url, rc );
      md_gateway_request_data_free( reqdat );
      return 0;
   }

   if( rc == HTTP_REDIRECT_REMOTE ) {
      // requested object is not here.
      // will not redirect; that can lead to loops.
      SG_error("ERR: Requested object %s is not local.  Will not redirect to avoid loops.\n", reqdat->fs_path );
      md_create_HTTP_response_ram_static( resp, "text/plain", 404, MD_HTTP_404_MSG, strlen(MD_HTTP_404_MSG) + 1 );
      md_gateway_request_data_free( reqdat );
      return 0;
   }

   // check authorization
   if( !(sb->st_mode & 0044) ) {
      // not volume-readable or world readable
      SG_error("ERR: Object %s is not volume-readable or world-readable (mode %o)\n", reqdat->fs_path, sb->st_mode );
      md_create_HTTP_response_ram_static( resp, "text/plain", 404, MD_HTTP_404_MSG, strlen(MD_HTTP_404_MSG) + 1 );
      md_gateway_request_data_free( reqdat );
      return 0;
   }

   // if this is a request for a directory, then bail
   if( S_ISDIR( sb->st_mode ) ) {
      SG_error("ERR: Object %s is a directory\n", reqdat->fs_path );
      md_create_HTTP_response_ram_static( resp, "text/plain", 400, MD_HTTP_400_MSG, strlen(MD_HTTP_400_MSG) + 1 );
      md_gateway_request_data_free( reqdat );
      return 0;
   }

   // not handled
   return 1;
}


// HTTP HEAD handler
struct md_HTTP_response* SG_server_HTTP_HEAD_handler( struct md_HTTP_connection_data* md_con_data ) {

   struct md_HTTP_response* resp = SG_CALLOC( struct md_HTTP_response, 1 );

   struct md_gateway_request_data reqdat;
   struct stat sb;
   
   int rc = syndicate_begin_read_request( md_con_data, resp, &reqdat, &sb );
   if( rc == 0 ) {
      // handled
      return 0;
   }

   // this object is local and is a file
   md_create_HTTP_response_ram_static( resp, "text/plain", 200, MD_HTTP_200_MSG, strlen(MD_HTTP_200_MSG) + 1 );
   http_make_default_headers( resp, sb.st_mtime, sb.st_size, true );

   md_gateway_request_data_free( &reqdat );
   
   return resp;
}


// HTTP GET handler
struct md_HTTP_response* SG_server_HTTP_GET_handler( struct md_HTTP_connection_data* md_con_data ) {

   struct syndicate_connection* syncon = (struct syndicate_connection*)md_con_data->cls;
   struct syndicate_state* state = syncon->state;

   struct md_HTTP_response* resp = SG_CALLOC( struct md_HTTP_response, 1 );

   // parse the url_path into its constituent components
   struct md_gateway_request_data reqdat;
   struct stat sb;

   int rc = syndicate_begin_read_request( md_con_data, resp, &reqdat, &sb );
   if( rc == 0 ) {
      // handled already
      return resp;
   }

   // not handled
   // what is this a request for?
   // a block?
   if( reqdat.block_id != SG_INVALID_BLOCK_ID ) {
      // serve back the block
      char* block = SG_CALLOC( char, state->core->blocking_factor );
      
      ssize_t size = fs_entry_read_block_local( state->core, reqdat.fs_path, reqdat.block_id, block, state->core->blocking_factor );
      if( size < 0 ) {
         SG_error( "fs_entry_read_block(%s.%" PRId64 "/%" PRIu64 ".%" PRId64 ") rc = %zd\n", reqdat.fs_path, reqdat.file_version, reqdat.block_id, reqdat.block_version, size );
         
         if( size == -EREMOTE ) {
            // data is not local
            md_create_HTTP_response_ram( resp, "text/plain", 410, "Block is not local\n", strlen("Block is not local\n") );
         }
         else {
            md_create_HTTP_response_ram( resp, "text/plain", 500, "INTERNAL SERVER ERROR\n", strlen("INTERNAL SERVER ERROR\n") + 1 );
         }

         md_gateway_request_data_free( &reqdat );
         free( block );
         return resp;
      }
      else {
         SG_debug( "served %zd bytes from %s.%" PRId64 "/%" PRIu64 ".%" PRId64 "\n", size, reqdat.fs_path, reqdat.file_version, reqdat.block_id, reqdat.block_version );
      }
      
      md_create_HTTP_response_ram_nocopy( resp, "application/octet-stream", 200, block, size );
      http_make_default_headers( resp, sb.st_mtime, size, true );

      md_gateway_request_data_free( &reqdat );
      return resp;
   }

   // request for a file or file manifest?
   else {
      if( reqdat.manifest_timestamp.tv_sec > 0 && reqdat.manifest_timestamp.tv_nsec > 0 ) {
         // request for a manifest
         // get the manifest and reply it
         char* manifest_txt = NULL;
         ssize_t manifest_txt_len = fs_entry_serialize_manifest( state->core, reqdat.fs_path, &manifest_txt, true );
         
         if( manifest_txt_len > 0 ) {
            md_create_HTTP_response_ram_nocopy( resp, "text/plain", 200, manifest_txt, manifest_txt_len );
            http_make_default_headers( resp, sb.st_mtime, manifest_txt_len, true );

            SG_debug( "served manifest %s.%" PRId64 "/manifest.%ld.%ld, %zd bytes\n", reqdat.fs_path, reqdat.file_version, reqdat.manifest_timestamp.tv_sec, reqdat.manifest_timestamp.tv_nsec, manifest_txt_len );
         }
         else {
            // send error response
            Serialization::ManifestMsg error_mmsg;
            fs_entry_manifest_error( &error_mmsg, manifest_txt_len, "" );
            
            md_sign< Serialization::ManifestMsg >( state->ms->gateway_key, &error_mmsg );
            
            char* manifest_bits = NULL;
            size_t manifest_len = 0;
            
            int rc = md_serialize< Serialization::ManifestMsg >( &error_mmsg, &manifest_bits, &manifest_len );
            
            if( rc != 0 ) {
               SG_error("md_serialize rc = %d\n", rc );
               char buf[100];
               snprintf(buf, 100, "md_serialize rc = %d\n", rc );
               http_io_error_resp( resp, 500, buf );
            }
            else {
               md_create_HTTP_response_ram_nocopy( resp, "text/plain", 200, manifest_bits, manifest_len );
               http_make_default_headers( resp, sb.st_mtime, manifest_len, true );

               SG_debug( "served error manifest %s.%" PRId64 "/manifest.%ld.%ld, %zd bytes\n", reqdat.fs_path, reqdat.file_version, reqdat.manifest_timestamp.tv_sec, reqdat.manifest_timestamp.tv_nsec, manifest_len );
            }
         }

         md_gateway_request_data_free( &reqdat );
         
         return resp;
      }
      else {

         // TODO: request for a file
         // redirect to its manifest for now
         md_create_HTTP_response_ram_static( resp, "text/plain", 400, "INVALID REQUEST", strlen("INVALID REQUEST") + 1 );
         md_gateway_request_data_free( &reqdat );

         return resp;
      }
   }

   // unreachable
   return NULL;
}


// extract some useful information from the message
bool syndicate_extract_file_info( Serialization::WriteMsg* msg, char const** fs_path, uint64_t* file_id, uint64_t* coordinator_id, int64_t* file_version ) {
   switch( msg->type() ) {
      case Serialization::WriteMsg::ACCEPTED: {
         SG_debug("%s", "Got ACCEPTED\n");
         if( !msg->has_accepted() ) {
            return false;
         }
         
         *file_id = msg->accepted().file_id();
         *coordinator_id = msg->accepted().coordinator_id();
         *fs_path = msg->accepted().fs_path().c_str();
         *file_version = msg->accepted().file_version();
         break;
      }
      case Serialization::WriteMsg::WRITE: {
         SG_debug("%s", "Got WRITE\n");
         if( !msg->has_metadata() || !msg->blocks().size() == 0 ) {
            return false;
         }

         *file_id = msg->metadata().file_id();
         *coordinator_id = msg->metadata().coordinator_id();
         *fs_path = msg->metadata().fs_path().c_str();
         *file_version = msg->metadata().file_version();

         break;
      }

      case Serialization::WriteMsg::TRUNCATE: {
         SG_debug("%s", "Got TRUNCATE\n");
         if( !msg->has_truncate() ) {
            SG_error("%s", "msg has no truncate block\n");
            return false;
         }
         if( msg->blocks().size() == 0 ) {
            SG_error("%s", "msg has no blocks\n");
            return false;
         }

         *file_id = msg->truncate().file_id();
         *coordinator_id = msg->truncate().coordinator_id();
         *fs_path = msg->truncate().fs_path().c_str();
         *file_version = msg->truncate().file_version();

         break;
      }

      case Serialization::WriteMsg::DETACH: {
         SG_debug("%s", "Got DETACH\n");
         if( !msg->has_detach() ) {
            return false;
         }

         *file_id = msg->detach().file_id();
         *coordinator_id = msg->detach().coordinator_id();
         *fs_path = msg->detach().fs_path().c_str();
         *file_version = msg->detach().file_version();

         break;
      }

      case Serialization::WriteMsg::RENAME: {
         SG_debug("%s", "Got RENAME\n");
         if( !msg->has_rename() ) {
            return false;
         }
         
         *file_id = msg->rename().file_id();
         *coordinator_id = msg->rename().coordinator_id();
         *file_version = msg->rename().file_version();
         *fs_path = msg->rename().old_fs_path().c_str();
         
         break;
      }
      
      default: {
         // unknown
         SG_error( "unknown message type %d\n", msg->type() );
         return false;
      }
   }

   return true;
}


// make an HTTP response from a protobuf ack
void syndicate_make_msg_ack( struct md_HTTP_connection_data* md_con_data, Serialization::WriteMsg* ack ) {
   string ack_str;
   char const* ack_txt = NULL;
   size_t ack_txt_len = 0;

   struct syndicate_connection* syncon = (struct syndicate_connection*)md_con_data->cls;
   struct syndicate_state *state = syncon->state;
   struct ms_client* client = state->ms;

   ack->set_signature( string("") );

   ack->SerializeToString( &ack_str );

   md_con_data->resp = SG_CALLOC( struct md_HTTP_response, 1 );

   // sign this message
   ack_txt = ack_str.data();
   ack_txt_len = ack_str.size();

   char* sigb64 = NULL;
   size_t sigb64_len = 0;

   int rc = md_sign_message( client->gateway_key, ack_txt, ack_txt_len, &sigb64, &sigb64_len );
   if( rc != 0 ) {
      SG_error("md_sign_message rc = %d\n", rc );
      md_create_HTTP_response_ram_static( md_con_data->resp, "text/plain", 500, MD_HTTP_500_MSG, strlen(MD_HTTP_500_MSG) + 1 );
      return;
   }

   ack->set_signature( string(sigb64, sigb64_len) );

   ack->SerializeToString( &ack_str );

   ack_txt = ack_str.data();
   ack_txt_len = ack_str.size();

   SG_debug( "ack message of length %zu\n", ack_txt_len );
   md_create_HTTP_response_ram( md_con_data->resp, "application/octet-stream", 200, ack_txt, ack_txt_len );

   free( sigb64 );
}


// fill a WriteMsg with a list of blocks
void syndicate_make_accepted_msg( Serialization::WriteMsg* writeMsg ) {
   writeMsg->set_type( Serialization::WriteMsg::ACCEPTED );
}


// extract and verify a write message
int syndicate_parse_write_message( struct syndicate_state* state, Serialization::WriteMsg* msg, char* msg_buf, size_t msg_sz ) {
   // extract the actual message
   int rc = md_parse< Serialization::WriteMsg >( msg, msg_buf, msg_sz );
   if( rc != 0 ) {
      SG_error("md_parse rc = %d\n", rc );
      return -EBADMSG;
   }
   
   rc = ms_client_verify_gateway_message< Serialization::WriteMsg >( state->ms, state->core->volume, SYNDICATE_UG, msg->gateway_id(), msg );
   if( rc != 0 ) {
      SG_error("Message from Gateway %" PRIu64 " could not be verified! rc = %d\n", msg->gateway_id(), rc );
      return -EBADMSG;
   }

   return 0;
}


// verify remote operation
int syndicate_verify_caller_privileges( struct syndicate_state* state, uint64_t gateway_id, uint64_t claimed_user_id, uint64_t claimed_volume_id, uint64_t caps ) {
   
   // can only receive messages from within our Volume
   if( claimed_volume_id != state->core->volume ) {
      SG_error("Invalid Volume %" PRIu64 "\n", claimed_volume_id );
      return -EINVAL;
   }
   
   // capability check---can this gateway even perform these operations?
   int err = ms_client_check_gateway_caps( state->core->ms, SYNDICATE_UG, gateway_id, caps );
   if( err != 0 ) {
      SG_error("ms_client_check_gateway_caps( %" PRIu64 " ) for capabilities 0x%" PRIX64 " rc = %d\n", gateway_id, caps, err );
      return err;
   }
   
   // validate user ID against gateway cert
   uint64_t user_id = (uint64_t)(-1);
   err = ms_client_get_gateway_user( state->core->ms, SYNDICATE_UG, gateway_id, &user_id );
   
   if( err != 0 ) {
      SG_error("ms_client_get_gateway_user( %" PRIu64 " ) rc = %d\n", gateway_id, err );
      return err;
   }
   
   if( user_id != claimed_user_id ) {
      // user mismatch
      SG_error("Caller claims to be running for user %" PRIu64 ", but the caller's certificate indicates user %" PRIu64 "\n", claimed_user_id, user_id );
      return -EPERM;
   }
   
   // validate volume ID against gateway cert
   uint64_t volume_id = (uint64_t)(-1);
   err = ms_client_get_gateway_volume( state->core->ms, SYNDICATE_UG, gateway_id, &volume_id );
   
   if( volume_id != claimed_volume_id ) {
      // volume mismatch
      SG_error("Caller claims to be running in Volume %" PRIu64 ", but the caller's certificate indicates Volume %" PRIu64 "\n", claimed_volume_id, volume_id );
      return -EINVAL;
   }
   
   return 0;
}


// POST finish--extract the pending message and handle it
void SG_server_HTTP_POST_finish( struct md_HTTP_connection_data* md_con_data ) {

   struct syndicate_connection* syncon = (struct syndicate_connection*)md_con_data->cls;
   struct syndicate_state *state = syncon->state;
   md_response_buffer_t* rb = md_con_data->rb;
   Serialization::WriteMsg ack;

   // prepare an ACK
   char const* fs_path = NULL;
   uint64_t file_id = 0;
   uint64_t coordinator_id = 0;
   int64_t file_version = -1;
   int rc = 0;
   bool no_ack = false;

   char* msg_buf = md_response_buffer_to_string( rb );
   size_t msg_sz = md_response_buffer_size( rb );
   Serialization::WriteMsg *msg = new Serialization::WriteMsg();

   SG_debug("received message of length %zu\n", msg_sz);

   // parse and verify the authenticity of the message
   rc = syndicate_parse_write_message( state, msg, msg_buf, msg_sz );

   free( msg_buf );

   if( rc != 0 ) {
      // can't handle this
      SG_error( "syndicate_parse_write_message rc = %d\n", rc );

      md_con_data->resp = SG_CALLOC( struct md_HTTP_response, 1 );
      
      if( rc == -EAGAIN ) {
         // tell the remote gateway to try again, while we refresh our UG listing with the MS
         char buf[10];
         sprintf(buf, "%d", rc);
         md_create_HTTP_response_ram( md_con_data->resp, "text/plain", 202, buf, strlen(buf) + 1);

         ms_client_start_config_reload( state->ms );
      }
      else {
         md_create_HTTP_response_ram( md_con_data->resp, "text/plain", 400, "INVALID REQUEST", strlen("INVALID REQUEST") + 1 );
      }

      delete msg;
      
      return;
   }
   
   fs_entry_init_write_message( &ack, state->core, Serialization::WriteMsg::ERROR );

   if( !syndicate_extract_file_info( msg, &fs_path, &file_id, &coordinator_id, &file_version ) ) {
      // missing information
      ack.set_errorcode( -EBADMSG );
      ack.set_errortxt( string("Missing message data") );

      SG_error( "%s", "extract_file_info failed\n" );
      syndicate_make_msg_ack( md_con_data, &ack );

      delete msg;
      return;
   }

   int64_t current_version = fs_entry_get_version( state->core, fs_path );
   if( current_version < 0 ) {
      // couldn't find file
      ack.set_errorcode( current_version );
      ack.set_errortxt( string("Could not determine version") );

      SG_error( "fs_entry_get_version(%s) rc = %" PRId64 "\n", fs_path, current_version);
      syndicate_make_msg_ack( md_con_data, &ack );

      delete msg;
      return;
   }

   SG_debug( "got WriteMsg(type=%d, path=%s)\n", msg->type(), fs_path );
   
   // handle the message
   switch( msg->type() ) {
      case Serialization::WriteMsg::WRITE: {
         // got a WRITE message; we'll need to respond with an ACCEPTED message
         // and update the appropriate file manifest to redirect to this client for the block data.
         // Also, begin downloading the affected blocks
         
         
         // verify that this gateway can, in fact, write to us
         rc = syndicate_verify_caller_privileges( state, msg->gateway_id(), msg->user_id(), msg->volume_id(), SG_CAP_WRITE_DATA );
         if( rc != 0 ) {
            SG_error("syndicate_verify_caller_privileges() rc = %d\n", rc );
            
            ack.set_errorcode( -EPERM );
            ack.set_errortxt( string("Insufficient capabilities or privileges") );
         }
         
         else {
            // update this file's manifest (republishing it in the process)
            rc = fs_entry_remote_write( state->core, fs_path, file_id, file_version, coordinator_id, msg );
            if( rc == 0 ) {
               // create an ACCEPTED request--ask the remote writer to hold on to the blocks so we can collate them later
               syndicate_make_accepted_msg( &ack );
            }
            else {

               // error
               SG_error( "fs_entry_handle_remote_write(%s) rc = %d\n", fs_path, rc );
               
               ack.set_errorcode( rc );
               ack.set_errortxt( string("failed to update manifest") );
            }
         }
         
         break;
      }

      case Serialization::WriteMsg::TRUNCATE: {
         // received a truncate message.
         // Truncate the file, and if successful,
         // send back an ACCEPTED
         
         
         // verify that this gateway can, in fact, write to us
         rc = syndicate_verify_caller_privileges( state, msg->gateway_id(), msg->user_id(), msg->volume_id(), SG_CAP_WRITE_DATA );
         if( rc != 0 ) {
            SG_error("syndicate_verify_caller_privileges() rc = %d\n", rc );
            
            ack.set_errorcode( -EPERM );
            ack.set_errortxt( string("Insufficient capabilities or privileges") );
         }
         else {
            rc = fs_entry_versioned_truncate( state->core, fs_path, file_id, coordinator_id, msg->truncate().size(), file_version, msg->user_id(), msg->volume_id(), msg->gateway_id(), true );
            if( rc == 0 ) {
               ack.set_type( Serialization::WriteMsg::ACCEPTED );
            }
            else {
               SG_error("fs_entry_versioned_truncate(%s) rc = %d\n", fs_path, rc );
               
               ack.set_errorcode( rc );
               ack.set_errortxt( string("failed to truncate") );
            }
         }
         
         break;
      }

      case Serialization::WriteMsg::DETACH: {
         // received DETACH request.
         // Unlink this file, and send back an ACCEPTED
         
         
         // verify that this gateway can, in fact, write to us
         rc = syndicate_verify_caller_privileges( state, msg->gateway_id(), msg->user_id(), msg->volume_id(), SG_CAP_WRITE_DATA | SG_CAP_WRITE_METADATA );
         if( rc != 0 ) {
            SG_error("syndicate_verify_caller_privileges() rc = %d\n", rc );
            
            ack.set_errorcode( -EPERM );
            ack.set_errortxt( string("Insufficient capabilities or privileges") );
         }
         else {
            rc = fs_entry_versioned_unlink( state->core, fs_path, file_id, coordinator_id, file_version, msg->user_id(), msg->volume_id(), msg->gateway_id(), true );
            if( rc == 0 ) {
               ack.set_type( Serialization::WriteMsg::ACCEPTED );
            }
            else {
               SG_error("fs_entry_versioned_unlink(%s) rc = %d\n", fs_path, rc );
               ack.set_errorcode( rc );
               ack.set_errortxt( string("failed to unlink") );
            }
         }

         break;
      }
      
      case Serialization::WriteMsg::RENAME: {
         // Received a RENAME message.
         // Carry out the rename (informing the MS of the operation).
         
         
         // verify that this gateway can, in fact, write to us
         rc = syndicate_verify_caller_privileges( state, msg->gateway_id(), msg->user_id(), msg->volume_id(), SG_CAP_WRITE_DATA | SG_CAP_WRITE_METADATA );
         if( rc != 0 ) {
            SG_error("syndicate_verify_caller_privileges() rc = %d\n", rc );
            
            ack.set_errorcode( -EPERM );
            ack.set_errortxt( string("Insufficient capabilities or privileges") );
         }
         else {
            rc = fs_entry_remote_rename( state->core, msg );
            if( rc == 0 ) {
               ack.set_type( Serialization::WriteMsg::ACCEPTED );
            }
            else {
               SG_error("fs_entry_remote_rename(%s --> %s) rc = %d\n", msg->rename().old_fs_path().c_str(), msg->rename().new_fs_path().c_str(), rc );
               ack.set_errorcode( rc );
               ack.set_errortxt( string("failed to rename") );
            }
         }
         
         break;
      }

      default: {
         break;
      }
   }

   if( !no_ack ) {
      syndicate_make_msg_ack( md_con_data, &ack );
   }
   else {
      md_con_data->resp = SG_CALLOC( struct md_HTTP_response, 1 );
      md_create_HTTP_response_ram( md_con_data->resp, "text/plain", 200, "OK\n", strlen("OK\n") + 1 );
   }

   delete msg;

   return;
}

// HTTP cleanup handler, called after the data has been sent
void SG_server_HTTP_cleanup(struct MHD_Connection *connection, void *con_cls, enum MHD_RequestTerminationCode term) {
   struct syndicate_connection* syncon = (struct syndicate_connection*)con_cls;
   free( syncon );
}


// initialize
int SG_server_init( struct syndicate_state* state, struct md_HTTP* http_server ) {
   
   // start HTTP server
   memset( http_server, 0, sizeof( struct md_HTTP ) );
   md_HTTP_init( http_server, MD_HTTP_TYPE_STATEMACHINE  );
   
   http_server->HTTP_connect = SG_server_HTTP_connect;
   http_server->HTTP_GET_handler = SG_server_HTTP_GET_handler;
   http_server->HTTP_cleanup = SG_server_HTTP_cleanup;
   http_server->HTTP_HEAD_handler = SG_server_HTTP_HEAD_handler;
   http_server->HTTP_POST_iterator = md_response_buffer_upload_iterator;
   http_server->HTTP_POST_finish = SG_server_HTTP_POST_finish;
   http_server->HTTP_authenticate = SG_server_HTTP_authenticate;

   SG_debug( "Starting Syndicate HTTP server on port %d\n", state->conf.portnum );
   int rc = md_start_HTTP( http_server, state->conf.portnum, &state->conf );
   if( rc < 0 ) {
      SG_error( "failed to start HTTP; rc = %d\n", rc );
      return -1;
   }

   return 0;
}


// shutdown
int SG_server_shutdown( struct md_HTTP* server ) {
   SG_debug("%s", "HTTP server shutdown\n");
   md_stop_HTTP( server );
   md_free_HTTP( server );
   return 0;
}
