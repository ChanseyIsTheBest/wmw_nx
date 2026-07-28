/* wmw_shims.c -- extra Bionic/NDK shims required by Where's My Water?
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Everything here is deliberately conservative: the goal is to let libwmw.so
 * and libfmodex.so load and run without ever reaching a service the Switch does
 * not have. Network calls fail cleanly, mmap is emulated over the heap, and the
 * dynamic linker answers "not available" for every optional library.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>

#include "util.h"
#include "so_util.h"
#include "libc_shim.h"
#include "wmw_shims.h"
#include "wmw_paths.h"
#include "opensles.h"

// Bionic/Linux mmap constants (independent of the host newlib values)
#define BIONIC_PROT_NONE   0x0
#define BIONIC_MAP_SHARED  0x1
#define BIONIC_MAP_PRIVATE 0x2
#define BIONIC_MAP_FIXED   0x10
#define BIONIC_MAP_ANON    0x20
#define BIONIC_MAP_FAILED  ((void *)-1)

// ---------------------------------------------------------------------------
// mmap/munmap emulation
//
// Horizon has no demand-paged file mapping, so we satisfy every request with
// a heap allocation. Anonymous maps are just zeroed memory; file-backed maps
// are filled once via pread at the requested offset. A small table remembers
// each base pointer so munmap can free it (callers pass the exact base
// back). MAP_FIXED is honoured only in the degenerate addr==NULL case.
// ---------------------------------------------------------------------------

typedef struct {
  void *base;
  size_t len;
} MmapRec;

static MmapRec *g_maps = NULL;
static size_t g_maps_len = 0, g_maps_cap = 0;
static Mutex g_maps_mtx;
static bool g_maps_mtx_init = false;

static void maps_lock(void) {
  if (!g_maps_mtx_init) { mutexInit(&g_maps_mtx); g_maps_mtx_init = true; }
  mutexLock(&g_maps_mtx);
}
static void maps_unlock(void) { mutexUnlock(&g_maps_mtx); }

static void maps_remember(void *base, size_t len) {
  maps_lock();
  if (g_maps_len == g_maps_cap) {
    size_t ncap = g_maps_cap ? g_maps_cap * 2 : 16;
    MmapRec *n = realloc(g_maps, ncap * sizeof(*n));
    if (n) { g_maps = n; g_maps_cap = ncap; }
  }
  if (g_maps_len < g_maps_cap) {
    g_maps[g_maps_len].base = base;
    g_maps[g_maps_len].len = len;
    g_maps_len++;
  }
  maps_unlock();
}

static int maps_forget(void *base) {
  int found = 0;
  maps_lock();
  for (size_t i = 0; i < g_maps_len; i++) {
    if (g_maps[i].base == base) {
      g_maps[i] = g_maps[g_maps_len - 1];
      g_maps_len--;
      found = 1;
      break;
    }
  }
  maps_unlock();
  return found;
}

void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
  (void)addr; (void)prot;
  if (length == 0)
    return BIONIC_MAP_FAILED;

  // 64-byte alignment keeps SIMD copies and FMOD mixer buffers happy
  void *mem = NULL;
  if (posix_memalign_fake(&mem, 64, length) != 0 || !mem)
    return BIONIC_MAP_FAILED;
  memset(mem, 0, length);

  if (!(flags & BIONIC_MAP_ANON) && fd >= 0) {
    // file-backed: pull the requested window in once. newlib has no pread, so
    // seek-read-restore by hand (mmap callers don't expect the fd position to
    // move).
    const off_t saved = lseek(fd, 0, SEEK_CUR);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    if (lseek(fd, offset, SEEK_SET) == offset) {
      ssize_t got = read(fd, mem, length);
      if (got < 0)
        debugPrintf("mmap: read(fd=%d,len=%zu,off=%lld) failed\n",
                    fd, length, (long long)offset);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    double dt = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1.0e6;
    debugPrintf("io: mmap file-backed fd=%d len=%zu took %.1f ms\n", fd, length, dt);
    if (saved >= 0)
      lseek(fd, saved, SEEK_SET);
  }

  maps_remember(mem, length);
  return mem;
}

int munmap_fake(void *addr, size_t length) {
  (void)length;
  if (addr && addr != BIONIC_MAP_FAILED && maps_forget(addr))
    free(addr);
  return 0;
}

int mlock_fake(const void *addr, size_t len) { (void)addr; (void)len; return 0; }
int munlock_fake(const void *addr, size_t len) { (void)addr; (void)len; return 0; }

// ---------------------------------------------------------------------------
// dynamic linker surface
//
// libwmw.so imports dlopen/dlsym/dlclose/dlerror. On Android it uses them to
// probe optional system libraries (GLES2 for the shader path it never takes on
// a GLES1 device, and the analytics libraries the Java layer would have loaded).
// Returning NULL for every library is the correct answer here: it makes the
// engine take its fixed-function GLES1 path and skip analytics entirely.
//
// libapminsighta.so / libapminsightb.so are deliberately NOT loaded. They are
// APM/analytics modules that only the Java layer ever touched -- they do not
// appear in libwmw.so's DT_NEEDED, so nothing in the engine needs them.
//
// As a safety net, unknown symbols are looked up across the loaded modules, so
// a dlsym() against libfmodex still resolves.
// ---------------------------------------------------------------------------

static int g_self_handle;

static int g_opensles_handle;

void *dlopen_fake(const char *filename, int flag) {
  (void)flag;
  if (!filename)
    return &g_self_handle; // dlopen(NULL) -> "this program"

  // FMOD Ex reaches OpenSL ES the Android way: dlopen("libOpenSLES.so") then
  // dlsym("slCreateEngine"). This build of libfmodex contains only two outputs
  // -- "FMOD NoSound Output" and "FMOD OpenSL ES Output" -- so refusing the
  // dlopen leaves it running NoSound, which is silent by design. Hand back a
  // sentinel and resolve the entry points from opensles.c.
  if (strstr(filename, "libOpenSLES")) {
    debugPrintf("dlopen(%s) -> opensles shim\n", filename);
    return &g_opensles_handle;
  }

  debugPrintf("dlopen(%s) -> NULL (stubbed)\n", filename);
  return NULL;
}

void *dlsym_fake(void *handle, const char *symbol) {
  if (!symbol)
    return NULL;

  if (handle == &g_opensles_handle) {
    if (!strcmp(symbol, "slCreateEngine"))
      return (void *)&slCreateEngine;
    #define SL_IID_CASE(n) if (!strcmp(symbol, "SL_IID_" #n)) return (void *)&SL_IID_##n
    SL_IID_CASE(ENGINE);
    SL_IID_CASE(PLAY);
    SL_IID_CASE(VOLUME);
    SL_IID_CASE(BUFFERQUEUE);
    SL_IID_CASE(ANDROIDSIMPLEBUFFERQUEUE);
    SL_IID_CASE(ANDROIDCONFIGURATION);
    SL_IID_CASE(OUTPUTMIX);
    // libfmodex.so resolves exactly six OpenSL symbols: slCreateEngine plus
    // SL_IID_ENGINE, SL_IID_PLAY, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
    // SL_IID_ANDROIDCONFIGURATION and SL_IID_RECORD. RECORD is for microphone
    // capture, which nothing here uses -- but it is looked up unconditionally
    // at init, and a NULL can be read as "this library is not usable".
    SL_IID_CASE(RECORD);
    #undef SL_IID_CASE
    debugPrintf("dlsym(opensles, %s) -> NULL\n", symbol);
    return NULL;
  }

  uintptr_t a = so_find_addr_in_loaded(symbol);
  if (a)
    return (void *)a;
  debugPrintf("dlsym(%s) -> NULL\n", symbol);
  return NULL;
}

int dlclose_fake(void *handle) { (void)handle; return 0; }

int dladdr_fake(const void *addr, void *info) {
  (void)addr; (void)info;
  return 0; // 0 == failure for dladdr; callers use it only for diagnostics
}

// ---------------------------------------------------------------------------
// offline network stubs -- the game never needs the network on Switch
// ---------------------------------------------------------------------------

int socket_fake(int domain, int type, int protocol) {
  (void)domain; (void)type; (void)protocol; errno = EAFNOSUPPORT; return -1;
}
int connect_fake(int fd, const void *addr, uint32_t len) {
  (void)fd; (void)addr; (void)len; errno = ENETUNREACH; return -1;
}
int setsockopt_fake(int fd, int level, int optname, const void *optval, uint32_t optlen) {
  (void)fd; (void)level; (void)optname; (void)optval; (void)optlen; return 0;
}
int shutdown_fake(int fd, int how) { (void)fd; (void)how; return 0; }
long sendto_fake(int fd, const void *buf, size_t len, int flags, const void *dst, uint32_t dlen) {
  (void)fd; (void)buf; (void)len; (void)flags; (void)dst; (void)dlen; errno = ENETUNREACH; return -1;
}
long recvfrom_fake(int fd, void *buf, size_t len, int flags, void *src, uint32_t *slen) {
  (void)fd; (void)buf; (void)len; (void)flags; (void)src; (void)slen; errno = ENETUNREACH; return -1;
}
int poll_fake(void *fds, unsigned long nfds, int timeout) {
  (void)fds; (void)nfds;
  if (timeout > 0) svcSleepThread((int64_t)timeout * 1000000LL);
  return 0; // nothing ever ready
}
int getaddrinfo_fake(const char *node, const char *service, const void *hints, void **res) {
  (void)node; (void)service; (void)hints;
  if (res) *res = NULL;
  return -2; // EAI_NONAME-ish; any non-zero is "lookup failed"
}
void freeaddrinfo_fake(void *res) { (void)res; }

// ---------------------------------------------------------------------------
// small libc gaps
// ---------------------------------------------------------------------------

// The engine's bundled SQLite locates scratch space via unixTempFileDir(),
// which tries $SQLITE_TMPDIR, then $TMPDIR, then the hard-coded /var/tmp,
// /usr/tmp and /tmp. None of those exist on Switch, and when the list is
// exhausted SQLite returns SQLITE_IOERR_GETTEMPPATH -- whose message is the
// unhelpfully generic "disk I/O error". Since the library is built with
// TEMP_STORE=1 (temp data goes to files, not memory), any statement needing a
// temp file fails while ordinary reads keep working.
//
// Answering TMPDIR is the cleanest fix: SQLite takes the first usable entry and
// never reaches the hard-coded paths.
void *getenv_fake(const char *name) {
  if (!name) return NULL;
  if (!strcmp(name, "TMPDIR") || !strcmp(name, "SQLITE_TMPDIR")) {
    static char tmpdir[WMW_PATH_MAX];
    if (!tmpdir[0])
      snprintf(tmpdir, sizeof(tmpdir), "%s/tmp", wmw_game_dir());
    return tmpdir;
  }
  return NULL;
}

int system_fake(const char *command) { (void)command; return -1; }

int open2_fake(const char *path, int flags) {
  return open_fake(path, flags); // reuse the Bionic->newlib flag conversion
}

long read_chk_fake(int fd, void *buf, size_t nbytes, size_t buflen) {
  if (nbytes > buflen) nbytes = buflen; // honour the fortify bound
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  long r = read(fd, buf, nbytes);
  clock_gettime(CLOCK_MONOTONIC, &b);
  double dt = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1.0e6;
  if (dt > 40.0)
    debugPrintf("io: SLOW read(fd=%d) %zu bytes took %.1f ms\n", fd, nbytes, dt);
  return r;
}

off_t lseek64_fake(int fd, off_t off, int whence) {
  return lseek(fd, off, whence); // off_t is already 64-bit on devkitA64
}

/* fcntl -- the command numbers DIFFER between bionic and newlib.
 *
 *   command    bionic/Linux arm64   newlib (devkitA64)
 *   F_GETLK            5                   7
 *   F_SETLK            6                   8
 *   F_SETLKW           7                   9
 *   F_GETOWN           9                   5
 *   F_SETOWN           8                   6
 *
 * Only F_DUPFD/F_GETFD/F_SETFD/F_GETFL/F_SETFL (0..4) actually agree. Switching
 * on the host's constants therefore mis-routes every lock call: SQLite's
 * F_SETLKW (7) lands on newlib's F_GETLK and its F_GETLK (5) lands on
 * F_GETOWN. Match on the Linux values explicitly.
 */
