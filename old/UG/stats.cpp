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

#include "stats.h"

const char* stat_names[] = {
   NULL,
   "syndicatefs_getattr        ",
   "syndicatefs_readlink       ",
   "syndicatefs_mknod          ",
   "syndicatefs_mkdir          ",
   "syndicatefs_unlink         ",
   "syndicatefs_rmdir          ",
   "syndicatefs_symlink        ",
   "syndicatefs_rename         ",
   "syndicatefs_link           ",
   "syndicatefs_chmod          ",
   "syndicatefs_chown          ",
   "syndicatefs_truncate       ",
   "syndicatefs_utime          ",
   "syndicatefs_open           ",
   "syndicatefs_read           ",
   "syndicatefs_write          ",
   "syndicatefs_statfs         ",
   "syndicatefs_flush          ",
   "syndicatefs_release        ",
   "syndicatefs_fsync          ",
   "syndicatefs_setxattr       ",
   "syndicatefs_getxattr       ",
   "syndicatefs_listxattr      ",
   "syndicatefs_removexattr    ",
   "syndicatefs_opendir        ",
   "syndicatefs_readdir        ",
   "syndicatefs_releasedir     ",
   "syndicatefs_fsyncdir       ",
   "syndicatefs_access         ",
   "syndicatefs_create         ",
   "syndicatefs_ftruncate      ",
   "syndicatefs_fgetattr       "
};


inline uint64_t time_microseconds() {
   timespec ts;
   clock_gettime( CLOCK_REALTIME, &ts );
   return (uint64_t)ts.tv_sec * 1000000LL + (uint64_t)ts.tv_nsec / 1000LL;  
}


void enter_func_active( uint64_t* begin_times, int type ) {
   begin_times[type] = time_microseconds();
}

void enter_func_inactive( uint64_t* begin_times, int type ) {
   return;
}

void leave_func_active( uint64_t* counts, uint64_t* elapsed, uint64_t* errors, uint64_t* begins, int type, int rc ) {
   ++ counts[type];
   if( rc != 0 )
      ++ errors[type];
   else
      elapsed[type] = time_microseconds() - begins[type];
}

void leave_func_inactive( uint64_t* counts, uint64_t* elapsed, uint64_t* errors, uint64_t* begins, int type, int rc ) {
   return;
}


Stats::Stats( char* op ) {
   if( op ) {
      this->output_path = (char*)calloc( strlen( op ), 1 );
      strcpy( this->output_path, op );
   }
   else {
      this->output_path = NULL;
   }
   
   memset( this->call_counts, 0, sizeof(uint64_t) * STAT_NUM_TYPES );
   memset( this->elapsed_time, 0, sizeof(uint64_t) * STAT_NUM_TYPES );
   memset( this->begin_call_times, 0, sizeof(uint64_t) * STAT_NUM_TYPES );
   memset( this->call_errors, 0, sizeof(uint64_t) * STAT_NUM_TYPES );
   
   this->gather_stats = true;
   this->enter_func = enter_func_active;
   this->leave_func = leave_func_active;
}

Stats::~Stats() {
   if( this->output_path )
      free( this->output_path );
}




void Stats::use_conf( struct md_syndicate_conf* conf ) {
   this->gather_stats = conf->gather_stats;
   if( this->gather_stats ) {
      this->enter_func = enter_func_active;
      this->leave_func = leave_func_active;
   }
   else {
      this->enter_func = enter_func_inactive;
      this->leave_func = leave_func_inactive;
   }
}


void Stats::enter( int stat_type ) {
   (*this->enter_func)( this->begin_call_times, stat_type );
}

void Stats::leave( int stat_type, int rc ) {
   (*this->leave_func) (this->call_counts, this->elapsed_time, this->call_errors, this->begin_call_times, stat_type, rc );
}

string Stats::dump() {
   if( this->gather_stats ) {
      
      string ret = "Number of calls:\n";
      
      char buf[100];
      for( int i = 1; i < STAT_NUM_TYPES; i++ ) {
         memset( buf, 0, 100 );
         sprintf(buf, "%" PRIu64, this->call_counts[i] );
         ret += string("    ") + string( stat_names[i] ) + string( buf ) + string("\n");
      }
      
      ret += "\nTime in each successful call (microseconds):\n";
      
      for( int i = 1; i < STAT_NUM_TYPES; i++ ) {
         memset( buf, 0, 100 );
         sprintf(buf, "%" PRIu64, this->elapsed_time[i] );
         ret += string("    ") + string( stat_names[i] ) + string( buf ) + string("\n");
      }
      
      ret += "\nAverage time per successful call (microseconds):\n";
      
      for( int i = 1; i < STAT_NUM_TYPES; i++ ) {
         memset( buf, 0, 100 );
         if( this->call_counts[i] != 0 && this->call_errors[i] < this->call_counts[i] )
            sprintf(buf, "%lf", (double)this->elapsed_time[i] / ((double)this->call_counts[i] - (double)this->call_errors[i]) );
         else
            sprintf(buf, "n/a");
         
         ret += string("    ") + string( stat_names[i] ) + string( buf ) + string("\n");
      }
      
      ret += "\nNumber of errors:\n";
      
      for( int i = 1; i < STAT_NUM_TYPES; i++ ) {
         memset( buf, 0, 100 );
         sprintf(buf, "%" PRId64, this->call_errors[i]);
         
         ret += string("   ") + string( stat_names[i] ) + string( buf ) + string("\n");
      }
      
      return ret;
   }
   else {
      return string("Statistic gathering is disabled in the configuration\n");
   }
}
