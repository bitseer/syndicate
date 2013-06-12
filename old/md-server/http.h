/*
   Copyright 2012 Jude Nelson

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

#ifndef _HTTP_H_
#define _HTTP_H_

#include "libsyndicate.h"

#include <queue>
#include <deque>
#include <sstream>

using namespace std;

// connection data
struct HTTP_connection_data {
   char* line_buf;
   size_t line_offset;
   struct md_user_entry *user;
   struct md_update update;
   int error;
};

// directory serialization data
struct HTTP_dir_serialization_data {
   response_buffer_t rb;
   struct md_user_entry* user;
   struct md_syndicate_conf* conf;
};


int http_init( struct md_HTTP* http, struct md_syndicate_conf* conf, struct md_user_entry** users );
int http_shutdown( struct md_HTTP* http );
void http_reload( struct md_user_entry** users, char** replica_servers );

#endif