#define LINUX_F_DUPFD  0
#define LINUX_F_GETFD  1
#define LINUX_F_SETFD  2
#define LINUX_F_GETFL  3
#define LINUX_F_SETFL  4
#define LINUX_F_GETLK  5
#define LINUX_F_SETLK  6
#define LINUX_F_SETLKW 7

// bionic's struct flock on LP64; only l_type is read back by SQLite.
struct bionic_flock {
  int16_t l_type;
  int16_t l_whence;
  int64_t l_start;
  int64_t l_len;
  int32_t l_pid;
};
#define BIONIC_F_UNLCK 2   // Linux: F_RDLCK 0, F_WRLCK 1, F_UNLCK 2

// SQLite 3.7.8 maps a failed lock through sqliteErrorFromPosixError(), which
// returns SQLITE_IOERR_LOCK -- "disk I/O error" -- for any errno that is not
// EACCES/EAGAIN/EBUSY/EINTR/ENOLCK/ETIMEDOUT. So a locking problem presents as
// an I/O error, not as "database is locked". Log what is actually being asked
// for: l_type tells read-lock from write-lock, l_start tells which of SQLite's
// reserved byte ranges (PENDING=0x40000000, RESERVED=0x40000001,
// SHARED=0x40000002..) is involved, so the lock progression is readable.
static const char *flock_type_name(int t) {
  switch (t) { case 0: return "RDLCK"; case 1: return "WRLCK"; case 2: return "UNLCK"; }
  return "?";
}

