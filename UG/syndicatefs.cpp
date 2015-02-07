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


// Implementation of the SyndicateFS methods.
//
// Special thanks to Joseph J. Pfeiffer for his Big Brother File System,
// from which the code here is derived.

#include "syndicatefs.h"

struct fuse_operations get_syndicatefs_opers() {
   struct fuse_operations fo;
   memset(&fo, 0, sizeof(fo));
   
   fo.getattr = syndicatefs_getattr;
   fo.readlink = syndicatefs_readlink;
   fo.mknod = syndicatefs_mknod;
   fo.mkdir = syndicatefs_mkdir;
   fo.unlink = syndicatefs_unlink;
   fo.rmdir = syndicatefs_rmdir;
   fo.symlink = syndicatefs_symlink;
   fo.rename = syndicatefs_rename;
   fo.link = syndicatefs_link;
   fo.chmod = syndicatefs_chmod;
   fo.chown = syndicatefs_chown;
   fo.truncate = syndicatefs_truncate;
   fo.utime = syndicatefs_utime;
   fo.open = syndicatefs_open;
   fo.read = syndicatefs_read;
   fo.write = syndicatefs_write;
   fo.statfs = syndicatefs_statfs;
   fo.flush = syndicatefs_flush;
   fo.release = syndicatefs_release;
   fo.fsync = syndicatefs_fsync;
   fo.setxattr = syndicatefs_setxattr;
   fo.getxattr = syndicatefs_getxattr;
   fo.listxattr = syndicatefs_listxattr;
   fo.removexattr = syndicatefs_removexattr;
   fo.opendir = syndicatefs_opendir;
   fo.readdir = syndicatefs_readdir;
   fo.releasedir = syndicatefs_releasedir;
   fo.fsyncdir = syndicatefs_fsyncdir;
   fo.init = syndicatefs_init;
   fo.access = syndicatefs_access;
   fo.create = syndicatefs_create;
   fo.ftruncate = syndicatefs_ftruncate;
   fo.fgetattr = syndicatefs_fgetattr;
  
   return fo;
}


/*
 * Get file attributes (lstat)
 */
int syndicatefs_getattr(const char *path, struct stat *statbuf) {
   
   struct md_syndicate_conf* conf = &SYNDICATEFS_DATA->conf;
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_getattr( %s, %p )\n", pthread_self(), path, statbuf );
   
   SYNDICATEFS_DATA->stats->enter( STAT_GETATTR );
   
   int rc = fs_entry_stat( SYNDICATEFS_DATA->core, path, statbuf, conf->owner, SYNDICATEFS_DATA->core->volume );
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_getattr rc = %d\n", pthread_self(), rc );
   
   SYNDICATEFS_DATA->stats->leave( STAT_GETATTR, rc );
   
   return rc;
}

/*
 * Read the target of a symbolic link.
 * In practice, this is a no-op, since there aren't any symlinks (yet)
 */
int syndicatefs_readlink(const char *path, char *link, size_t size) {

   SYNDICATEFS_DATA->stats->enter( STAT_READLINK );
   logmsg( SYNDICATEFS_DATA->logfile, "syndicatefs_readlink on path %s, size %u\n", pthread_self(), path, size);
   logerr( SYNDICATEFS_DATA->logfile, "ERR: not implemented\n");
   
   SYNDICATEFS_DATA->stats->leave( STAT_READLINK, -1 );
   return -EINVAL;
}


/*
 * Create a file node with open(), mkfifo(), or mknod(), depending on the mode.
 * Right now, only normal files are supported.
 */
int syndicatefs_mknod(const char *path, mode_t mode, dev_t dev) {

   struct md_syndicate_conf* conf = &SYNDICATEFS_DATA->conf;
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_mknod( %s, %o, %d )\n", pthread_self(), path, mode, dev );
   
   SYNDICATEFS_DATA->stats->enter( STAT_MKNOD );
   
   int rc = fs_entry_mknod( SYNDICATEFS_DATA->core, path, mode, dev, conf->owner, SYNDICATEFS_DATA->core->volume );
   
   SYNDICATEFS_DATA->stats->leave( STAT_MKNOD, rc );
   return rc;
}


