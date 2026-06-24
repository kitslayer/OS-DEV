/*
 * fanfs.h — userspace file materialization (fanotify-pre-content style, M1128).
 *
 * A userspace daemon registers itself as the handler for the /fan subtree; when
 * any process READS /fan/<name>, the kernel parks the reader, hands the name to
 * the daemon, and the daemon generates ("materializes") the file's bytes and
 * provides them back — the read then returns that content. It's a userspace
 * filesystem / on-demand fetch, built on the same park/wait/reply rendezvous as
 * the syscall supervisor (M1124) but aimed at the VFS instead of the syscall
 * table. One request in flight at a time; the rendezvous runs interrupts-off
 * (the syscalls are interrupt gates) so there is no lost wakeup.
 */
#pragma once

long fanfs_read(const char *name, void *buf, unsigned long max);  /* reader: /fan/<name> -> daemon-provided bytes (blocks) */
long fanfs_serve(void);                                           /* daemon: become the /fan handler; 0/-1 */
long fanfs_wait(char *namebuf, int max);                          /* daemon: block until a read request; fills the name; len/-1 */
long fanfs_provide(const void *content, unsigned long len);       /* daemon: hand bytes back to the blocked reader; len/-1 */
