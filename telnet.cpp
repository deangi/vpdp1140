#include "telnet.h"
#include "console.h"
#include "platform.h"
#include <WiFi.h>

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

// Output is buffered so the CPU emulation never blocks on WiFi.
static uint8_t  g_tx[4096];
static uint32_t g_tx_len = 0;

void telnet_begin(uint16_t port, bool enabled) {
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
  g_tx_len = 0;          // drop any stale output
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
    console_key_push(c);
  }
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
    if (g_tx_len) {                         // flush queued console output
      g_client.write(g_tx, g_tx_len);
      g_tx_len = 0;
    }
  } else if (g_client) {                    // client went away
    g_client.stop();
    g_client_ip[0] = 0;
    g_tx_len = 0;
    LOG("telnet: client disconnected");
  }
}

void telnet_write(uint8_t c) {
  if (!g_started || !(g_client && g_client.connected())) return;
  // Escape IAC in the data stream; drop bytes if the buffer is full.
  uint32_t need = (c == T_IAC) ? 2 : 1;
  if (g_tx_len + need > sizeof(g_tx)) return;
  g_tx[g_tx_len++] = c;
  if (c == T_IAC) g_tx[g_tx_len++] = T_IAC;
}

bool        telnet_connected() { return g_client && g_client.connected(); }
const char* telnet_client_ip() { return g_client_ip; }
uint16_t    telnet_port()      { return g_port; }
bool        telnet_enabled()   { return g_enabled; }