int fcntl_fake(int fd, int cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  int ret = 0;
  switch (cmd) {
    case LINUX_F_DUPFD: ret = dup(fd); break;
    case LINUX_F_GETFL: ret = O_RDWR; break;
    case LINUX_F_GETFD:
    case LINUX_F_SETFD:
    case LINUX_F_SETFL: ret = 0; break;

    case LINUX_F_SETLK:
    case LINUX_F_SETLKW: {
      // Single process, no other writers: every lock request succeeds.
      struct bionic_flock *fl = va_arg(ap, struct bionic_flock *);
      if (fl && wmw_io_tracked(fd))
        wmw_io_trace("  db fcntl(fd=%d, %s, %s start=%lld len=%lld)\n",
                     fd, cmd == LINUX_F_SETLK ? "SETLK" : "SETLKW",
                     flock_type_name(fl->l_type),
                     (long long)fl->l_start, (long long)fl->l_len);
      ret = 0;
      break;
    }

    case LINUX_F_GETLK: {
      // "Is anyone holding a conflicting lock?" -- SQLite reads the answer back
      // out of the struct. Leaving it untouched means it sees the lock type it
      // just filled in and concludes the file is contended.
      struct bionic_flock *fl = va_arg(ap, struct bionic_flock *);
      if (fl) fl->l_type = BIONIC_F_UNLCK;
      if (wmw_io_tracked(fd))
        wmw_io_trace("  db fcntl(fd=%d, GETLK) -> UNLCK\n", fd);
      ret = 0;
      break;
    }

    default:
      if (wmw_io_tracked(fd))
        wmw_io_trace("  db fcntl(fd=%d, cmd=%d) [unhandled]\n", fd, cmd);
      ret = 0;
      break;
  }
  va_end(ap);
  return ret;
}

