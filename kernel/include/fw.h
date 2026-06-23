/*
 * fw.h — a tiny packet filter ("ipfw-lite"): first-match rules on the RX and TX
 * paths. It hooks the single chokepoints nic_send (outbound) and nic_receive
 * (inbound), so no per-protocol cooperation is needed. Default policy is ALLOW
 * and a frame that matches no rule passes — so with no rules configured it is a
 * fast no-op, fully additive. Rules are configured by writing verbs to /proc/fw
 * and listed (with per-rule hit counts) by reading it.
 */
#pragma once
#include <stdint.h>

#define FW_IN  0
#define FW_OUT 1

/* Verdict for a raw Ethernet frame: 1 = allow, 0 = drop. `dir` is FW_IN/FW_OUT.
 * Non-IPv4 frames (ARP, etc.) always pass, so a rule set can't wedge ARP. */
int  fw_check(int dir, const void *frame, int len);

/* /proc/fw: read = the rule table + hit counts; write = a verb:
 *   "drop|allow  in|out|both  icmp|tcp|udp|any  [port]"   add a rule
 *   "flush"                                               clear all rules     */
int  fw_format(char *out, int max);
void fw_control(const char *cmd, int len);
