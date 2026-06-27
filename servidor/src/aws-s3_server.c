#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../include/bucket_handler.h"

#define PORT    9000
#define TAM_BUF 4096

/*
 * copiarEntreLocalBuckets – copia un objeto entre dos buckets locales
 * usando un pipe anónimo. Evita cargar el archivo completo en memoria:
 * descargarArchivoStream escribe en el extremo de escritura del pipe,
 * y luego subirArchivoStream lee desde el extremo de lectura.
 *
 * Nota: pipe tiene un buffer del kernel (~64 KB en Linux). Para los
 * tamaños de archivo razonables de este proyecto es suficiente.
 * Si se necesitaran archivos mayores, la alternativa sin librerías
 * externas sería un archivo temporal en /tmp.
 */
static int copiarEntreLocalBuckets(const char* bOrig, const char* cOrig,
                                   const char* bDest, const char* cDest) {
    int fds[2];
    if (pipe(fds) < 0) return BUCKET_ERROR;

    uint32_t tamano = 0;
    int res = descargarArchivoStream(bOrig, cOrig, fds[1], &tamano);
    close(fds[1]);  /* señalar EOF al extremo de lectura */

    if (res != BUCKET_OK) { close(fds[0]); return res; }

    res = subirArchivoStream(bDest, cDest, fds[0], tamano);
    close(fds[0]);
    return res;
}

static int crearSocket(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in direccion = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(PORT),
    };

    if (bind(sock, (struct sockaddr*)&direccion, sizeof(direccion)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(sock, 1) < 0) { perror("listen"); exit(1); }
    return sock;
}

static int leerLinea(int fd, char* buf, size_t tam) {
    size_t r = 0;
    char c;
    while (r < tam - 1 && read(fd, &c, 1) > 0) {
        if (c == '\n') break;
        if (c != '\r') buf[r++] = c;
    }
    buf[r] = '\0';
    return (int)r;
}

static int enviarRespuesta(int fd, const char* msg) {
    size_t len = strlen(msg);
    size_t enviado = 0;
    while (enviado < len) {
        int r = (int)send(fd, msg + enviado, len - enviado, 0);
        if (r <= 0) return -1;
        enviado += (size_t)r;
    }
    return 0;
}