int getpriority_fake(int which, int who) { (void)which; (void)who; return 0; }
int setpriority_fake(int which, int who, int prio) { (void)which; (void)who; (void)prio; return 0; }

char *getcwd_fake(char *buf, size_t size) {
  // The engine receives all of its real paths through NativeInitDirs; cwd is
  // only used to build a few relative fallbacks, so root is a safe answer.
  if (!buf || size == 0) return NULL;
  buf[0] = '/';
  if (size > 1) buf[1] = '\0';
  else buf[0] = '\0';
  return buf;
}

int chdir_fake(const char *path) { (void)path; return 0; }

int mkdir_fake(const char *path, unsigned int mode) {
  if (mkdir(path, mode) == 0)
    return 0;
  if (errno == EEXIST)
    return 0; // already there is success for the engine's save-dir creation
  return -1;
}

int readlink_fake(const char *path, char *buf, size_t bufsiz) {
  (void)path; (void)buf; (void)bufsiz; errno = EINVAL; return -1;
}

int utime_fake(const char *path, const void *times) {
  (void)path; (void)times; return 0; // timestamps are cosmetic here
}

void sincos_fake(double x, double *s, double *c) {
  if (s) *s = sin(x);
  if (c) *c = cos(x);
}

char *strcasestr_fake(const char *haystack, const char *needle) {
  if (!*needle)
    return (char *)haystack;
  for (; *haystack; haystack++) {
    const char *h = haystack, *n = needle;
    while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
      h++; n++;
    }
    if (!*n)
      return (char *)haystack;
  }
  return NULL;
}

