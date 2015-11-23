/*
   Copyright 2015 The Trustees of Princeton University

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

#include "libsyndicate/proc.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

struct SG_proc {
   
   bool active;                 // set to true if the group is active 
   
   pid_t pid;                   // PID of child driver 
   int fd_in;                   // input pipe to the child 
   int fd_out;                  // output pipe from the child 
   int fd_err;                  // error pipe from the child
   
   FILE* fout;                  // bufferred I/O wrapper around fd_out, so we can easily read it 
   
   char* exec_str;              // string to feed into exec
   char* exec_arg;              // arg to feed

   struct SG_proc* next;        // next process (linked list)
};


struct SG_proc_group {
   
   struct SG_proc** procs;      // group of processes 
   int num_procs;               // number of actual processes initialized
   int capacity;                // length of procs 
   
   struct SG_proc* free;        // linked list of free processes
   sem_t num_free;              // number of free processes
   
   bool active;                 // whether or not we can acquire new processes
   
   pthread_rwlock_t lock;       // lock governing access to this structure 
};


// rlock a proc group 
int SG_proc_group_rlock( struct SG_proc_group* group ) {
   return pthread_rwlock_rdlock( &group->lock );
}

// wlock a proc group 
int SG_proc_group_wlock( struct SG_proc_group* group ) {
   return pthread_rwlock_wrlock( &group->lock );
}

// unlock a proc group 
int SG_proc_group_unlock( struct SG_proc_group* group ) {
   return pthread_rwlock_unlock( &group->lock );
}


// allocate space for a process
// return the pointer to the newly-allocated region on success
// return NULL on OOM
struct SG_proc* SG_proc_alloc( int num_procs ) {
   return SG_CALLOC( struct SG_proc, num_procs );  
}


// free a process:
// * close its file descriptors 
// * free proc
// NOTE: no attempt to kill the actual process is attempted.
// the caller must do that itself.
void SG_proc_free_data( struct SG_proc* proc ) {
   
   if( proc != NULL ) {
      if( proc->pid > 0 ) {
            
         close( proc->fd_in );
         fclose( proc->fout );      // closes proc->fd_out
         close( proc->fd_err );
      }
      
      if( proc->exec_str != proc->exec_arg ) {
         SG_safe_free( proc->exec_arg );
      }

      SG_safe_free( proc->exec_str );

      memset( proc, 0, sizeof(struct SG_proc) );
   }
}

void SG_proc_free( struct SG_proc* proc ) {
   SG_debug("SG_proc_free %p\n", proc);
   SG_proc_free_data( proc );
   SG_safe_free( proc );
}


// get the PID of a process 
pid_t SG_proc_pid( struct SG_proc* p ) {
   
   return p->pid;
}


// get the exec argument of the process 
char const* SG_proc_exec_arg( struct SG_proc* p ) {
   
   return p->exec_arg;
}

// get stdin to a process
int SG_proc_stdin( struct SG_proc* p ) {
   return p->fd_in;
}

// get a filestream wrapper of a process's stdout
FILE* SG_proc_stdout_f( struct SG_proc* p ) {
   return p->fout;
}

// allocate space for a process group 
// return the pointer to the newly-allocated region on success 
// return NULL on OOM 
struct SG_proc_group* SG_proc_group_alloc( int num_groups ) {
   return SG_CALLOC( struct SG_proc_group, num_groups );
}


// initialize a process group, with zero processes 
// return 0 on success 
// return -ENOMEM on OOM 
int SG_proc_group_init( struct SG_proc_group* group ) {
   
   int rc = 0;
   
   rc = pthread_rwlock_init( &group->lock, NULL );
   if( rc != 0 ) {
      
      SG_error("pthread_rwlock_init rc = %d\n", rc );
      return -ENOMEM;
   }
   
   rc = sem_init( &group->num_free, 0, 0 );
   if( rc != 0 ) {
      
      rc = -errno;
      SG_error("sem_init rc = %d\n", rc );
      
      pthread_rwlock_destroy( &group->lock );
      return rc;
   }

   group->active = true;

   return 0;
}


// take a process off a list 
// return 0 if removed 
// return -ENOENT if not removed
int SG_proc_list_remove( struct SG_proc** list, struct SG_proc* remove ) {
   
   struct SG_proc* prev = *list;
   
   // list empty?
   if( *list == NULL ) {
      return -ENOENT;
   }
   
   // match head?
   if( *list == remove ) {
      
      *list = (*list)->next;
      return 0;
   }
   
   // match tail?
   for( struct SG_proc* p = *list; p != NULL; p = p->next ) {
      
      if( p == remove ) {
         
         prev->next = p->next;
         return 0;
      }
      
      prev = p;
   }
   
   return -ENOENT;
}


// pop a process
// return NULL if empty
struct SG_proc* SG_proc_list_pop( struct SG_proc** list ) {
   
   struct SG_proc* head = *list;
   
   // list empty?
   if( *list == NULL ) {
      return NULL;
   }
   
   *list = head->next;
   return head;
}


// add a process to a list's head
// always succeeds
int SG_proc_list_insert( struct SG_proc** list, struct SG_proc* insert ) {
   
   if( (*list) == NULL ) {
      *list = insert;
      insert->next = NULL;
   }
   else {
      struct SG_proc* p = *list;
      for( ; p->next != NULL; p = p->next );
      p->next = insert;
      insert->next = NULL;
   }
   return 0;
}


// get a pointer to the head of the process group's freelist 
struct SG_proc** SG_proc_group_freelist( struct SG_proc_group* group ) {
   return &group->free;
}


// send a signal to all processes in a proces group
// always succeeds
int SG_proc_group_kill( struct SG_proc_group* group, int signal ) {
   
   int rc = 0;
   
   SG_proc_group_wlock( group );
   
   for( int i = 0; i < group->capacity; i++ ) {
      
      rc = 0;
      if( group->procs[i] == NULL ) {
         continue;
      }

      if( SG_proc_pid( group->procs[i] ) <= 1 ) {
         continue;
      }
      
      if( getpgid( group->procs[i]->pid ) == getpgid( 0 ) ) {
         
         // in this group
         rc = kill( SG_proc_pid( group->procs[i] ), signal );
         if( rc != 0 ) {
            rc = -errno;
            SG_warn("kill(%d, %d) rc = %d\n", SG_proc_pid( group->procs[i] ), signal, rc );
         }
      }
     
      /* 
      if( rc == 0 ) {
         
         SG_proc_list_remove( &group->free, group->procs[i] );
         SG_proc_free( group->procs[i] );
         group->procs[i] = NULL;
      }
      */
   }
   
   SG_proc_group_unlock( group );
   
   return 0;
}


