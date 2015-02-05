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
/*
 * Utility functions (debugging, etc)
 */
 
#include "libsyndicate/util.h"
#include "libsyndicate/libsyndicate.h"

int _SG_DEBUG_MESSAGES = 0;
int _SG_INFO_MESSAGES = 0;

// log all warnings and errors by default
int _SG_WARN_MESSAGES = 1;
int _SG_ERROR_MESSAGES = 1;


void md_set_debug_level( int d ) {
   if( d <= 0 ) {
      // no debugging
      _SG_DEBUG_MESSAGES = 0;
      _SG_INFO_MESSAGES = 0;
   }
   if( d >= 1 ) {
      _SG_INFO_MESSAGES = 1;
   }
   if( d >= 2 ) {
      // info and debug 
      _SG_DEBUG_MESSAGES = 1;
   }
}

void md_set_error_level( int e ) {
   if( e <= 0 ) {
      // no error 
      _SG_ERROR_MESSAGES = 0;
      _SG_WARN_MESSAGES = 0;
   }
   if( e >= 1 ) {
      _SG_ERROR_MESSAGES = 1;
   }
   if( e >= 2 ) {
      _SG_WARN_MESSAGES = 1;
   }
}

int md_get_debug_level() {
   return _SG_DEBUG_MESSAGES;
}

int md_get_error_level() {
   return _SG_ERROR_MESSAGES;
}

