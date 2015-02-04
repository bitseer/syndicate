#include "libsyndicate.h"
#include "syndicate.h"

#include <getopt.h>

int main(int argc, char** argv) {
   md_debug(1);
   md_error(1);
   SG_debug("%s\n", "starting up debugging");
   SG_error("%s\n", "starting up errors");

   int c;
   char* config_file = (char*)CLIENT_DEFAULT_CONFIG;
   int portnum = 0;
   bool foreground = false;

   char* logfile = NULL;
   char* pidfile = NULL;

   struct md_HTTP syndicate_http;
   char* username = NULL;
   char* password = NULL;
   char* volume_name = NULL;
   char* volume_secret = NULL;
   char* ms_url = NULL;

   bool local_write_test = true;
   bool ping_test = true;
   char* host_string = NULL;

   static struct option syndicate_options[] = {
      {"config-file",     required_argument,   0, 'c'},
      {"volume-name",     required_argument,   0, 'v'},
      {"volume-secret",   required_argument,   0, 's'},
      {"username",        required_argument,   0, 'u'},
      {"password",        required_argument,   0, 'p'},
      {"port",            required_argument,   0, 'P'},
      {"foreground",      no_argument,         0, 'f'},
      {"no-write-test",   no_argument,         0, 'n'},
      {"no-ping-test",    no_argument,         0, 'N'},
      {"MS",              required_argument,   0, 'm'},
      {"path-entry",      required_argument,   0, 'E'},
      {0, 0, 0, 0}
   };

   int opt_index = 0;

   while((c = getopt_long(argc, argv, "c:v:s:u:p:P:fm:nNE:", syndicate_options, &opt_index)) != -1) {
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
         case 'f': {
            foreground = true;
            break;
         }
         case 'n': {
            local_write_test = false;
            break;
         }
         case 'N': {
            ping_test = false;
            break;
         }
         case 'E': {
            host_string = optarg;
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

   //sleep(15);
   
   struct md_syndicate_conf* conf = syndicate_get_conf();
   if( portnum == 0 )
      portnum = conf->httpd_portnum;

   struct syndicate_state* state = syndicate_get_state();

   // synchronous everything
   conf->default_read_freshness = 0;
   conf->default_write_freshness = 0;


   if( host_string == NULL ) {
      host_string = conf->hostname;
   }
   
   char dir[PATH_MAX];
   memset(dir, 0, PATH_MAX);
   sprintf(dir, "/%s", host_string);


   char file[PATH_MAX];
   memset(file, 0, PATH_MAX);
   sprintf(file, "/%s/%s", host_string, host_string);


   // write to the file
   ssize_t file_size = conf->blocking_factor * 100;
   char* buf = SG_CALLOC( char, file_size );
   char fill = rand() % 26 + 'A';
   memset( buf, fill, file_size );

   char* wbuf = SG_CALLOC( char, conf->blocking_factor );
   memset( wbuf, fill, conf->blocking_factor );
   

   struct fs_file_handle* fh = NULL;
   ssize_t nw = 0;


   struct timespec ts, ts2;
   
   char const* cleanup_fmt = "/bin/rm -rf %s %s /tmp/localwrite";
   char* cleanup_buf = SG_CALLOC( char, strlen(cleanup_fmt) + strlen(conf->staging_root) + strlen(conf->data_root) + 1 );
   sprintf(cleanup_buf, cleanup_fmt, conf->data_root, conf->staging_root);

   if( local_write_test ) {
      system(cleanup_buf);

      // local write without Syndicate, for comparison
      DATA_BLOCK("raw write");

      SG_BEGIN_TIMING_DATA(ts);

      rc = mkdir("/tmp/localwrite", 0777 );
      if( rc != 0 ) {
         rc = -errno;
         SG_error("mkdir /tmp/localwrite errno = %d\n", rc );
         syndicate_destroy();
         exit(1);
      }

      char path[PATH_MAX];

      for( int i = 0; i < 100; i++ ) {
         memset(path, 0, PATH_MAX);
         sprintf(path, "/tmp/localwrite/%d", i );

         FILE* f = fopen( path, "w" );
         if( f == NULL ) {
            int errsv = errno;
            SG_error("fopen(%s) errno = %d\n", path, errsv);
            exit(1);
         }
         ssize_t nw = fwrite( wbuf, 1, conf->blocking_factor, f );
         if( nw != conf->blocking_factor ) {
            int errsv = errno;
            SG_error("fwrite(%s) errno = %d\n", path, errsv );
            syndicate_destroy();
            exit(1);
         }

         fclose( f );
      }

      SG_END_TIMING_DATA(ts, ts2, "disk write, direct");

      // local read without Syndicate, for comparison
      DATA_BLOCK("raw read");


      SG_BEGIN_TIMING_DATA(ts);

      for( int i = 0; i < 100; i++ ) {
         memset(path, 0, PATH_MAX);
         sprintf(path, "/tmp/localwrite/%d", i );

         FILE* f = fopen( path, "r" );
         if( f == NULL ) {
            int errsv = errno;
            SG_error("fopen(%s) errno = %d\n", path, errsv);
            syndicate_destroy();
            exit(1);
         }
         ssize_t nr = fread( wbuf, 1, conf->blocking_factor, f );
         if( nr != conf->blocking_factor ) {
            int errsv = errno;
            SG_error("fread(%s) errno = %d\n", path, errsv );
            syndicate_destroy();
            exit(1);
         }
         fclose( f );
      }

      SG_END_TIMING_DATA(ts, ts2, "disk read, direct");
   }

   if( ping_test ) {
      DATA_BLOCK("ping");

      // ping google and save the results
      system("/bin/ping -c 1 syndicate-metadata.appspot.com > /tmp/ping.out");
      FILE* f = fopen("/tmp/ping.out", "r");
      if( f != NULL ) {
         char* buf = NULL;
         while(1) {
            size_t n = 0;
            ssize_t nr = getline( &buf, &n, f );
            if( nr == -1 )
               break;

            // remove \n
            buf[nr-1] = 0;

            // get just the ping time
            char* tmp = buf;
            char* tmp2 = NULL;
            while( 1 ) {
               char* tok = strtok_r( tmp, " ", &tmp2 );
               tmp = NULL;

               if( tok == NULL )
                  break;

               if( strncmp(tok, "time=", 5) == 0 ) {
                  double value = 0.0;
                  sscanf( tok, "time=%lf", &value );
                  DATA( "rtt", value / 1e3 );
                  break;
               }
            }
         }
         if( buf )
            free( buf );
      }
      else {
         SG_error("not found: %s\n", "/tmp/ping.out" );
         syndicate_destroy();
         exit(1);
      }
      fclose(f);
   }

   DATA_BLOCK("mkdir");
   
   // mkdir(dir)
   SG_BEGIN_TIMING_DATA(ts);
   
   rc = fs_entry_mkdir( state->core, dir, 0777, conf->owner, conf->volume );
   if( rc != 0 ) {
      SG_error("fs_entry_mkdir(%s) rc = %d\n", dir, rc );
      syndicate_destroy();
      exit(1);
   }

   SG_END_TIMING_DATA(ts, ts2, "mkdir");

   DATA_BLOCK("open");

   SG_BEGIN_TIMING_DATA(ts);
   
   // create the file
   fh = fs_entry_open( state->core, file, NULL, conf->owner, conf->volume, O_CREAT | O_SYNC | O_RDWR, 0666, &rc );
   if( rc != 0 ) {
      SG_error("fs_entry_open(%s) rc = %d\n", file, rc );
      syndicate_destroy();
      exit(1);
   }

   SG_END_TIMING_DATA(ts, ts2, "open");

   DATA_BLOCK("local write");

   // mark the file as stale
   fs_entry_wlock( fh->fent );
   fs_entry_mark_read_stale( fh->fent );
   fs_entry_unlock( fh->fent );
   
   SG_BEGIN_TIMING_DATA(ts);
   
   // write the file
   nw = fs_entry_write( state->core, fh, buf, file_size, 0 );
   if( nw != file_size ) {
      SG_error("fs_entry_write(%s) rc = %ld\n", file, nw );
      syndicate_destroy();
      exit(1);
   }

   SG_END_TIMING_DATA(ts, ts2, "local write + MS refresh");

   for( int i = 0; i < 10; i++ ) {
      char sbuf[1000];
      sprintf(sbuf, "local read %d", i);

      DATA_BLOCK(sbuf);

      // mark the file as stale
      fs_entry_wlock( fh->fent );
      fs_entry_mark_read_stale( fh->fent );
      fs_entry_unlock( fh->fent );


      SG_BEGIN_TIMING_DATA(ts);

      nw = fs_entry_read( state->core, fh, buf, file_size, 0 );
      if( nw != file_size ) {
         SG_error("fs_entry_read(%s) rc = %ld\n", file, nw );
         syndicate_destroy();
         exit(1);
      }

      sprintf(sbuf, "local read %d + MS refresh", i );
      SG_END_TIMING_DATA(ts, ts2, sbuf);
   }

   DATA_BLOCK("close");
   
   // close
   rc = fs_entry_close( state->core, fh );
   if( rc != 0 ) {
      SG_error("fs_entry_close(%s) rc = %d\n", file, rc );
      syndicate_destroy();
      exit(1);
   }

   free( fh );


   DATA_BLOCK("unlink");

   SG_BEGIN_TIMING_DATA(ts);
   
   // unlink
   rc = fs_entry_versioned_unlink( state->core, file, -1, conf->owner, conf->volume );
   if( rc != 0 ) {
      SG_error("fs_entry_unlock(%s) rc = %d\n", file, rc );
      syndicate_destroy();
      exit(1);
   }

   SG_END_TIMING_DATA(ts, ts2, "unlink");
   
   DATA_BLOCK("rmdir");

   SG_BEGIN_TIMING_DATA(ts);
   
   // rmdir
   rc = fs_entry_rmdir( state->core, dir, conf->owner, conf->volume );
   if( rc != 0 ) {
      SG_error("fs_entry_rmdir(%s) rc = %d\n", dir, rc );
      syndicate_destroy();
      exit(1);
   }

   SG_END_TIMING_DATA(ts, ts2, "rmdir");
   
   DATA_BLOCK("");

   syndicate_destroy();

   //system(cleanup_buf);
   free( buf );
   free( wbuf );
   return 0;
}