// attempt to join with all processes in a process group.
// does not block.
// free the ones that got joined
// return the number of *unjoined* processes on success 
// NOTE the group must *not* be locked
int SG_proc_group_tryjoin( struct SG_proc_group* group ) {
   
   int rc = 0;
   int num_joined = 0;
   int num_procs = 0;

   struct SG_proc* free_list = NULL;
   
   SG_proc_group_wlock( group );

   SG_debug("join group %p\n", group);
   
   num_procs = group->num_procs;
   
   for( int i = 0; i < group->capacity; i++ ) {
      
      if( group->procs[i] != NULL ) {
         
         SG_debug("join %p (group %p)\n", group->procs[i], group);

         rc = SG_proc_tryjoin( group->procs[i], NULL );
         if( rc < 0 ) {
            
            if( rc != -EAGAIN ) {
               SG_error("SG_proc_tryjoin(%d) rc = %d\n", SG_proc_pid( group->procs[i] ), rc );
            }
         }
         else {
            
            // child is dead.  ensure removed.
            SG_proc_list_remove( &group->free, group->procs[i] );
            SG_proc_list_insert( &free_list, group->procs[i] );

            group->procs[i] = NULL;
            
            num_joined++;
            group->num_procs--;
         }
      }
   }
   
   SG_proc_group_unlock( group );
   
   for( struct SG_proc* p = free_list; p != NULL; ) {

      struct SG_proc* p2 = p->next;

      SG_proc_free( p );
      p = p2;
   }

   return num_procs - num_joined;
}


// stop a group of processes 
// wait up to timeout seconds before SIGKILL'ing them (if zero, SIGKILL them immediately)
// free up all processes once they die.
// return 0 on success.
// NOTE: the group must *not* be locked
int SG_proc_group_stop( struct SG_proc_group* group, int timeout ) {
   
   SG_proc_group_wlock( group );
   
   if( timeout > 0 ) {
      
      // ask them to die first   
      for( int i = 0; i < group->capacity; i++ ) {
         
         if( group->procs[i] != NULL ) {
            
            SG_proc_kill( group->procs[i], SIGINT );
         }
      }
      
      sleep( timeout );
   }
   
   // kill them 
   for( int i = 0; i < group->capacity; i++ ) {
      
      if( group->procs[i] != NULL ) {
         
         SG_proc_kill( group->procs[i], SIGKILL );
         
         SG_proc_list_remove( &group->free, group->procs[i] );
            
         SG_proc_free( group->procs[i] );
         group->procs[i] = NULL;
      }
   }
   
   SG_proc_group_unlock( group );
   
   return 0;
}


// calculate the time till a deadline 
// return 0 on success
// return -EAGAIN if the deadline has been exceeded
static int SG_proc_stop_deadline( struct timespec* deadline, struct timespec* timeout ) {
   
   struct timespec now;
   clock_gettime( CLOCK_MONOTONIC, &now );
   
   if( now.tv_sec > deadline->tv_sec || (now.tv_sec == deadline->tv_sec && now.tv_nsec > deadline->tv_nsec) ) {
      
      // timed out.
      return -EAGAIN;
   }
   else {
      
      // next deadline
      timeout->tv_sec = deadline->tv_sec - now.tv_sec;
      timeout->tv_nsec = deadline->tv_nsec - now.tv_nsec;
      
      if( timeout->tv_nsec < 0 ) {
         timeout->tv_nsec += 1000000000L;
         timeout->tv_sec--;
      }
   }
   
   return 0;
}


