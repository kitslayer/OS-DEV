# Milestone 29 — Networking from the shell (ping + DNS)

**Goal:** use the network from userspace — ping a host, and resolve a domain
name to an IP. Builds directly on M13 (e1000 NIC + ARP + ICMP).

## `ping` (`net_ping_gateway`)
Exposes the M13 ICMP echo: ARP-resolve the gateway, send three echo requests,
count the replies. A `SYS_ping` syscall + a shell **`ping`** command report
`gateway 10.0.2.2: 3/3 replies`.

## `resolve` — UDP + DNS (`dns_resolve`)
The new capability. A minimal **DNS over UDP** client:
1. ARP-resolve the resolver (SLIRP's 10.0.2.3).
2. Build a DNS A-record query — the header plus the **length-prefixed label**
   encoding of the hostname (`example.com` → `7"example" 3"com" 0`).
3. Wrap it in **UDP / IPv4 / Ethernet** and send.
4. Parse the response: skip the header + question, walk the answer records
   (handling the `0xC0` **name-compression** pointer), and pull the 4-byte IP
   out of the first type-A record.

A `SYS_resolve` syscall + a shell **`resolve <host>`** command print the IP.

## What we proved
```
osdev$ ping
gateway 10.0.2.2: 3/3 replies
osdev$ resolve example.com
example.com -> 172.66.147.243
```
A real domain resolved to a real address — the query left the machine over our
own e1000 + IP + UDP stack, and we parsed a real DNS reply.

## Note
Both syscalls `sti` first: they block on the timer for their receive timeouts,
and the int-gate had cleared the interrupt flag.

## Files
- `kernel/net.c` — `net_ping_gateway`, `dns_resolve` (UDP + DNS)
- `kernel/syscall.c`, `user/shell.c` — `ping` / `resolve`
