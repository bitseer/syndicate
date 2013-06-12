#include "mdserverd.h"


// are we running?
bool g_running = true;

// path to the config file
char* g_config_file = NULL;

// path to the secrets file
char* g_secrets_file = NULL;

// global configuration
struct md_syndicate_conf* g_conf = NULL;

// HTTP daemon 
struct md_HTTP http;

// semaphore for reloading
sem_t reload_sem;

pthread_t reload_thread;


// usage
void usage( char* name, int exitrc ) {
   fprintf(stderr,
"\
Usage: %s [-f] [-c CONFIG] [-m MASTER_COPY_DIR] [-P HTTP_PORTNUM] [-u USER_SECRETS] [-l LOGFILE_PATH] [-p PID_FILE] [-k PID]\n\
Options:\n\
   -f                        Run in the foreground\n\
   -c CONFIG                 Use an alternate config file at CONFIG\n\
   -m MASTER_COPY_DIR        Use an alternate master copy directory instead of the one in the config\n\
   -P HTTP_PORTNUM           Listen on port HTTP_PORTNUM\n\
   -u USER_SECRETS           Read user ID, username, and password information from the file at USER_SECRETS\n\
   -l LOGFILE_PATH           Redirect stdout and stderr to LOGFILE_PATH\n\
   -p PID_FILE               Record the process's PID in PID_FILE\n\
   -k PID                    Reload a particular server's configuration, identifed by its PID\n\
\n\
Secrets file format:\n\
   user_id:username:SHA1(password)\n\
   user_id:username:SHA1(password)\n\
   ...\n\
\n\
where user_id is the user's numeric ID in Syndicate; username is their Syndicate username, and SHA1(password) is the SHA-1 hash of \n\
the user's Syndicate password\n",
name );
            
   exit(exitrc);
}


// signal handler for SIGINT, SIGTERM
void die_handler(int param) {
   if( g_running ) {
      g_running = false;

      sem_post( &reload_sem );
      
      if( g_conf->md_pidfile_path )
         unlink( g_conf->md_pidfile_path );

      if( g_conf->md_logfile )
         fclose( g_conf->md_logfile );
      
      google::protobuf::ShutdownProtobufLibrary();
      http_shutdown( &http );
      validator_shutdown();
      md_shutdown();

      if( g_config_file ) 
         free( g_config_file );

      if( g_secrets_file )
         free( g_secrets_file );

      exit(0);
   }
}


// signal handler for reloading
void reload_handler( int param ) {
   if( g_running ) {
      sem_post( &reload_sem );
   }
}

// set up signal handler
void setup_signals(void) {
   signal( SIGTERM, die_handler );
   signal( SIGINT, die_handler );
   signal( SIGQUIT, die_handler );
   signal( SIGUSR1, reload_handler );
}

// listen for a reload
void* reloader( void* arg ) {
   while( g_running ) {
      sem_wait( &reload_sem );

      if( !g_running )
         return NULL;

      
      // do the reload
      struct md_syndicate_conf new_conf;
      memset( &new_conf, 0, sizeof(new_conf) );
      
      if( g_config_file == NULL )
         continue;

      int rc = md_read_conf( g_config_file, &new_conf );
      if( rc != 0 ) {
         errorf("md_read_conf rc = %d\n", rc );
         continue;
      }

      // get the users next
      struct md_user_entry** users = md_parse_secrets_file( g_secrets_file );
      if( users == NULL ) {
         errorf("%s", "md_parse_secrets_file rc = NULL\n");
         continue;
      }
      
      // reload the HTTP server with new users
      md_http_wlock( &http );

      struct md_user_entry** old_users = http.users;
      char** old_replicas = g_conf->replica_urls;
      
      http.users = users;
      g_conf->replica_urls = new_conf.replica_urls;
      new_conf.replica_urls = NULL;

      md_http_unlock( &http );

      if( old_replicas )
         FREE_LIST( old_replicas );
      
      for( int i = 0; old_users[i] != NULL; i++ ) {
         md_free_user_entry( old_users[i] );
         free( old_users[i] );
      }
      free( old_users );

      md_free_conf( &new_conf );

      // do the reload
      http_reload( users, g_conf->replica_urls );

   }

   return NULL;
}