// wait for a given process to die.  waitpid() and join with it. 
// return 0 on success
// return -EAGAIN if the process is still running, and should be killed
// return -ECHILD if the process is already dead
static int SG_proc_wait( struct SG_proc* proc, int* child_rc, int timeout ) {
   
   int rc = 0;
   pid_t child_pid = 0;
   sigset_t sigchld_sigset;
   siginfo_t sigchld_info;
   struct timespec ts;
   struct timespec deadline;
   
   ts.tv_sec = timeout;
   ts.tv_nsec = 0;
   
   clock_gettime( CLOCK_MONOTONIC, &deadline );
   deadline.tv_sec += timeout;
   
   // first, see if we can join 
   child_pid = waitpid( proc->pid, &rc, WNOHANG );
   if( child_pid == proc->pid ) {
      
      // joined!
      *child_rc = rc;
      return 0;
   }
   
   while( 1 ) {
      
      // wait for a child to die...
      sigemptyset( &sigchld_sigset );
      sigaddset( &sigchld_sigset, SIGCHLD );
      
      rc = sigtimedwait( &sigchld_sigset, &sigchld_info, &ts );
      if( rc < 0 ) {
         
         rc = -errno;
         if( rc == -EAGAIN ) {
            
            // timed out
            return rc;
         }
         else if( rc == -EINTR ) {
            
            // interrupted
            // re-calculate the delay and try again 
            rc = SG_proc_stop_deadline( &deadline, &ts );
            if( rc < 0 ) {
            
               // expired 
               return -EAGAIN;
            }
            else {
               
               continue;
            }
         }
      }
      else {
         
         // was it *this* child that died?
         child_pid = waitpid( proc->pid, &rc, WNOHANG );
         if( child_pid < 0 ) {
            
            // nope
            rc = -errno;
            if( rc == -ECHILD ) {
               
               // this process is already dead 
               return -ECHILD;
            }
            else {
               
               // this is a bug--the only other possible error is EINVAL (which indicates it's our fault)
               SG_error("BUG: waitpid(%d) rc = %d\n", proc->pid, rc );
               break;
            }
         }
         else {
            
            // joined!
            *child_rc = rc;
            return 0;
         }
      }
   }
   
   return 0;
}


// stop a process, giving it a given number of seconds in between us 
// asking it to stop, and us kill -9'ing it.
// if timeout is <= 0, then kill -9 it directly.
// masks ESRCH (e.g. even if proc->pid is <= 0)
// return 0 on success 
int SG_proc_stop( struct SG_proc* proc, int timeout ) {
   
   int rc = 0;
   int child_rc = 0;
   
   if( proc->pid <= 0 ) {
      
      // not running
      return 0;
   }
   
   if( timeout <= 0 ) {
      
      rc = SG_proc_kill( proc, SIGKILL );
      if( rc < 0 ) {
         return rc;
      }
   }
   else {
      
      rc = SG_proc_kill( proc, SIGINT );
      if( rc < 0 ) {
         return rc;
      }
      
      // wait for it to die...
      rc = SG_proc_wait( proc, &child_rc, timeout );
      if( rc == -ECHILD ) {
         
         // already dead 
         rc = 0;
      }
      
      if( rc == 0 ) {
         
         // joined!
         return rc;
      }
      else if( rc == -EAGAIN ) {
         
         // timed out.  kill and reap.
         SG_proc_kill( proc, SIGKILL );
         SG_proc_tryjoin( proc, NULL );
         rc = 0;
      }
   }
   
   return 0;
}


// free a process group:
// * call SG_proc_free on each still-initialized process
// NOTE: does not kill the processes!
// NOTE: group should either be write-locked, or the group should not be accessible by anyone but the caller.
void SG_proc_group_free( struct SG_proc_group* group ) {
   
   for( int i = 0; i < group->capacity; i++ ) {
      
      if( group->procs[i] != NULL ) {
         SG_proc_free( group->procs[i] );
         group->procs[i] = NULL;
      }
   }
   
   SG_safe_free( group->procs );
   group->procs = NULL;
   
   pthread_rwlock_destroy( &group->lock );
   sem_destroy( &group->num_free );
   
   memset( group, 0, sizeof(struct SG_proc_group) );
}


