#include "common.h"
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// Maanejo de errores 

// Envía exactamente 'length' bytes a través de fd, intentando repetir si la llamada parciales.
int send_all(int fd, const void *buffer, size_t length) {
    const char *ptr = (const char *) buffer;
    size_t sent = 0;

    while (sent < length) {
        ssize_t rc = send(fd, ptr + sent, length - sent, 0);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            return -1;
        }
        sent += (size_t) rc;
    }

    return 0;
}

// Recibe exactamente 'length' bytes desde fd en buffer; retorna 1 si ok, 0 si desconexión, -1 si error.
int recv_all(int fd, void *buffer, size_t length) {
    char *ptr = (char *) buffer;
    size_t received = 0;

    while (received < length) {
        ssize_t rc = recv(fd, ptr + received, length - received, 0);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            return 0;
        }
        received += (size_t) rc;
    }

    return 1;
}

// Envia un paquete ChatPacket completo usando la función send_all.
int send_packet(int fd, const ChatPacket *packet) {
    return send_all(fd, packet, sizeof(*packet));
}

// Recibe un paquete ChatPacket completo usando recv_all.
int recv_packet(int fd, ChatPacket *packet) {
    return recv_all(fd, packet, sizeof(*packet));
}

// Inicializa un ChatPacket en cero y fija el comando.
void init_packet(ChatPacket *packet, unsigned char command) {
    memset(packet, 0, sizeof(*packet));
    packet->command = command;
}

// Copia cadena segura con terminación nula, evitando desbordamientos.
void safe_copy(char *dest, size_t dest_size, const char *src) {
    if (dest_size == 0) {
        return;
    }

    if (src == NULL) {
        dest[0] = '\0';
        return;
    }

    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

// Quita salto de línea y retorno de carro al final de una cadena.
void trim_newline(char *text) {
    size_t len;

    if (text == NULL) {
        return;
    }

    len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[len - 1] = '\0';
        len--;
    }
}

// Valida que el estado sea uno de los permitidos: ACTIVO, OCUPADO o INACTIVO.
int is_valid_status(const char *status) {
    return status != NULL &&
        (strcmp(status, STATUS_ACTIVO) == 0 ||
         strcmp(status, STATUS_OCUPADO) == 0 ||
         strcmp(status, STATUS_INACTIVO) == 0);
}
