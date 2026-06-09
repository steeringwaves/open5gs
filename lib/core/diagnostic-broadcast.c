#include "diagnostic-broadcast.h"
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DIAG_DEFAULT_DEST_IP "127.0.0.199"
#define DIAG_DEFAULT_DEST_PORT 2287
#define DIAG_BUF_SIZE 2048

/* Runtime config — overridden via diagnostic_broadcast_configure().
 * The defaults preserve upstream behaviour so an unconfigured daemon
 * keeps broadcasting to 127.0.0.199:2287. */
static struct {
    bool enabled;
    char address[64];
    int port;
} cfg = {
    .enabled = true,
    .address = DIAG_DEFAULT_DEST_IP,
    .port = DIAG_DEFAULT_DEST_PORT,
};

void diagnostic_broadcast_configure(bool enabled,
        const char *address, int port)
{
    cfg.enabled = enabled;
    if (address && *address) {
        strncpy(cfg.address, address, sizeof(cfg.address) - 1);
        cfg.address[sizeof(cfg.address) - 1] = '\0';
    }
    if (port > 0) cfg.port = port;
}

void diagnostic_broadcast_internal(const char *fmt, ...) {
  char json[DIAG_BUF_SIZE - 2]; // leave space for 0x02 and 0x03
  char message[DIAG_BUF_SIZE];
  va_list args;

  if (!cfg.enabled) return;

  va_start(args, fmt);
  vsnprintf(json, sizeof(json), fmt, args);
  va_end(args);

  size_t json_len = strlen(json);
  message[0] = 0x02; // Start framing
  memcpy(message + 1, json, json_len);
  message[json_len + 1] = 0x03; // End framing

  // Create and send UDP packet
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    return;
  }

  struct sockaddr_in dest = {
      .sin_family = AF_INET,
      .sin_port = htons(cfg.port),
  };
  inet_pton(AF_INET, cfg.address, &dest.sin_addr);

  sendto(sock, message, json_len + 2, 0, (struct sockaddr *)&dest,
         sizeof(dest));

  close(sock);
}