// add a process to a process group, and put the proc into the free list
// the group takes ownership of proc
// return 0 on success
// return -ENOMEM on OOM 
// return -EEXIST if this process is already in the group
int SG_proc_group_add( struct SG_proc_group* group, struct SG_proc* proc ) {
   
   int rc = 0;
   struct SG_proc** new_procs = NULL;
   bool need_space = true;
   
   SG_proc_group_wlock( group );

   if( group->procs == NULL ) {
     
      group->num_procs = 0; 
      group->capacity = 2;
      group->procs = SG_CALLOC( struct SG_proc*, group->capacity );
      if( group->procs == NULL ) {
         
         rc = -ENOMEM;
      }
      else {
         
         group->procs[0] = proc;
         group->num_procs++;
      }
   }
   else {
      
      // sanity check 
      for( int i = 0; i < group->capacity; i++ ) {
         if( group->procs[i] == proc ) {
            
            SG_proc_group_unlock( group );
            return -EEXIST;
         }
      }
      
      // find a free slot 
      for( int i = 0; i < group->capacity; i++ ) {
         
         if( group->procs[i] == NULL ) {
            group->procs[i] = proc;
            group->num_procs++;

            need_space = false;
            break;
         }
      }
      
      if( need_space ) {
         
         // need more space
         new_procs = SG_CALLOC( struct SG_proc*, group->capacity * 2 ); 
         if( new_procs == NULL ) {
            
            rc = -ENOMEM;
         }
         else {
           
            memcpy( new_procs, group->procs, sizeof(struct SG_proc*) * group->capacity );
            SG_safe_free( group->procs );

            // insert new proc
            group->capacity *= 2;
            group->procs = new_procs;
            group->procs[ group->num_procs ] = proc;
            group->num_procs++;
         }
      }
   }
   
   if( rc == 0 ) {

       // insert into the free list
       SG_proc_list_insert( &group->free, proc );

       SG_debug("Process group %p has %p (%d procs)\n", group, proc, group->num_procs );
   }

   SG_proc_group_unlock( group );
   return rc;
}


// remove a process from a process group
// does not free or stop it.
// return 0 on success 
// return -ENOENT if it's not found 
int SG_proc_group_remove( struct SG_proc_group* group, struct SG_proc* proc ) {
   
   int rc = 0;
   
   SG_proc_group_wlock( group );
   
   if( group->procs == NULL ) {
      
      rc = -ENOENT;
   }
   else {
      
      for( int i = 0; i < group->capacity; i++ ) {
         
         if( group->procs[i] == proc ) {
            
            proc = NULL;
            group->procs[i] = NULL;
            
            // remove from the free list, if it is there 
            SG_proc_list_remove( &group->free, proc );
            group->procs[i] = NULL;
            break;
         }
      }
      
      if( proc != NULL ) {
         
         // not found 
         rc = -ENOENT;
      }
   }
   
   SG_proc_group_unlock( group );
   return rc;
}


// how big is a group?
int SG_proc_group_size( struct SG_proc_group* g ) {
   return g->num_procs;
}


// read a signed 64-bit integer from a file stream, appended by a newline
// masks EINTR
// return 0 on success, and set *result 
// return -EIO if no int could be parsed 
// return -ENODATA if EOF 
int SG_proc_read_int64( FILE* f, int64_t* result ) {
   
   int c = 0;
   int i = 0;
   char intbuf[100];
   char* tmp = NULL;
   
   memset( intbuf, 0, 100 );
   while( 1 ) {

      c = fgetc( f );
      
      if( c == '\n' ) {
         break;
      }

      if( c == EOF ) {
         return -ENODATA;
      }

      intbuf[i] = c;
      i++;
   }

   *result = (int64_t)strtoll( intbuf, &tmp, 10 );
   if( *tmp != '\0' ) {

      // not an int 
      return -EIO;
   }
    
   return 0;
}


// get a chunk from the reader worker: size, newline, data
// if chunk->data is NULL, it will be malloc'ed.  If not, the existing memory will be used,
// and this method will error if it receives too much data.
// return 0 on success, and set up the given SG_chunk (storing the length to chunk->len)
// return -ENOMEM on OOM 
// return -ENODATA on EOF
// return -EIO if the output is unparsable
int SG_proc_read_chunk( FILE* f, struct SG_chunk* chunk ) {
   
   int rc = 0;
   ssize_t nr = 0;
   int64_t size = 0;
   int64_t off = 0;
   int buf_size = 65536;
   char buf[65536];     // for pumping data from the worker to the gateway
   
   rc = SG_proc_read_int64( f, &size );
   if( rc < 0 ) {
      
      SG_error("SG_proc_read_int64('SIZE') rc = %d\n", rc );
      return rc;
   }
   
   // set up the chunk
   // TODO: investigate splice(2)'ing the data, if memory pressure becomes a problem
   chunk->len = (unsigned)size;
   chunk->data = SG_CALLOC( char, size );
   
   if( chunk->data == NULL ) {
      
      // will be unable to hold the data.
      // discard the chunk instead, to keep the driver happy,
      // and report an error.
      SG_error("%s", "OOM; discarding chunk\n");
      
      for( int64_t i = 0; i + buf_size < size; i += buf_size ) {
         
         nr = md_read_uninterrupted( fileno(f), buf, buf_size );
         if( nr < 0 ) {
            
            SG_error("md_read_uninterrupted(%d) rc = %zd\n", fileno(f), nr );
            break;
         }
         else if( nr < buf_size ) {
            
            SG_error("EOF on md_read_uninterrupted(%d)\n", fileno(f) );
            break;
         }
      }
      
      nr = md_read_uninterrupted( fileno(f), buf, size % buf_size );
      if( nr < 0 ) {
         
         SG_error("md_read_uninterrupted(%d) rc = %zd\n", fileno(f), nr );
      }
      else if( nr < (size % buf_size) ) {
         
         SG_error("EOF on md_read_uninterrupted(%d)\n", fileno(f) );
      }
      
      return -ENOMEM;
   }
   
   // feed it in 
   for( int64_t i = 0; i + buf_size < size; i += buf_size ) {
      
      nr = md_read_uninterrupted( fileno(f), chunk->data + i, buf_size );
      if( nr < 0 ) {
         
         SG_error("md_read_uninterrupted(%d) rc = %zd\n", fileno(f), nr );
         rc = -EIO;
         break;
      }
      else if( nr < buf_size ) {
         
         SG_error("EOF on md_read_uninterrupted(%d)\n", fileno(f) );
         rc = -ENODATA;
         break;
      }
      
      off += i;
   }
   
   nr = md_read_uninterrupted( fileno(f), chunk->data + off, size % buf_size );
   if( nr < 0 ) {
      
      SG_error("md_read_uninterrupted(%d) rc = %zd\n", fileno(f), nr );
      rc = -EIO;
   }
   else if( nr < (size % buf_size) ) {
      
      SG_error("EOF on md_read_uninterrupted(%d)\n", fileno(f) );
      rc = -ENODATA;
   }
   
   return rc;
}


