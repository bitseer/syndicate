#include "libsyndicate.h"
#include "syndicate.h"

#include <signal.h>
#include <getopt.h>

#define SERVE_FILE "/testfile"

int serve = 1;

void sighandle( int value ) {
   serve = 0;
}

int main(int argc, char** argv) {
   md_debug(1);
   md_error(1);
   SG_debug("%s\n", "starting up debugging");
   SG_error("%s\n", "starting up errors");

   int c;
   char* config_file = (char*)CLIENT_DEFAULT_CONFIG;
   int portnum = 0;

   struct md_HTTP syndicate_http;
   char* username = NULL;
   char* password = NULL;
   char* volume_name = NULL;
   char* volume_secret = NULL;
   char* ms_url = NULL;

   static struct option syndicate_options[] = {
      {"config-file",     required_argument,   0, 'c'},
      {"volume-name",     required_argument,   0, 'v'},
      {"volume-secret",   required_argument,   0, 's'},
      {"username",        required_argument,   0, 'u'},
      {"password",        required_argument,   0, 'p'},
      {"port",            required_argument,   0, 'P'},
      {"MS",              required_argument,   0, 'm'},
      {0, 0, 0, 0}
   };

   int opt_index = 0;

   while((c = getopt_long(argc, argv, "c:v:s:u:p:P:fm:", syndicate_options, &opt_index)) != -1) {
      switch( c ) {
         case 'v': {
            volume_name = optarg;
            break;
         }
         case 'c': {
            config_file = optarg;
            break;
         }
         case 's': {
            volume_secret = optarg;
            break;
         }
         case 'u': {
            username = optarg;
            break;
         }
         case 'p': {
            password = optarg;
            break;
         }
         case 'P': {
            portnum = strtol(optarg, NULL, 10);
            break;
         }
         case 'm': {
            ms_url = optarg;
            break;
         }
         default: {
            break;
         }
      }
   }
   
   int rc = syndicate_init( config_file, &syndicate_http, portnum, ms_url, volume_name, volume_secret, username, password );
   if( rc != 0 )
      exit(1);

   struct md_syndicate_conf* conf = syndicate_get_conf();
   if( portnum == 0 )
      portnum = conf->httpd_portnum;

   struct syndicate_state* state = syndicate_get_state();

   signal( SIGINT, sighandle );
   signal( SIGQUIT, sighandle );
   signal( SIGTERM, sighandle );

   // synchronous everything
   conf->default_write_freshness = 0;
   
   char file[PATH_MAX];
   memset(file, 0, PATH_MAX);
   strcpy( file, SERVE_FILE );


   // write to the file
   ssize_t file_size = conf->blocking_factor * 100;
   char* buf = SG_CALLOC( char, file_size );
   char fill = rand() % 26 + 'A';
   memset( buf, fill, file_size );

   struct fs_file_handle* fh = NULL;
   ssize_t nw = 0;

   struct timespec ts, ts2;
   
   char const* cleanup_fmt = "/bin/rm -rf %s %s /tmp/localwrite";
   char* cleanup_buf = SG_CALLOC( char, strlen(cleanup_fmt) + strlen(conf->staging_root) + strlen(conf->data_root) + 1 );
   sprintf(cleanup_buf, cleanup_fmt, conf->data_root, conf->staging_root);

   system(cleanup_buf);


   DATA_BLOCK( "create" );

   SG_BEGIN_TIMING_DATA( ts );
   
   // create the file
   fh = fs_entry_open( state->core, file, NULL, conf->owner, conf->volume, O_CREAT | O_SYNC | O_RDWR, 0666, &rc );
   if( rc != 0 ) {
      SG_error("fs_entry_open(%s) rc = %d\n", file, rc );
      exit(1);
   }

   SG_END_TIMING_DATA( ts, ts2, "create" );

   DATA_BLOCK( "write" );

   // mark the file as stale
   fs_entry_wlock( fh->fent );
   fs_entry_mark_read_stale( fh->fent );
   fs_entry_unlock( fh->fent );

   SG_BEGIN_TIMING_DATA( ts );
   
   // write the file
   nw = fs_entry_write( state->core, fh, buf, file_size, 0 );
   if( nw != file_size ) {
      SG_error("fs_entry_write(%s) rc = %ld\n", file, nw );
      exit(1);
   }

   SG_END_TIMING_DATA( ts, ts2, "local write + MS revalidate" );

   DATA_BLOCK( "close" );

   SG_BEGIN_TIMING_DATA( ts );
   
   // close
   rc = fs_entry_close( state->core, fh );
   if( rc != 0 ) {
      SG_error("fs_entry_close(%s) rc = %d\n", file, rc );
      exit(1);
   }


   SG_END_TIMING_DATA( ts, ts2, "close" );

   free( fh );


   // serve the file!
   DATA_BLOCK("serve");
   printf("OPT begin_serve\n");

   printf("state = %p, state->core = %p\n", state, state->core );
   while( serve ) {
      sleep( 1 );
   }
   
   printf("OPT end_serve\n");
   DATA_BLOCK("unlink");

   SG_BEGIN_TIMING_DATA( ts );

   // unlink
   rc = fs_entry_versioned_unlink( state->core, file, -1, conf->owner, conf->volume );
   if( rc != 0 ) {
      SG_error("fs_entry_unlock(%s) rc = %d\n", file, rc );
      exit(1);
   }

   SG_END_TIMING_DATA( ts, ts2, "unlink" );

   DATA_BLOCK("");
   
   syndicate_destroy();

   system(cleanup_buf);
   free( buf );
   return 0;
}