// --- toolchain-coverage insurance -----------------------------------------
// devkitA64's newlib does not reliably export these two. Shimming them here
// costs nothing and removes a link-time failure mode; because they are only
// referenced through the import table by their _fake names, they cannot
// collide with a newlib definition if one does exist.

int sched_yield_fake(void) {
  svcSleepThread(0); // yield the remainder of this timeslice
  return 0;
}

int isatty_fake(int fd) {
  (void)fd;
  return 0; // nothing here is a terminal
}

// The Switch has no user model, so newlib does not provide getuid(). The engine
// only needs a stable value -- it pairs it with getpwuid() when building a
// home-directory path, which our getpwuid_fake answers with "/" regardless.
// 1000 is an ordinary unprivileged uid; nothing here compares against 0.
unsigned int getuid_fake(void) { return 1000; }

// --- additional entry points pulled by libfmodex.so ------------------------
//
// libfmodex.so has 31 undefined symbols that libwmw.so does not, so they are
// absent from the table generated off the game library alone. so_resolve() runs
// against the same table for both modules, so they have to live here.
//
// The socket family belongs to FMOD's internet-streaming support, which this
// port never reaches: the game only ever calls createSound()/createStream() on
// local paths. Every call fails cleanly rather than blocking.

void operator_delete_fake(void *p) { free(p); }

void cxa_pure_virtual_fake(void) {
  debugPrintf("__cxa_pure_virtual called -- pure virtual method invoked\n");
}

void fd_set_chk_fake(int fd, void *set, unsigned long setsize) {
  (void)setsize;
  if (!set || fd < 0) return;
  unsigned long *bits = (unsigned long *)set;
  bits[fd / (8 * sizeof(unsigned long))] |= 1UL << (fd % (8 * sizeof(unsigned long)));
}

int  accept_fake(int fd, void *addr, unsigned int *len) { (void)fd; (void)addr; (void)len; return -1; }
int  bind_fake(int fd, const void *addr, unsigned int len) { (void)fd; (void)addr; (void)len; return -1; }
int  listen_fake(int fd, int backlog) { (void)fd; (void)backlog; return -1; }
long recv_fake(int fd, void *buf, size_t len, int flags) { (void)fd; (void)buf; (void)len; (void)flags; return -1; }
long send_fake(int fd, const void *buf, size_t len, int flags) { (void)fd; (void)buf; (void)len; (void)flags; return -1; }
int  select_fake(int n, void *r, void *w, void *e, void *timeout) {
  (void)n; (void)r; (void)w; (void)e; (void)timeout;
  return 0; // nothing ready, ever
}
void *gethostbyname_fake(const char *name) { (void)name; return NULL; }
unsigned int inet_addr_fake(const char *cp) { (void)cp; return 0xFFFFFFFFu; } // INADDR_NONE