// program execution starts here!
int main( int argc, char** argv ) {
   curl_global_init(CURL_GLOBAL_ALL);

   // start up protocol buffers
   GOOGLE_PROTOBUF_VERIFY_VERSION;
   
   g_config_file = strdup( METADATA_DEFAULT_CONFIG );
   
   // process command-line options
   int c;
   int rc;
   bool make_daemon = true;
   bool good_input = true;
   char* mc_root = NULL;
   char* logfile = NULL;
   char* pidfile = NULL;
   int portnum = 0;
   int reload_pid = 0;
   
   while((c = getopt(argc, argv, "fc:m:p:u:l:P:k:")) != -1) {
      switch( c ) {
         case 'f': {
            make_daemon = false;
            break;
         }
         case '?': {
            usage( argv[0], 1 );
         }
         case 'c': {
            g_config_file = realpath( optarg, NULL );
            break;
         }
         case 'm': {
            mc_root = realpath( optarg, NULL );
            break;
         }
         case 'P': {
            portnum = strtol( optarg, NULL, 10 );
            if( portnum <= 0 )
               good_input = false;
            break;
         }
         case 'u': {
            g_secrets_file = realpath( optarg, NULL );
            break;
         }
         case 'l': {
            logfile = strdup( optarg );
            break;
         }
         case 'p': {
            pidfile = realpath( optarg, NULL );
            break;
         }
         case 'k': {
            reload_pid = strtol( optarg, NULL, 10 );
            if( reload_pid <= 0 )
               good_input = false;
            break;
         }
         default: {
            fprintf(stderr, "Ignoring unrecognized option %c\n", c);
            good_input = false;
            break;
         }
      }
   }
   
   if( !good_input ) {
      usage( argv[0], 1 );
   }

   if( reload_pid > 0 ) {
      // all we're doing is telling a running metadata server to reload
      kill( reload_pid, SIGUSR1 );
      exit(0);
   }
   
   // read the config
   struct md_syndicate_conf conf;
   g_conf = &conf;

   dbprintf("reading config %s\n", g_config_file);
   if( md_read_conf( g_config_file, &conf ) != 0 ) {
      errorf("Could not read config at %s\n", g_config_file);
      usage( argv[0], 1 );
   }
   
   // user-given portnum?
   if( portnum > 0 ) {
      conf.portnum = portnum;
   }
   if( conf.portnum == 0 ) {
      errorf("Invalid port number %d.  Specify PORTNUM in the config file or pass -p\n", conf.portnum);
      exit(1);
   }
   
   // master copy supplied in args?
   if( mc_root ) {
      if( conf.master_copy_root ) {
         free( conf.master_copy_root );
      }
      conf.master_copy_root = mc_root;
   }
   
   // secrets file supplied in args?
   if( g_secrets_file ) {
      if( conf.secrets_file ) {
         free( conf.secrets_file );
      }
      conf.secrets_file = g_secrets_file;
   }
   else if( conf.secrets_file ) {
      g_secrets_file = strdup( conf.secrets_file );
   }
  
   // pidfile supplied in args?
   if( pidfile ) {
      if( conf.md_pidfile_path ) {
         free( conf.md_pidfile_path );
      }
      conf.md_pidfile_path = pidfile;
   }

   dbprintf("%s", "initializing libsyndicate\n");
   
   // set the config
   if( md_init( &conf, NULL ) != 0 )
      exit(1);
   
   md_connect_timeout( conf.query_timeout );
   md_signals( 0 );        // no signals

   dbprintf("reading users file %s\n", conf.secrets_file );

   // read the users file
   struct md_user_entry **users = NULL;
   if( conf.secrets_file ) {
      users = md_parse_secrets_file( conf.secrets_file );
      if( users == NULL ) {
         exit(1);
      }
   }
   else {
      errorf("No secrets file given.  Pass -u or specify a value for %s in the config\n", SECRETS_FILE_KEY );
      usage( argv[0], 1 );
   }

   // need to daemonize?
   if( make_daemon ) {
      FILE* log = NULL;
      rc = md_daemonize( logfile, conf.md_pidfile_path, &log );
      if( rc < 0 ) {
         errorf("md_daemonize rc = %d\n", rc );
         exit(1);
      }

      if( log )
         conf.md_logfile = log;
   }

   // setup the reload semaphore
   sem_init( &reload_sem, 0, 0 );

   reload_thread = md_start_thread( reloader, NULL, true );
   
   // start HTTP
   rc = http_init( &http, &conf, users );
   if( rc != 0 ) {
      exit(1);
   }

   // start validator
   rc = validator_init( &conf );
   if( rc != 0 ) {
      exit(1);
   }

   setup_signals();

   while( 1 ) {
      sleep(1);
   }

   // we never reach this point--the signal handler cleans up
   return 0;
}

