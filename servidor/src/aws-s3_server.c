#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#define PORT 9000
#define BUFFER_SIZE 1024

static int crear_socket() {
    // IPV4 TCP, para UDP es SOCK_DGRAM.
    // la media
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("Socket"); exit(1); }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(PORT),
    };

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }

    if (listen(sock, 1) < 0) {
        perror("listen"); exit(1);
    }

    return sock;
}

int main( int argc, char* argv[] ) {
    int server = crear_socket();
    printf("Escuchando en el puerto: %d \n", PORT);

    int client = accept(server, NULL, NULL);
    if (client < 0) { perror("accept"); exit(1); }
    printf("Cliente conectado\n");

    char buffer[BUFFER_SIZE];
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytesRead = read(client, buffer, BUFFER_SIZE - 1);
        if (bytesRead <= 0) {
            printf("Cliente desconectado\n");
            break;
        }
        printf("Cliente dice: %s\n", buffer);

        if (strncmp(buffer, "EXIT", 4) == 0) break;

        const char* resp = "OK\n";
        send(client, resp, strlen(resp), 0);
    }
}