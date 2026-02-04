#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#define TIMEOUT 1000
#define MAX_GROUPS 10
#define MAX_CLIENTS 10
#define BUFFER_SIZE 256
#define EXIT_KEYWORD "EXIT"
#define SHOW_GROUPS "SHOW_ALL_GROUPS"
#define SHOW_CLIENTS "SHOW_ALL_CLIENTS"

typedef struct {
    int socket;
    char name[50];
    int report[MAX_CLIENTS];
    time_t last_active;
} Client;

typedef struct {
    int groupID;
    char groupName[50];
    int indexNumbers[MAX_CLIENTS];
} Group;

Client clients[MAX_CLIENTS];
Group groups[MAX_GROUPS];

/* ---------------- Utility Functions ---------------- */

void error(const char *msg) {
    perror(msg);
    exit(1);
}

void getTimeStamp(char *timestamp, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(timestamp, size, "[%H:%M]", t);
}

int get_client_index_by_socket(int sock) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].socket == sock) return i;
    return -1;
}

int find_client_socket(const char *name) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].socket && strcmp(clients[i].name, name) == 0)
            return clients[i].socket;
    return -1;
}

void remove_client_from_every_group(int index) {
    for (int i = 0; i < MAX_GROUPS; i++)
        for (int j = 0; j < MAX_CLIENTS; j++)
            if (groups[i].indexNumbers[j] == index)
                groups[i].indexNumbers[j] = -1;
}

void remove_client(int sock, fd_set *master) {
    int idx = get_client_index_by_socket(sock);
    if (idx == -1) return;

    printf("Client %s disconnected\n", clients[idx].name);
    remove_client_from_every_group(idx);

    close(sock);
    FD_CLR(sock, master);

    memset(&clients[idx], 0, sizeof(Client));
}

bool reportCheck(int ridx) {
    int active = 0, count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].socket) active++;

    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[ridx].report[i]) count++;

    int limit = (active <= 2) ? 3 : (active / 2 + 1);
    if (limit > 5) limit = 5;

    return count >= limit;
}

/* ---------------- Main ---------------- */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    int sockfd, fdmax, portno = atoi(argv[1]);
    struct sockaddr_in serv_addr, cli_addr;
    fd_set master, read_fds;
    socklen_t clilen;
    char buffer[BUFFER_SIZE];

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) error("socket");

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portno);

    if (bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
        error("bind");

    listen(sockfd, 5);
    printf("Server listening on port %d\n", portno);

    FD_ZERO(&master);
    FD_SET(sockfd, &master);
    FD_SET(STDIN_FILENO, &master);
    fdmax = sockfd;

    while (1) {
        read_fds = master;
        struct timeval tv = {1, 0};

        if (select(fdmax + 1, &read_fds, NULL, NULL, &tv) < 0)
            error("select");

        time_t now = time(NULL);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].socket &&
                difftime(now, clients[i].last_active) > TIMEOUT) {

                send(clients[i].socket,
                     ">> Kicked Out due to idleness...",
                     30, 0);
                remove_client(clients[i].socket, &master);
            }
        }

        for (int i = 0; i <= fdmax; i++) {
            if (!FD_ISSET(i, &read_fds)) continue;

            /* ---- New Connection ---- */
            if (i == sockfd) {
                clilen = sizeof(cli_addr);
                int newfd = accept(sockfd, (struct sockaddr*)&cli_addr, &clilen);
                if (newfd < 0) continue;

                read(newfd, buffer, BUFFER_SIZE - 1);
                buffer[strcspn(buffer, "\n")] = 0;

                bool duplicate = false;
                for (int k = 0; k < MAX_CLIENTS; k++)
                    if (clients[k].socket && !strcmp(clients[k].name, buffer))
                        duplicate = true;

                if (duplicate) {
                    send(newfd, ">> User with same name already exists.", 40, 0);
                    close(newfd);
                    continue;
                }

                for (int k = 0; k < MAX_CLIENTS; k++) {
                    if (!clients[k].socket) {
                        clients[k].socket = newfd;
                        strncpy(clients[k].name, buffer, 49);
                        clients[k].last_active = time(NULL);
                        FD_SET(newfd, &master);
                        if (newfd > fdmax) fdmax = newfd;
                        printf("Client %s connected\n", buffer);
                        break;
                    }
                }
            }

            /* ---- Client Message ---- */
            else if (i != STDIN_FILENO) {
                int n = recv(i, buffer, BUFFER_SIZE - 1, 0);
                if (n <= 0) {
                    remove_client(i, &master);
                    continue;
                }

                buffer[n] = '\0';
                int idx = get_client_index_by_socket(i);
                clients[idx].last_active = time(NULL);

                if (!strcmp(buffer, EXIT_KEYWORD)) {
                    remove_client(i, &master);
                    continue;
                }

                /* ---- Private Message ---- */
                if (buffer[0] == '@') {
                    char user[50], msg[BUFFER_SIZE];
                    sscanf(buffer, "@%49s %255[^\n]", user, msg);

                    int rsock = find_client_socket(user);
                    if (rsock != -1) {
                        char ts[30], out[BUFFER_SIZE + 80];
                        getTimeStamp(ts, sizeof(ts));
                        snprintf(out, sizeof(out), "%s%s : %s",
                                 ts, clients[idx].name, msg);
                        send(rsock, out, strlen(out), 0);
                    }
                }
            }
        }
    }
}
