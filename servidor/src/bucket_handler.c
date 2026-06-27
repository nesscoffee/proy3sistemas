#include "bucket_handler.h"

#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>

/* ============================================================
 * Utilidades internas
 * ============================================================ */

/* Ordena y fusiona huecos contiguos en la lista de espacios libres. */
static void mergeHuecos(EspacioLibre* huecos, uint32_t* n) {
    /* Ordenar por offset (burbuja simple; n ≤ MAX_ARCHIVOS = 100) */
    for (uint32_t i = 0; i < *n; i++) {
        for (uint32_t j = i + 1; j < *n; j++) {
            if (huecos[j].offset < huecos[i].offset) {
                EspacioLibre t = huecos[i];
                huecos[i]      = huecos[j];
                huecos[j]      = t;
            }
        }
    }

    /* Fusionar huecos adyacentes */
    uint32_t w = 0;
    for (uint32_t i = 0; i < *n; i++) {
        if (w > 0 &&
            huecos[w-1].offset + huecos[w-1].tamano >= huecos[i].offset) {
            uint32_t fin    = huecos[i].offset   + huecos[i].tamano;
            uint32_t curFin = huecos[w-1].offset + huecos[w-1].tamano;
            if (fin > curFin)
                huecos[w-1].tamano = fin - huecos[w-1].offset;
        } else {
            huecos[w++] = huecos[i];
        }
    }
    *n = w;
}

/* Devuelve la ruta al archivo de bucket en `ruta` (buffer de MAX_RUTA bytes). */
static void rutaBucket(const char* nombre, char* ruta) {
    snprintf(ruta, MAX_RUTA, "buckets/%s", nombre);
}

/* Lee exactamente `n` bytes del descriptor `fd` al buffer `buf`.
 * Retorna 0 en éxito o -1 si la conexión se cierra antes de tiempo. */
static int leerExacto(int fd, void* buf, size_t n) {
    size_t leido = 0;
    while (leido < n) {
        ssize_t r = read(fd, (char*)buf + leido, n - leido);
        if (r <= 0) return -1;
        leido += (size_t)r;
    }
    return 0;
}

/* Escribe exactamente `n` bytes de `buf` al descriptor `fd`.
 * Retorna 0 en éxito o -1 ante error. */
static int escribirExacto(int fd, const void* buf, size_t n) {
    size_t enviado = 0;
    while (enviado < n) {
        ssize_t r = write(fd, (const char*)buf + enviado, n - enviado);
        if (r <= 0) return -1;
        enviado += (size_t)r;
    }
    return 0;
}

/* ============================================================
 * Operaciones de bucket
 * ============================================================ */

int crearBucket(const char* nombre) {
    mkdir("buckets", 0755);

    char ruta[MAX_RUTA];
    rutaBucket(nombre, ruta);

    /* "x" → falla si ya existe (C11) */
    FILE* arch = fopen(ruta, "wbx");
    if (arch == NULL)
        return (errno == EEXIST) ? BUCKET_YA_EXISTE : BUCKET_ERROR;

    BloqueDirectorio dir = {0};
    int ok = (fwrite(&dir, sizeof(dir), 1, arch) == 1);
    fclose(arch);
    return ok ? BUCKET_OK : BUCKET_ERROR;
}

int existeBucket(const char* nombre) {
    char ruta[MAX_RUTA];
    rutaBucket(nombre, ruta);
    struct stat st;
    return (stat(ruta, &st) == 0) ? 0 : -1;
}

int eliminarBucket(const char* nombre, int forzar) {
    if (existeBucket(nombre) != 0) return BUCKET_NO_ENCONTRADO;

    char ruta[MAX_RUTA];
    rutaBucket(nombre, ruta);

    if (!forzar) {
        FILE* arch = fopen(ruta, "rb");
        if (!arch) return BUCKET_ERROR;

        BloqueDirectorio dir;
        int ok = (fread(&dir, sizeof(dir), 1, arch) == 1);
        fclose(arch);
        if (!ok) return BUCKET_ERROR;
        if (dir.cantidadArchivos > 0) return BUCKET_ERROR;   /* no está vacío */
    }

    return (remove(ruta) == 0) ? BUCKET_OK : BUCKET_ERROR;
}

