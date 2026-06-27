#pragma once

#include <stdint.h>
#include <stddef.h>

#define MAX_RUTA 256
#define MAX_ARCHIVOS 100

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
    BUCKET_OK = 0,
    BUCKET_YA_EXISTE = -1,
    BUCKET_ERROR = -2,
    BUCKET_NO_ENCONTRADO = -3,
    BUCKET_LLENO = -4
} EstadoBucket;

int crearBucket(const char* nombre);
int existeBucket(const char* nombre);
int subirArchivo(const char* bucket, const char* clave, const void* datos, uint32_t tamano);
int descargarArchivo(const char* bucket, const char* clave, void** datos, uint32_t* tamano);
int eliminarArchivo(const char* bucket, const char* clave, int recursivo);
int eliminarBucket(const char* nombre, int forzar);
int listarContenido(const char* bucket, const char* prefijo, char* salida, size_t* tamSalida);