/* Converts a hex character to its integer value */
static char from_hex(char ch) {
  return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

/* Converts an integer value to its hex character*/
static char to_hex(char code) {
  static char hex[] = "0123456789abcdef";
  return hex[code & 15];
}


/*
 * Get the current time in seconds since the epoch, local time
 */
int64_t md_current_time_seconds() {
   struct timespec ts;
   int rc = clock_gettime( CLOCK_REALTIME, &ts );
   if( rc != 0 ) {
      return -errno;
   }
   
   int64_t ts_sec = (int64_t)ts.tv_sec;
   return ts_sec;
}


// get the current time in milliseconds
int64_t md_current_time_millis() {
   struct timespec ts;
   int rc = clock_gettime( CLOCK_REALTIME, &ts );
   if( rc != 0 ) {
      return -errno;
   }
   
   int64_t ts_sec = (int64_t)ts.tv_sec;
   int64_t ts_nsec = (int64_t)ts.tv_nsec;
   return (ts_sec * 1000) + (ts_nsec / 1000000);
}

/*
 * Get the user's umask
 */
mode_t md_get_umask() {
  mode_t mask = umask(0);
  umask(mask);
  return mask;
}


// calculate the sha-256 hash of something.
// caller must free the hash buffer.
unsigned char* sha256_hash_data( char const* input, size_t len ) {
   unsigned char* obuf = (unsigned char*)calloc( SHA256_DIGEST_LENGTH, 1 );
   SHA256( (unsigned char*)input, len, obuf );
   return obuf;
}

size_t sha256_len(void) {
   return SHA256_DIGEST_LENGTH;
}

// calculate the sha-256 hash of a string
unsigned char* sha256_hash( char const* input ) {
   return sha256_hash_data( input, strlen(input) );
}

// duplicate a sha1
unsigned char* sha256_dup( unsigned char const* sha256 ) {
   unsigned char* ret = (unsigned char*)calloc( SHA256_DIGEST_LENGTH, 1 );
   memcpy( ret, sha256, SHA_DIGEST_LENGTH );
   return ret;
}

// compare two SHA256 hashes
int sha256_cmp( unsigned char const* hash1, unsigned char const* hash2 ) {
   if( hash1 == NULL ) {
      return -1;
   }
   if( hash2 == NULL ) {
      return 1;
   }
   return strncasecmp( (char*)hash1, (char*)hash2, SHA_DIGEST_LENGTH );
}


// make a sha-256 hash printable
char* sha256_printable( unsigned char const* hash ) {
   char* ret = (char*)calloc( sizeof(char) * (2 * SHA256_DIGEST_LENGTH + 1), 1 );
   char buf[3];
   for( int i = 0; i < SHA256_DIGEST_LENGTH; i++ ) {
      sprintf(buf, "%02x", hash[i] );
      ret[2*i] = buf[0];
      ret[2*i + 1] = buf[1];
   }
   
   return ret;
}

// make a printable sha256 from data
char* sha256_hash_printable( char const* input, size_t len) {
   unsigned char* hash = sha256_hash_data( input, len );
   char* hash_str = sha256_printable( hash );
   free( hash );
   return hash_str;
}

// make a printable sha-1 hash into data
unsigned char* sha256_data( char const* printable ) {
   unsigned char* ret = (unsigned char*)calloc( SHA256_DIGEST_LENGTH, 1 );
   
   for( size_t i = 0; i < strlen( printable ); i+=2 ) {
      unsigned char tmp1 = (unsigned)from_hex( printable[i] );
      unsigned char tmp2 = (unsigned)from_hex( printable[i+1] );
      ret[i >> 1] = (tmp1 << 4) | tmp2;
   }
   
   return ret;
}


// hash a file
unsigned char* sha256_file( char const* path ) {
   FILE* f = fopen( path, "r" );
   if( !f ) {
      return NULL;
   }
   
   SHA256_CTX context;
   SHA256_Init( &context );
   unsigned char* new_checksum = (unsigned char*)calloc( SHA256_DIGEST_LENGTH, 1 );
   unsigned char buf[32768];
   
   ssize_t num_read;
   while( !feof( f ) ) {
      num_read = fread( buf, 1, 32768, f );
      if( ferror( f ) ) {
         SG_error("sha256_file: I/O error reading %s\n", path );
         SHA256_Final( new_checksum, &context );
         free( new_checksum );
         fclose( f );
         return NULL;
      }
      
      SHA256_Update( &context, buf, num_read );
   }
   fclose(f);
   
   SHA256_Final( new_checksum, &context );
   
   return new_checksum;
}

// hash a file, given its descriptor 
unsigned char* sha256_fd( int fd ) {
   SHA256_CTX context;
   SHA256_Init( &context );
   unsigned char* new_checksum = (unsigned char*)calloc( SHA256_DIGEST_LENGTH, 1 );
   unsigned char buf[32768];
   
   ssize_t num_read = 1;
   while( num_read > 0 ) {
      num_read = read( fd, buf, 32768 );
      if( num_read < 0 ) {
         SG_error("sha256_file: I/O error reading FD %d, errno=%d\n", fd, -errno );
         SHA256_Final( new_checksum, &context );
         free( new_checksum );
         return NULL;
      }
      
      SHA256_Update( &context, buf, num_read );
   }
   
   SHA256_Final( new_checksum, &context );
   
   return new_checksum;
}


// load a file into RAM
// return a pointer to the bytes.
// set the size.
char* md_load_file( char const* path, off_t* size ) {
   struct stat statbuf;
   int rc = stat( path, &statbuf );
   if( rc != 0 ) {
      return NULL;
   }
   
   char* ret = SG_CALLOC( char, statbuf.st_size );
   if( ret == NULL ) {
      return NULL;
   }
   
   FILE* f = fopen( path, "r" );
   if( !f ) {
      free( ret );
      return NULL;
   }
   
   *size = fread( ret, 1, statbuf.st_size, f );
   
   if( *size != statbuf.st_size ) {
      free( ret );
      fclose( f );
      return NULL;
   }
   
   fclose( f );
   return ret;
}

// read, but mask EINTR 
ssize_t md_read_uninterrupted( int fd, char* buf, size_t len ) {
   
   ssize_t num_read = 0;
   while( (unsigned)num_read < len ) {
      ssize_t nr = read( fd, buf + num_read, len - num_read );
      if( nr < 0 ) {
         
         int errsv = -errno;
         if( errsv == -EINTR ) {
            continue;
         }
         
         return errsv;
      }
      if( nr == 0 ) {
         break;
      }
      
      num_read += nr;
   }
   
   return num_read;
}


// recv, but mask EINTR 
ssize_t md_recv_uninterrupted( int fd, char* buf, size_t len, int flags ) {
   
   ssize_t num_read = 0;
   while( (unsigned)num_read < len ) {
      ssize_t nr = recv( fd, buf + num_read, len - num_read, flags );
      if( nr < 0 ) {
         
         int errsv = -errno;
         if( errsv == -EINTR ) {
            continue;
         }
         
         return errsv;
      }
      if( nr == 0 ) {
         break;
      }
      
      num_read += nr;
   }
   
   return num_read;
}

// write, but mask EINTR
ssize_t md_write_uninterrupted( int fd, char const* buf, size_t len ) {
   
   ssize_t num_written = 0;
   while( (unsigned)num_written < len ) {
      ssize_t nw = write( fd, buf + num_written, len - num_written );
      if( nw < 0 ) {
         
         int errsv = -errno;
         if( errsv == -EINTR ) {
            continue;
         }
         
         return errsv;
      }
      if( nw == 0 ) {
         break;
      }
      
      num_written += nw;
   }
   
   return num_written;
}


// send, but mask EINTR
ssize_t md_send_uninterrupted( int fd, char const* buf, size_t len, int flags ) {
   
   ssize_t num_written = 0;
   while( (unsigned)num_written < len ) {
      ssize_t nw = send( fd, buf + num_written, len - num_written, flags );
      if( nw < 0 ) {
         
         int errsv = -errno;
         if( errsv == -EINTR ) {
            continue;
         }
         
         return errsv;
      }
      if( nw == 0 ) {
         break;
      }
      
      num_written += nw;
   }
   
   return num_written;
}

// create an AF_UNIX local socket 
// if bind_on is true, then this binds and listens on the socket
// otherwise, it connects
int md_unix_socket( char const* path, bool server ) {
   
   if( path == NULL ) {
      return -EINVAL;
   }
   
   struct sockaddr_un addr;
   int fd = 0;
   int rc = 0;
   
   memset(&addr, 0, sizeof(struct sockaddr_un));
   
   // sanity check 
   if( strlen(path) >= sizeof(addr.sun_path) - 1 ) {
      SG_error("%s is too long\n", path );
      return -EINVAL;
   }
   
   // create the socket 
   fd = socket( AF_UNIX, SOCK_STREAM, 0 );
   if( fd < 0 ) {
      fd = -errno;
      SG_error("socket(%s) rc = %d\n", path, fd );
      return fd;
   }
   
   // set up the sockaddr
   addr.sun_family = AF_UNIX;
   strncpy(addr.sun_path, path, strlen(path) );

   // server?
   if( server ) {
      // bind on it 
      rc = bind( fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_un) );
      if( rc < 0 ) {
         rc = -errno;
         SG_error("bind(%s) rc = %d\n", path, rc );
         close( fd );
         return rc;
      }
      
      // listen on it
      rc = listen( fd, 100 );
      if( rc < 0 ) {
         SG_error("listen(%s) rc = %d\n", path, rc );
         close( fd );
         return rc;
      }
   }
   else {
      // client 
      rc = connect( fd, (struct sockaddr*)&addr, sizeof(struct sockaddr_un) );
      if( rc < 0 ) {
         rc = -errno;
         SG_error("connect(%s) rc = %d\n", path, rc );
         close( fd );
         return rc;
      }
   }
   
   return fd;
}


