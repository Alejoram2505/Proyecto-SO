#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common.h"

static volatile int cliente_activo = 1;
static pthread_mutex_t estado_mutex = PTHREAD_MUTEX_INITIALIZER;
static char estado_actual[16] = STATUS_ACTIVO;
static char usuario_actual[32];
static int sockfd_global = -1;

static void print_help(void) {
    printf("Comandos disponibles:\n");
    printf("  /broadcast <mensaje>\n");
    printf("  /msg <usuario> <mensaje>\n");
    printf("  /status <ACTIVE|BUSY|INACTIVE>\n");
    printf("  /list\n");
    printf("  /info <usuario>\n");
    printf("  /help\n");
    printf("  /exit\n");
}

static void set_status_local(const char *status) {
    pthread_mutex_lock(&estado_mutex);
    safe_copy(estado_actual, sizeof(estado_actual), status);
    pthread_mutex_unlock(&estado_mutex);
}

static void send_simple_command(unsigned char command, const char *sender, const char *target, const char *payload) {
    ChatPacket packet;

    init_packet(&packet, command);
    safe_copy(packet.sender, sizeof(packet.sender), sender);
    safe_copy(packet.target, sizeof(packet.target), target);
    safe_copy(packet.payload, sizeof(packet.payload), payload);
    packet.payload_len = (uint16_t) strlen(packet.payload);

    if (send_packet(sockfd_global, &packet) != 0) {
        perror("send");
        cliente_activo = 0;
    }
}

static void *receiver_thread(void *arg) {
    (void) arg;

    while (cliente_activo) {
        ChatPacket packet;
        int rc = recv_packet(sockfd_global, &packet);

        if (rc != 1) {
            printf("Conexión cerrada por el servidor.\n");
            cliente_activo = 0;
            break;
        }

        switch (packet.command) {
            case CMD_OK:
                printf("[OK] %s\n", packet.payload);
                if (is_valid_status(packet.payload)) {
                    set_status_local(packet.payload);
                }
                break;

            case CMD_ERROR:
                printf("[ERROR] %s\n", packet.payload);
                break;

            case CMD_MSG:
                printf("[%s -> %s] %s\n",
                       packet.sender,
                       packet.target[0] != '\0' ? packet.target : "ALL",
                       packet.payload);
                if (strcmp(packet.sender, "SERVER") == 0 &&
                    strstr(packet.payload, STATUS_INACTIVO) != NULL) {
                    set_status_local(STATUS_INACTIVO);
                }
                break;

            case CMD_USER_LIST:
                printf("[USUARIOS] %s\n", packet.payload);
                break;

            case CMD_USER_INFO:
                printf("[INFO] %s\n", packet.payload);
                break;

            case CMD_DISCONNECTED:
                printf("[DESCONECTADO] %s salió del chat.\n", packet.payload);
                break;

            default:
                printf("[AVISO] Comando desconocido del servidor: %u\n", packet.command);
                break;
        }
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    struct sockaddr_in server_addr;
    pthread_t receiver_tid;
    char line[1200];

    if (argc != 4) {
        fprintf(stderr, "Uso: %s <username> <IP_servidor> <puerto>\n", argv[0]);
        return 1;
    }

    safe_copy(usuario_actual, sizeof(usuario_actual), argv[1]);

    sockfd_global = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_global < 0) {
        perror("socket");
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((uint16_t) atoi(argv[3]));

    if (inet_pton(AF_INET, argv[2], &server_addr.sin_addr) <= 0) {
        fprintf(stderr, "IP de servidor inválida.\n");
        close(sockfd_global);
        return 1;
    }

    if (connect(sockfd_global, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sockfd_global);
        return 1;
    }

    send_simple_command(CMD_REGISTER, usuario_actual, "", usuario_actual);

    if (pthread_create(&receiver_tid, NULL, receiver_thread, NULL) != 0) {
        perror("pthread_create");
        close(sockfd_global);
        return 1;
    }

    print_help();

    while (cliente_activo && fgets(line, sizeof(line), stdin) != NULL) {
        char *arg1;
        char *arg2;

        trim_newline(line);
        if (line[0] == '\0') {
            continue;
        }

        if (strcmp(line, "/help") == 0) {
            print_help();
            continue;
        }

        if (strcmp(line, "/list") == 0) {
            send_simple_command(CMD_LIST, usuario_actual, "", "");
            continue;
        }

        if (strcmp(line, "/exit") == 0) {
            send_simple_command(CMD_LOGOUT, usuario_actual, "", "");
            cliente_activo = 0;
            break;
        }

        if (strncmp(line, "/broadcast ", 11) == 0) {
            send_simple_command(CMD_BROADCAST, usuario_actual, "", line + 11);
            continue;
        }

        if (strncmp(line, "/status ", 8) == 0) {
            if (!is_valid_status(line + 8)) {
                printf("Status inválido. Use ACTIVE, BUSY o INACTIVE.\n");
            } else {
                send_simple_command(CMD_STATUS, usuario_actual, "", line + 8);
            }
            continue;
        }

        if (strncmp(line, "/info ", 6) == 0) {
            send_simple_command(CMD_INFO, usuario_actual, line + 6, "");
            continue;
        }

        if (strncmp(line, "/msg ", 5) == 0) {
            arg1 = line + 5;
            arg2 = strchr(arg1, ' ');
            if (arg2 == NULL) {
                printf("Uso: /msg <usuario> <mensaje>\n");
                continue;
            }
            *arg2 = '\0';
            arg2++;
            if (*arg2 == '\0') {
                printf("Uso: /msg <usuario> <mensaje>\n");
                continue;
            }
            send_simple_command(CMD_DIRECT, usuario_actual, arg1, arg2);
            continue;
        }

        printf("Comando no reconocido. Use /help.\n");
    }

    cliente_activo = 0;
    shutdown(sockfd_global, SHUT_RDWR);
    pthread_join(receiver_tid, NULL);
    close(sockfd_global);
    return 0;
}
