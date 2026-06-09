#include "telnet.h"
#include "console.h"
#include "platform.h"
#include "fifo.h"
#include <WiFi.h>
#include "esp_attr.h"
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

// Telnet protocol bytes
#define T_IAC   255
#define T_DONT  254
#define T_DO    253
#define T_WONT  252
#define T_WILL  251
#define T_SB    250
#define T_SE    240
#define OPT_BINARY    0
#define OPT_ECHO      1
#define OPT_SGA       3
#define OPT_LINEMODE  34

static WiFiServer  g_server(23);
static WiFiClient  g_client;
static bool        g_enabled = false;
static bool        g_started = false;
static uint16_t    g_port = 23;
static char        g_client_ip[20] = {0};

// 8 KB output FIFO (KL11 push, telnet_poll drain) and 8 KB input FIFO
// (drain_rx push, kl11::poll pop). Both storages live in PSRAM via
// EXT_RAM_BSS_ATTR. SPSC on core 1 - producer and consumer for each
// FIFO both run inside loop(), so plain volatile head/tail is enough.
#define VPDP_TELNET_FIFO_BYTES 8192   // must be power of two
EXT_RAM_BSS_ATTR static uint8_t telnet_out_storage[VPDP_TELNET_FIFO_BYTES];
EXT_RAM_BSS_ATTR static uint8_t telnet_in_storage[VPDP_TELNET_FIFO_BYTES];
static Fifo g_telnet_out;
static Fifo g_telnet_in;
static bool g_fifos_inited = false;

static void ensure_fifos_inited() {
  if (g_fifos_inited) return;
  g_telnet_out.init(telnet_out_storage, VPDP_TELNET_FIFO_BYTES);
  g_telnet_in.init(telnet_in_storage,  VPDP_TELNET_FIFO_BYTES);
  g_fifos_inited = true;
}

void telnet_begin(uint16_t port, bool enabled) {
  ensure_fifos_inited();
  g_enabled = enabled;
  g_port    = port;
  if (!enabled) { LOG("telnet: disabled in config"); return; }
  g_server = WiFiServer(port);
  g_server.begin();
  g_server.setNoDelay(true);
  g_started = true;
  LOG("telnet: listening on port %u", port);
}

static void send_iac(uint8_t verb, uint8_t opt) {
  uint8_t b[3] = { T_IAC, verb, opt };
  g_client.write(b, 3);
}

static void on_connect() {
  IPAddress ip = g_client.remoteIP();
  strncpy(g_client_ip, ip.toString().c_str(), sizeof(g_client_ip) - 1);
  g_client_ip[sizeof(g_client_ip) - 1] = 0;
  LOG("telnet: client connected from %s", g_client_ip);
  // Put the client in character-at-a-time mode.
  send_iac(T_WILL, OPT_ECHO);
  send_iac(T_WILL, OPT_SGA);
  send_iac(T_WONT, OPT_LINEMODE);
  send_iac(T_DO,   OPT_BINARY);
  g_telnet_out.clear();     // drop any stale output queued before connect
}

static void drain_rx() {
  while (g_client.available()) {
    int ch = g_client.read();
    if (ch < 0) break;
    uint8_t c = (uint8_t)ch;

    if (c == T_IAC) {                       // skip telnet command
      int verb = g_client.read();
      if (verb == T_SB) {                   // sub-negotiation: read to IAC SE
        int prev = -1, b;
        while ((b = g_client.read()) >= 0) {
          if (prev == T_IAC && b == T_SE) break;
          prev = b;
        }
      } else if (verb == T_WILL || verb == T_WONT ||
                 verb == T_DO   || verb == T_DONT) {
        g_client.read();                    // consume the option byte
      }
      continue;
    }
    if (c == 0x00) continue;                // telnet CR NUL -> drop NUL
    if (c == 0x0A) continue;                // CR LF -> drop LF (CR kept)
    g_telnet_in.push(c);                    // drop-newest if 8 KB full
  }
}

bool telnet_in_pop(uint8_t* out) {
  return g_telnet_in.pop(out);
}

void telnet_poll() {
  if (!g_started) return;

  // Accept a new connection.
  if (g_server.hasClient()) {
    WiFiClient nc = g_server.available();
    if (g_client && g_client.connected()) {
      nc.print("\r\nvpdp1140: console already in use\r\n");
      nc.stop();
    } else {
      g_client = nc;
      g_client.setNoDelay(true);
      on_connect();
    }
  }

  if (g_client && g_client.connected()) {
    drain_rx();
    // Flush queued console output in contiguous chunks from the FIFO.
    // peek() returns the largest run that doesn't wrap, so at most two
    // calls are needed to drain the ring. We stop early if write() can't
    // take everything we offered (socket buffer full); the rest stays
    // queued for the next telnet_poll().
    const uint8_t* p;
    size_t n;
    while ((n = g_telnet_out.peek(&p)) > 0) {
      size_t w = g_client.write(p, n);
      if (w == 0) break;
      g_telnet_out.consume(w);
      if (w < n) break;
    }
  } else if (g_client) {                    // client went away
    g_client.stop();
    g_client_ip[0] = 0;
    g_telnet_out.clear();
    LOG("telnet: client disconnected");
  } else {
    // No client connected at all: drop any queued output so it doesn't
    // accumulate stale bytes that a future client would see on connect.
    g_telnet_out.clear();
  }
}

void telnet_write(uint8_t c) {
  // If telnet is disabled in config the FIFO would never drain, so
  // drop bytes at the source. When enabled but no client is connected
  // we still buffer; telnet_poll() then clears the FIFO each iteration
  // so it never accumulates stale bytes a future client would see.
  // IAC bytes in the data stream are escaped by emitting them twice
  // (RFC 854). A full FIFO drops new bytes silently.
  if (!g_started) return;
  g_telnet_out.push(c);
  if (c == T_IAC) g_telnet_out.push(T_IAC);
}

bool        telnet_connected() { return g_client && g_client.connected(); }
bool        telnet_listening() { return g_started && WiFi.status() == WL_CONNECTED; }
const char* telnet_client_ip() { return g_client_ip; }
uint16_t    telnet_port()      { return g_port; }
bool        telnet_enabled()   { return g_enabled; }
