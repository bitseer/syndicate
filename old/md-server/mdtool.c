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

#include "mdtool.h"


// global configuration
struct md_syndicate_conf CONF;

// print usage and exit
void usage( char* name, int exitrc ) {
   fprintf(stderr, "Usage: %s [OPTS] [CMD]\n", name);
   fprintf(stderr, "Terminology:\n");
   fprintf(stderr, "  DIR is the root of the master copy\n");
   fprintf(stderr, "  PATH is a path rooted at DIR.  So, if DIR=/foo/bar/, and PATH=/baz/goo, then\n");
   fprintf(stderr, "  the absolute path to goo is /foo/bar/baz/goo.  PATH must be rooted in DIR.\n");
   fprintf(stderr, "\n");
   fprintf(stderr, "Option syntax:\n");
   fprintf(stderr, "  -c, --conf=CONF     Path to the configuration file to use\n");
   fprintf(stderr, "  -d, --dir=DIR       Perform operation in DIR.  If not specified, DIR is the\n");
   fprintf(stderr, "                      value of MDROOT in the configuration file\n");
   fprintf(stderr, "\n");
   fprintf(stderr, "Command syntax:\n");
   fprintf(stderr, "  add PATH URL OWNER MODE SIZE MTIME [CHECKSUM]   Add entry for URL for path PATH in base DIR.\n");
   fprintf(stderr, "  unpack FILE                                     Recreate a directory hierarchy from an existing metadata in the file at PATH.\n");
   fprintf(stderr, "  remove PATH                                     Remove URL entry for PATH in DIR.\n");
   fprintf(stderr, "  make                                            Walk DIR and print the metadata to standard out.\n");
   exit(exitrc);
}


// program execution starts here
int main( int argc, char** argv ) {
   char mdroot[PATH_MAX];
   memset(mdroot, 0, PATH_MAX);
   char const* conf_path = METADATA_DEFAULT_CONFIG;
   
   // load the config
   struct md_syndicate_conf conf;
   
   int c;
   int i = 1;
   
   static struct option long_options[] = {
      {"dir",     required_argument,   0, 'd'},
      {"conf",    optional_argument,   0, 'c'},
      {0, 0, 0, 0}
   };
   
   int opt_index = 0;

   while((c = getopt_long(argc, argv, "c:d:", long_options, &opt_index)) != -1) {
      switch( c ) {
         case 'd': {
            memset(mdroot, 0, PATH_MAX );
            if( realpath(optarg, mdroot) == NULL ) {
               fprintf(stderr, "Could not convert %s to absolute path\n", optarg);
               exit(1);
            }
            break;
         }
         case 'c': {
            conf_path = optarg;
            break;
         }
         case '?': {
            usage( argv[0], 1 );
         }
         default: {
            fprintf(stderr, "Ignoring unrecognized option %c\n", c);
            break;
         }
      }
   }
   
   int rc = md_read_conf( (char*)conf_path, &conf);
   if( rc == 0 ) {
      strcpy( mdroot, conf.master_copy_root );
   }
   else {
      if( getcwd( mdroot, PATH_MAX ) == NULL ) {
         perror("Could determine master copy root\n");
         exit(1);
      }
   }
   
   md_init( &conf, NULL );
   
   i = optind;
   
   // act on the command
   if( strcmp( argv[i], "add" ) == 0 ) {
      if( i + 5 < argc ) {
         struct md_entry file_to_add;
         memset( &file_to_add, 0, sizeof(file_to_add) );
         file_to_add.path = argv[i+1];
         file_to_add.url = argv[i+2];
         file_to_add.owner = strtol(argv[i+3], NULL, 10);
         file_to_add.mode = strtol(argv[i+4], NULL, 8);
         file_to_add.size = strtol(argv[i+5], NULL, 10);
         file_to_add.mtime_sec = strtol(argv[i+6], NULL, 10 );
         file_to_add.mtime_nsec = 0;
         if( i + 7 < argc ) {
            file_to_add.checksum = sha1_data( argv[i+7] );
         }
         
         if( file_to_add.owner == 0 ) {
            fprintf(stderr, "ERR: no UID specified.  Set METADATA_UID in your config or pass a UID\n");
            exit(1);
         }
         file_to_add.local_path = NULL;
         
         rc = md_add_mc_entry( mdroot, &file_to_add );
         if( rc != 0 ) {
            fprintf(stderr, "ERR: could not add (%s, %s) to master copy, rc = %d\n", argv[i+1], argv[i+2], rc);
            exit(1);
         }
         exit(0);
      }
      else {
         usage( argv[0], 1 );
      }
   }
   
   
   else if( strcmp( argv[i], "unpack" ) == 0 ) {
      if( i + 1 < argc ) {
         // open the file
         ifstream file(argv[i + 1]);
         if( !file.is_open() ) {
            fprintf(stderr, "ERR: could not read %s\n", argv[i+1] );
            exit(1);
         }

         // line buffer
         string line;
         
         int line_cnt = 0;
         bool err = false;
         
         while( !file.eof() ) {
            getline( file, line );
            if( line.size() < 1 )
               continue;
            
            struct md_entry next_file;
            memset( &next_file, 0, sizeof(next_file) );
            rc = md_extract( (char*)line.c_str(), &next_file );
            
            if( rc != 0 ) {
               fprintf(stderr, "ERR: could not read line %s\n", line.c_str() );
               err = true;
               continue;
            }
            
            else {
               rc = md_add_mc_entry( mdroot, &next_file );
               if( rc != 0 ) {
                  fprintf(stderr, "ERR: could not add (%s, %s) to master copy, rc = %d\n", next_file.path, next_file.url, rc );
                  err = true;
               }
            }
            
            md_entry_free( &next_file );
            
            line_cnt ++;
         }
         
         file.close();
         
         if( err ) {
            exit(1);
         }
         else {
            exit(0);
         }
      }
      else {
         usage( argv[0], 1 );
      }
   }
   
   else if( strcmp( argv[i], "remove" ) == 0 ) {
      if( i + 1 < argc ) {
         struct md_entry file_to_remove;
         file_to_remove.path = argv[i+1];
         file_to_remove.url = NULL;
         file_to_remove.local_path = NULL;
         
         // read this entry
         struct md_entry existing_file;
         memset( &existing_file, 0, sizeof(existing_file) );
         rc = md_read_entry( mdroot, argv[i+1], &existing_file );
         if( rc != 0 ) {
            fprintf(stderr, "ERR: could not read %s in %s\n", argv[i+1], mdroot );
            exit(1);
         }
         
         rc = md_remove_mc_entry( mdroot, &file_to_remove );
         if( rc != 0 ) {
            fprintf(stderr, "ERR: could not remove %s from master copy, rc = %d\n", file_to_remove.path, rc );
            exit(1);
         }
         exit(0);
      }
      else {
         usage( argv[0], 1 );
      }
   }
   
   else if( strcmp( argv[i], "make" ) == 0 ) {
      struct md_entry** ents = md_walk_mc_dir( mdroot, true );
      if( ents == NULL ) {
         fprintf(stderr, "ERR: could not walk %s\n", mdroot);
         exit(1);
      }
      int j = 0;
      while( ents[j] != NULL ) {
         char* buf = md_to_string( ents[j], NULL );
         printf("%s\n", buf);
         free( buf );
         md_entry_free( ents[j] );
         free(ents[j]);
         j++;
      }
      free(ents);
      
      exit(0);
   }
   
   else {
      usage( argv[0], 1 );
   }
   return 0;   
}
