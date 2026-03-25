#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>

#include "protocolo.h"

int send_all(int fd, const void *buffer, size_t length);
int recv_all(int fd, void *buffer, size_t length);
int send_packet(int fd, const ChatPacket *packet);
int recv_packet(int fd, ChatPacket *packet);
void init_packet(ChatPacket *packet, unsigned char command);
void safe_copy(char *dest, size_t dest_size, const char *src);
void trim_newline(char *text);
int is_valid_status(const char *status);

#endif