/** Create a directory (mkdir) */
int syndicatefs_mkdir(const char *path, mode_t mode) {

   struct md_syndicate_conf* conf = &SYNDICATEFS_DATA->conf;
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_mkdir( %s, %o )\n", pthread_self(), path, mode );
   
   SYNDICATEFS_DATA->stats->enter( STAT_MKDIR );
   
   int rc = fs_entry_mkdir( SYNDICATEFS_DATA->core, path, mode, conf->owner, SYNDICATEFS_DATA->core->volume );
   
   SYNDICATEFS_DATA->stats->leave( STAT_MKDIR, rc );
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_mkdir rc = %d\n", pthread_self(), rc );
   return rc;
}

/** Remove a file (unlink) */
int syndicatefs_unlink(const char* path) {
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_unlink( %s )\n", pthread_self(), path );

   SYNDICATEFS_DATA->stats->enter( STAT_UNLINK );
   
   int rc = fs_entry_unlink( SYNDICATEFS_DATA->core, path, SYNDICATEFS_DATA->conf.owner, SYNDICATEFS_DATA->core->volume );

   SYNDICATEFS_DATA->stats->leave( STAT_UNLINK, rc );
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_unlink rc = %d\n", pthread_self(), rc);
   return rc;
}

/** Remove a directory (rmdir) */
int syndicatefs_rmdir(const char *path) {
   
   struct md_syndicate_conf* conf = &SYNDICATEFS_DATA->conf;
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_rmdir( %s )\n", pthread_self(), path );
   
   SYNDICATEFS_DATA->stats->enter( STAT_RMDIR );
   
   int rc = fs_entry_rmdir( SYNDICATEFS_DATA->core, path, conf->owner, SYNDICATEFS_DATA->core->volume );
   
   SYNDICATEFS_DATA->stats->leave( STAT_RMDIR, rc );

   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_rmdir rc = %d\n", pthread_self(), rc );
   return rc;
}


/** Create a symbolic link (symlink) */
int syndicatefs_symlink(const char *path, const char *link) {

   SYNDICATEFS_DATA->stats->enter( STAT_SYMLINK );
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_symlink on path %s, link %s\n", pthread_self(), path, link);
   SYNDICATEFS_DATA->stats->leave( STAT_SYMLINK, -1 );
   return -EPERM; // not supported
}


/** Rename a file.  Paths are FS-relative! (rename) */
int syndicatefs_rename(const char *path, const char *newpath) {
   
   struct md_syndicate_conf* conf = &SYNDICATEFS_DATA->conf;
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_rename( %s, %s )\n", pthread_self(), path, newpath );
   
   SYNDICATEFS_DATA->stats->enter( STAT_RENAME );

   int rc = fs_entry_rename( SYNDICATEFS_DATA->core, path, newpath, conf->owner, SYNDICATEFS_DATA->core->volume );

   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_rename( %s, %s ) rc = %d\n", pthread_self(), path, newpath, rc );
   
   SYNDICATEFS_DATA->stats->leave( STAT_RENAME, rc );
   return rc;
}


/** Create a hard link to a file (link) */
int syndicatefs_link(const char *path, const char *newpath) {
   SYNDICATEFS_DATA->stats->enter( STAT_LINK );
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_link hard from %s to %s\n", pthread_self(), path, newpath);
   SYNDICATEFS_DATA->stats->leave( STAT_LINK, -1 );
   return -EXDEV;    // not supported
}


/** Change the permission bits of a file (chmod) */
int syndicatefs_chmod(const char *path, mode_t mode) {
   
   struct md_syndicate_conf* conf = &SYNDICATEFS_DATA->conf;
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_chmod( %s, %o )\n", pthread_self(), path, mode );
   
   SYNDICATEFS_DATA->stats->enter( STAT_CHMOD );
   
   int rc = fs_entry_chmod( SYNDICATEFS_DATA->core, path, conf->owner, SYNDICATEFS_DATA->core->volume, mode );
   
   SYNDICATEFS_DATA->stats->leave( STAT_CHMOD, rc );
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_chmod rc = %d\n", rc);
   return rc;
}


