#pragma once

#include <stdint.h>
#include <stddef.h>

#define MAX_RUTA      256
#define MAX_ARCHIVOS  100
#define CHUNK_SIZE    4096   /* tamaño del bloque para el stream */

typedef struct {
    char     ruta[MAX_RUTA];
    uint32_t tamano;
    uint32_t offset;
} ArchivoMetadata;

typedef struct {
    uint32_t offset;
    uint32_t tamano;
} EspacioLibre;

typedef struct {
    ArchivoMetadata archivos[MAX_ARCHIVOS];
    EspacioLibre    espaciosLibres[MAX_ARCHIVOS];
    uint32_t        cantidadArchivos;
    uint32_t        cantidadHuecos;
} BloqueDirectorio;

typedef enum {
    BUCKET_OK            =  0,
    BUCKET_YA_EXISTE     = -1,
    BUCKET_ERROR         = -2,
    BUCKET_NO_ENCONTRADO = -3,
    BUCKET_LLENO         = -4
} EstadoBucket;

/* ---------- operaciones de bucket ---------- */
int crearBucket   (const char* nombre);
int existeBucket  (const char* nombre);
int eliminarBucket(const char* nombre, int forzar);

/* ---------- operaciones de archivo (stream) ---------- */
int subirArchivoStream    (const char* bucket, const char* clave,
                           int fd_origen,  uint32_t tamano);

int descargarArchivoStream(const char* bucket, const char* clave,
                           int fd_destino, uint32_t* tamano);

/* ---------- otras operaciones ---------- */
int eliminarArchivo(const char* bucket, const char* clave, int recursivo);
int listarContenido(const char* bucket, const char* prefijo,
                    char* salida, size_t* tamSalida);
int obtenerTamanoArchivo(const char* bucket, const char* clave,
                         uint32_t* tamano);