// writes a signed 64-bit integer to a file descriptor, appended by a newline 
// masks EINTR 
// return 0 on success 
// return -errno if write fails (including -EPIPE)
int SG_proc_write_int64( int fd, int64_t value ) {
   
   ssize_t nw = 0;
   char value_buf[100];
   
   memset( value_buf, 0, 100 );
   
   snprintf( value_buf, 99, "%" PRId64 "\n", value );
   
   nw = md_write_uninterrupted( fd, value_buf, strlen(value_buf) );
   if( nw < 0 ) {
      
      return (int)nw;
   }
   
   return 0;
}


// send a chunk to a worker
// NOTE: the caller should try to read a character reply from the worker's output stream--either '0' or something else
// return 0 on success
// return -ENOMEM on OOM 
// return -ENODATA if we could not write (e.g. SIGPIPE)
// return -EIO if we could not get a reply.
int SG_proc_write_chunk( int out_fd, struct SG_chunk* chunk ) {
   
   int rc = 0;
   
   // send chunk size 
   rc = SG_proc_write_int64( out_fd, (int64_t)chunk->len );
   if( rc < 0 ) {
      
      SG_error("SG_proc_write_int64(%d (%" PRId64 ")) rc = %d\n", out_fd, (int64_t)chunk->len, rc );
      
      return -ENODATA;
   }
   
   // send chunk itself
   rc = md_write_uninterrupted( out_fd, chunk->data, chunk->len );
   if( rc < 0 ) {
      
      SG_error("md_write_uninterrupted(%d) rc = %d\n", out_fd, rc );
      
      return -ENODATA;
   }

   // send newline delimiter 
   rc = md_write_uninterrupted( out_fd, "\n", 1 );
   if( rc < 0 ) {

      SG_error("md_write_uninterrupted(%d) rc = %d\n", out_fd, rc);

      return -ENODATA;
   }
   
   return 0;
}