/* ============================================================
 * Helpers de directorio (lectura/escritura del BloqueDirectorio)
 * ============================================================ */

static int leerDirectorio(FILE* arch, BloqueDirectorio* dir) {
    rewind(arch);
    return (fread(dir, sizeof(*dir), 1, arch) == 1) ? 0 : -1;
}

static int escribirDirectorio(FILE* arch, const BloqueDirectorio* dir) {
    rewind(arch);
    return (fwrite(dir, sizeof(*dir), 1, arch) == 1) ? 0 : -1;
}

/* Busca `clave` en el directorio; retorna su índice o -1 si no existe. */
static int buscarArchivo(const BloqueDirectorio* dir, const char* clave) {
    for (uint32_t i = 0; i < dir->cantidadArchivos; i++)
        if (strcmp(dir->archivos[i].ruta, clave) == 0)
            return (int)i;
    return -1;
}

/* Elimina la entrada en el índice `idx` y registra su espacio como libre. */
static void liberarEntrada(BloqueDirectorio* dir, uint32_t idx) {
    if (dir->cantidadHuecos < MAX_ARCHIVOS) {
        dir->espaciosLibres[dir->cantidadHuecos].offset = dir->archivos[idx].offset;
        dir->espaciosLibres[dir->cantidadHuecos].tamano = dir->archivos[idx].tamano;
        dir->cantidadHuecos++;
    }
    /* Compactar el arreglo de metadatos */
    for (uint32_t i = idx; i < dir->cantidadArchivos - 1; i++)
        dir->archivos[i] = dir->archivos[i + 1];
    dir->cantidadArchivos--;
}

/* Elige un hueco por «primer ajuste» (first-fit).
 * Si lo usa parcialmente deja el resto como hueco nuevo.
 * Retorna el offset donde se debe escribir el nuevo contenido. */
static uint32_t asignarEspacio(BloqueDirectorio* dir, uint32_t tamano, FILE* arch) {
    /* Buscar primer hueco suficientemente grande */
    for (uint32_t i = 0; i < dir->cantidadHuecos; i++) {
        if (dir->espaciosLibres[i].tamano >= tamano) {
            uint32_t offset    = dir->espaciosLibres[i].offset;
            uint32_t restante  = dir->espaciosLibres[i].tamano - tamano;

            /* Eliminar el hueco usado */
            for (uint32_t j = i; j < dir->cantidadHuecos - 1; j++)
                dir->espaciosLibres[j] = dir->espaciosLibres[j + 1];
            dir->cantidadHuecos--;

            /* Si sobra espacio, registrarlo como hueco nuevo */
            if (restante > 0 && dir->cantidadHuecos < MAX_ARCHIVOS) {
                dir->espaciosLibres[dir->cantidadHuecos].offset = offset + tamano;
                dir->espaciosLibres[dir->cantidadHuecos].tamano = restante;
                dir->cantidadHuecos++;
                mergeHuecos(dir->espaciosLibres, &dir->cantidadHuecos);
            }
            return offset;
        }
    }

    /* Sin hueco adecuado → agregar al final */
    fseek(arch, 0, SEEK_END);
    return (uint32_t)ftell(arch);
}

/* ============================================================
 * subirArchivoStream
 *
 * Lee `tamano` bytes desde `fd_origen` en bloques de CHUNK_SIZE
 * y los escribe directamente en el archivo de bucket, sin cargar
 * el contenido completo en memoria.
 * ============================================================ */
