/* TRON_PGO=gen: what clang's (Linux-built) profile runtime needs from glibc that the console's libc
 * names differently, and the periodic dump. Adapted from the God of War Ascension PS5 port. The title
 * never exits normally (it is killed), so a thread writes the profile every 30 s to
 * /app0/pgo/tron.profraw (written to a .tmp and renamed, so a kill leaves the last complete one). */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern "C" {
extern FILE* __stderrp;

int* __errno_location(void) { return &errno; }
int __fprintf_chk(FILE* f, int flag, const char* fmt, ...) {
  (void)flag;
  va_list ap;
  va_start(ap, fmt);
  const int r = vfprintf(f, fmt, ap);
  va_end(ap);
  return r;
}
int __snprintf_chk(char* s, size_t n, int flag, size_t slen, const char* fmt, ...) {
  (void)flag;
  (void)slen;
  va_list ap;
  va_start(ap, fmt);
  const int r = vsnprintf(s, n, fmt, ap);
  va_end(ap);
  return r;
}
int __vfprintf_chk(FILE* f, int flag, const char* fmt, va_list ap) {
  (void)flag;
  return vfprintf(f, fmt, ap);
}
int __vsnprintf_chk(char* s, size_t n, int flag, size_t slen, const char* fmt, va_list ap) {
  (void)flag;
  (void)slen;
  return vsnprintf(s, n, fmt, ap);
}
void* __memcpy_chk(void* d, const void* s, size_t n, size_t dn) { (void)dn; return memcpy(d, s, n); }
char* __strncpy_chk(char* d, const char* s, size_t n, size_t dn) { (void)dn; return strncpy(d, s, n); }
long __isoc23_strtol(const char* s, char** e, int b) { return strtol(s, e, b); }
int prctl(int option, ...) { (void)option; return -1; }
/* glibc struct utsname: six 65-byte fields; the runtime reads nodename */
int uname(void* u) {
  memset(u, 0, 6 * 65);
  strcpy((char*)u, "PS5");
  strcpy((char*)u + 65, "ps5");
  return 0;
}
FILE* tron_pgo_stderr; /* the runtime's `stderr` (renamed in its archive copy, game.cmake) */

extern uint64_t __llvm_profile_get_size_for_buffer(void);
extern int __llvm_profile_write_buffer(char* buf);
/* No binary ids (the runtime's reader expects another ELF layout). */
int __llvm_write_binary_ids(void* writer) {
  (void)writer;
  return 0;
}

static void dump(const char* path) {
  const uint64_t n = __llvm_profile_get_size_for_buffer();
  char* buf = static_cast<char*>(malloc(n));
  if (!buf) return;
  if (__llvm_profile_write_buffer(buf) == 0) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* fp = fopen(tmp, "wb");
    if (fp) {
      int ok = fwrite(buf, 1, n, fp) == n;
      fclose(fp);
      if (ok) rename(tmp, path);
    }
  }
  free(buf);
}

static void* pgo_thread(void* arg) {
  (void)arg;
  tron_pgo_stderr = __stderrp;
  mkdir("/app0/pgo", 0777);
  for (;;) {
    sleep(30);
    dump("/app0/pgo/tron.profraw");
  }
  return NULL;
}

__attribute__((constructor)) static void pgo_start(void) {
  pthread_t t;
  if (pthread_create(&t, NULL, pgo_thread, NULL) == 0) pthread_detach(t);
}

}  // extern "C"
