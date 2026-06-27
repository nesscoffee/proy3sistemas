#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#define PUERTO 9000
#define SERVIDOR "127.0.0.1"
#define TAM_BUF 4096
#define MAX_ARCHIVOS_LOCALES 8192

static int conectarServidor(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }

    struct sockaddr_in direccion = {
        .sin_family = AF_INET,
        .sin_port   = htons(PUERTO),
    };
    inet_pton(AF_INET, SERVIDOR, &direccion.sin_addr);

    if (connect(sock, (struct sockaddr*)&direccion, sizeof(direccion)) < 0) {
        perror("connect"); close(sock); return -1;
    }
    return sock;
}

static int enviarTodo(int fd, const void* datos, size_t tam) {
    size_t enviado = 0;
    while (enviado < tam) {
        int r = (int)send(fd, (const char*)datos + enviado, tam - enviado, 0);
        if (r <= 0) return -1;
        enviado += (size_t)r;
    }
    return 0;
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

static int esRutaS3(const char* p) {
    return strchr(p, ':') != NULL;
}

static void parsearRutaS3(const char* p, char* bucket, size_t bTam, char* clave, size_t cTam) {
    const char* dosPuntos = strchr(p, ':');
    size_t longBucket = (size_t)(dosPuntos - p);
    snprintf(bucket, bTam, "%.*s", (int)longBucket, p);
    snprintf(clave, cTam, "%s", dosPuntos + 1);
}

static int caminarDirectorio(const char* base, const char* relActual,
                              char archivos[][TAM_BUF], uint32_t* tamanos,
                              uint32_t* n, uint32_t max) {
    char rutaComp[4096];
    if (relActual[0])
        snprintf(rutaComp, sizeof(rutaComp), "%s/%s", base, relActual);
    else
        snprintf(rutaComp, sizeof(rutaComp), "%s", base);

    DIR* d = opendir(rutaComp);
    if (!d) return -1;

    struct dirent* entrada;
    while ((entrada = readdir(d)) != NULL && *n < max) {
        if (strcmp(entrada->d_name, ".") == 0 || strcmp(entrada->d_name, "..") == 0)
            continue;

        char subRuta[4096];
        if (relActual[0])
            snprintf(subRuta, sizeof(subRuta), "%s/%s", relActual, entrada->d_name);
        else
            snprintf(subRuta, sizeof(subRuta), "%s", entrada->d_name);

        char rutaArchivo[4096];
        snprintf(rutaArchivo, sizeof(rutaArchivo), "%s/%s", rutaComp, entrada->d_name);

        struct stat st;
        if (stat(rutaArchivo, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            caminarDirectorio(base, subRuta, archivos, tamanos, n, max);
        } else if (S_ISREG(st.st_mode)) {
            snprintf(archivos[*n], TAM_BUF, "%s", subRuta);
            tamanos[*n] = (uint32_t)st.st_size;
            (*n)++;
        }
    }

    closedir(d);
    return 0;
}

static int leerListado(int fd, char archivos[][TAM_BUF], uint32_t* tamanos, uint32_t* n) {
    char linea[TAM_BUF];
    *n = 0;
    while (leerLinea(fd, linea, sizeof(linea)) > 0) {
        if (strcmp(linea, ".") == 0) break;
        char* tuberia = strchr(linea, '|');
        if (!tuberia) continue;
        *tuberia = '\0';
        snprintf(archivos[*n], TAM_BUF, "%s", linea);
        tamanos[*n] = (uint32_t)atol(tuberia + 1);
        (*n)++;
    }
    return 0;
}

static int lsCmd(int argc, char* argv[]) {
    int indiceArgs[2], nArgs = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--recursivo") == 0) continue;
        if (nArgs < 2) indiceArgs[nArgs++] = i;
    }

    int sock = conectarServidor();
    if (sock < 0) return 1;

    char cmd[TAM_BUF];
    if (nArgs == 0)
        snprintf(cmd, sizeof(cmd), "LS\n");
    else if (nArgs == 1)
        snprintf(cmd, sizeof(cmd), "LS %s\n", argv[indiceArgs[0]]);
    else
        snprintf(cmd, sizeof(cmd), "LS %s %s\n", argv[indiceArgs[0]], argv[indiceArgs[1]]);

    enviarTodo(sock, cmd, strlen(cmd));

    char resp[TAM_BUF];
    while (leerLinea(sock, resp, sizeof(resp)) > 0) {
        if (strcmp(resp, ".") == 0) break;
        printf("%s\n", resp);
    }

    close(sock);
    return 0;
}

static int mbCmd(int argc, char* argv[]) {
    if (argc < 3) { fprintf(stderr, "Uso: %s mb <bucket>\n", argv[0]); return 1; }

    int sock = conectarServidor();
    if (sock < 0) return 1;

    char cmd[TAM_BUF];
    snprintf(cmd, sizeof(cmd), "CR %s\n", argv[2]);
    enviarTodo(sock, cmd, strlen(cmd));

    char resp[TAM_BUF];
    leerLinea(sock, resp, sizeof(resp));
    printf("%s\n", resp);

    close(sock);
    return 0;
}

static int rbCmd(int argc, char* argv[]) {
    if (argc < 3) { fprintf(stderr, "Uso: %s rb <bucket> [--force]\n", argv[0]); return 1; }

    int flagForce = 0;
    for (int i = 3; i < argc; i++)
        if (strcmp(argv[i], "--force") == 0) flagForce = 1;

    int sock = conectarServidor();
    if (sock < 0) return 1;

    char cmd[TAM_BUF];
    if (flagForce)
        snprintf(cmd, sizeof(cmd), "RB %s FORCE\n", argv[2]);
    else
        snprintf(cmd, sizeof(cmd), "RB %s\n", argv[2]);

    enviarTodo(sock, cmd, strlen(cmd));

    char resp[TAM_BUF];
    leerLinea(sock, resp, sizeof(resp));
    printf("%s\n", resp);

    close(sock);
    return 0;
}

static int cpCmd(int argc, char* argv[]) {
    if (argc < 4) { fprintf(stderr, "Uso: %s cp <origen> <destino>\n", argv[0]); return 1; }

    int esS3Orig = esRutaS3(argv[2]);
    int esS3Dest = esRutaS3(argv[3]);

    if (esS3Orig && esS3Dest) {
        int sock = conectarServidor();
        if (sock < 0) return 1;

        char bOrig[TAM_BUF], cOrig[TAM_BUF], bDest[TAM_BUF], cDest[TAM_BUF];
        parsearRutaS3(argv[2], bOrig, sizeof(bOrig), cOrig, sizeof(cOrig));
        parsearRutaS3(argv[3], bDest, sizeof(bDest), cDest, sizeof(cDest));

        char cmd[TAM_BUF];
        snprintf(cmd, sizeof(cmd), "CP %s %s %s %s\n", bOrig, cOrig, bDest, cDest);
        enviarTodo(sock, cmd, strlen(cmd));

        char resp[TAM_BUF];
        leerLinea(sock, resp, sizeof(resp));
        printf("%s\n", resp);
        close(sock);
    }
    else if (!esS3Orig && esS3Dest) {
        // Upload local -> S3
        FILE* f = fopen(argv[2], "rb");
        if (!f) { perror(argv[2]); return 1; }

        fseek(f, 0, SEEK_END);
        long tam = ftell(f);
        if (tam < 0) { fclose(f); return 1; }
        rewind(f);

        void* datos = malloc((size_t)tam);
        if (!datos) { fclose(f); return 1; }
        if (fread(datos, 1, (size_t)tam, f) != (size_t)tam) {
            free(datos); fclose(f); return 1;
        }
        fclose(f);

        char bucket[TAM_BUF], clave[TAM_BUF];
        parsearRutaS3(argv[3], bucket, sizeof(bucket), clave, sizeof(clave));

        int sock = conectarServidor();
        if (sock < 0) { free(datos); return 1; }

        char cmd[TAM_BUF];
        snprintf(cmd, sizeof(cmd), "PUT %s %s %ld\n", bucket, clave, tam);
        enviarTodo(sock, cmd, strlen(cmd));
        enviarTodo(sock, datos, (size_t)tam);
        free(datos);

        char resp[TAM_BUF];
        leerLinea(sock, resp, sizeof(resp));
        printf("%s\n", resp);
        close(sock);
    }
    else if (esS3Orig && !esS3Dest) {
        // Download S3 -> local
        char bucket[TAM_BUF], clave[TAM_BUF];
        parsearRutaS3(argv[2], bucket, sizeof(bucket), clave, sizeof(clave));

        int sock = conectarServidor();
        if (sock < 0) return 1;

        char cmd[TAM_BUF];
        snprintf(cmd, sizeof(cmd), "GET %s %s\n", bucket, clave);
        enviarTodo(sock, cmd, strlen(cmd));

        char resp[TAM_BUF];
        leerLinea(sock, resp, sizeof(resp));
        if (strncmp(resp, "OK", 2) != 0) {
            printf("%s\n", resp);
            close(sock);
            return 1;
        }

        uint32_t tamDatos = (uint32_t)atol(resp + 3);
        void* datos = malloc(tamDatos);
        if (!datos) { close(sock); return 1; }

        size_t leido = 0;
        while (leido < tamDatos) {
            int r = (int)read(sock, (char*)datos + leido, tamDatos - leido);
            if (r <= 0) break;
            leido += (size_t)r;
        }

        close(sock);

        if (leido != tamDatos) { free(datos); fprintf(stderr, "Error: datos incompletos\n"); return 1; }

        FILE* f = fopen(argv[3], "wb");
        if (!f) { perror(argv[3]); free(datos); return 1; }
        fwrite(datos, 1, tamDatos, f);
        fclose(f);
        free(datos);

        printf("OK\n");
    }
    else {
        fprintf(stderr, "Error: al menos uno debe ser ruta S3 (bucket:clave)\n");
        return 1;
    }

    return 0;
}

static int mvCmd(int argc, char* argv[]) {
    if (argc < 4) { fprintf(stderr, "Uso: %s mv <origen> <destino>\n", argv[0]); return 1; }

    if (!esRutaS3(argv[2]) || !esRutaS3(argv[3])) {
        fprintf(stderr, "Error: mv solo soporta rutas S3\n");
        return 1;
    }

    int sock = conectarServidor();
    if (sock < 0) return 1;

    char bOrig[TAM_BUF], cOrig[TAM_BUF], bDest[TAM_BUF], cDest[TAM_BUF];
    parsearRutaS3(argv[2], bOrig, sizeof(bOrig), cOrig, sizeof(cOrig));
    parsearRutaS3(argv[3], bDest, sizeof(bDest), cDest, sizeof(cDest));

    char cmd[TAM_BUF];
    snprintf(cmd, sizeof(cmd), "MV %s %s %s %s\n", bOrig, cOrig, bDest, cDest);
    enviarTodo(sock, cmd, strlen(cmd));

    char resp[TAM_BUF];
    leerLinea(sock, resp, sizeof(resp));
    printf("%s\n", resp);

    close(sock);
    return 0;
}

static int rmCmd(int argc, char* argv[]) {
    if (argc < 3) { fprintf(stderr, "Uso: %s rm <bucket> <clave> [--recursivo]\n", argv[0]); return 1; }
    if (argc < 4) { fprintf(stderr, "Uso: %s rm <bucket> <clave> [--recursivo]\n", argv[0]); return 1; }

    int flagRecursivo = 0;
    for (int i = 4; i < argc; i++)
        if (strcmp(argv[i], "--recursivo") == 0) flagRecursivo = 1;

    int sock = conectarServidor();
    if (sock < 0) return 1;

    char cmd[TAM_BUF];
    if (flagRecursivo)
        snprintf(cmd, sizeof(cmd), "RM %s %s RECURSIVO\n", argv[2], argv[3]);
    else
        snprintf(cmd, sizeof(cmd), "RM %s %s\n", argv[2], argv[3]);

    enviarTodo(sock, cmd, strlen(cmd));

    char resp[TAM_BUF];
    leerLinea(sock, resp, sizeof(resp));
    printf("%s\n", resp);

    close(sock);
    return 0;
}

static int syncCmd(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Uso: %s sync <origen> <destino> [--delete]\n", argv[0]);
        return 1;
    }

    int flagDelete = 0;
    for (int i = 4; i < argc; i++)
        if (strcmp(argv[i], "--delete") == 0) flagDelete = 1;

    int esS3Orig = esRutaS3(argv[2]);
    int esS3Dest = esRutaS3(argv[3]);

    // Ensure exactly one S3 path
    if (esS3Orig == esS3Dest) {
        fprintf(stderr, "Error: uno debe ser directorio local y el otro ruta S3\n");
        return 1;
    }

    int sock = conectarServidor();
    if (sock < 0) return 1;

    if (!esS3Orig && esS3Dest) {
        // Upload sync: local -> S3
        char bucket[TAM_BUF], prefijo[TAM_BUF];
        parsearRutaS3(argv[3], bucket, sizeof(bucket), prefijo, sizeof(prefijo));

        // Walk local directory
        char (*archivosLocales)[TAM_BUF] = malloc(MAX_ARCHIVOS_LOCALES * TAM_BUF);
        uint32_t* tamanosLocales = malloc(MAX_ARCHIVOS_LOCALES * sizeof(uint32_t));
        uint32_t nLocales = 0;

        caminarDirectorio(argv[2], "", archivosLocales, tamanosLocales, &nLocales, MAX_ARCHIVOS_LOCALES);

        // Get remote listing
        char cmdLs[TAM_BUF];
        snprintf(cmdLs, sizeof(cmdLs), "LS %s %s\n", bucket, prefijo);
        enviarTodo(sock, cmdLs, strlen(cmdLs));

        char (*archivosRemotos)[TAM_BUF] = malloc(MAX_ARCHIVOS_LOCALES * TAM_BUF);
        uint32_t* tamanosRemotos = malloc(MAX_ARCHIVOS_LOCALES * sizeof(uint32_t));
        uint32_t nRemotos = 0;
        leerListado(sock, archivosRemotos, tamanosRemotos, &nRemotos);

        // Upload missing or changed files
        for (uint32_t i = 0; i < nLocales; i++) {
            char claveS3[TAM_BUF];
            snprintf(claveS3, sizeof(claveS3), "%s%s", prefijo, archivosLocales[i]);

            int encontrado = 0;
            for (uint32_t j = 0; j < nRemotos; j++) {
                if (strcmp(archivosRemotos[j], claveS3) == 0 &&
                    tamanosRemotos[j] == tamanosLocales[i]) {
                    encontrado = 1;
                    break;
                }
            }

            if (!encontrado) {
                // Read local file
                char rutaLocal[4096];
                snprintf(rutaLocal, sizeof(rutaLocal), "%s/%s", argv[2], archivosLocales[i]);

                FILE* f = fopen(rutaLocal, "rb");
                if (!f) continue;

                fseek(f, 0, SEEK_END);
                long tamArch = ftell(f);
                if (tamArch < 0) { fclose(f); continue; }
                rewind(f);

                void* datos = malloc((size_t)tamArch);
                if (!datos) { fclose(f); continue; }
                fread(datos, 1, (size_t)tamArch, f);
                fclose(f);

                // Send PUT
                char cmdPut[TAM_BUF];
                snprintf(cmdPut, sizeof(cmdPut), "PUT %s %s %ld\n", bucket, claveS3, tamArch);
                enviarTodo(sock, cmdPut, strlen(cmdPut));
                enviarTodo(sock, datos, (size_t)tamArch);
                free(datos);

                char resp[TAM_BUF];
                leerLinea(sock, resp, sizeof(resp));
                if (strncmp(resp, "OK", 2) == 0)
                    printf("SUBIDO %s\n", claveS3);
                else
                    printf("ERROR %s: %s\n", claveS3, resp);
            }
        }

        // Delete remote files not in local
        if (flagDelete) {
            for (uint32_t j = 0; j < nRemotos; j++) {
                const char* rutaRel = archivosRemotos[j] + strlen(prefijo);
                int encontrado = 0;
                for (uint32_t i = 0; i < nLocales; i++) {
                    if (strcmp(archivosLocales[i], rutaRel) == 0) { encontrado = 1; break; }
                }
                if (!encontrado) {
                    char cmdRm[TAM_BUF];
                    snprintf(cmdRm, sizeof(cmdRm), "RM %s %s\n", bucket, archivosRemotos[j]);
                    enviarTodo(sock, cmdRm, strlen(cmdRm));
                    char resp[TAM_BUF];
                    leerLinea(sock, resp, sizeof(resp));
                    if (strncmp(resp, "OK", 2) == 0)
                        printf("ELIMINADO %s\n", archivosRemotos[j]);
                }
            }
        }

        free(archivosLocales);
        free(tamanosLocales);
        free(archivosRemotos);
        free(tamanosRemotos);
    }
    else {
        // Download sync: S3 -> local
        char bucket[TAM_BUF], prefijo[TAM_BUF];
        parsearRutaS3(argv[2], bucket, sizeof(bucket), prefijo, sizeof(prefijo));

        // Get remote listing
        char cmdLs[TAM_BUF];
        snprintf(cmdLs, sizeof(cmdLs), "LS %s %s\n", bucket, prefijo);
        enviarTodo(sock, cmdLs, strlen(cmdLs));

        char (*archivosRemotos)[TAM_BUF] = malloc(MAX_ARCHIVOS_LOCALES * TAM_BUF);
        uint32_t* tamanosRemotos = malloc(MAX_ARCHIVOS_LOCALES * sizeof(uint32_t));
        uint32_t nRemotos = 0;
        leerListado(sock, archivosRemotos, tamanosRemotos, &nRemotos);

        // Walk local to know existing files
        char (*archivosLocales)[TAM_BUF] = malloc(MAX_ARCHIVOS_LOCALES * TAM_BUF);
        uint32_t* tamanosLocales = malloc(MAX_ARCHIVOS_LOCALES * sizeof(uint32_t));
        uint32_t nLocales = 0;

        struct stat stBase;
        if (stat(argv[3], &stBase) == 0 && S_ISDIR(stBase.st_mode))
            caminarDirectorio(argv[3], "", archivosLocales, tamanosLocales, &nLocales, MAX_ARCHIVOS_LOCALES);

        // Download missing or changed files
        size_t longPrefijo = strlen(prefijo);

        for (uint32_t j = 0; j < nRemotos; j++) {
            const char* rutaRel = archivosRemotos[j] + longPrefijo;
            if (*rutaRel == '/') rutaRel++;

            char rutaLocal[4096];
            snprintf(rutaLocal, sizeof(rutaLocal), "%s/%s", argv[3], rutaRel);

            int descargar = 1;
            for (uint32_t i = 0; i < nLocales; i++) {
                if (strcmp(archivosLocales[i], rutaRel) == 0 &&
                    tamanosLocales[i] == tamanosRemotos[j]) {
                    descargar = 0;
                    break;
                }
            }

            if (descargar) {
                char cmdGet[TAM_BUF];
                snprintf(cmdGet, sizeof(cmdGet), "GET %s %s\n", bucket, archivosRemotos[j]);
                enviarTodo(sock, cmdGet, strlen(cmdGet));

                char resp[TAM_BUF];
                leerLinea(sock, resp, sizeof(resp));
                if (strncmp(resp, "OK", 2) != 0) {
                    printf("ERROR %s: %s\n", archivosRemotos[j], resp);
                    continue;
                }

                uint32_t tamDatos = (uint32_t)atol(resp + 3);
                void* datos = malloc(tamDatos);
                if (!datos) continue;

                size_t leido = 0;
                while (leido < tamDatos) {
                    int r = (int)read(sock, (char*)datos + leido, tamDatos - leido);
                    if (r <= 0) break;
                    leido += (size_t)r;
                }

                if (leido != tamDatos) { free(datos); continue; }

                // Create parent directories
                for (char* p = rutaLocal + 1; *p; p++) {
                    if (*p == '/') {
                        *p = '\0';
                        mkdir(rutaLocal, 0755);
                        *p = '/';
                    }
                }

                FILE* f = fopen(rutaLocal, "wb");
                if (f) {
                    fwrite(datos, 1, tamDatos, f);
                    fclose(f);
                    printf("DESCARGADO %s\n", archivosRemotos[j]);
                }
                free(datos);
            }
        }

        // Delete local files not in remote
        if (flagDelete) {
            for (uint32_t i = 0; i < nLocales; i++) {
                int encontrado = 0;
                for (uint32_t j = 0; j < nRemotos; j++) {
                    const char* rutaRel = archivosRemotos[j] + longPrefijo;
                    if (*rutaRel == '/') rutaRel++;
                    if (strcmp(archivosLocales[i], rutaRel) == 0) { encontrado = 1; break; }
                }
                if (!encontrado) {
                    char rutaLocal[4096];
                    snprintf(rutaLocal, sizeof(rutaLocal), "%s/%s", argv[3], archivosLocales[i]);
                    if (remove(rutaLocal) == 0) printf("ELIMINADO_LOCAL %s\n", rutaLocal);
                }
            }
        }

        free(archivosLocales);
        free(tamanosLocales);
        free(archivosRemotos);
        free(tamanosRemotos);
    }

    close(sock);
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <comando> [args...]\n", argv[0]);
        fprintf(stderr, "Comandos: ls, mb, rb, cp, mv, rm, sync\n");
        return 1;
    }

    if (strcmp(argv[1], "ls") == 0) return lsCmd(argc, argv);
    if (strcmp(argv[1], "mb") == 0) return mbCmd(argc, argv);
    if (strcmp(argv[1], "rb") == 0) return rbCmd(argc, argv);
    if (strcmp(argv[1], "cp") == 0) return cpCmd(argc, argv);
    if (strcmp(argv[1], "mv") == 0) return mvCmd(argc, argv);
    if (strcmp(argv[1], "rm") == 0) return rmCmd(argc, argv);
    if (strcmp(argv[1], "sync") == 0) return syncCmd(argc, argv);

    fprintf(stderr, "Comando desconocido: %s\n", argv[1]);
    return 1;
}
