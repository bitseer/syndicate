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

#ifndef _BLACKLIST_H_
#define _BLACKLIST_H_

#include "libsyndicate.h"

using namespace std;

struct blacklisted_ent {
   char* url;
   int64_t last_version;
};

typedef map<string, struct blacklisted_ent*> BlacklistSet;     // map URL to blacklisted ent

class Blacklist {

public:
   Blacklist();
   ~Blacklist();
   
   int init( char* path, struct md_syndicate_conf* conf );
   
   int blacklist_url( char* base_url, int64_t version );
   int unblacklist_url( char* base_url );
   bool get_version( char* base_url, int64_t* version );
   
   int read_blacklist_file( char* path, BlacklistSet* bl );
   int write_blacklist_file( char* path, BlacklistSet* bl );
   
   int save( char* path );
   
   BlacklistSet* get_blacklist() { return &this->bl; }
   
private:
   
   BlacklistSet bl;
   pthread_mutex_t lock;
   struct md_syndicate_conf* conf;
};


#endif
