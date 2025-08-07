#include "diagnostic-broadcast.h"
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DIAG_DEST_IP "127.0.0.199"
#define DIAG_DEST_PORT 2287
#define DIAG_BUF_SIZE 2048

void diagnostic_broadcast_internal(const char *fmt, ...) {
  char json[DIAG_BUF_SIZE - 2]; // leave space for 0x02 and 0x03
  char message[DIAG_BUF_SIZE];
  va_list args;

  va_start(args, fmt);
  vsnprintf(json, sizeof(json), fmt, args);
  va_end(args);

  size_t json_len = strlen(json);
  message[0] = 0x02; // Start framing
  memcpy(message + 1, json, json_len);
  message[json_len + 1] = 0x03; // End framing

  // Create and send UDP packet
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
    return;

  struct sockaddr_in dest = {
      .sin_family = AF_INET,
      .sin_port = htons(DIAG_DEST_PORT),
  };
  inet_pton(AF_INET, DIAG_DEST_IP, &dest.sin_addr);

  sendto(sock, message, json_len + 2, 0, (struct sockaddr *)&dest,
         sizeof(dest));

  close(sock);
}
