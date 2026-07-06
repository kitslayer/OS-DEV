// mmapfile.c — file-backed mmap demo (M1136). Map an existing file into memory
// and read it by faulting pages in from disk on demand; show the mapping equals
// a normal read of the file, and that it's MAP_PRIVATE (writes stay in RAM, the
// file is untouched).
#include "ulib.h"

int main(void) {
    const char *path = "MOTD.TXT";                 // an existing boot-disk file

    char buf[1024];
    long n = sys_readfile(path, buf, sizeof buf - 1);
    if (n <= 0) { print("cannot read MOTD.TXT\n"); sys_sleep(20000); return 1; }

    char *m = (char *)sys_mmap_file(path, 4096, 0);   // shared=0: MAP_PRIVATE (see mmapsharedtest for MAP_SHARED, M1544)
    if (!m) { print("mmap_file failed\n"); sys_sleep(20000); return 1; }

    print("mmap'd MOTD.TXT; touching the mapping faults its page in from disk.\n");
    print("first line via the mapping -> \"");
    for (long i = 0; i < n && m[i] != '\n'; i++) { char c[2] = { m[i], 0 }; print(c); }   // first touch FAULTS
    print("\"\n");

    int match = 1;
    for (long i = 0; i < n; i++) if (m[i] != buf[i]) { match = 0; break; }
    print(match ? "  mapping == file contents (read via the fault path): YES\n"
                : "  mapping == file contents: NO\n");

    // MAP_PRIVATE: a write through the mapping must NOT reach the file on disk.
    char was = m[0];
    m[0] = '#';
    char buf2[1024];
    long n2 = sys_readfile(path, buf2, sizeof buf2 - 1);
    int priv = (n2 == n) && (buf2[0] == was) && (m[0] == '#');
    print(priv ? "  wrote '#' to the mapping; file on disk UNCHANGED (MAP_PRIVATE): YES\n"
               : "  MAP_PRIVATE check: NO\n");

    sys_setcolor(9); print("file-backed mmap: lazily paged from the file, private on write.\n"); sys_setcolor(0);
    sys_sleep(20000);
    return 0;
}