/** Change the owner and group of a file (chown) */
int syndicatefs_chown(const char *path, uid_t user_id, gid_t volume_id) {
   // can't be done here--Syndicate uses 64-bit user_ids and volume_ids
   return -ENOSYS;
}


/** Change the size of a file (truncate) */
int syndicatefs_truncate(const char *path, off_t newsize) {
   struct md_syndicate_conf* conf = &SYNDICATEFS_DATA->conf;
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_truncate( %s, %ld )\n", pthread_self(), path, newsize );

   SYNDICATEFS_DATA->stats->enter( STAT_TRUNCATE );

   int rc = fs_entry_truncate( SYNDICATEFS_DATA->core, path, newsize, conf->owner, SYNDICATEFS_DATA->core->volume );

   SYNDICATEFS_DATA->stats->leave( STAT_TRUNCATE, rc );
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_truncate rc = %d\n", pthread_self(), rc );
   return rc;
}


/** Change the access and/or modification times of a file (utime) */
int syndicatefs_utime(const char *path, struct utimbuf *ubuf) {
   struct md_syndicate_conf* conf = &SYNDICATEFS_DATA->conf;
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_utime( %s, {%d, %d} )\n", pthread_self(), path, ubuf->actime, ubuf->modtime );
   
   SYNDICATEFS_DATA->stats->enter( STAT_UTIME );
   
   int rc = fs_entry_utime( SYNDICATEFS_DATA->core, path, ubuf, conf->owner, SYNDICATEFS_DATA->core->volume );
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_utime rc = %d\n", pthread_self(), rc);
   SYNDICATEFS_DATA->stats->leave( STAT_UTIME, rc );
   return rc;
}



/** File open operation (O_CREAT and O_EXCL will *not* be passed to this method, according to the documentation) */
int syndicatefs_open(const char *path, struct fuse_file_info *fi) {

   struct md_syndicate_conf* conf = &SYNDICATEFS_DATA->conf;
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_open( %s, %p (flags = %o) )\n", pthread_self(), path, fi, fi->flags );
   
   SYNDICATEFS_DATA->stats->enter( STAT_OPEN );
   
   int err = 0;
   struct fs_file_handle* fh = fs_entry_open( SYNDICATEFS_DATA->core, path, conf->owner, SYNDICATEFS_DATA->core->volume, fi->flags, ~conf->usermask, &err );
   
   // store the read handle
   fi->fh = (uint64_t)fh;

   // force direct I/O
   fi->direct_io = 1;
   
   SYNDICATEFS_DATA->stats->leave( STAT_OPEN, err );
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_open rc = %d\n", pthread_self(), err );
   
   return err;
}


/** Read data from an open file.  Return number of bytes read. */
int syndicatefs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_read( %s, %p, %ld, %ld, %p )\n", pthread_self(), path, buf, size, offset, fi );
   
   SYNDICATEFS_DATA->stats->enter( STAT_READ );
   
   struct fs_file_handle* fh = (struct fs_file_handle*)fi->fh;
   ssize_t rc = fs_entry_read( SYNDICATEFS_DATA->core, fh, buf, size, offset );
   
   if( rc < 0 ) {
      SYNDICATEFS_DATA->stats->leave( STAT_READ, -1 );
      logerr( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_read rc = %ld\n", pthread_self(), rc );
      return -1;
   }
   
   // fill the remainder of buf with 0's
   if( rc < (signed)size ) {
      memset( buf + rc, 0, size - rc );
   }
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_read rc = %ld\n", pthread_self(), rc );
   
   SYNDICATEFS_DATA->stats->leave( STAT_READ, (rc >= 0 ? 0 : rc) );
   return rc;
}


