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

#include "fs_entry.h"
#include "test_fs_entry.h"

pthread_t _threads[NUM_THREADS];
struct test_thread_args _args[NUM_THREADS];
uid_t _user = 12346;
gid_t _vol = 1;

void* thread_main( void* arg ) {
   struct test_thread_args* args = (struct test_thread_args*)arg;
   int thread_id = args->id;
   struct fs_core* core = args->fs;
   
   char cwd[PATH_MAX];
   memset(cwd, 0, PATH_MAX);
   getcwd( cwd, PATH_MAX );
   
   // read data for a local file
   char* local_filename = (char*)calloc( strlen(LOCAL_FILE) + 1 + (log(thread_id) + 1.0) + 1, 1 );
   sprintf(local_filename, "%s.%d", LOCAL_FILE, thread_id );
   
   // read data from a local directory
   char* local_dirname = (char*)calloc( strlen(LOCAL_DIR) + 1 + (log(thread_id) + 1.0) + 1, 1 );
   sprintf(local_dirname, "%s.%d", LOCAL_DIR, thread_id );
   
   char* tmp = NULL;
   
   char* fpath_url_dir = md_prepend( (char*)"file://", cwd, NULL );
   char* fpath_url = md_fullpath( fpath_url_dir, local_filename, NULL );
   free( fpath_url_dir );
   
   char* local_path = md_fullpath( (char*)"/", (char*)local_filename, NULL );
   char* local_dir = md_fullpath( (char*)"/", (char*)local_dirname, NULL );
   
   fpath_url_dir = md_prepend( (char*)"file://", cwd, NULL );
   char* fpath_dir_url = md_fullpath( fpath_url_dir, local_dirname, NULL );
   free( fpath_url_dir );
   
   int err = 0;
   int rc = 0;
   
   // read data from a file within the directory
   char* local_filename2 = (char*)calloc( strlen(LOCAL_FILE2) + (int)(log(thread_id) + 1.0) + 3, 1 );
   sprintf(local_filename2, "%s.%d", LOCAL_FILE2, thread_id);
   fpath_url_dir = md_prepend( (char*)"file://", cwd, NULL );
   tmp = md_fullpath( fpath_url_dir, local_dir, NULL );
   
   char* fpath_url2 = md_fullpath( tmp, local_filename2, NULL );
   free( tmp );
   
   tmp = md_fullpath((char*)"/", local_dir, NULL );
   char* local_path2 = md_fullpath( tmp, (char*)local_filename2, NULL );
   free( tmp );
   free( fpath_url_dir );
   
   tmp = md_prepend( (char*)"file://", cwd, NULL );
   char* origin_file_url = md_fullpath( tmp, LOCAL_FILE, NULL );
   free( tmp );
   
   char* origin_file_path = md_fullpath( (char*)"/", LOCAL_FILE, NULL );
   
   // test 0: open an existing file
   printf("Thread %d: open existing file %s...", thread_id, origin_file_path );
   struct fs_file_handle* original = fs_entry_open( core, origin_file_path, origin_file_url, _user, _vol, O_RDONLY, 0, &err );
   printf("err = %d\n", err );
   
   if( original == NULL )
      return NULL;
   
   
   // test 1: create a filesystem and put a file in it
   
   printf("Thread %d: create filesystem\n", thread_id);
   
   tmp = fs_entry_to_string( core->root );
   printf("Thread %d:   core->root = %s\n", thread_id, tmp );
   free( tmp );
   
   printf("Thread %d: create file %s...", thread_id, local_path);
   struct fs_file_handle* create1 = fs_entry_create( core, local_path, fpath_url, _user, _vol, 0664, &err );
   printf("err = %d\n", err);
   
   if( create1 == NULL )
      return NULL;
   
   tmp = fs_file_handle_to_string( create1 );
   printf("Thread %d: created file handle: %s\n", thread_id, tmp);
   free( tmp );
   
   printf("Thread %d: close %s... ", thread_id, local_path);
   rc = fs_entry_close( core, create1 );
   printf("rc = %d\n", rc);
   
   if( rc != 0 )
      return NULL;
   
   
   // test 2: open files for reading, writing, and read/write
   
   // open the file three times: once read-only, once write-only, once read-write.
   
   printf("Thread %d:   fs_entry_open O_RDONLY... ", thread_id);
   struct fs_file_handle* fd_rdonly = fs_entry_open( core, local_path, fpath_url, _user, _vol, O_RDONLY, ~core->conf->usermask & 0777, &rc );
   printf("fd = %p, rc = %d\n", fd_rdonly, rc );
   
   if( rc != 0 || fd_rdonly == NULL )
      return NULL;
   
   tmp = fs_file_handle_to_string( fd_rdonly );
   printf("Thread %d:   fd_rdonly = %s\n", thread_id, tmp );
   free( tmp );
   
   printf("Thread %d:   fs_entry_open O_WRONLY... ", thread_id);
   struct fs_file_handle* fd_wronly = fs_entry_open( core, local_path, fpath_url, _user, _vol, O_WRONLY, ~core->conf->usermask & 0777, &rc );
   printf("fd = %p, rc = %d\n", fd_wronly, rc );
   
   if( rc != 0 || fd_wronly == NULL )
      return NULL;
   
   tmp = fs_file_handle_to_string( fd_wronly );
   printf("Thread %d:   fd_wronly = %s\n", thread_id, tmp );
   free( tmp );
   
   printf("Thread %d:   fs_entry_open O_RDWR... ", thread_id);
   struct fs_file_handle* fd_rdwr = fs_entry_open( core, local_path, fpath_url, _user, _vol, O_RDWR, ~core->conf->usermask & 0777, &rc );
   printf("fd = %p, rc = %d\n", fd_rdwr, rc );
   
   if( rc != 0 || fd_rdwr == NULL )
      return NULL;
   
   tmp = fs_file_handle_to_string( fd_rdwr );
   printf("Thread %d:   fd_rdwr = %s\n", thread_id, tmp );
   free( tmp );
   
   
   
   // test 3: create a directory
   printf("Thread %d:   fs_entry_mkdir... ", thread_id );
   int dir_rc = fs_entry_mkdir( core, local_dir, fpath_dir_url, _user, _vol, 0755, &rc );
   printf("dir_rc = %d, rc = %d\n", dir_rc, rc );
   
   if( dir_rc != 0 ) {
      return NULL;
   }
   
   
   
   // test 4: put a file in the directory
   printf("Thread %d: create file %s... ", thread_id, local_path2);
   struct fs_file_handle* create2 = fs_entry_create( core, local_path2, fpath_url2, _user, _vol, 0664, &err );
   printf("err = %d\n", err);
   
   if( create2 == NULL )
      return NULL;
   
   tmp = fs_file_handle_to_string( create2 );
   printf("Thread %d: created file handle: %s\n", thread_id, tmp);
   free( tmp );
   
   // test 4A: open the directory
   printf("Thread %d:   fs_entry_opendir(%s)... ", thread_id, local_dir );
   struct fs_dir_handle* local_dirh = fs_entry_opendir( core, local_dir, _user, _vol, &rc );
   printf("rc = %d\n", rc );
   
   if( local_dirh == NULL )
      return NULL;
   
   // test 4B: read the directory
   printf("Thread %d:   fs_entry_readdir(%s)... ", thread_id, local_dir );
   struct fs_dir_entry** dents = fs_entry_readdir( local_dirh, &rc );
   printf("rc = %d\n", rc );
   
   if( dents == NULL )
      return NULL;
   
   for( int i = 0; dents[i] != NULL; i++ ) {
      printf("Thread %d:   entry: %s\n", thread_id, dents[i]->d_name );
   }
   
   fs_dir_entry_destroy_all( dents );
   
   // test 4C: close the diretory
   printf("Thread %d:   fs_entry_closedir(%s)... ", thread_id, local_dir );
   rc = fs_entry_closedir( core, local_dirh );
   printf("rc = %d\n", rc );
   
   if( rc != 0 ) {
      return NULL;
   }
   
   
   // test5: read the contents of the original file
   struct stat sb;
   rc = stat( GET_PATH(origin_file_url), &sb );
   if( rc != 0 ) {
      SG_error("Could not stat %s\n", GET_PATH(origin_file_url) );
      return NULL;
   }
   char* filebuf = (char*)calloc( sb.st_size, 1 );
   
   printf("Thread %d: read file %s... ", thread_id, origin_file_path );
   ssize_t num_read = fs_entry_read( original, filebuf, sb.st_size, 0 );
   printf("num_read = %ld\n", num_read );
   
   if( num_read != sb.st_size ) {
      return NULL;
   }
   
   
   // test 6: write the contents to the local file (test O_RDWR)
   printf("Thread %d: write data to %s... ", thread_id, local_path );
   ssize_t num_written = fs_entry_write( fd_rdwr, filebuf, num_read, 0 );
   printf("num_written = %ld\n", num_written );
   
   if( num_written != num_read )
      return NULL;
   
   
   // test 7: write the contents to the second local file (test O_WRONLY)
   printf("Thread %d: write data to %s... ", thread_id, local_path2 );
   num_written = fs_entry_write( create2, filebuf, num_read, 0 );
   printf("num_written = %ld\n", num_written );
   
   if( num_written != num_read )
      return NULL;
   
   
   // test 8: close file within the local directory
   printf("Thread %d: close %s... ", thread_id, local_path2);
   rc = fs_entry_close( core, create2 );
   printf("rc = %d\n", rc);
   
   if( rc != 0 )
      return NULL;
      
   
   // test 9: close read-only and write-only handles
   // close the handles
   printf("Thread %d:   fs_entry_close %p... ", thread_id, fd_rdonly );
   rc = fs_entry_close( core, fd_rdonly );
   printf("rc = %d\n", rc);
   if( rc != 0 )
      return NULL;
   
   tmp = fs_file_handle_to_string( fd_rdonly );
   printf("Thread %d:   fd_rdonly = %s\n", thread_id, tmp );
   free( tmp );
   
   
   printf("Thread %d:   fs_entry_close %p... ", thread_id, fd_wronly );
   rc = fs_entry_close( core, fd_wronly );
   printf("rc = %d\n", rc);
   if( rc != 0 )
      return NULL;
   
   tmp = fs_file_handle_to_string( fd_wronly );
   printf("Thread %d:   fd_wronly = %s\n", thread_id, tmp );
   free( tmp );
   
   // test 10: unlink file
   printf("Thread %d:   fs_entry_detach %s... ", thread_id, local_path );
   rc = fs_entry_detach( core, local_path, _user, _vol );
   printf("rc = %d\n", rc );
   if( rc != 0 )
      return NULL;
   
   
   // test 11: unlink file within the local directory
   printf("Thread %d:   fs_entry_detach %s... ", thread_id, local_path2 );
   rc = fs_entry_detach( core, local_path2, _user, _vol );
   printf("rc = %d\n", rc );
   if( rc != 0 )
      return NULL;
   
   
   // test 12: unlink local directory
   printf("Thread %d:   fs_entry_rmdir %s... ", thread_id, local_dir );
   rc = fs_entry_detach( core, local_dir, _user, _vol );
   printf("rc = %d\n", rc );
   if( rc != 0 ) 
      return NULL;
   
      
   // test 14: close handle to already-removed file
   printf("Thread %d:   fs_entry_close %p... ", thread_id, fd_rdwr );
   rc = fs_entry_close( core, fd_rdwr );
   printf("rc = %d\n", rc);
   if( rc != 0 ) 
      return NULL;
   
   tmp = fs_file_handle_to_string( fd_rdwr );
   printf("Thread %d:   fd_rdwr = %s\n", thread_id, tmp );
   free( tmp );
   
   // test 15: open, read, print, and close the root directory
   printf("Thread %d:   fs_entry_opendir(/)... ", thread_id );
   struct fs_dir_handle* root_dir = fs_entry_opendir( core, (char*)"/", _user, _vol, &rc );
   printf("rc = %d\n", rc );
   if( root_dir == NULL || rc != 0 )
      return NULL;
   
   dents = fs_entry_readdir( root_dir, &err );
   fs_entry_closedir( core, root_dir );
   
   for( int i = 0; dents[i] != NULL; i++ ) {
      printf("Thread %d:   entry: %s\n", thread_id, dents[i]->d_name );
   }
   
   fs_dir_entry_destroy_all( dents );
   
   return NULL;
}