// start a (long-running) worker process, and store the relevant information in an SG_proc.
// if given, feed the worker its config (as a string), its secrets (as a string), and its driver info (as a string)
// set up pipes to link the worker to the gateway.
// return 0 on success 
// return -EINVAL on invalid arguments
// return -EMFILE if we're out of fds
// return -ENFILE if the host is out of fds
// return -ECHILD if the child failed to start
int SG_proc_start( struct SG_proc* proc, char const* exec_path, char const* exec_arg, struct SG_chunk* config, struct SG_chunk* secrets, struct SG_chunk* driver ) {
   
   int rc = 0;
   pid_t pid = 0;
   int child_input[2];
   int child_output[2];
   char ready_buf[10];
   FILE* fout = NULL;

   struct SG_chunk empty_json;
   empty_json.data = (char*)"{}";
   empty_json.len = 2;

   // sanity check...
   if( driver == NULL ) {
      return -EINVAL;
   }

   long max_open = sysconf( _SC_OPEN_MAX );
   char* exec_str_dup = SG_strdup_or_null( exec_path );
   char* exec_arg_dup = SG_strdup_or_null( exec_arg );
   
   char* empty_env[1] = {
      NULL
   };
   
   if( exec_str_dup == NULL ) {
      
      SG_safe_free( exec_arg_dup );
      return -ENOMEM;
   }
   
   if( exec_arg_dup == NULL ) {
      
      SG_safe_free( exec_str_dup );
      return -ENOMEM;
   }
   
   // not running yet 
   proc->pid = 0;
   
   memset( ready_buf, 0, 10 );
   
   rc = pipe( child_input );
   if( rc != 0 ) {
      
      SG_safe_free( exec_str_dup );
      SG_safe_free( exec_arg_dup );
      
      rc = -errno;
      SG_error("pipe rc = %d\n", rc );
      return rc;
   }
   
   rc = pipe( child_output );
   if( rc != 0 ) {
      
      rc = -errno;
      SG_error("pipe rc = %d\n", rc );
      
      close( child_input[0] );
      close( child_input[1] );
      
      SG_safe_free( exec_str_dup );
      SG_safe_free( exec_arg_dup );
      
      return rc;
   }
   
   fout = fdopen( child_output[0], "r" );
   if( fout == NULL ) {
      
      close( child_input[0] );
      close( child_input[1] );
      
      SG_safe_free( exec_str_dup );
      SG_safe_free( exec_arg_dup );
      
      return rc;
   }

   pid = fork();
   if( pid < 0 ) {
      
      rc = -errno;
      write( STDERR_FILENO, "fork failed\n", strlen("fork failed\n") );
      
      close( child_input[0] );
      close( child_input[1] );
      fclose( fout );           // closes child_output[0]
      close( child_output[1] );
      
      SG_safe_free( exec_str_dup );
      SG_safe_free( exec_arg_dup );
      
      return rc;
   }
   else if( pid == 0 ) {
      
      // child
      // NOTE: reentrant (async-safe) methods *only* at this point
      close( child_output[0] );
      close( child_input[1] );
      
      close( STDIN_FILENO );
      close( STDOUT_FILENO );
                                  
      rc = dup2( child_input[0], STDIN_FILENO );
      if( rc < 0 ) {
         
         rc = -errno;
         _exit(rc);
      }
      
      rc = dup2( child_output[1], STDOUT_FILENO );
      if( rc < 0 ) {
         
         rc = -errno;
         _exit(rc);
      }
      
      // close everything else 
      for( long i = 3; i < max_open; i++ ) {
         close(i);
      }
      
      // start the helper 
      rc = execle( exec_path, exec_path, exec_arg, NULL, empty_env );
      if( rc != 0 ) {
         
         // NOTE: can't fprintf() :(
         write( STDERR_FILENO, "execle() failed!\n", strlen("execle() failed!\n") );
      }
      
      // keep gcc happy 
      _exit(rc);
   }
   else if( pid > 0 ) {
      
      // parent 
      close( child_input[0] );
      close( child_output[1] );
      
      proc->pid = pid;
      proc->fd_in = child_input[1];
      proc->fd_out = child_output[0];
      proc->fd_err = STDERR_FILENO;
      proc->exec_arg = exec_arg_dup;
      proc->exec_str = exec_str_dup;
      proc->fout = fout;

      if( config == NULL ) {
         config = &empty_json;
      }

      if( secrets == NULL ) {
         secrets = &empty_json;
      } 

      // feed in the config 
      rc = SG_proc_write_chunk( proc->fd_in, config );
      if( rc != 0 ) {
         
         SG_error("SG_proc_write_chunk('CONFIG') rc = %d\n", rc );
         SG_proc_free_data( proc );
         
         return rc;
      }
      
      // feed in the secrets 
      rc = SG_proc_write_chunk( proc->fd_in, secrets );
      if( rc != 0 ) {
         
         SG_error("SG_proc_write_chunk('SECRETS') rc = %d\n", rc );
         SG_proc_free_data( proc );

         return rc;
      }
      
      // feed in the driver 
      rc = SG_proc_write_chunk( proc->fd_in, driver );
      if( rc != 0 ) {
         
         SG_error("SG_proc_write_chunk('DRIVER') rc = %d\n", rc );
         SG_proc_free_data( proc );

         return rc;
      }
      
      // wait for "0\n" or "1\n"
      rc = md_read_uninterrupted( child_output[0], ready_buf, 2 );
      if( rc < 0 ) {
         
         rc = -errno;
         SG_error("read(%d) rc = %d\n", child_output[0], rc );
         SG_proc_free_data( proc );

         return rc;
      }
      
      if( rc != 2 ) {
         
         // too small--child died or misbehaved
         if( rc != -ECHILD ) {
             SG_error("read(%d) rc = %d, assuming ECHILD\n", child_output[0], rc );
         }

         rc = -ECHILD;

         SG_proc_free_data( proc );
         return rc;
      }
      
      if( ready_buf[0] != '0' ) {
         
         SG_error("worker failed to initialize, exit code '%c'\n", ready_buf[0] );
         rc = -ECHILD;

         SG_proc_free_data( proc );
         return rc;
      }
   }
   
   return 0;
}


// kill a worker, but mask ESRCH
// ensure the worker has a valid pid, and is in our process group
// return 0 on success
// return -EINVAL for invalid pid, or a process not in our process group
int SG_proc_kill( struct SG_proc* proc, int signal ) {
   
   int rc = 0;
   
   if( proc == NULL || proc->pid < 2 || getpgid( proc->pid ) != getpgid( 0 ) ) {
      return -EINVAL;
   }
   
   rc = kill( proc->pid, signal );
   if( rc < 0 ) {
      
      rc = -errno;
      if( rc == -ESRCH ) {
         return 0;
      }
      else {
         return -EINVAL;
      }
   }
   
   return 0;
}