// --- path-mapped file operations ------------------------------------------

// Paths SQLite probes: the database, its journal, and any temp directory it is
// considering. Everything else (thousands of asset lookups) is uninteresting.
static int is_db_path(const char *p) {
  return p && (strstr(p, "water") || strstr(p, "-journal") ||
               strstr(p, "etilqs") || strstr(p, "/tmp"));
}

int access_fake(const char *path, int mode) {
  char pbuf[WMW_PATH_MAX];
  const char *rp = wmw_resolve(path, pbuf, sizeof(pbuf));

  // SQLite probes candidate temp directories with access(dir, R_OK|W_OK|X_OK).
  // The execute bit is meaningless on FAT and newlib may refuse it, so answer
  // for directories directly: if it exists and is one, it is usable.
  // A file we currently hold open cannot be stat()ed or access()ed by path on
  // Horizon (see wmw_io_fd_for_path). We know it exists -- we are holding it.
  if (wmw_io_fd_for_path(rp) >= 0)
    return 0;

  wmw_file_lock();
  struct stat st;
  const int isdir = (stat(rp, &st) == 0 && S_ISDIR(st.st_mode));
  const int r = isdir ? 0 : access(rp, mode);
  wmw_file_unlock();
  if (is_db_path(rp))
    wmw_io_trace("  db access(%s, %d) -> %d%s\n", rp, mode, r, isdir ? " [dir]" : "");
  return r;
}

int remove_fake(const char *path) {
  char pbuf[WMW_PATH_MAX];
  const char *rp = wmw_resolve(path, pbuf, sizeof(pbuf));
  wmw_file_lock();
  const int r = remove(rp);
  wmw_file_unlock();
  if (r < 0 && errno != ENOENT)
    debugPrintf("io: remove(%s) FAILED errno=%d (%s)\n", rp, errno, strerror(errno));
  return r;
}

// SQLite deletes its rollback journal at the end of every write transaction.
// A failed delete becomes the same generic "disk I/O error", so name it.
int unlink_fake(const char *path) {
  char pbuf[WMW_PATH_MAX];
  const char *rp = wmw_resolve(path, pbuf, sizeof(pbuf));
  wmw_file_lock();
  const int r = unlink(rp);
  wmw_file_unlock();
  if (is_db_path(rp))
    wmw_io_trace("  db unlink(%s) -> %d (errno %d)\n", rp, r, r < 0 ? errno : 0);
  if (r < 0 && errno != ENOENT)
    debugPrintf("io: unlink(%s) FAILED errno=%d (%s)\n", rp, errno, strerror(errno));
  return r;
}

// --- raw file I/O -----------------------------------------------------------
//
// The engine bundles SQLite, which drives the database through open/read/write/
// lseek/fstat/ftruncate/fsync/unlink rather than stdio. When any of them fails
// SQLite surfaces a single generic message -- "disk I/O error" -- with no
// indication of which call broke. These wrappers are pass-throughs that log the
// failing call and errno once each, so a bad run identifies itself.

// --- descriptor tracking ---------------------------------------------------

static int s_tracked[16];
static int s_tracked_n;

// Per-descriptor record of the path it was opened from. Needed for two things:
// synthesising inode numbers, and -- more importantly -- answering path-based
// queries about files that are currently open. See wmw_io_fd_for_path().
static struct { int fd; uint64_t ino; char path[192]; } s_ino[32];
static int s_ino_n;

uint64_t wmw_io_fake_ino_path(const char *path) {
  // FNV-1a over the path; any stable, well-distributed value will do. Bit 63 is
  // set so a synthetic inode can never collide with a real small one.
  uint64_t h = 1469598103934665603ULL;
  for (const unsigned char *p = (const unsigned char *)path; p && *p; p++) {
    h ^= *p;
    h *= 1099511628211ULL;
  }
  return (h | (1ULL << 63));
}

