#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <dirent.h>

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

static int enviarComandoSimple(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char cmd[TAM_BUF];
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);

    int sock = conectarServidor();
    if (sock < 0) return 1;

    enviarTodo(sock, cmd, strlen(cmd));

    char resp[TAM_BUF];
    leerLinea(sock, resp, sizeof(resp));
    printf("%s\n", resp);

    close(sock);
    return (strncmp(resp, "OK", 2) == 0) ? 0 : 1;
}

/* Sube archivo local al servidor usando los streams */
static int subirArchivoLocalAS3(const char* rutaLocal, const char* bucket,
                                const char* clave, long tamArch) {
    FILE* f = fopen(rutaLocal, "rb");
    if (!f) { perror(rutaLocal); return 1; }

    int sock = conectarServidor();
    if (sock < 0) { fclose(f); return 1; }

    char cmd[TAM_BUF];
    snprintf(cmd, sizeof(cmd), "PUT %s %s %ld\n", bucket, clave, tamArch);
    enviarTodo(sock, cmd, strlen(cmd));

    char chunk[TAM_BUF];
    size_t leido;
    int error = 0;
    while ((leido = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (enviarTodo(sock, chunk, leido) < 0) { error = 1; break; }
    }
    fclose(f);

    if (error) { fprintf(stderr, "Error: fallo de red durante subida\n"); close(sock); return 1; }

    char resp[TAM_BUF];
    leerLinea(sock, resp, sizeof(resp));
    printf("%s\n", resp);

    close(sock);
    return (strncmp(resp, "OK", 2) == 0) ? 0 : 1;
}

/* Descarga archivo del servidor al local usando los streams */
static int descargarArchivoS3ALocal(const char* bucket, const char* clave,
                                    const char* rutaLocal) {
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

    /* Crear directorios padres */
    for (char* p = (char*)rutaLocal + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(rutaLocal, 0755); *p = '/'; }
    }

    FILE* f = fopen(rutaLocal, "wb");
    if (!f) { perror(rutaLocal); close(sock); return 1; }

    char chunk[TAM_BUF];
    uint32_t pendiente = tamDatos;
    int error = 0;
    while (pendiente > 0) {
        uint32_t bloque = (pendiente < TAM_BUF) ? pendiente : TAM_BUF;
        int r = (int)read(sock, chunk, bloque);
        if (r <= 0) { error = 1; break; }
        if (fwrite(chunk, 1, (size_t)r, f) != (size_t)r) { error = 1; break; }
        pendiente -= (uint32_t)r;
    }
    fclose(f);
    close(sock);

    if (error) {
        fprintf(stderr, "Error: transferencia incompleta\n");
        remove(rutaLocal);
        return 1;
    }
    return 0;
}

static int lsCmd(int argc, char* argv[]) {
    int indiceArgs[2], nArgs = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--recursive") == 0) continue;
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
    return enviarComandoSimple("CR %s\n", argv[2]);
}

static int rbCmd(int argc, char* argv[]) {
    if (argc < 3) { fprintf(stderr, "Uso: %s rb <bucket> [--force]\n", argv[0]); return 1; }

    int flagForce = 0;
    for (int i = 3; i < argc; i++)
        if (strcmp(argv[i], "--force") == 0) flagForce = 1;

    return flagForce
        ? enviarComandoSimple("RB %s FORCE\n", argv[2])
        : enviarComandoSimple("RB %s\n", argv[2]);
}

static int cpCmd(int argc, char* argv[]) {
    if (argc < 4) { fprintf(stderr, "Uso: %s cp <origen> <destino>\n", argv[0]); return 1; }

    int esS3Orig = esRutaS3(argv[2]);
    int esS3Dest = esRutaS3(argv[3]);

    if (esS3Orig && esS3Dest) {
        char bOrig[TAM_BUF], cOrig[TAM_BUF], bDest[TAM_BUF], cDest[TAM_BUF];
        parsearRutaS3(argv[2], bOrig, sizeof(bOrig), cOrig, sizeof(cOrig));
        parsearRutaS3(argv[3], bDest, sizeof(bDest), cDest, sizeof(cDest));
        return enviarComandoSimple("CP %s %s %s %s\n", bOrig, cOrig, bDest, cDest);
    }

    if (!esS3Orig && esS3Dest) {
        struct stat st;
        if (stat(argv[2], &st) != 0) { perror(argv[2]); return 1; }

        char bucket[TAM_BUF], clave[TAM_BUF];
        parsearRutaS3(argv[3], bucket, sizeof(bucket), clave, sizeof(clave));

        return subirArchivoLocalAS3(argv[2], bucket, clave, (long)st.st_size);
    }

    if (esS3Orig && !esS3Dest) {
        char bucket[TAM_BUF], clave[TAM_BUF];
        parsearRutaS3(argv[2], bucket, sizeof(bucket), clave, sizeof(clave));

        int ok = descargarArchivoS3ALocal(bucket, clave, argv[3]);
        if (ok == 0) printf("OK\n");
        return ok;
    }

    fprintf(stderr, "Error: al menos uno debe ser ruta S3 (bucket:clave)\n");
    return 1;
}