// Try to join with a child.  Does not block
// send it SIGINT and wait on it
// return 0 on success, and store the exit status to *child_success.  This masks ECHILD if the child is already dead
// return -EINVAL for invalid PID 
// return -EAGAIN if the child is still running
int SG_proc_tryjoin( struct SG_proc* proc, int* child_status ) {
   
   int rc = 0;
   pid_t child_pid = 0;
   
   if( proc->pid < 2 ) {
      return -EINVAL;
   }
   
   while( true ) {
         
      // try to join with it 
      child_pid = waitpid( proc->pid, &rc, WNOHANG );
      
      if( child_pid < 0 ) {
         
         // ECHILD, EINTR, or EAGAIN
         rc = -errno;
         if( rc == -EINTR ) {
            
            // might have caught SIGCHILD. Try again 
            continue;
         }
         else if( rc == -EAGAIN ) {
            
            // caller should try again 
            return rc;
         }
         
         else if( rc == -ECHILD ) {
            
            // child is already dead
            rc = 0;
            break;
         }
         
         SG_error("waitpid(%d) rc = %d\n", proc->pid, rc );
         break;
      }
      else {
         
         if( child_status != NULL ) {
            *child_status = rc;
         }
         
         // success 
         break;
      }
   }
   
   return 0;
}


// reload a process group 
// * respawn any running workers with the new exec_str string, if it in fact changed.
// NOTE: group must be write-locked
int SG_proc_group_reload( struct SG_proc_group* group, char const* new_exec_str, struct SG_chunk* new_config, struct SG_chunk* new_secrets, struct SG_chunk* new_driver ) {
   
   int rc = 0;
   
   struct SG_proc* old_proc = NULL;
   struct SG_proc* new_proc = NULL;
   
   for( int i = 0; i < group->capacity; i++ ) {
      
      if( group->procs[i] == NULL ) {
         continue;
      }
      
      // different exec?
      if( strcmp( group->procs[i]->exec_str, new_exec_str ) != 0 ) {
         
         // start up the new process, and have it stand by to be swapped in...
         new_proc = SG_proc_alloc( 1 );
         if( new_proc == NULL ) {
            
            // OOM
            // just stop this process instead 
            SG_proc_stop( group->procs[i], 1 );
            
            SG_proc_list_remove( &group->free, group->procs[i] );
            
            SG_proc_free( group->procs[i] );
            group->procs[i] = NULL;
            continue;
         }
         
         rc = SG_proc_start( new_proc, new_exec_str, group->procs[i]->exec_arg, new_config, new_secrets, new_driver );
         if( rc != 0 ) {
            
            SG_error("SG_proc_start(exec_arg='%s') rc = %d\n", group->procs[i]->exec_arg, rc );
            
            SG_proc_stop( new_proc, 0 );
            SG_proc_free( new_proc );
            new_proc = NULL;
            
            // stop this process instead 
            SG_proc_stop( group->procs[i], 1 );
            
            SG_proc_list_remove( &group->free, group->procs[i] );
            
            SG_proc_free( group->procs[i] );
            group->procs[i] = NULL;
         }
         else {
            
            // switch to this new process
            old_proc = group->procs[i];
            group->procs[i] = new_proc;
            
            // stop the old process 
            SG_proc_stop( old_proc, 1 );
            SG_proc_free( old_proc );
            old_proc = NULL;
         }
      }
   }
   
   return rc;
}


// get a free process, and prevent it from receiving I/O from anyone else (i.e. take it off the free list)
// return the non-NULL proc on success
// blocks if there are no free processes 
// NOTE: group must NOT be locked
// NOTE; this call blocks if there are available processes
struct SG_proc* SG_proc_group_acquire( struct SG_proc_group* group ) {
   
   while( group->active ) {
      
      SG_proc_group_wlock( group );
      
      struct SG_proc* proc = SG_proc_list_pop( &group->free );
      if( proc == NULL ) {
         
         // out of processes
         if( group->free == NULL ) {
            
            // ...because there are none in the first place
            SG_warn("No free process in group %p; sleeping...\n", group );

            SG_proc_group_unlock( group );

            sem_wait( &group->num_free );
            continue;
         }

         SG_proc_group_unlock( group );
         
         // NOTE this is considered safe here, since the semaphore won't disappear on us
         // (i.e. we ensure that any worker who could call this method will be dead before 
         // we free up the process group.
         sem_wait( &group->num_free );
         
         continue;
      }
      
      else {
         
         // acquired!
         SG_proc_group_unlock( group );
         
         return proc;
      }
   }

   if( !group->active ) {
      SG_warn("Inactive process group %p\n", group );
   }
   
   // if we reach here, then it means our process group is dead
   return NULL;
}


// release a process now that we've used it (i.e. adding it back to the free list)
// return 0 on success 
// NOTE: group must NOT be locked 
int SG_proc_group_release( struct SG_proc_group* group, struct SG_proc* proc ) {
   
   SG_proc_group_wlock( group );
   
   SG_proc_list_insert( &group->free, proc );
   sem_post( &group->num_free );
   
   SG_proc_group_unlock( group );
   
   return 0;
}



