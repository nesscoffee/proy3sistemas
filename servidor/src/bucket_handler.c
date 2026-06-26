#include "../include/bucket_handler.h"
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

int crear_bucket(const char* nombre) {
   // rwxr-xr-x
   mkdir("buckets", 0755);
   char filename[256];
   sprintf(filename, "buckets/%s", nombre);

   FILE* archivo = fopen(filename, "wbx");
   if (archivo == NULL) {
       return (errno == EEXIST) ? BUCKET_ALREADY_EXISTS : BUCKET_ERROR;
   }

   BloqueDirectorio directorio = {};
   if( fwrite(&directorio, sizeof(BloqueDirectorio), 1, archivo) != 1 ) {
       fclose(archivo);
       return BUCKET_ERROR;
   }

   fclose(archivo);
   return BUCKET_OK;
}

int existe_bucket(const char *nombre) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "buckets/%s", nombre);

    struct stat st;
    return (stat(path, &st) == 0) ? 0 : -1;
}

int anadir_archivo(const char *nombre, const char *archivo) {
    if (existe_bucket(nombre) != 0) {
        return BUCKET_NOT_FOUND;
    }

    // sacar el tamaño para saber si cabe en algun hueco.
    FILE *src = fopen(archivo, "rb");
    if (src == NULL) {
        return BUCKET_ERROR;
    }
    fseek(src, 0, SEEK_END);
    long tamanoArchivo = ftell(src);
    if (tamanoArchivo <= 0) {
        fclose(src);
        return BUCKET_ERROR;
    }
    rewind(src);

    char bucket_path[MAX_PATH];
    snprintf(bucket_path, sizeof(bucket_path), "buckets/%s", nombre);
    FILE *bucket = fopen(bucket_path, "rb+");
    if (bucket == NULL) {
        fclose(src);
        return BUCKET_ERROR;
    }


    BloqueDirectorio dir;
    if (fread(&dir, sizeof(BloqueDirectorio), 1, bucket) != 1) {
        fclose(src);
        fclose(bucket);
        return BUCKET_ERROR;
    }

    if (dir.cantidad_archivos >= MAX_FILES) {
        fclose(src);
        fclose(bucket);
        return BUCKET_FULL;
    }

    uint32_t offset;
    int hueco_index = -1;

    // Siguiendo el first fit
    for (uint32_t i = 0; i < dir.cantidad_huecos; i++) {
        if (dir.espacios_libres[i].size >= (uint32_t)tamanoArchivo) {
            hueco_index = (int)i;
            break;
        }
    }

    if (hueco_index >= 0) {
        EspacioLibre *hueco = &dir.espacios_libres[hueco_index];
        offset = hueco->offset;
        uint32_t restante = hueco->size - (uint32_t)tamanoArchivo;

        // Correr los huecos a la izquierda desde el que se metio
        for (uint32_t i = (uint32_t)hueco_index; i < dir.cantidad_huecos - 1; i++) {
            dir.espacios_libres[i] = dir.espacios_libres[i + 1];
        }
        dir.cantidad_huecos--;

        if (restante > 0) {
            if (dir.cantidad_huecos < MAX_FILES) {
                dir.espacios_libres[dir.cantidad_huecos].offset = offset + (uint32_t)tamanoArchivo;
                dir.espacios_libres[dir.cantidad_huecos].size    = restante;
                dir.cantidad_huecos++;
            }
        }
    } else {
        // No hay ningún hueco, entonces al final
        fseek(bucket, 0, SEEK_END);
        long end_offset = ftell(bucket);
        if (end_offset < 0) {
            fclose(src);
            fclose(bucket);
            return BUCKET_ERROR;
        }
        offset = (uint32_t)end_offset;
    }

    fseek(bucket, offset, SEEK_SET);
    char buffer[8192];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, bytes, bucket) != bytes) {
            fclose(src);
            fclose(bucket);
            return BUCKET_ERROR;
        }
    }

    strncpy(dir.files[dir.cantidad_archivos].path, archivo, MAX_PATH - 1);
    dir.files[dir.cantidad_archivos].path[MAX_PATH - 1] = '\0';
    dir.files[dir.cantidad_archivos].size   = (uint32_t)tamanoArchivo;
    dir.files[dir.cantidad_archivos].offset = offset;
    dir.cantidad_archivos++;

    rewind(bucket);
    if (fwrite(&dir, sizeof(BloqueDirectorio), 1, bucket) != 1) {
        fclose(src);
        fclose(bucket);
        return BUCKET_ERROR;
    }

    fclose(src);
    fclose(bucket);
    return BUCKET_OK;
}