/** Write data to an open file (pwrite) */
int syndicatefs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {

   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_write( %s, %p, %ld, %ld, %p )\n", pthread_self(), path, buf, size, offset, fi->fh );
   
   SYNDICATEFS_DATA->stats->enter( STAT_WRITE );
   
   struct fs_file_handle* fh = (struct fs_file_handle*)fi->fh;
   
   ssize_t rc = fs_entry_write( SYNDICATEFS_DATA->core, fh, buf, size, offset );
   
   SYNDICATEFS_DATA->stats->leave( STAT_WRITE, (rc >= 0 ? 0 : rc)  );
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_write rc = %d\n", pthread_self(), rc );
   return (int)rc;
}


/** Get file system statistics
 *
 * The 'f_frsize', 'f_favail', 'f_fsid' and 'f_flag' fields are ignored
 *
 */
int syndicatefs_statfs(const char *path, struct statvfs *statv) {
   struct md_syndicate_conf* conf = &SYNDICATEFS_DATA->conf;
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_statfs( %s, %p )\n", pthread_self(), path, statv );
   
   SYNDICATEFS_DATA->stats->enter( STAT_STATFS );
   
   int rc = fs_entry_statfs( SYNDICATEFS_DATA->core, path, statv, conf->owner, SYNDICATEFS_DATA->core->volume );
   
   SYNDICATEFS_DATA->stats->leave( STAT_STATFS, rc );
   return rc;
}


/** Possibly flush cached data (No-op) */
int syndicatefs_flush(const char *path, struct fuse_file_info *fi) {
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_flush( %s, %p )\n", pthread_self(), path, fi );
   
   SYNDICATEFS_DATA->stats->enter( STAT_FLUSH );

   struct fs_file_handle* fh = (struct fs_file_handle*)fi->fh;
   
   int rc = fs_entry_fsync( SYNDICATEFS_DATA->core, fh );
   
   SYNDICATEFS_DATA->stats->leave( STAT_FLUSH, rc );

   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_flush rc = %d\n", pthread_self(), rc );
   return rc;
}


/** Release an open file (close) */
int syndicatefs_release(const char *path, struct fuse_file_info *fi) {

   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_release( %s, %p )\n", pthread_self(), path, fi );
   
   SYNDICATEFS_DATA->stats->enter( STAT_RELEASE );
   
   struct fs_file_handle* fh = (struct fs_file_handle*)fi->fh;
   
   int rc = fs_entry_close( SYNDICATEFS_DATA->core, fh );
   if( rc != 0 ) {
      logerr( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_release: fs_entry_close rc = %d\n", pthread_self(), rc );
   }
   
   free( fh );
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_release rc = %d\n", pthread_self(), rc );
   
   SYNDICATEFS_DATA->stats->leave( STAT_RELEASE, rc );
   return rc;
}


/** Synchronize file contents (fdatasync, fsync)
 *
 * If the datasync parameter is non-zero, then only the user data
 * should be flushed, not the meta data.
 *
 */
int syndicatefs_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_fsync( %s, %d, %p )\n", pthread_self(), path, datasync, fi );
   
   SYNDICATEFS_DATA->stats->enter( STAT_FSYNC );
   
   struct fs_file_handle* fh = (struct fs_file_handle*)fi->fh;
   int rc = 0;
   if( datasync == 0 )
      rc = fs_entry_fdatasync( SYNDICATEFS_DATA->core, fh );
   
   if( rc == 0 )
      fs_entry_fsync( SYNDICATEFS_DATA->core, fh );
      
   SYNDICATEFS_DATA->stats->leave( STAT_FSYNC, rc );

   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_fsync rc = %d\n", pthread_self(), rc );
   return rc;
}