int main(void) {
    int servidor = crearSocket();
    printf("Escuchando en puerto %d\n", PORT);
    mkdir("buckets", 0755);

    while (1) {
        int cliente = accept(servidor, NULL, NULL);
        if (cliente < 0) { perror("accept"); continue; }
        printf("Cliente conectado\n");

        char linea[TAM_BUF];
        while (leerLinea(cliente, linea, sizeof(linea)) > 0) {
            char cmd[16] = {0};
            char arg1[256] = {0}, arg2[256] = {0};
            char arg3[256] = {0}, arg4[256] = {0};

            int n = sscanf(linea, "%15s %255s %255s %255s %255s",
                           cmd, arg1, arg2, arg3, arg4);

            if (n < 1) continue;

            if (strcmp(cmd, "EXIT") == 0) break;

            if (strcmp(cmd, "CR") == 0) {
                int res = crearBucket(arg1);
                if (res == BUCKET_OK) enviarRespuesta(cliente, "OK\n");
                else if (res == BUCKET_YA_EXISTE) enviarRespuesta(cliente, "ERROR: Bucket ya existe\n");
                else enviarRespuesta(cliente, "ERROR: No se pudo crear bucket\n");
            }
            else if (strcmp(cmd, "RB") == 0) {
                int forzar = (n >= 3 && strcmp(arg2, "FORCE") == 0) ? 1 : 0;
                int res = eliminarBucket(arg1, forzar);
                if (res == BUCKET_OK) enviarRespuesta(cliente, "OK\n");
                else if (res == BUCKET_NO_ENCONTRADO) enviarRespuesta(cliente, "ERROR: Bucket no encontrado\n");
                else enviarRespuesta(cliente, "ERROR: Bucket no esta vacio (use FORCE)\n");
            }
            else if (strcmp(cmd, "LS") == 0) {
                char salida[TAM_BUF * 4];
                size_t tamSal = sizeof(salida);
                salida[0] = '\0';

                const char* bucket  = (n >= 2 && arg1[0]) ? arg1 : NULL;
                const char* prefijo = (n >= 3 && arg2[0]) ? arg2 : NULL;

                int res = listarContenido(bucket, prefijo, salida, &tamSal);
                if (res == BUCKET_OK)
                    enviarRespuesta(cliente, salida);
                else
                    enviarRespuesta(cliente, "ERROR: Bucket no encontrado\n");
            }
            else if (strcmp(cmd, "PUT") == 0) {
                /* Protocolo: PUT <bucket> <clave> <tamano>\n<bytes...>
                 * Lee directamente desde el socket sin pasar por memoria. */
                if (n < 4) { enviarRespuesta(cliente, "ERROR: Argumentos insuficientes\n"); continue; }

                uint32_t tamDatos = (uint32_t)atol(arg3);
                int res = subirArchivoStream(arg1, arg2, cliente, tamDatos);

                if (res == BUCKET_OK)                 enviarRespuesta(cliente, "OK\n");
                else if (res == BUCKET_NO_ENCONTRADO) enviarRespuesta(cliente, "ERROR: Bucket no encontrado\n");
                else if (res == BUCKET_LLENO)         enviarRespuesta(cliente, "ERROR: Bucket lleno\n");
                else                                  enviarRespuesta(cliente, "ERROR: No se pudo subir archivo\n");
            }
            else if (strcmp(cmd, "GET") == 0) {
                /* Protocolo: GET <bucket> <clave>\n  ->  OK <tam>\n<bytes...>
                 * Primero consultamos el tamaño para poder enviar el encabezado
                 * antes de iniciar el stream de datos al socket. */
                uint32_t tamDatos = 0;
                int res = obtenerTamanoArchivo(arg1, arg2, &tamDatos);
                if (res != BUCKET_OK) {
                    enviarRespuesta(cliente, "ERROR: Archivo no encontrado\n");
                    continue;
                }

                char encabezado[64];
                snprintf(encabezado, sizeof(encabezado), "OK %u\n", tamDatos);
                enviarRespuesta(cliente, encabezado);

                /* Stream directo al socket: sin malloc, sin buffer intermedio */
                uint32_t enviado = 0;
                descargarArchivoStream(arg1, arg2, cliente, &enviado);
            }
            else if (strcmp(cmd, "RM") == 0) {
                int recursivo = (n >= 4 && strcmp(arg3, "RECURSIVO") == 0) ? 1 : 0;
                int res = eliminarArchivo(arg1, arg2, recursivo);
                if (res == BUCKET_OK)                 enviarRespuesta(cliente, "OK\n");
                else if (res == BUCKET_NO_ENCONTRADO) enviarRespuesta(cliente, "ERROR: Archivo no encontrado\n");
                else                                  enviarRespuesta(cliente, "ERROR: No se pudo eliminar\n");
            }
            else if (strcmp(cmd, "CP") == 0) {
                /* Copia entre buckets locales usando un pipe, sin malloc. */
                int res = copiarEntreLocalBuckets(arg1, arg2, arg3, arg4);

                if (res == BUCKET_OK)                 enviarRespuesta(cliente, "OK\n");
                else if (res == BUCKET_NO_ENCONTRADO) enviarRespuesta(cliente, "ERROR: Archivo o bucket no encontrado\n");
                else if (res == BUCKET_LLENO)         enviarRespuesta(cliente, "ERROR: Bucket lleno\n");
                else                                  enviarRespuesta(cliente, "ERROR: No se pudo copiar\n");
            }
            else if (strcmp(cmd, "MV") == 0) {
                /* Mover = copiar + eliminar origen. */
                int res = copiarEntreLocalBuckets(arg1, arg2, arg3, arg4);
                if (res != BUCKET_OK) {
                    enviarRespuesta(cliente, "ERROR: No se pudo mover archivo\n");
                    continue;
                }
                eliminarArchivo(arg1, arg2, 0);
                enviarRespuesta(cliente, "OK\n");
            }
            else {
                enviarRespuesta(cliente, "ERROR: Comando desconocido\n");
            }
        }

        printf("Cliente desconectado\n");
        close(cliente);
    }

    close(servidor);
    return 0;
}