// run a subprocess
// gather the output into the output buffer (allocating it if needed).
// return 0 on success
// return 1 on output truncate 
// return negative on error
// set the subprocess exit status in *exit_status
int SG_proc_subprocess( char const* cmd_path, char* const argv[], char* const env[], char const* input, size_t input_len, char** output, size_t* output_len, size_t max_output, int* exit_status ) {
   
   int inpipe[2];
   int outpipe[2];
   int rc = 0;
   pid_t pid = 0;
   int max_fd = 0;
   ssize_t nr = 0;
   ssize_t nw = 0;
   int status = 0;
   bool alloced = false;
   
   if( cmd_path == NULL ) {
      return -EINVAL;
   }
   
   // open the pipes
   rc = pipe( outpipe );
   if( rc != 0 ) {
      
      rc = -errno;
      SG_error("pipe() rc = %d\n", rc );
      return rc;
   }
   
   if( input != NULL ) {
      rc = pipe( inpipe );
      if( rc != 0 ) {
         
         rc = -errno;
         SG_error("pipe() rc = %d\n", rc );
         
         close( outpipe[0] );
         close( outpipe[1] );
         return rc;
      }
   }
   
   max_fd = sysconf(_SC_OPEN_MAX);
   
   // fork the child 
   pid = fork();
   
   if( pid == 0 ) {
      
      close( outpipe[0] );
      
      if( input == NULL ) {
         
         close( STDIN_FILENO );
      }
      else {
         
         // will send input 
         close( inpipe[1] );
         dup2( inpipe[0], STDIN_FILENO );
      }
      
      // send stdout to p[1]
      rc = dup2( outpipe[1], STDOUT_FILENO );
      
      if( rc < 0 ) {
         
         rc = errno;
         close( outpipe[1] );
         
         if( input != NULL ) {
            close( inpipe[0] );
         }
         
         _exit(rc);
      }
      
      // close everything else but stdout
      for( int i = 0; i < max_fd; i++ ) {
         
         if( i != STDOUT_FILENO && i != STDIN_FILENO && i != STDERR_FILENO ) {
            close( i );
         }
      }
      
      // run the command 
      if( env != NULL ) {
         rc = execve( cmd_path, argv, env );
      }
      else {
         
         char** noenv = { NULL };
         rc = execve( cmd_path, argv, noenv );
      }
      
      if( rc != 0 ) {
         
         rc = errno;
         _exit(rc);
      }
      else {
         
         // make gcc happy 
         _exit(0);
      }
   }
   else if( pid > 0 ) {
      
      // parent 
      close(outpipe[1]);
      
      // allocate output 
      if( output != NULL ) {
         if( *output == NULL && max_output > 0 ) {
            
            *output = SG_CALLOC( char, max_output );
            if( *output == NULL ) {
               
               // out of memory 
               close( outpipe[0] );
               
               if( input != NULL ) {
                  close( inpipe[0] );
                  close( inpipe[1] );
               }
               
               return -ENOMEM;
            }
            
            alloced = true;
         }
      }
      
      // send input 
      if( input != NULL ) {
         
         close(inpipe[0]);
      
         nw = md_write_uninterrupted( inpipe[1], input, input_len );
         if( nw < 0 ) {
            
            SG_error("md_write_uninterrupted rc = %zd\n", nw );
            close( inpipe[1] );
            close( outpipe[0] );
            
            if( alloced ) {
               
               free( *output );
               *output = NULL;
            }
            return nw;
         }   
         
         // end of input 
         close( inpipe[1] );
      }
   
      // get output 
      if( output != NULL && *output != NULL && max_output > 0 ) {
         
         nr = md_read_uninterrupted( outpipe[0], *output, max_output );
         if( nr < 0 ) {
            
            SG_error("md_read_uninterrupted rc = %zd\n", nr );
            close( outpipe[0] );
            
            if( alloced ) {
               
               free( *output );
               *output = NULL;
            }
            return nr;
         }
         
         *output_len = nr;
         
         // deny child future writes
         close( outpipe[0] );
      }
      
      // wait for child
      rc = waitpid( pid, &status, 0 );
      if( rc < 0 ) {
         
         rc = -errno;
         SG_error("waitpid(%d) rc = %d\n", pid, rc );
         
         if( alloced ) {
            
            free( *output );
            *output = NULL;
         }
         
         return rc;
      }
      
      if( WIFEXITED( status ) ) {
         
         *exit_status = WEXITSTATUS( status );
      }
      else if( WIFSIGNALED( status ) ) {
         
         SG_error("command '%s' failed with signal %d\n", cmd_path, WTERMSIG( status ) );
         *exit_status = status;
      }
      else {
         
         // indicate start/stop
         SG_error("command '%s' was started/stopped externally\n", cmd_path );
         *exit_status = -EPERM;
      }
      
      return 0;
   }
   else {
      
      rc = -errno;
      SG_error("fork() rc = %d\n", rc );
      return rc;
   }
}