/** Set extended attributes (lsetxattr) */
int syndicatefs_setxattr(const char *path, const char *name, const char *value, size_t size, int flags) {

   struct md_syndicate_conf* conf = &SYNDICATEFS_DATA->conf;
   
   char* safe_value = (char*)calloc( size + 1, 1 );
   strncpy( safe_value, value, size );
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_setxattr( %s, %s, %s, %d, %x )\n", pthread_self(), path, name, safe_value, size, flags );
   free( safe_value );
   
   SYNDICATEFS_DATA->stats->enter( STAT_SETXATTR );
   
   int rc = fs_entry_setxattr( SYNDICATEFS_DATA->core, path, name, value, size, flags, conf->owner, SYNDICATEFS_DATA->core->volume );
   
   SYNDICATEFS_DATA->stats->leave( STAT_SETXATTR, rc );

   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_setxattr rc = %d\n", pthread_self(), rc );
   return rc;
}


/** Get extended attributes (lgetxattr) */
int syndicatefs_getxattr(const char *path, const char *name, char *value, size_t size) {

   struct md_syndicate_conf* conf = &SYNDICATEFS_DATA->conf;
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_getxattr( %s, %s, %p, %d )\n", pthread_self(), path, name, value, size );
   
   SYNDICATEFS_DATA->stats->enter( STAT_GETXATTR );
   
   int rc = fs_entry_getxattr( SYNDICATEFS_DATA->core, path, name, value, size, conf->owner, SYNDICATEFS_DATA->core->volume );
   
   SYNDICATEFS_DATA->stats->leave( STAT_GETXATTR, rc );

   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_getxattr rc = %d\n", pthread_self(), rc );
   return rc;
}


/** List extended attributes (llistxattr) */
int syndicatefs_listxattr(const char *path, char *list, size_t size) {

   struct md_syndicate_conf* conf = &SYNDICATEFS_DATA->conf;
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_listxattr( %s, %p, %d )\n", pthread_self(), path, list, size );
   
   SYNDICATEFS_DATA->stats->enter( STAT_LISTXATTR );
   
   int rc = fs_entry_listxattr( SYNDICATEFS_DATA->core, path, list, size, conf->owner, SYNDICATEFS_DATA->core->volume );
   
   SYNDICATEFS_DATA->stats->leave( STAT_LISTXATTR, rc );

   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_listxattr rc = %d\n", pthread_self(), rc );
   
   return rc;
}


/** Remove extended attributes (lremovexattr) */
int syndicatefs_removexattr(const char *path, const char *name) {
   struct md_syndicate_conf* conf = &SYNDICATEFS_DATA->conf;
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_removexattr( %s, %s )\n", pthread_self(), path, name );
   
   SYNDICATEFS_DATA->stats->enter( STAT_REMOVEXATTR );
   
   int rc = fs_entry_removexattr( SYNDICATEFS_DATA->core, path, name, conf->owner, SYNDICATEFS_DATA->core->volume );

   SYNDICATEFS_DATA->stats->leave( STAT_REMOVEXATTR, rc );

   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_removexattr rc = %d\n", pthread_self(), rc );
   return rc;
}


/** Open directory (opendir) */
int syndicatefs_opendir(const char *path, struct fuse_file_info *fi) {

   struct md_syndicate_conf* conf = &SYNDICATEFS_DATA->conf;
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_opendir( %s, %p )\n", pthread_self(), path, fi );
   
   SYNDICATEFS_DATA->stats->enter( STAT_OPENDIR );

   int rc = 0;
   struct fs_dir_handle* fdh = fs_entry_opendir( SYNDICATEFS_DATA->core, path, conf->owner, SYNDICATEFS_DATA->core->volume, &rc );
   
   if( rc == 0 )
      fi->fh = (uint64_t)fdh;
   
   SYNDICATEFS_DATA->stats->leave( STAT_OPENDIR, rc );
   
   logmsg( SYNDICATEFS_DATA->logfile,  "%16lx: syndicatefs_opendir rc = %d\n", pthread_self(), rc );
   
   return rc;
}