void print_fs( struct fs_entry* fent, int depth, bool verbose ) {
   if( fent == NULL )
      return;
   
   long cur_dir = fs_entry_name_hash(".");
   long prev_dir = fs_entry_name_hash("..");
   
   for( int i = 0; i < depth; i++ )
      printf("  ");
   
   if( verbose ) {
      char* buf = fs_entry_to_string( fent );
      printf(buf);
      printf("\n");
      free( buf );
   }
   else {
      printf(fent->name);
      printf("\n");
   }
   
   if( fent->ftype == FTYPE_DIR ) {
      for( fs_entry_set::iterator itr = fent->children->begin(); itr != fent->children->end(); itr++ ) {
         if( itr->second == NULL )
            continue;
         
         if( itr->first == cur_dir || itr->first == prev_dir )
            continue;
         
         if( itr->second == fent ) {
            printf("SERIOUS ERROR: parent linked into its own directory!\n");
            exit(1);
         }
         
         print_fs( itr->second, depth + 1, verbose );
      }
   }
}


int main( int argc, char** argv ) {
   // create an FS
   struct fs_core fs;
   const char* md_url = "http://localhost:8888/";
   
   if( chdir( TEST_DIR ) != 0 ) {
      SG_error("Missing directory: %s\n", TEST_DIR );
      exit(1);
   }
   
   char cwd[PATH_MAX];
   memset(cwd, 0, PATH_MAX);
   getcwd( cwd, PATH_MAX );
   
   struct md_syndicate_conf conf;
   md_read_conf((char*)"/etc/syndicate/syndicate-client.conf", &conf);
   
   // reset publish directory
   if( conf.publish_dir ) {
      free( conf.publish_dir );
      conf.publish_dir = strdup( cwd );
   }
   
   md_init( &conf, NULL );
   
   fs_core_init( &fs, (char*)md_url, _user, _vol, 0664, &conf );
   fs_entry_mkfs( &fs, cwd, _user, _vol );
   
   // launch workers
   for( int i = 0; i < NUM_THREADS; i++ ) {
      _args[i].id = i+1;
      _args[i].fs = &fs;
      pthread_create( &_threads[i], NULL, thread_main, &_args[i] );
   }
   
   // join workers
   for( int i = 0; i < NUM_THREADS; i++ ) {
      pthread_join( _threads[i], NULL );
   }
   
   printf("\n\nFilesystem hierarchy:\n");
   print_fs( fs.root, 0, false );
   printf("\n");
   print_fs( fs.root, 0, true );
   
   return 0;
}