// dump data to a temporary file.
// on success, allocate and return the path to the temporary file and return 0
// on failure, return negative and remove the temporary file that was created
int md_write_to_tmpfile( char const* tmpfile_fmt, char const* buf, size_t buflen, char** tmpfile_path ) {
   
   char* so_path = strdup( tmpfile_fmt );
   ssize_t rc = 0;
   
   int fd = mkstemp( so_path );
   if( fd < 0 ) {
      rc = -errno;
      SG_error("mkstemp(%s) rc = %zd\n", so_path, rc );
      free( so_path );
      return rc;
   }
   
   // write it out
   rc = md_write_uninterrupted( fd, buf, buflen );
   
   close( fd );
   
   if( rc < 0 ) {
      // failure 
      unlink( so_path );
      free( so_path );
   }
   else {
      *tmpfile_path = so_path;
   }
   
   return rc;
}

//////// courtesy of http://www.geekhideout.com/urlcode.shtml  //////////


/* Returns a url-encoded version of str */
/* IMPORTANT: be sure to free() the returned string after use */
// return the encoded URL on success
// return NULL on OOM
char *md_url_encode(char const *str, size_t len) {
  char *pstr = (char*)str;
  char *buf = (char*)calloc(len * 3 + 1, 1);
  
  if( buf == NULL ) {
     return NULL;
  }
  
  char *pbuf = buf;
  size_t cnt = 0;
  while (cnt < len) {
    if (isalnum(*pstr) || *pstr == '-' || *pstr == '_' || *pstr == '.' || *pstr == '~') {
      *pbuf++ = *pstr;
    }
    else if (*pstr == ' ') {
      *pbuf++ = '+';
    }
    else {
      *pbuf++ = '%';
      *pbuf++ = to_hex(*pstr >> 4);
      *pbuf++ = to_hex(*pstr & 15);
    }
    pstr++;
    cnt++;
  }
  *pbuf = '\0';
  return buf;
}