/** Read directory (readdir)
 *
 * This supersedes the old getdir() interface.  New applications
 * should use this.
 *
 * The filesystem may choose between two modes of operation:
 *
 * 1) The readdir implementation ignores the offset parameter, and
 * passes zero to the filler function's offset.  The filler
 * function will not return '1' (unless an error happens), so the
 * whole directory is read in a single readdir operation.  This
 * works just like the old getdir() method.
 *
 * 2) The readdir implementation keeps track of the offsets of the
 * directory entries.  It uses the offset parameter and always
 * passes non-zero offset to the filler function.  When the buffer
 * is full (or an error happens) the filler function will return
 * '1'.
 *
 */
int syndicatefs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {

   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_readdir( %s, %p, %p, %ld, %p )\n", pthread_self(), path, buf, filler, offset, fi );
   
   SYNDICATEFS_DATA->stats->enter( STAT_READDIR );

   struct fs_dir_handle* fdh = (struct fs_dir_handle *)fi->fh;     // get back our DIR instance
   
   int rc = 0;
   struct fs_dir_entry** dirents = fs_entry_readdir( SYNDICATEFS_DATA->core, fdh, &rc );
   
   if( rc == 0 && dirents ) {
      
      // fill in the directory data
      int i = 0;
      while( dirents[i] != NULL ) {
         if( filler(buf, dirents[i]->data.name, NULL, 0) != 0 ) {
            logerr( SYNDICATEFS_DATA->logfile, "%16lx: ERR: syndicatefs_readdir filler: buffer full\n");
            rc = -ENOMEM;
            break;
         }
         i++;
      }
   }
   
   fs_dir_entry_destroy_all( dirents );
   free( dirents );
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_readdir rc = %d\n", pthread_self(), rc );
   
   SYNDICATEFS_DATA->stats->leave( STAT_READDIR, rc );
   return rc;
}


/** Release directory (closedir) */
int syndicatefs_releasedir(const char *path, struct fuse_file_info *fi) {
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_releasedir( %s, %p )\n", pthread_self(), path, fi );
   
   SYNDICATEFS_DATA->stats->enter( STAT_RELEASEDIR );
   
   struct fs_dir_handle* fdh = (struct fs_dir_handle*)fi->fh;
   
   int rc = fs_entry_closedir( SYNDICATEFS_DATA->core, fdh );
   
   free( fdh );
   
   SYNDICATEFS_DATA->stats->leave( STAT_RELEASEDIR, rc );

   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_releasedir rc = %d\n", pthread_self(), rc );
   return rc;
}


/** Synchronize directory contents (no-op) */
int syndicatefs_fsyncdir(const char *path, int datasync, struct fuse_file_info *fi) {
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_fsyncdir( %s, %d, %p )\n", pthread_self(), path, datasync, fi );
   
   SYNDICATEFS_DATA->stats->enter( STAT_FSYNCDIR );
   
   SYNDICATEFS_DATA->stats->leave( STAT_FSYNCDIR, 0 );

   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_fsyncdir rc = %d\n", pthread_self(), 0 );
   return 0;
}


/**
 * Check file access permissions (access)
 */
int syndicatefs_access(const char *path, int mask) {
   struct md_syndicate_conf* conf = &SYNDICATEFS_DATA->conf;
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_access( %s, %x )\n", pthread_self(), path, mask );
   
   SYNDICATEFS_DATA->stats->enter( STAT_ACCESS );
   
   int rc = fs_entry_access( SYNDICATEFS_DATA->core, path, mask, conf->owner, SYNDICATEFS_DATA->core->volume );
      
   SYNDICATEFS_DATA->stats->leave( STAT_ACCESS, rc );

   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_access rc = %d\n", pthread_self(), rc );
   return rc;
}


/**
 * Create and open a file (creat)
 */
int syndicatefs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
   struct md_syndicate_conf* conf = &SYNDICATEFS_DATA->conf;
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_create( %s, %o, %p )\n", pthread_self(), path, mode, fi );
   
   SYNDICATEFS_DATA->stats->enter( STAT_CREATE );
   
   int rc = 0;
   struct fs_file_handle* fh = fs_entry_create( SYNDICATEFS_DATA->core, path, conf->owner, SYNDICATEFS_DATA->core->volume, mode, &rc );
   
   if( rc == 0 && fh != NULL ) {
      fi->fh = (uint64_t)( fh );
   }
   
   SYNDICATEFS_DATA->stats->leave( STAT_CREATE, rc );

   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_create rc = %d\n", pthread_self(), rc );
   return rc;
}