void wmw_io_note_path(int fd, const char *path) {
  const uint64_t ino = wmw_io_fake_ino_path(path);
  for (int i = 0; i < s_ino_n; i++) {
    if (s_ino[i].fd == fd) {
      s_ino[i].ino = ino;
      snprintf(s_ino[i].path, sizeof(s_ino[i].path), "%s", path ? path : "");
      return;
    }
  }
  if (s_ino_n < (int)(sizeof(s_ino) / sizeof(*s_ino))) {
    s_ino[s_ino_n].fd = fd;
    s_ino[s_ino_n].ino = ino;
    snprintf(s_ino[s_ino_n].path, sizeof(s_ino[s_ino_n].path), "%s", path ? path : "");
    s_ino_n++;
  }
}

/* Return an open descriptor for `path`, or -1.
 *
 * This exists because of a genuine difference between Horizon and Linux.
 * devkitPro implements stat()/access() by OPENING the file to query it, and
 * Horizon refuses to open a file that is already open for write. So on Switch
 * a path-based query about a file you currently hold open FAILS, where on
 * Linux or Android it succeeds.
 *
 * SQLite hits this squarely: having taken a RESERVED lock on water.db it
 * checks the database still exists, the stat fails, and 3.7.8 turns that into
 * SQLITE_IOERR -- reported as the entirely uninformative "disk I/O error".
 *
 * Answering from the descriptor we already hold restores Linux semantics. */
int wmw_io_fd_for_path(const char *path) {
  if (!path || !*path) return -1;
  for (int i = 0; i < s_ino_n; i++)
    if (s_ino[i].path[0] && strcmp(s_ino[i].path, path) == 0)
      return s_ino[i].fd;
  return -1;
}

uint64_t wmw_io_fake_ino_fd(int fd) {
  for (int i = 0; i < s_ino_n; i++)
    if (s_ino[i].fd == fd) return s_ino[i].ino;
  return 0;
}

void wmw_io_track(int fd, int add) {
  if (!add) {
    for (int i = 0; i < s_ino_n; i++)
      if (s_ino[i].fd == fd) { s_ino[i] = s_ino[--s_ino_n]; break; }
  }
  if (add) {
    if (s_tracked_n < (int)(sizeof(s_tracked) / sizeof(*s_tracked)))
      s_tracked[s_tracked_n++] = fd;
    return;
  }
  for (int i = 0; i < s_tracked_n; i++)
    if (s_tracked[i] == fd) { s_tracked[i] = s_tracked[--s_tracked_n]; return; }
}

int wmw_io_tracked(int fd) {
  for (int i = 0; i < s_tracked_n; i++)
    if (s_tracked[i] == fd) return 1;
  return 0;
}

void wmw_io_trace(const char *fmt, ...) {
  static int budget = 400;   // enough to cover boot plus a few transactions
  if (budget <= 0) return;
  budget--;
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  debugPrintf("%s", buf);
}

static void io_fail(const char *what, int fd, long a, long b) {
  static int reported[8];
  static const char *names[8] = { "read","write","lseek","close","fsync","ftruncate",0,0 };
  for (int i = 0; i < 6; i++) {
    if (names[i] && strcmp(names[i], what) == 0) {
      if (reported[i]) return;
      reported[i] = 1;
      break;
    }
  }
  debugPrintf("io: %s(fd=%d, %ld, %ld) FAILED errno=%d (%s)\n",
              what, fd, a, b, errno, strerror(errno));
}

