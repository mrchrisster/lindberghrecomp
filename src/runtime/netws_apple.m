/* netws_apple.m - netws.h on macOS: Network.framework's own WebSocket
 * (ws:// and wss://, the system's certificate checks), received messages
 * queued for lgws_recv */
#import <Foundation/Foundation.h>
#import <Network/Network.h>
#include "netws.h"
#include <stdio.h>

static nw_connection_t g_conn;
static dispatch_queue_t g_q;
static volatile int g_state = -1;
static NSMutableArray<NSData *> *g_inbox;
static NSLock *g_lock;

static void receive_next(nw_connection_t c)
{
    nw_connection_receive_message(c, ^(dispatch_data_t content, nw_content_context_t ctx, bool done, nw_error_t err) {
        (void)ctx; (void)done;
        if (err || c != g_conn) { if (c == g_conn) g_state = -1; return; }
        if (content) {
            NSMutableData *d = [NSMutableData data];
            dispatch_data_apply(content, ^bool(dispatch_data_t r, size_t o, const void *b, size_t n) { (void)r; (void)o; [d appendBytes:b length:n]; return true; });
            [g_lock lock]; if (g_inbox.count < 4096) [g_inbox addObject:d]; [g_lock unlock];
        }
        receive_next(c);
    });
}

int lgws_open(const char *url)
{
    lgws_close();
    if (!g_q) { g_q = dispatch_queue_create("netws", DISPATCH_QUEUE_SERIAL); g_inbox = [NSMutableArray array]; g_lock = [NSLock new]; }
    [g_lock lock]; [g_inbox removeAllObjects]; [g_lock unlock];
    nw_endpoint_t ep = nw_endpoint_create_url(url);
    if (!ep) return -1;
    const int tls = !strncmp(url, "wss:", 4);
    nw_parameters_t p = nw_parameters_create_secure_tcp(tls ? NW_PARAMETERS_DEFAULT_CONFIGURATION : NW_PARAMETERS_DISABLE_PROTOCOL,
                                                        NW_PARAMETERS_DEFAULT_CONFIGURATION);
    nw_protocol_options_t ws = nw_ws_create_options(nw_ws_version_13);
    nw_ws_options_set_auto_reply_ping(ws, true);
    nw_protocol_stack_prepend_application_protocol(nw_parameters_copy_default_protocol_stack(p), ws);
    nw_connection_t c = nw_connection_create(ep, p);
    g_conn = c; g_state = 0;
    nw_connection_set_queue(c, g_q);
    nw_connection_set_state_changed_handler(c, ^(nw_connection_state_t s, nw_error_t e) {
        if (c != g_conn) return;
        if (s == nw_connection_state_ready) { g_state = 1; fprintf(stderr, "[netws] open: %s\n", url); receive_next(c); }
        else if (s == nw_connection_state_failed || s == nw_connection_state_cancelled) {
            if (g_state != -1 && e) fprintf(stderr, "[netws] closed: error %d\n", nw_error_get_error_code(e));
            g_state = -1;
        }
    });
    nw_connection_start(c);
    return 0;
}
int lgws_state(void) { return g_state; }
int lgws_send(const uint8_t *b, size_t n)
{
    nw_connection_t c = g_conn;
    if (!c || g_state != 1) return 0;
    dispatch_data_t d = dispatch_data_create(b, n, g_q, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    nw_protocol_metadata_t m = nw_ws_create_metadata(nw_ws_opcode_binary);
    nw_content_context_t ctx = nw_content_context_create("send");
    nw_content_context_set_metadata_for_protocol(ctx, m);
    nw_connection_send(c, d, ctx, true, NW_CONNECTION_SEND_IDEMPOTENT_CONTENT);
    return 1;
}
int lgws_recv(uint8_t *b, size_t cap)
{
    if (!g_lock) return 0;
    [g_lock lock];
    NSData *d = g_inbox.count ? g_inbox[0] : nil;
    if (d) [g_inbox removeObjectAtIndex:0];
    [g_lock unlock];
    if (!d) return 0;
    size_t n = d.length < cap ? d.length : cap;
    memcpy(b, d.bytes, n);
    return (int)n;
}
void lgws_close(void)
{
    nw_connection_t c = g_conn;
    g_conn = nil; g_state = -1;
    if (c) nw_connection_cancel(c);
}