/**
 * Change the size of an file (ftruncate)
 */
int syndicatefs_ftruncate(const char *path, off_t length, struct fuse_file_info *fi) {

   struct md_syndicate_conf* conf = &SYNDICATEFS_DATA->conf;
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_ftruncate( %s, %ld, %p )\n", pthread_self(), path, length, fi );
   
   SYNDICATEFS_DATA->stats->enter( STAT_FTRUNCATE );

   struct fs_file_handle* fh = (struct fs_file_handle*)fi->fh;
   int rc = fs_entry_ftruncate( SYNDICATEFS_DATA->core, fh, length, conf->owner, SYNDICATEFS_DATA->core->volume );
   if( rc != 0 ) {
      SG_error( "fs_entry_ftruncate rc = %d\n", rc );
   }
   
   SYNDICATEFS_DATA->stats->leave( STAT_FTRUNCATE, rc );

   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_ftrunctate rc = %d\n", pthread_self(), rc );
   
   return rc;
}


/**
 * Get attributes from an open file (fstat)
 */
int syndicatefs_fgetattr(const char *path, struct stat *statbuf, struct fuse_file_info *fi) {
   
   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_fgetattr( %s, %p, %p )\n", pthread_self(), path, statbuf, fi );
   
   SYNDICATEFS_DATA->stats->enter( STAT_FGETATTR );
   
   struct fs_file_handle* fh = (struct fs_file_handle*)(fi->fh);
   int rc = fs_entry_fstat( SYNDICATEFS_DATA->core, fh, statbuf );
   
   SYNDICATEFS_DATA->stats->leave( STAT_FGETATTR, rc );

   logmsg( SYNDICATEFS_DATA->logfile, "%16lx: syndicatefs_fgetattr rc = %d\n", pthread_self(), rc );
   
   return rc;
}




/**
 * Initialize filesystem
 *
 * The return value will passed in the private_data field of
 * fuse_context to all file operations and as a parameter to the
 * destroy() method.
 *
 */
// Undocumented but extraordinarily useful fact:  the fuse_context is
// set up before this function is called, and
// fuse_get_context()->private_data returns the user_data passed to
// fuse_main().  Really seems like either it should be a third
// parameter coming in here, or else the fact should be documented
// (and this might as well return void, as it did in older versions of
// FUSE).
void *syndicatefs_init(struct fuse_conn_info *conn) {
   return SYNDICATEFS_DATA;
}

/**
 * Clean up filesystem
 *
 * Called on filesystem exit.
 */
void syndicatefs_destroy(void *userdata) {
   return;
}


// handle extra options for FUSE
static fuse_args g_fargs = FUSE_ARGS_INIT( 0, NULL );

int syndicatefs_handle_fuse_opt( int fuse_opt, char* fuse_arg ) {
   int rc = 0;
   SG_debug("Fuse opt: -%c\n", fuse_opt);
   
   switch( fuse_opt ) {
      
      case 's': {
         // single thread mode
         fuse_opt_add_arg( &g_fargs, "-s" );
         break;
      }
      case 'o': {
         // some fuse argument
         char* buf = SG_CALLOC( char, strlen(fuse_arg) + 3 );
         sprintf(buf, "-o%s", fuse_arg );
         fuse_opt_add_arg( &g_fargs, buf );
         free( buf );
         break;
      }
      default: {
         rc = -1;
         break;
      }
   }
   
   return rc;
}


// combined option parser 
int syndicatefs_handle_opt( int opt_c, char* opt_s ) {
   // try to handle internal opt 
   int rc = UG_handle_opt( opt_c, opt_s );
   if( rc != 0 ) {
      // try to handle it as a FUSE opt 
      rc = syndicatefs_handle_fuse_opt( opt_c, opt_s );
   }
   return rc;
}