long ssize_read_fake(int fd, void *buf, size_t n) {
  long r = (long)read(fd, buf, n);

  /* POSIX guarantees that a read starting at or past end-of-file returns 0.
   * Horizon does not: it fails the read with -1/EIO instead.
   *
   * This matters enormously to SQLite. unixRead() reads:
   *
   *     got = seekAndRead(...);
   *     if      (got == amt) return SQLITE_OK;
   *     else if (got <  0)   return SQLITE_IOERR_READ;      <- "disk I/O error"
   *     else               { memset(tail, 0); return SQLITE_IOERR_SHORT_READ; }
   *
   * Probing past the end of the rollback journal to see whether another record
   * follows is an ordinary, expected operation -- on Linux it returns 0, SQLite
   * zero-fills, notes the journal is finished and commits. On Switch the same
   * probe returns -1, which SQLite can only interpret as the disk having failed
   * underneath it, so it aborts the transaction:
   *
   *     db lseek(fd=7, 9216, 0) -> 9216      (journal is 8720 bytes)
   *     db read(fd=7, 8) -> -1               errno 5
   *     [Walaber] Database error: disk I/O error
   *
   * Restore the POSIX behaviour: at or past EOF, report EOF.
   */
  if (r < 0) {
    const off_t pos = lseek(fd, 0, SEEK_CUR);
    struct stat st;
    if (pos >= 0 && fstat(fd, &st) == 0) {
      if (pos >= st.st_size) {
        r = 0;                                  // clean EOF
      } else if ((off_t)(pos + (off_t)n) > st.st_size) {
        // Straddles the end: return just the bytes that exist, as POSIX does.
        const size_t avail = (size_t)(st.st_size - pos);
        r = (long)read(fd, buf, avail);
      }
    }
  }

  if (wmw_io_tracked(fd))
    wmw_io_trace("  db read(fd=%d, %ld) -> %ld\n", fd, (long)n, r);

  if (r < 0) {
    io_fail("read", fd, (long)n, 0);
  } else if ((size_t)r != n) {
    // A short read is not a syscall failure, but SQLite treats it as
    // SQLITE_IOERR_SHORT_READ -- whose message is, unhelpfully, the same
    // generic "disk I/O error". Worth naming separately.
    static int shorts;
    if (shorts < 6) {
      shorts++;
      debugPrintf("io: SHORT read(fd=%d) asked %ld got %ld (offset %ld)\n",
                  fd, (long)n, r, (long)lseek(fd, 0, SEEK_CUR));
    }
  }
  return r;
}

long ssize_write_fake(int fd, const void *buf, size_t n) {
  const long r = (long)write(fd, buf, n);
  if (wmw_io_tracked(fd))
    wmw_io_trace("  db write(fd=%d, %ld) -> %ld\n", fd, (long)n, r);
  if (r < 0 || (size_t)r != n) io_fail("write", fd, (long)n, r);
  return r;
}

long lseek_fake2(int fd, long off, int whence) {
  const long r = (long)lseek(fd, (off_t)off, whence);
  if (wmw_io_tracked(fd))
    wmw_io_trace("  db lseek(fd=%d, %ld, %d) -> %ld\n", fd, off, whence, r);
  if (r < 0) io_fail("lseek", fd, off, whence);
  return r;
}

int close_fake(int fd) {
  if (wmw_io_tracked(fd)) {
    wmw_io_trace("  db close(fd=%d)\n", fd);
    wmw_io_track(fd, 0);
  }
  // Releasing a handle mutates the same table open() scans. See libc_shim.c.
  wmw_file_lock();
  const int r = close(fd);
  wmw_file_unlock();
  if (r < 0) io_fail("close", fd, 0, 0);
  return r;
}

// The Switch has no write-back cache we can flush per-file; report success.
int fsync_fake(int fd) { (void)fd; return 0; }

// This used to be a no-op returning success, which is a lie SQLite believes:
// it truncates rollback journals and shrinks the database through this call,
// and silently not doing so leaves stale bytes where it expects none.
int ftruncate_fake(int fd, long len) {
  const int r = ftruncate(fd, (off_t)len);
  if (wmw_io_tracked(fd))
    wmw_io_trace("  db ftruncate(fd=%d, %ld) -> %d\n", fd, len, r);
  if (r < 0) io_fail("ftruncate", fd, len, 0);
  return r;
}