/* Returns a url-decoded version of str */
/* IMPORTANT: be sure to free() the returned string after use */
char *md_url_decode(char const *str, size_t* len) {
  char *pstr = (char*)str, *buf = (char*)calloc(strlen(str) + 1, 1), *pbuf = buf;
  size_t l = 0;
  while (*pstr) {
    if (*pstr == '%') {
      if (pstr[1] && pstr[2]) {
        *pbuf++ = from_hex(pstr[1]) << 4 | from_hex(pstr[2]);
        pstr += 2;
        l++;
      }
    } 
    else if (*pstr == '+') { 
      *pbuf++ = ' ';
      l++;
    } 
    else {
      *pbuf++ = *pstr;
      l++;
    }
    pstr++;
  }
  *pbuf = '\0';
  l++;
  if( len != NULL ) {
     *len = l;
  }
  
  return buf;
}

//////////////////////////////////////////////////////////////////////////

// Base64 decode and encode, from https://gist.github.com/barrysteyn/4409525#file-base64decode-c

int calcDecodeLength(const char* b64input, size_t len) { //Calculates the length of a decoded base64 string
  int padding = 0;

  if (b64input[len-1] == '=' && b64input[len-2] == '=') //last two chars are =
    padding = 2;
  else if (b64input[len-1] == '=') //last char is =
    padding = 1;

  return (int)len*0.75 - padding;
}

int md_base64_decode(const char* b64message, size_t b64message_len, char** buffer, size_t* buffer_len) { //Decodes a base64 encoded string
  BIO *bio, *b64;
  int decodeLen = calcDecodeLength(b64message, b64message_len);
  long len = 0;
  *buffer = (char*)malloc(decodeLen+1);
  FILE* stream = fmemopen((void*)b64message, b64message_len, "r");

  b64 = BIO_new(BIO_f_base64());
  bio = BIO_new_fp(stream, BIO_NOCLOSE);
  bio = BIO_push(b64, bio);
  BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL); //Do not use newlines to flush buffer
  len = BIO_read(bio, *buffer, b64message_len);
    //Can test here if len == decodeLen - if not, then return an error
  (*buffer)[len] = '\0';

  BIO_free_all(bio);
  fclose(stream);

  *buffer_len = (size_t)len;

  return (0); //success
}


int md_base64_encode(char const* message, size_t msglen, char** buffer) { //Encodes a string to base64
  BIO *bio, *b64;
  FILE* stream;
  int encodedSize = 4*ceil((double)msglen/3);
  *buffer = (char *)malloc(encodedSize+1);

  stream = fmemopen(*buffer, encodedSize+1, "w");
  b64 = BIO_new(BIO_f_base64());
  bio = BIO_new_fp(stream, BIO_NOCLOSE);
  bio = BIO_push(b64, bio);
  BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL); //Ignore newlines - write everything in one line
  BIO_write(bio, message, msglen);
  (void)BIO_flush(bio);
  BIO_free_all(bio);
  fclose(stream);

  return (0); //success
}

//////////////////////////////////////////////////////////////////////////

// pseudo random number generator
static uint32_t Q[4096], c=362436; /* choose random initial c<809430660 and */
                                         /* 4096 random 32-bit integers for Q[]   */
                                         
pthread_mutex_t CMWC4096_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t CMWC4096(void) {
   pthread_mutex_lock( &CMWC4096_lock );
   
   uint64_t t, a=18782LL;
   static uint32_t i=4095;
   uint32_t x,r=0xfffffffe;
   
   i=(i+1)&4095;
   t=a*Q[i]+c;
   c=(t>>32);
   x=t+c;
   
   if( x < c ) {
      x++;
      c++;
   }
   
   Q[i]=r-x;
   
   uint32_t ret = Q[i];
   
   pthread_mutex_unlock( &CMWC4096_lock );
   return ret;
}

uint32_t md_random32(void) {
   return CMWC4096();
}