int subirArchivoStream(const char* bucket, const char* clave,
                       int fd_origen, uint32_t tamano) {
    if (existeBucket(bucket) != 0) return BUCKET_NO_ENCONTRADO;

    char ruta[MAX_RUTA];
    rutaBucket(bucket, ruta);

    FILE* arch = fopen(ruta, "rb+");
    if (!arch) return BUCKET_ERROR;

    BloqueDirectorio dir;
    if (leerDirectorio(arch, &dir) < 0) { fclose(arch); return BUCKET_ERROR; }

    int idx = buscarArchivo(&dir, clave);

    if (idx >= 0) {
        ArchivoMetadata* meta = &dir.archivos[idx];

        if (meta->tamano == tamano) {
            /* Mismo tamaño → sobreescribir en el mismo lugar */
            fseek(arch, (long)meta->offset, SEEK_SET);
            char chunk[CHUNK_SIZE];
            uint32_t pendiente = tamano;
            while (pendiente > 0) {
                uint32_t bloque = (pendiente < CHUNK_SIZE) ? pendiente : CHUNK_SIZE;
                if (leerExacto(fd_origen, chunk, bloque) < 0) {
                    fclose(arch); return BUCKET_ERROR;
                }
                if (fwrite(chunk, 1, bloque, arch) != bloque) {
                    fclose(arch); return BUCKET_ERROR;
                }
                pendiente -= bloque;
            }
            if (escribirDirectorio(arch, &dir) < 0) { fclose(arch); return BUCKET_ERROR; }
            fclose(arch);
            return BUCKET_OK;
        }

        /* Tamaño diferente → liberar espacio anterior y reasignar */
        liberarEntrada(&dir, (uint32_t)idx);
        mergeHuecos(dir.espaciosLibres, &dir.cantidadHuecos);
    }

    if (dir.cantidadArchivos >= MAX_ARCHIVOS) { fclose(arch); return BUCKET_LLENO; }

    uint32_t offset = asignarEspacio(&dir, tamano, arch);

    /* Escribir datos en streaming */
    fseek(arch, (long)offset, SEEK_SET);
    char chunk[CHUNK_SIZE];
    uint32_t pendiente = tamano;
    while (pendiente > 0) {
        uint32_t bloque = (pendiente < CHUNK_SIZE) ? pendiente : CHUNK_SIZE;
        if (leerExacto(fd_origen, chunk, bloque) < 0) {
            fclose(arch); return BUCKET_ERROR;
        }
        if (fwrite(chunk, 1, bloque, arch) != bloque) {
            fclose(arch); return BUCKET_ERROR;
        }
        pendiente -= bloque;
    }

    /* Registrar nueva entrada de metadatos */
    ArchivoMetadata* nuevo = &dir.archivos[dir.cantidadArchivos++];
    strncpy(nuevo->ruta, clave, MAX_RUTA - 1);
    nuevo->ruta[MAX_RUTA - 1] = '\0';
    nuevo->tamano = tamano;
    nuevo->offset = offset;

    if (escribirDirectorio(arch, &dir) < 0) { fclose(arch); return BUCKET_ERROR; }
    fclose(arch);
    return BUCKET_OK;
}

/* ============================================================
 * descargarArchivoStream
 *
 * Escribe el contenido del objeto en `fd_destino` en bloques de
 * CHUNK_SIZE bytes, sin almacenar el archivo completo en memoria.
 * Establece *tamano con la cantidad de bytes enviados.
 * ============================================================ */
int descargarArchivoStream(const char* bucket, const char* clave,
                           int fd_destino, uint32_t* tamano) {
    if (existeBucket(bucket) != 0) return BUCKET_NO_ENCONTRADO;

    char ruta[MAX_RUTA];
    rutaBucket(bucket, ruta);

    FILE* arch = fopen(ruta, "rb");
    if (!arch) return BUCKET_ERROR;

    BloqueDirectorio dir;
    if (leerDirectorio(arch, &dir) < 0) { fclose(arch); return BUCKET_ERROR; }

    int idx = buscarArchivo(&dir, clave);
    if (idx < 0) { fclose(arch); return BUCKET_NO_ENCONTRADO; }

    ArchivoMetadata* meta = &dir.archivos[idx];
    *tamano = meta->tamano;

    fseek(arch, (long)meta->offset, SEEK_SET);

    char chunk[CHUNK_SIZE];
    uint32_t pendiente = meta->tamano;
    while (pendiente > 0) {
        uint32_t bloque = (pendiente < CHUNK_SIZE) ? pendiente : CHUNK_SIZE;
        if (fread(chunk, 1, bloque, arch) != bloque) {
            fclose(arch); return BUCKET_ERROR;
        }
        if (escribirExacto(fd_destino, chunk, bloque) < 0) {
            fclose(arch); return BUCKET_ERROR;
        }
        pendiente -= bloque;
    }

    fclose(arch);
    return BUCKET_OK;
}

/* ============================================================
 * eliminarArchivo
 * ============================================================ */
