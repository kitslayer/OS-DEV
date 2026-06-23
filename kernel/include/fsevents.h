/*
 * fsevents.h — filesystem change notification (M1085).
 *
 * A small circular log of recent create/write/delete/mkdir/rename/symlink
 * events on the writable filesystems, recorded at the VFS mutation chokepoints
 * (kernel/vfs.c) and read back as /proc/fsevents — the inotify/FSEvents idiom,
 * for file managers, live-reload, and "did my file change" tooling. Read-only
 * observability over events the VFS already sees; no FS behaviour changes.
 */
#pragma once

void fsevents_record(char op, const char *path);   /* op: 'w' write 'd' delete 'm' mkdir 'r' rename 'l' symlink */
int  fsevents_format(char *buf, int max);           /* recent events oldest-first as text; bytes written */
