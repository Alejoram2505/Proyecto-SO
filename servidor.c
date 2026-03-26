#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

#define MAX_CLIENTES 100

typedef struct {
    char username[32];
    char ip[INET_ADDRSTRLEN];
    char status[16];
    int sockfd;
    int activo;
    time_t ultimo_mensaje;
} Cliente;

typedef struct {
    int sockfd;
    struct sockaddr_in addr;
} ThreadArgs;

static Cliente lista[MAX_CLIENTES];
static pthread_mutex_t mutex_lista = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t servidor_activo = 1;
static int listen_fd = -1;

// Maneja señales SIGINT/SIGTERM para apagar de forma segura el servidor.
static void handle_signal(int signal_number) {
    (void) signal_number;
    servidor_activo = 0;
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
}

// Llena un paquete como si viniera del servidor (sender = SERVER).
static void fill_server_packet(ChatPacket *packet, unsigned char command, const char *target, const char *payload) {
    init_packet(packet, command);
    safe_copy(packet->sender, sizeof(packet->sender), "SERVER");
    safe_copy(packet->target, sizeof(packet->target), target);
    safe_copy(packet->payload, sizeof(packet->payload), payload);
    packet->payload_len = (uint16_t) strlen(packet->payload);
}

// Envía un paquete servidor pre-armado al socket cliente especificado.
static int send_server_packet(int sockfd, unsigned char command, const char *target, const char *payload) {
    ChatPacket packet;
    fill_server_packet(&packet, command, target, payload);
    return send_packet(sockfd, &packet);
}

// Construye la cadena de usuarios activos con estados para /list.
static int build_user_list(char *buffer, size_t buffer_size) {
    size_t used = 0;
    int first = 1;
    int i;

    buffer[0] = '\0';

    pthread_mutex_lock(&mutex_lista);
    for (i = 0; i < MAX_CLIENTES; i++) {
        int written;

        if (!lista[i].activo) {
            continue;
        }

        written = snprintf(buffer + used, buffer_size - used, "%s%s,%s",
                           first ? "" : ";",
                           lista[i].username,
                           lista[i].status);
        if (written < 0 || (size_t) written >= buffer_size - used) {
            pthread_mutex_unlock(&mutex_lista);
            return -1;
        }
        used += (size_t) written;
        first = 0;
    }
    pthread_mutex_unlock(&mutex_lista);

    return 0;
}

// Registra cliente nuevo en lista si no hay duplicados y hay espacio.
static int register_client(const char *username, const char *ip, int sockfd) {
    int free_index = -1;
    int i;

    pthread_mutex_lock(&mutex_lista);
    for (i = 0; i < MAX_CLIENTES; i++) {
        if (lista[i].activo) {
            if (strcmp(lista[i].username, username) == 0 || strcmp(lista[i].ip, ip) == 0) {
                pthread_mutex_unlock(&mutex_lista);
                return -1;
            }
        } else if (free_index == -1) {
            free_index = i;
        }
    }

    if (free_index == -1) {
        pthread_mutex_unlock(&mutex_lista);
        return -1;
    }

    memset(&lista[free_index], 0, sizeof(lista[free_index]));
    safe_copy(lista[free_index].username, sizeof(lista[free_index].username), username);
    safe_copy(lista[free_index].ip, sizeof(lista[free_index].ip), ip);
    safe_copy(lista[free_index].status, sizeof(lista[free_index].status), STATUS_ACTIVO);
    lista[free_index].sockfd = sockfd;
    lista[free_index].activo = 1;
    lista[free_index].ultimo_mensaje = time(NULL);
    pthread_mutex_unlock(&mutex_lista);

    return 0;
}

