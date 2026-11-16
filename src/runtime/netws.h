/*
 * netws.h - one WebSocket (ws:// or wss://), binary messages: netplay's path
 * over the Internet, through the relay (tools/netplay-relay, a Cloudflare
 * Worker). Android: the game's Java class NetWs (JNI, netws.c); macOS:
 * Network.framework (netws_apple.m); elsewhere none (lgws_open fails).
 * Thread-safe enough for netplay: one opener, one sender, one receiver.
 */
#ifndef LINDBERGH_NETWS_H
#define LINDBERGH_NETWS_H
#include <stddef.h>
#include <stdint.h>
int  lgws_open(const char *url);                    /* starts connecting; 0, or -1 when there is no WebSocket here */
int  lgws_state(void);                              /* 0 connecting, 1 open, -1 closed or failed */
int  lgws_send(const uint8_t *b, size_t n);         /* 1 sent */
int  lgws_recv(uint8_t *b, size_t cap);             /* a message's length, 0 none waiting */
void lgws_close(void);
#endif