uint64_t md_random64(void) {
   uint64_t upper = md_random32();
   uint64_t lower = md_random32();
   
   return (upper << 32) | lower;
}


int md_util_init(void) {
   // pseudo random number init
   int rfd = open("/dev/urandom", O_RDONLY );
   if( rfd < 0 ) {
      return -errno;
   }

   ssize_t nr = read( rfd, Q, 4096 * sizeof(uint32_t) );
   if( nr < 0 ) {
      close( rfd );
      return -errno;
   }
   if( nr != 4096 * sizeof(uint32_t) ) {
      close( rfd );
      return -ENODATA;
   }
   
   close( rfd );
   return 0;
}


// sleep for the given timespec amount of time, transparently handing EINTR
int md_sleep_uninterrupted( struct timespec* ts ) {
   
   struct timespec now;
   int rc = 0;
   
   rc = clock_gettime( CLOCK_MONOTONIC, &now );
   
   if( rc != 0 ) {
      rc = -errno;
      return rc;
   }
   
   struct timespec deadline;
   deadline.tv_sec = now.tv_sec + ts->tv_sec;
   deadline.tv_nsec = now.tv_nsec + ts->tv_nsec;
   
   while( true ) {
      
      rc = clock_nanosleep( CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL );
      if( rc != 0 ) {
         
         rc = -errno;
         if( rc == -EINTR ) {
            continue;
         }
         else {
            return rc;
         }
      }
      else {
         break;
      }
   }
   
   return 0;
}

// strip characters in strip from the end of str, replacing them with 0
// return the number of characters stripped
int md_strrstrip( char* str, char const* strip ) {
   
   size_t strip_len = strlen(strip);
   char* tmp = str + strlen(str) - 1;
   bool found = false;
   int itrs = 0;
   
   while( tmp != str ) {
      
      found = false;
      for( unsigned int i = 0; i < strip_len; i++ ) {
         if( *tmp == strip[i] ) {
            *tmp = 0;
            found = true;
            break;
         }
      }
      
      if( !found ) {
         break;
      }
      else {
         tmp--;
         itrs++;
      }
   }
   
   return itrs;
}

// translate zlib error codes 
static int md_zlib_err( int zerr ) {
   if( zerr == Z_MEM_ERROR ) {
      return -ENOMEM;
   }
   else if( zerr == Z_BUF_ERROR ) {
      return -ENOMEM;
   }
   else if( zerr == Z_DATA_ERROR ) {
      return -EINVAL;
   }
   return -EPERM;
}

// compress a string, returning the compressed string.
int md_deflate( char* in, size_t in_len, char** out, size_t* out_len ) {
   
   uint32_t out_buf_len = compressBound( in_len );
   char* out_buf = SG_CALLOC( char, out_buf_len );
   
   if( out_buf == NULL ) {
      return -ENOMEM;
   }
   
   uLongf out_buf_len_ul = out_buf_len;
   
   int rc = compress2( (unsigned char*)out_buf, &out_buf_len_ul, (unsigned char*)in, in_len, 9 );
   if( rc != Z_OK ) {
      SG_error("compress2 rc = %d\n", rc );
      
      free( out_buf );
      return md_zlib_err( rc );
   }
   
   *out = (char*)out_buf;
   *out_len = out_buf_len_ul;
   
   SG_debug("compressed %zu bytes down to %zu bytes\n", in_len, *out_len );
   
   return 0;
}

// decompress a string, returning the normal string 
int md_inflate( char* in, size_t in_len, char** out, size_t* out_len ) {
   
   uLongf out_buf_len = *out_len;
   char* out_buf = SG_CALLOC( char, out_buf_len );;
   
   if( out_buf == NULL ) {
      return -ENOMEM;
   }
   
   while( true ) {
      
      // try this size
      int rc = uncompress( (unsigned char*)out_buf, &out_buf_len, (unsigned char*)in, in_len );
      
      // not enough space?
      if( rc == Z_MEM_ERROR ) {
         
         // double it and try again 
         out_buf_len *= 2;
         
         char* tmp = (char*)realloc( out_buf, out_buf_len );
         
         if( tmp == NULL ) {
            free( out_buf );
            return -ENOMEM;
         }
         else {
            out_buf = tmp;
         }
         
         continue;
      }
      else if( rc != Z_OK ) {
         SG_error("uncompress rc = %d\n", rc );
         
         free( out_buf );
         return md_zlib_err( rc );
      }
      
      // success!
      *out = out_buf;
      *out_len = out_buf_len;
      break;
   }
   
   SG_debug("decompressed %zu bytes up to %zu bytes\n", in_len, *out_len );
   
   return 0;
}