// Actualiza el timestamp de actividad y vuelve a ACTIVO si estaba INACTIVO.
static int update_client_activity(const char *username) {
    int i;

    pthread_mutex_lock(&mutex_lista);
    for (i = 0; i < MAX_CLIENTES; i++) {
        if (lista[i].activo && strcmp(lista[i].username, username) == 0) {
            lista[i].ultimo_mensaje = time(NULL);
            if (strcmp(lista[i].status, STATUS_INACTIVO) == 0) {
                safe_copy(lista[i].status, sizeof(lista[i].status), STATUS_ACTIVO);
            }
            pthread_mutex_unlock(&mutex_lista);
            return 0;
        }
    }
    pthread_mutex_unlock(&mutex_lista);

    return -1;
}

// Cambia el estado de un cliente específico y actualiza su último mensaje.
static int set_client_status(const char *username, const char *status) {
    int i;

    pthread_mutex_lock(&mutex_lista);
    for (i = 0; i < MAX_CLIENTES; i++) {
        if (lista[i].activo && strcmp(lista[i].username, username) == 0) {
            safe_copy(lista[i].status, sizeof(lista[i].status), status);
            lista[i].ultimo_mensaje = time(NULL);
            pthread_mutex_unlock(&mutex_lista);
            return 0;
        }
    }
    pthread_mutex_unlock(&mutex_lista);

    return -1;
}

// Obtiene IP y estado de un cliente por nombre (usado por comando /info).
static int get_client_info(const char *username, char *ip, size_t ip_size, char *status, size_t status_size) {
    int i;

    pthread_mutex_lock(&mutex_lista);
    for (i = 0; i < MAX_CLIENTES; i++) {
        if (lista[i].activo && strcmp(lista[i].username, username) == 0) {
            safe_copy(ip, ip_size, lista[i].ip);
            safe_copy(status, status_size, lista[i].status);
            pthread_mutex_unlock(&mutex_lista);
            return 0;
        }
    }
    pthread_mutex_unlock(&mutex_lista);

    return -1;
}

// Busca y retorna socket de cliente por nombre (o -1 si no existe).
static int get_client_socket(const char *username) {
    int i;
    int sockfd = -1;

    pthread_mutex_lock(&mutex_lista);
    for (i = 0; i < MAX_CLIENTES; i++) {
        if (lista[i].activo && strcmp(lista[i].username, username) == 0) {
            sockfd = lista[i].sockfd;
            break;
        }
    }
    pthread_mutex_unlock(&mutex_lista);

    return sockfd;
}

// Notifica a todos los clientes activos que un usuario se desconectó.
static void notify_disconnection(const char *username) {
    ChatPacket packet;
    int sockets[MAX_CLIENTES];
    int count = 0;
    int i;

    fill_server_packet(&packet, CMD_DISCONNECTED, "ALL", username);

    pthread_mutex_lock(&mutex_lista);
    for (i = 0; i < MAX_CLIENTES; i++) {
        if (lista[i].activo) {
            sockets[count++] = lista[i].sockfd;
        }
    }
    pthread_mutex_unlock(&mutex_lista);

    for (i = 0; i < count; i++) {
        send_packet(sockets[i], &packet);
    }
}