static int mvCmd(int argc, char* argv[]) {
    if (argc < 4) { fprintf(stderr, "Uso: %s mv <origen> <destino>\n", argv[0]); return 1; }

    if (!esRutaS3(argv[2]) || !esRutaS3(argv[3])) {
        fprintf(stderr, "Error: mv solo soporta rutas S3\n");
        return 1;
    }

    char bOrig[TAM_BUF], cOrig[TAM_BUF], bDest[TAM_BUF], cDest[TAM_BUF];
    parsearRutaS3(argv[2], bOrig, sizeof(bOrig), cOrig, sizeof(cOrig));
    parsearRutaS3(argv[3], bDest, sizeof(bDest), cDest, sizeof(cDest));

    return enviarComandoSimple("MV %s %s %s %s\n", bOrig, cOrig, bDest, cDest);
}

static int rmCmd(int argc, char* argv[]) {
    if (argc < 3) { fprintf(stderr, "Uso: %s rm <bucket> <clave> [--recursive]\n", argv[0]); return 1; }
    if (argc < 4) { fprintf(stderr, "Uso: %s rm <bucket> <clave> [--recursive]\n", argv[0]); return 1; }

    int flagrecursive = 0;
    for (int i = 4; i < argc; i++)
        if (strcmp(argv[i], "--recursive") == 0) flagrecursive = 1;

    return flagrecursive
        ? enviarComandoSimple("RM %s %s RECURSIVE\n", argv[2], argv[3])
        : enviarComandoSimple("RM %s %s\n", argv[2], argv[3]);
}

/* Sincronización upload: local → S3.
 * Recorre el directorio local, lista el bucket, sube archivos faltantes o
 * modificados y, si flagDelete está activo, elimina los remotos huérfanos. */
static int syncUpload(const char* dirLocal, const char* bucket,
                      const char* prefijo, int flagDelete, int sock) {
    char (*archivosLocales)[TAM_BUF] = malloc(MAX_ARCHIVOS_LOCALES * TAM_BUF);
    uint32_t* tamanosLocales = malloc(MAX_ARCHIVOS_LOCALES * sizeof(uint32_t));
    uint32_t nLocales = 0;

    caminarDirectorio(dirLocal, "", archivosLocales, tamanosLocales, &nLocales, MAX_ARCHIVOS_LOCALES);

    char cmdLs[TAM_BUF];
    snprintf(cmdLs, sizeof(cmdLs), "LS %s %s\n", bucket, prefijo);
    enviarTodo(sock, cmdLs, strlen(cmdLs));

    char (*archivosRemotos)[TAM_BUF] = malloc(MAX_ARCHIVOS_LOCALES * TAM_BUF);
    uint32_t* tamanosRemotos = malloc(MAX_ARCHIVOS_LOCALES * sizeof(uint32_t));
    uint32_t nRemotos = 0;
    leerListado(sock, archivosRemotos, tamanosRemotos, &nRemotos);

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
            char rutaLocal[4096];
            snprintf(rutaLocal, sizeof(rutaLocal), "%s/%s", dirLocal, archivosLocales[i]);

            struct stat stArch;
            if (stat(rutaLocal, &stArch) != 0) continue;

            FILE* f = fopen(rutaLocal, "rb");
            if (!f) continue;

            char cmdPut[TAM_BUF];
            snprintf(cmdPut, sizeof(cmdPut), "PUT %s %s %ld\n", bucket, claveS3, (long)stArch.st_size);
            enviarTodo(sock, cmdPut, strlen(cmdPut));

            char chunk[TAM_BUF];
            size_t leido;
            int errorUp = 0;
            while ((leido = fread(chunk, 1, sizeof(chunk), f)) > 0) {
                if (enviarTodo(sock, chunk, leido) < 0) { errorUp = 1; break; }
            }
            fclose(f);

            if (errorUp) { printf("ERROR %s: fallo de red\n", claveS3); continue; }

            char resp[TAM_BUF];
            leerLinea(sock, resp, sizeof(resp));
            if (strncmp(resp, "OK", 2) == 0)
                printf("SUBIDO %s\n", claveS3);
            else
                printf("ERROR %s: %s\n", claveS3, resp);
        }
    }

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
    return 0;
}

