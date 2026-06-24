#pragma once
/*
 * unixsock.h — path-keyed AF_UNIX stream sockets (M1169).
 *
 * Local bidirectional byte-stream IPC addressed by a pathname — the Unix-domain
 * socket model (the transport behind D-Bus/X11/Wayland) adapted to this OS's
 * name-based, fd-table-free design. A server unix_listen()s on a path, clients
 * unix_connect() to it, the server unix_accept()s pending connections, and both
 * ends exchange bytes with unix_send/unix_recv over an integer ENDPOINT id
 * (conn<<1 | side) — a tiny socket handle that stands in for a file descriptor
 * and, because it indexes a global kernel table, survives fork(). recv blocks
 * (task_block) until data arrives or the peer closes (EOF).
 */

int  unix_listen(const char *path);                            /* -> listener id, or -1 */
int  unix_connect(const char *path);                           /* -> endpoint id (client side A), or -1 */
int  unix_accept(int lid);                                     /* -> endpoint id (server side B); blocks; -1 bad lid */
long unix_send(int ep, const void *buf, unsigned long len);    /* bytes written (may be short), -1 if peer/ep closed */
long unix_recv(int ep, void *buf, unsigned long max);          /* bytes read, 0 = peer closed & drained, -1 bad ep */
int  unix_close(int ep);                                       /* close one endpoint; wakes the peer; 0/-1 */
int  unix_wait_any(const int *eps, int n);                     /* poll/epoll: index of first readable ep (blocks once); -1 (M1170) */
int  unix_format(char *b, int max);                            /* /proc/unix table */