int eliminarArchivo(const char* bucket, const char* clave, int recursivo) {
    if (existeBucket(bucket) != 0) return BUCKET_NO_ENCONTRADO;

    char ruta[MAX_RUTA];
    rutaBucket(bucket, ruta);

    FILE* arch = fopen(ruta, "rb+");
    if (!arch) return BUCKET_ERROR;

    BloqueDirectorio dir;
    if (leerDirectorio(arch, &dir) < 0) { fclose(arch); return BUCKET_ERROR; }

    size_t   longClave = strlen(clave);
    uint32_t eliminados = 0;

    for (uint32_t i = 0; i < dir.cantidadArchivos; ) {
        int coincide = recursivo
            ? (strncmp(dir.archivos[i].ruta, clave, longClave) == 0)
            : (strcmp (dir.archivos[i].ruta, clave)            == 0);

        if (coincide) {
            liberarEntrada(&dir, i);
            eliminados++;
        } else {
            i++;
        }
    }

    if (eliminados == 0) { fclose(arch); return BUCKET_NO_ENCONTRADO; }

    mergeHuecos(dir.espaciosLibres, &dir.cantidadHuecos);

    if (escribirDirectorio(arch, &dir) < 0) { fclose(arch); return BUCKET_ERROR; }
    fclose(arch);
    return BUCKET_OK;
}

/* ============================================================
 * listarContenido
 * ============================================================ */
int listarContenido(const char* bucket, const char* prefijo,
                    char* salida, size_t* tamSalida) {
    size_t pos = 0;

    if (bucket == NULL) {
        /* Listar todos los buckets */
        DIR* d = opendir("buckets");
        if (d) {
            struct dirent* entrada;
            while ((entrada = readdir(d)) != NULL) {
                if (entrada->d_name[0] == '.') continue;

                char rc[MAX_RUTA];
                snprintf(rc, sizeof(rc), "buckets/%s", entrada->d_name);
                struct stat st;
                if (stat(rc, &st) != 0 || !S_ISREG(st.st_mode)) continue;

                int n = snprintf(salida + pos, *tamSalida - pos,
                                 "%s\n", entrada->d_name);
                if (n < 0 || (size_t)n >= *tamSalida - pos) break;
                pos += (size_t)n;
            }
            closedir(d);
        }
    } else {
        /* Listar objetos dentro de un bucket */
        if (existeBucket(bucket) != 0) return BUCKET_NO_ENCONTRADO;

        char ruta[MAX_RUTA];
        rutaBucket(bucket, ruta);

        FILE* arch = fopen(ruta, "rb");
        if (!arch) return BUCKET_ERROR;

        BloqueDirectorio dir;
        if (leerDirectorio(arch, &dir) < 0) { fclose(arch); return BUCKET_ERROR; }
        fclose(arch);

        size_t longPrefijo = prefijo ? strlen(prefijo) : 0;

        for (uint32_t i = 0; i < dir.cantidadArchivos; i++) {
            if (prefijo &&
                strncmp(dir.archivos[i].ruta, prefijo, longPrefijo) != 0)
                continue;

            int n = snprintf(salida + pos, *tamSalida - pos,
                             "%s|%u\n",
                             dir.archivos[i].ruta,
                             dir.archivos[i].tamano);
            if (n < 0 || (size_t)n >= *tamSalida - pos) break;
            pos += (size_t)n;
        }
    }

    /* Centinela de fin de listado */
    int n = snprintf(salida + pos, *tamSalida - pos, ".\n");
    if (n > 0) pos += (size_t)n;
    *tamSalida = pos;
    return BUCKET_OK;
}

/* ============================================================
 * obtenerTamanoArchivo
 * ============================================================ */
int obtenerTamanoArchivo(const char* bucket, const char* clave,
                         uint32_t* tamano) {
    if (existeBucket(bucket) != 0) return BUCKET_NO_ENCONTRADO;

    char ruta[MAX_RUTA];
    rutaBucket(bucket, ruta);

    FILE* arch = fopen(ruta, "rb");
    if (!arch) return BUCKET_ERROR;

    BloqueDirectorio dir;
    if (leerDirectorio(arch, &dir) < 0) { fclose(arch); return BUCKET_ERROR; }
    fclose(arch);

    int idx = buscarArchivo(&dir, clave);
    if (idx < 0) return BUCKET_NO_ENCONTRADO;

    *tamano = dir.archivos[idx].tamano;
    return BUCKET_OK;
}