// alloc and then mlock 
int mlock_calloc( struct mlock_buf* buf, size_t len ) {
   memset( buf, 0, sizeof( struct mlock_buf ) );
   
   int rc = posix_memalign( &buf->ptr, sysconf(_SC_PAGESIZE), len );
   if( rc != 0 ) {
      return rc;
   }
   
   memset( buf->ptr, 0, len );
   
   buf->len = len;
   
   rc = mlock( buf->ptr, buf->len );
   if( rc != 0 ) {
      free( buf->ptr );
      
      buf->ptr = NULL;
      buf->len = 0;
      
      return rc;
   }
   
   return 0;
}

// free an mlock'ed buf (unlock it first)
int mlock_free( struct mlock_buf* buf ) {
   if( buf->ptr != NULL ) {
      memset( buf->ptr, 0, buf->len );
      munlock( buf->ptr, buf->len );
      free( buf->ptr );
   }
   buf->ptr = NULL;
   buf->len = 0;
   return 0;
}

// duplicate a string into an mlock'ed buffer, allocating if needed
int mlock_dup( struct mlock_buf* dest, char const* src, size_t src_len ) {
   if( dest->ptr == NULL ) {
      int rc = mlock_calloc( dest, src_len );
      if( rc != 0 ) {
         SG_error("mlock_calloc rc = %d\n", rc );
         return rc;
      }
   }
   else if( dest->len < src_len) {
      SG_error("%s", "not enough space\n");
      return -EINVAL;
   }
   
   memcpy( dest->ptr, src, src_len );
   return 0;
}

// duplicate an mlock'ed buffer's contents, allocating dest if need be.
int mlock_buf_dup( struct mlock_buf* dest, struct mlock_buf* src ) {
   if( dest->ptr == NULL ) {
      int rc = mlock_calloc( dest, src->len );
      if( rc != 0 ) {
         SG_error("mlock_calloc rc = %d\n", rc );
         return rc;
      }
   }
   else if( dest->len < src->len ) {
      SG_error("%s", "not enough space\n");
      return -EINVAL;
   }
   
   memcpy( dest->ptr, src->ptr, src->len );
   return 0;
}



// flatten a response buffer into a byte string
static char* md_response_buffer_to_string_impl( md_response_buffer_t* rb, int extra_space ) {
   size_t total_len = 0;
   for( unsigned int i = 0; i < rb->size(); i++ ) {
      total_len += (*rb)[i].second;
   }
   
   char* msg_buf = SG_CALLOC( char, total_len + extra_space );
   size_t offset = 0;
   for( unsigned int i = 0; i < rb->size(); i++ ) {
      memcpy( msg_buf + offset, (*rb)[i].first, (*rb)[i].second );
      offset += (*rb)[i].second;
   }

   return msg_buf;
}

// flatten a response buffer into a byte string
// do not null-terminate
char* md_response_buffer_to_string( md_response_buffer_t* rb ) {
   return md_response_buffer_to_string_impl( rb, 0 );
}


// flatten a response buffer into a byte string, null-terminating it
char* md_response_buffer_to_c_string( md_response_buffer_t* rb ) {
   return md_response_buffer_to_string_impl( rb, 1 );
}

// free a response buffer
void md_response_buffer_free( md_response_buffer_t* rb ) {
   if( rb == NULL ) {
      return;
   }
   if( rb->size() == 0 ) {
      return;
   }
   
   for( unsigned int i = 0; i < rb->size(); i++ ) {
      if( rb->at(i).first != NULL ) {
         free( rb->at(i).first );
         rb->at(i).first = NULL;
      }
      rb->at(i).second = 0;
   }
   rb->clear();
}

// size of a response buffer
off_t md_response_buffer_size( md_response_buffer_t* rb ) {
   off_t ret = 0;
   for( unsigned int i = 0; i < rb->size(); i++ ) {
      ret += rb->at(i).second;
   }
   return ret;
}
   

// get task ID (no glibc wrapper around this...)
pid_t gettid(void) {
   return syscall( __NR_gettid );
}
