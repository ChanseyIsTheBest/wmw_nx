/* wmw_shims.h -- extra Bionic/NDK shims required by Where's My Water?
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * libc_shim.[ch] already covers the large common surface -- stdio over the fake
 * __sF, fortify _chk wrappers, stat/dirent layout conversion, locale,
 * semaphores and rwlocks. These are the additional entry points that
 * libwmw.so / libfmodex.so pull that it does not cover: anonymous and file
 * mmap, the dynamic-linker surface the engine probes optional libraries
 * through, offline network stubs, and a handful of small libc gaps.
 */

#ifndef __WMW_SHIMS_H__
#define __WMW_SHIMS_H__

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// --- memory mapping -------------------------------------------------------
void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap_fake(void *addr, size_t length);
int mlock_fake(const void *addr, size_t len);
int munlock_fake(const void *addr, size_t len);

// --- dynamic linker (engine probes optional libraries through these) ------
void *dlopen_fake(const char *filename, int flag);
void *dlsym_fake(void *handle, const char *symbol);
int dlclose_fake(void *handle);
int dladdr_fake(const void *addr, void *info);

// --- offline network stubs ------------------------------------------------
int socket_fake(int domain, int type, int protocol);
int connect_fake(int fd, const void *addr, uint32_t len);
int setsockopt_fake(int fd, int level, int optname, const void *optval, uint32_t optlen);
int shutdown_fake(int fd, int how);
long sendto_fake(int fd, const void *buf, size_t len, int flags, const void *dst, uint32_t dlen);
long recvfrom_fake(int fd, void *buf, size_t len, int flags, void *src, uint32_t *slen);
int poll_fake(void *fds, unsigned long nfds, int timeout);
int getaddrinfo_fake(const char *node, const char *service, const void *hints, void **res);
void freeaddrinfo_fake(void *res);

// --- small libc gaps ------------------------------------------------------
void *getenv_fake(const char *name);         // answers TMPDIR; NULL otherwise
int system_fake(const char *command);        // no shell -> -1
int open2_fake(const char *path, int flags); // __open_2
long read_chk_fake(int fd, void *buf, size_t nbytes, size_t buflen); // __read_chk
off_t lseek64_fake(int fd, off_t off, int whence);
int fcntl_fake(int fd, int cmd, ...);
int getpriority_fake(int which, int who);
int setpriority_fake(int which, int who, int prio);
char *getcwd_fake(char *buf, size_t size);
int chdir_fake(const char *path);
int mkdir_fake(const char *path, unsigned int mode);
int readlink_fake(const char *path, char *buf, size_t bufsiz);
int utime_fake(const char *path, const void *times);
void sincos_fake(double x, double *s, double *c);
char *strcasestr_fake(const char *haystack, const char *needle);
int  sched_yield_fake(void);                 // newlib/libnx coverage varies
int  isatty_fake(int fd);                    // no tty on Switch
unsigned int getuid_fake(void);              // no user model on Switch

// --- raw file I/O, as used by the engine's bundled SQLite -----------------
// SQLite reports every low-level failure as the single message "disk I/O
// error", which says nothing about which call failed. These wrappers pass
// straight through and log failures with errno so the next run names it.
long ssize_read_fake(int fd, void *buf, size_t n);
long ssize_write_fake(int fd, const void *buf, size_t n);
long lseek_fake2(int fd, long off, int whence);
int  close_fake(int fd);
int  fsync_fake(int fd);
int  ftruncate_fake(int fd, long len);

// The engine's bundled SQLite is the only user of raw open()/read()/write();
// assets all go through stdio. Tracking those descriptors lets us trace exactly
// the database's file activity and nothing else, which is the only way to see
// behind SQLite's single generic "disk I/O error".
void wmw_io_track(int fd, int add);
int  wmw_io_tracked(int fd);
void wmw_io_trace(const char *fmt, ...);

// devkitPro's filesystem layer does not necessarily supply meaningful inode
// numbers, but SQLite keys its per-file bookkeeping on (st_dev, st_ino): two
// descriptors reporting the same inode are treated as the same file by the same
// process, and lock state is shared between them. The engine keeps a connection
// to checked_water_tmp.db open while working on water.db, so if both report
// inode 0 SQLite concludes one connection is holding a conflicting lock on the
// other's file. These give each opened path a stable, distinct identity.
void     wmw_io_note_path(int fd, const char *path);
uint64_t wmw_io_fake_ino_fd(int fd);
uint64_t wmw_io_fake_ino_path(const char *path);

// An open descriptor for `path`, or -1. Horizon cannot stat() or access() a
// file that is already open for write -- unlike Linux -- so path-based queries
// about our own open files must be answered from the descriptor instead.
int wmw_io_fd_for_path(const char *path);

// --- path-mapped file operations -----------------------------------------
// These take Android-side paths and must go through wmw_resolve(); see
// wmw_paths.c for why the engine hands us bare POSIX absolute paths.
int access_fake(const char *path, int mode);
int remove_fake(const char *path);
int unlink_fake(const char *path);

// --- additional entry points pulled by libfmodex.so ------------------------
// FMOD Ex carries a net-stream feature and a C++ runtime tail. None of it is
// reachable in this port (the game only ever opens local files), so the socket
// surface fails cleanly and the C++ bits map onto the ordinary allocator.
void operator_delete_fake(void *p);          // _ZdlPv
void cxa_pure_virtual_fake(void);            // __cxa_pure_virtual
void fd_set_chk_fake(int fd, void *set, unsigned long setsize); // __FD_SET_chk
int  accept_fake(int fd, void *addr, unsigned int *len);
int  bind_fake(int fd, const void *addr, unsigned int len);
int  listen_fake(int fd, int backlog);
long recv_fake(int fd, void *buf, size_t len, int flags);
long send_fake(int fd, const void *buf, size_t len, int flags);
int  select_fake(int n, void *r, void *w, void *e, void *timeout);
void *gethostbyname_fake(const char *name);
unsigned int inet_addr_fake(const char *cp);

#endif
