#pragma once

#include <stdint.h>
#define MAX_PATH 256
#define MAX_FILES 100

typedef struct {
    char     path[MAX_PATH];
    uint32_t size;
    uint32_t offset;
} ArchivoMetadata;

typedef struct {
    uint32_t offset;
    uint32_t size;
} EspacioLibre;

typedef struct {
    ArchivoMetadata files[MAX_FILES];
    EspacioLibre    espacios_libres[MAX_FILES];
    uint32_t        cantidad_archivos;
    uint32_t        cantidad_huecos;
} BloqueDirectorio;

typedef enum {
    BUCKET_OK = 0,
    BUCKET_ALREADY_EXISTS = -1,
    BUCKET_ERROR = -2
} BucketStatus;

int crear_bucket(char *nombre);