// syndicatefs-specific usage 
void syndicatefs_usage() {
   fprintf(stderr, "\n\
syndicatefs-specific options:\n\
   -s\n\
            Run single-threaded.\n\
   -o ARG\n\
            Pass a FUSE option.\n\
\n");
}


// combined usage 
void usage( char const* progname ) {
   md_common_usage( progname );
   UG_usage();
   syndicatefs_usage();
}

// Program execution starts here!
int main(int argc, char** argv) {

   int fuse_stat = 0;
   int rc = 0;
   int fuse_optind = 0;

   fuse_opt_add_arg( &g_fargs, argv[0] );

   // prevent root from mounting this, since we don't really do much
   // in the way of checking access.
#ifndef _FIREWALL
   if( getuid() == 0 || geteuid() == 0 ) {
      perror("Running SyndicateFS as root opens unnacceptable security holes\n");
      return 1;
   }
#endif
   
   struct md_opts syn_opts;
   
   memset( &syn_opts, 0, sizeof(struct md_opts));
   UG_opts_init();
   
   // get options
   rc = md_opts_parse( &syn_opts, argc, argv, &fuse_optind, UG_SHORTOPTS "so:", syndicatefs_handle_opt );
   if( rc != 0 ) {
      usage( argv[0] );
      exit(1);
   }
   
   // get back UG opts 
   struct UG_opts ug_opts;
   UG_opts_get( &ug_opts );
   
   // force direct io
   // TODO: investigate kernel_cache, direct_io, atomic_o_truncate, big_writes, splice_*, auto_cache
   fuse_opt_add_arg( &g_fargs, "-odirect_io" );
   
   // allow other users 
   fuse_opt_add_arg( &g_fargs, "-oallow_other" );
   
   // foreground?
   if( syn_opts.foreground ) {
      fuse_opt_add_arg( &g_fargs, "-f" );
   }
   
   // we need a mountpoint, and possibly other options.
   // this should be the first non-opt argument
   if( syn_opts.first_nonopt_arg == NULL ) {
      usage( argv[0] );
      fprintf(stderr, "You must give a mountpoint!\n");
      exit(1);
   }
   
   // get absolute path to mountpoint
   char* mountpoint = realpath( syn_opts.first_nonopt_arg, NULL );
   if( mountpoint == NULL ) {
      int errsv = errno;
      fprintf(stderr, "syndicatefs: realpath(%s) rc = %d\n", syn_opts.first_nonopt_arg, -errsv );
      exit(1);
   }
   
   // add mountpoint 
   fuse_opt_add_arg( &g_fargs, mountpoint );

   struct md_HTTP syndicate_http;
   memset( &syndicate_http, 0, sizeof(struct md_HTTP) );
   
   // start core services
   rc = syndicate_init( &syn_opts, &ug_opts );
   if( rc != 0 ) {
      fprintf(stderr, "Syndicate failed to initialize\n");
      exit(1);
   }
   
   struct syndicate_state* state = syndicate_get_state();

   // start back-end HTTP server
   rc = SG_server_init( state, &syndicate_http );
   if( rc != 0 )
      exit(1);
   
   // set as running
   syndicate_set_running();
      
   printf("\n\nSyndicateFS starting up\n\n");

   struct fuse_operations syndicatefs_oper = get_syndicatefs_opers();

   // GO GO GO!!!
   fuse_stat = fuse_main(g_fargs.argc, g_fargs.argv, &syndicatefs_oper, state );

   SG_error( " fuse_main returned %d\n", fuse_stat);

   printf( "\n\nSyndicateFS shutting down\n\n");

   free( mountpoint );

   SG_server_shutdown( &syndicate_http );

   int wait_replicas = -1;
   if( !ug_opts.flush_replicas ) {
      wait_replicas = 0;
   }
   
   syndicate_destroy( wait_replicas );
   
   curl_global_cleanup();
   google::protobuf::ShutdownProtobufLibrary();
   
   return fuse_stat;
}

