/*
 * Depricated; will not be used in the future.
 */

/*
   Copyright 2011 Jude Nelson

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

#include "blacklist.h"


Blacklist::Blacklist() {  
   pthread_mutex_init( &this->lock, NULL );
}

Blacklist::~Blacklist() { 
   pthread_mutex_destroy( &this->lock );
}

int Blacklist::init( char* path, struct md_syndicate_conf* conf ) {
   this->conf = conf;
   return this->read_blacklist_file( path, &this->bl );
}


void free_blacklisted_ent( struct blacklisted_ent* ent ) {
   if( ent->url ) {
      free( ent->url );
      ent->url = NULL;
   }
}

int Blacklist::blacklist_url( char* base_url, int64_t version ) {
   pthread_mutex_lock( &this->lock );
   
   // is this URL already blacklisted?  If so, just update the time
   string url_str = base_url;
   BlacklistSet::iterator i = this->bl.find( url_str );
   if( i != this->bl.end() ) {
      // entry exists
      i->second->last_version = version;
   }
   else {
      struct blacklisted_ent* ent = (struct blacklisted_ent*)calloc( sizeof(struct blacklisted_ent), 1 );
      ent->url = strdup( base_url );
      ent->last_version = version;
      this->bl[ url_str ] = ent;
   }
   
   pthread_mutex_unlock( &this->lock );
   
   dbprintf("Blacklist::blacklist_url: blacklisted %s\n", base_url );
   return 0;
}


int Blacklist::unblacklist_url( char* base_url ) {
   pthread_mutex_lock( &this->lock );
   
   // is this URL already blacklisted?
   string url_str = base_url;
   BlacklistSet::iterator i = this->bl.find( url_str );
   if( i != this->bl.end() ) {
      // entry exists!  unblacklist it
      struct blacklisted_ent* ent = i->second;
      this->bl.erase( i );
      free_blacklisted_ent( ent );
      free( ent->url );
   }
   
   pthread_mutex_unlock( &this->lock );
   
   dbprintf("Blacklist::unblacklist_url: unblacklisted %s\n", base_url );
   return 0;
}


bool Blacklist::get_version( char* base_url, int64_t* version ) {
   pthread_mutex_lock( &this->lock );
   
   string url_str = base_url;
   BlacklistSet::iterator i = this->bl.find( url_str );
   bool rc;
   if( i != this->bl.end() )
      rc = true;
   else
      rc = false;
   
   if( rc ) {
      *version = i->second->last_version;
   }
   pthread_mutex_unlock( &this->lock );
   return rc;
}


// line format:  URL blacklist_time
int Blacklist::read_blacklist_file( char* path, BlacklistSet* bls ) {
   FILE* f = fopen( path, "r" );
   if( !f ) {
      dbprintf("Blacklist::read_blacklist_file: could not open %s (errno %d)\n", path, -errno );
      return -errno;
   }
   
   char* eof = NULL;
   char url[URL_MAX+1];
   
   int line_cnt = 0;
   while( true ) {
      line_cnt++;
      
      memset( url, 0, URL_MAX+1 );
      eof = fgets( url, URL_MAX, f );
      if( eof == NULL )
         break;
      
      struct blacklisted_ent* ent = (struct blacklisted_ent*)calloc( sizeof(struct blacklisted_ent), 1 );
      
      char* url_str = strtok( url, " \t\n" );
      if( url_str == NULL ) {
         dbprintf("Blacklist::read_blacklist_file: invalid line %d\n", line_cnt );
         continue;
      }
      
      char* blacklist_time_str = strtok( NULL, " \t\n" );
      if( blacklist_time_str == NULL ) {
         dbprintf("Blacklist::read_blacklist_file: invalid line %d\n", line_cnt );
         continue;
      }
      
      int64_t version = strtol( blacklist_time_str, NULL, 10 );
      if( version == 0 ) {
         dbprintf("Blacklist::read_blacklist_file: invalid line %d\n", line_cnt );
         continue;
      }
      
      ent->url = strdup( url_str );
      ent->last_version = version;
      
      string bl_key = url_str;
      BlacklistSet::iterator i = bls->find( bl_key );
      if( i == bls->end() ) {
         (*bls)[ bl_key ] = ent;
      }
      else {
         // take the one with the later version
         if( i->second->last_version < ent->last_version ) {
            struct blacklisted_ent* old = i->second;
            bls->erase( i );
            free_blacklisted_ent( old );
            free(old);
            (*bls)[bl_key] = ent;
         }
         else {
            free_blacklisted_ent( ent );
            free( ent );
         }
      }
   }
   
   fclose( f );
   return 0;
}


int Blacklist::write_blacklist_file( char* path, BlacklistSet* bls ) {
   FILE* f = fopen( path, "w" );
   if( !f ) {
      dbprintf("Blacklist::write_blacklist_file: could not open %s (errno %d)\n", path, -errno );
      return -errno;
   }
   
   for( BlacklistSet::iterator i = bls->begin(); i != bls->end(); i++ ) {
      fprintf( f, "%s   %lld\n", i->second->url, i->second->last_version );
   }
   
   fclose( f );
   return 0;
}


int Blacklist::save( char* path ) {
   return this->write_blacklist_file( path, &this->bl );
}