/* Sincronización download: S3 → local.
 * Lista el bucket, descarga archivos faltantes o modificados y, si
 * flagDelete está activo, elimina los locales huérfanos. */
static int syncDownload(const char* bucket, const char* prefijo,
                        const char* dirLocal, int flagDelete, int sock) {
    char cmdLs[TAM_BUF];
    snprintf(cmdLs, sizeof(cmdLs), "LS %s %s\n", bucket, prefijo);
    enviarTodo(sock, cmdLs, strlen(cmdLs));

    char (*archivosRemotos)[TAM_BUF] = malloc(MAX_ARCHIVOS_LOCALES * TAM_BUF);
    uint32_t* tamanosRemotos = malloc(MAX_ARCHIVOS_LOCALES * sizeof(uint32_t));
    uint32_t nRemotos = 0;
    leerListado(sock, archivosRemotos, tamanosRemotos, &nRemotos);

    char (*archivosLocales)[TAM_BUF] = malloc(MAX_ARCHIVOS_LOCALES * TAM_BUF);
    uint32_t* tamanosLocales = malloc(MAX_ARCHIVOS_LOCALES * sizeof(uint32_t));
    uint32_t nLocales = 0;

    struct stat stBase;
    if (stat(dirLocal, &stBase) == 0 && S_ISDIR(stBase.st_mode))
        caminarDirectorio(dirLocal, "", archivosLocales, tamanosLocales, &nLocales, MAX_ARCHIVOS_LOCALES);

    size_t longPrefijo = strlen(prefijo);

    for (uint32_t j = 0; j < nRemotos; j++) {
        const char* rutaRel = archivosRemotos[j] + longPrefijo;
        if (*rutaRel == '/') rutaRel++;

        char rutaLocal[4096];
        snprintf(rutaLocal, sizeof(rutaLocal), "%s/%s", dirLocal, rutaRel);

        int descargar = 1;
        for (uint32_t i = 0; i < nLocales; i++) {
            if (strcmp(archivosLocales[i], rutaRel) == 0 &&
                tamanosLocales[i] == tamanosRemotos[j]) {
                descargar = 0;
                break;
            }
        }

        if (descargar) {
            int ok = descargarArchivoS3ALocal(bucket, archivosRemotos[j], rutaLocal);
            if (ok == 0)
                printf("DESCARGADO %s\n", archivosRemotos[j]);
            else
                printf("ERROR %s: fallo de descarga\n", archivosRemotos[j]);
        }
    }

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
                snprintf(rutaLocal, sizeof(rutaLocal), "%s/%s", dirLocal, archivosLocales[i]);
                if (remove(rutaLocal) == 0) printf("ELIMINADO_LOCAL %s\n", rutaLocal);
            }
        }
    }

    free(archivosLocales);
    free(tamanosLocales);
    free(archivosRemotos);
    free(tamanosRemotos);
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

    if (esS3Orig == esS3Dest) {
        fprintf(stderr, "Error: uno debe ser directorio local y el otro ruta S3\n");
        return 1;
    }

    int sock = conectarServidor();
    if (sock < 0) return 1;

    if (!esS3Orig && esS3Dest) {
        char bucket[TAM_BUF], prefijo[TAM_BUF];
        parsearRutaS3(argv[3], bucket, sizeof(bucket), prefijo, sizeof(prefijo));
        syncUpload(argv[2], bucket, prefijo, flagDelete, sock);
    } else {
        char bucket[TAM_BUF], prefijo[TAM_BUF];
        parsearRutaS3(argv[2], bucket, sizeof(bucket), prefijo, sizeof(prefijo));
        syncDownload(bucket, prefijo, argv[3], flagDelete, sock);
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