// Elimina al cliente de la lista y opcionalmente notifica al resto.
static void remove_client(const char *username, int notify) {
    int i;
    int found = 0;

    pthread_mutex_lock(&mutex_lista);
    for (i = 0; i < MAX_CLIENTES; i++) {
        if (lista[i].activo && strcmp(lista[i].username, username) == 0) {
            memset(&lista[i], 0, sizeof(lista[i]));
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&mutex_lista);

    if (found && notify) {
        notify_disconnection(username);
    }
}

// Envía mensaje de chat a todos los clientes conectados.
static void broadcast_message(const char *sender, const char *message) {
    ChatPacket packet;
    int sockets[MAX_CLIENTES];
    int count = 0;
    int i;

    init_packet(&packet, CMD_MSG);
    safe_copy(packet.sender, sizeof(packet.sender), sender);
    safe_copy(packet.target, sizeof(packet.target), "ALL");
    safe_copy(packet.payload, sizeof(packet.payload), message);
    packet.payload_len = (uint16_t) strlen(packet.payload);

    pthread_mutex_lock(&mutex_lista);
    for (i = 0; i < MAX_CLIENTES; i++) {
        if (lista[i].activo) {
            sockets[count++] = lista[i].sockfd;
        }
    }
    pthread_mutex_unlock(&mutex_lista);

    for (i = 0; i < count; i++) {
        send_packet(sockets[i], &packet);
    }
}

// Envía mensaje solo al cliente target; retorna -1 si no existe target.
static int send_direct_message(const char *sender, const char *target, const char *message) {
    ChatPacket packet;
    int target_fd;

    target_fd = get_client_socket(target);
    if (target_fd < 0) {
        return -1;
    }

    init_packet(&packet, CMD_MSG);
    safe_copy(packet.sender, sizeof(packet.sender), sender);
    safe_copy(packet.target, sizeof(packet.target), target);
    safe_copy(packet.payload, sizeof(packet.payload), message);
    packet.payload_len = (uint16_t) strlen(packet.payload);

    return send_packet(target_fd, &packet);
}

// Hilo que monitoriza inactividad y cambia estado a INACTIVO tras timeout.
static void *inactivity_thread(void *arg) {
    (void) arg;

    while (servidor_activo) {
        time_t now = time(NULL);
        int sockets[MAX_CLIENTES];
        char usernames[MAX_CLIENTES][32];
        int count = 0;
        int i;

        pthread_mutex_lock(&mutex_lista);
        for (i = 0; i < MAX_CLIENTES; i++) {
            if (!lista[i].activo) {
                continue;
            }

            if (strcmp(lista[i].status, STATUS_INACTIVO) != 0 &&
                difftime(now, lista[i].ultimo_mensaje) >= INACTIVITY_TIMEOUT) {
                safe_copy(lista[i].status, sizeof(lista[i].status), STATUS_INACTIVO);
                sockets[count] = lista[i].sockfd;
                safe_copy(usernames[count], sizeof(usernames[count]), lista[i].username);
                count++;
            }
        }
        pthread_mutex_unlock(&mutex_lista);

        for (i = 0; i < count; i++) {
            send_server_packet(sockets[i], CMD_MSG, usernames[i], "Tu status cambió a INACTIVE");
        }

        sleep(1);
    }

    return NULL;
}

// Hilo por cliente que procesa comandos recibidos del cliente conectado.
static void *client_thread(void *arg) {
    ThreadArgs *thread_args = (ThreadArgs *) arg;
    int sockfd = thread_args->sockfd;
    char client_ip[INET_ADDRSTRLEN];
    char username[32] = {0};
    ChatPacket packet;
    int registered = 0;
    int keep_running = 1;

    inet_ntop(AF_INET, &thread_args->addr.sin_addr, client_ip, sizeof(client_ip));
    free(thread_args);

    if (recv_packet(sockfd, &packet) != 1 || packet.command != CMD_REGISTER) {
        send_server_packet(sockfd, CMD_ERROR, "", "Debe registrarse primero");
        close(sockfd);
        return NULL;
    }

    if (packet.sender[0] == '\0' || packet.payload[0] == '\0') {
        send_server_packet(sockfd, CMD_ERROR, "", "Nombre de usuario inválido");
        close(sockfd);
        return NULL;
    }

    safe_copy(username, sizeof(username), packet.sender);
    if (register_client(username, client_ip, sockfd) != 0) {
        send_server_packet(sockfd, CMD_ERROR, username, "Usuario o IP ya existe");
        close(sockfd);
        return NULL;
    }

    registered = 1;
    update_client_activity(username);

    {
        char welcome[128];
        snprintf(welcome, sizeof(welcome), "Bienvenido %s", username);
        send_server_packet(sockfd, CMD_OK, username, welcome);
    }

    while (keep_running && servidor_activo) {
        int rc = recv_packet(sockfd, &packet);
        if (rc != 1) {
            break;
        }

        update_client_activity(username);

        switch (packet.command) {
            case CMD_BROADCAST:
                broadcast_message(username, packet.payload);
                break;

            case CMD_DIRECT:
                if (packet.target[0] == '\0') {
                    send_server_packet(sockfd, CMD_ERROR, username, "Destinatario inválido");
                } else if (send_direct_message(username, packet.target, packet.payload) != 0) {
                    send_server_packet(sockfd, CMD_ERROR, username, "Destinatario no conectado");
                }
                break;

            case CMD_LIST: {
                char user_list[sizeof(packet.payload)];
                if (build_user_list(user_list, sizeof(user_list)) != 0) {
                    send_server_packet(sockfd, CMD_ERROR, username, "No fue posible generar la lista");
                } else {
                    send_server_packet(sockfd, CMD_USER_LIST, username, user_list);
                }
                break;
            }

            case CMD_INFO: {
                char ip[INET_ADDRSTRLEN];
                char status[16];
                char payload[sizeof(packet.payload)];

                if (packet.target[0] == '\0') {
                    send_server_packet(sockfd, CMD_ERROR, username, "Usuario no especificado");
                } else if (get_client_info(packet.target, ip, sizeof(ip), status, sizeof(status)) != 0) {
                    send_server_packet(sockfd, CMD_ERROR, username, "Usuario no conectado");
                } else {
                    snprintf(payload, sizeof(payload), "%s,%s", ip, status);
                    send_server_packet(sockfd, CMD_USER_INFO, username, payload);
                }
                break;
            }

            case CMD_STATUS:
                if (!is_valid_status(packet.payload)) {
                    send_server_packet(sockfd, CMD_ERROR, username, "Status inválido");
                } else {
                    set_client_status(username, packet.payload);
                    send_server_packet(sockfd, CMD_OK, username, packet.payload);
                }
                break;

            case CMD_LOGOUT:
                send_server_packet(sockfd, CMD_OK, username, "Logout exitoso");
                keep_running = 0;
                break;

            default:
                send_server_packet(sockfd, CMD_ERROR, username, "Comando no soportado");
                break;
        }
    }

    if (registered) {
        remove_client(username, 1);
    }
    close(sockfd);
    return NULL;
}

// Función principal: arranca servidor TCP, acepta conexiones y crea hilos.
int main(int argc, char *argv[]) {
    struct sockaddr_in server_addr;
    pthread_t timeout_tid;

    if (argc != 2) {
        fprintf(stderr, "Uso: %s <puerto>\n", argv[0]);
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    {
        int opt = 1;
        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            perror("setsockopt");
            close(listen_fd);
            return 1;
        }
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons((uint16_t) atoi(argv[1]));

    if (bind(listen_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, MAX_CLIENTES) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    if (pthread_create(&timeout_tid, NULL, inactivity_thread, NULL) != 0) {
        perror("pthread_create");
        close(listen_fd);
        return 1;
    }

    printf("Servidor escuchando en el puerto %s\n", argv[1]);

    while (servidor_activo) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *) &client_addr, &client_len);

        if (client_fd < 0) {
            if (errno == EINTR && !servidor_activo) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        ThreadArgs *args = (ThreadArgs *) malloc(sizeof(ThreadArgs));
        pthread_t tid;

        if (args == NULL) {
            perror("malloc");
            close(client_fd);
            continue;
        }

        args->sockfd = client_fd;
        args->addr = client_addr;

        if (pthread_create(&tid, NULL, client_thread, args) != 0) {
            perror("pthread_create");
            close(client_fd);
            free(args);
            continue;
        }

        pthread_detach(tid);
    }

    servidor_activo = 0;
    pthread_join(timeout_tid, NULL);

    if (listen_fd >= 0) {
        close(listen_fd);
    }

    return 0;
}
