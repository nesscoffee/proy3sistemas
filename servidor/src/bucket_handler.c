#include "../include/bucket_handler.h"
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>

static void mergeHuecos(EspacioLibre* huecos, uint32_t* n) {
    for (uint32_t i = 0; i < *n; i++) {
        for (uint32_t j = i + 1; j < *n; j++) {
            if (huecos[j].offset < huecos[i].offset) {
                EspacioLibre t = huecos[i];
                huecos[i] = huecos[j];
                huecos[j] = t;
            }
        }
    }
    uint32_t w = 0;
    for (uint32_t i = 0; i < *n; i++) {
        if (w > 0 && huecos[w-1].offset + huecos[w-1].tamano >= huecos[i].offset) {
            uint32_t fin = huecos[i].offset + huecos[i].tamano;
            uint32_t curFin = huecos[w-1].offset + huecos[w-1].tamano;
            if (fin > curFin)
                huecos[w-1].tamano = fin - curFin;
        } else {
            huecos[w++] = huecos[i];
        }
    }
    *n = w;
}

int crearBucket(const char* nombre) {
    mkdir("buckets", 0755);
    char ruta[256];
    snprintf(ruta, sizeof(ruta), "buckets/%s", nombre);

    FILE* arch = fopen(ruta, "wbx");
    if (arch == NULL)
        return (errno == EEXIST) ? BUCKET_YA_EXISTE : BUCKET_ERROR;

    BloqueDirectorio dir = {0};
    if (fwrite(&dir, sizeof(dir), 1, arch) != 1) {
        fclose(arch);
        return BUCKET_ERROR;
    }
    fclose(arch);
    return BUCKET_OK;
}

int existeBucket(const char* nombre) {
    char ruta[MAX_RUTA];
    snprintf(ruta, sizeof(ruta), "buckets/%s", nombre);
    struct stat st;
    return (stat(ruta, &st) == 0) ? 0 : -1;
}

int subirArchivo(const char* nombre, const char* clave, const void* datos, uint32_t tamano) {
    if (existeBucket(nombre) != 0) return BUCKET_NO_ENCONTRADO;

    char ruta[256];
    snprintf(ruta, sizeof(ruta), "buckets/%s", nombre);
    FILE* arch = fopen(ruta, "rb+");
    if (arch == NULL) return BUCKET_ERROR;

    BloqueDirectorio dir;
    if (fread(&dir, sizeof(dir), 1, arch) != 1) { fclose(arch); return BUCKET_ERROR; }

    int existente = -1;
    for (uint32_t i = 0; i < dir.cantidadArchivos; i++) {
        if (strcmp(dir.archivos[i].ruta, clave) == 0) {
            existente = (int)i;
            break;
        }
    }

    uint32_t offset;

    if (existente >= 0) {
        ArchivoMetadata* meta = &dir.archivos[existente];
        if (meta->tamano == tamano) {
            meta->tamano = tamano;
            fseek(arch, meta->offset, SEEK_SET);
            if (fwrite(datos, 1, tamano, arch) != tamano) { fclose(arch); return BUCKET_ERROR; }
            rewind(arch);
            if (fwrite(&dir, sizeof(dir), 1, arch) != 1) { fclose(arch); return BUCKET_ERROR; }
            fclose(arch);
            return BUCKET_OK;
        }
        if (dir.cantidadHuecos < MAX_ARCHIVOS) {
            dir.espaciosLibres[dir.cantidadHuecos].offset = meta->offset;
            dir.espaciosLibres[dir.cantidadHuecos].tamano = meta->tamano;
            dir.cantidadHuecos++;
            mergeHuecos(dir.espaciosLibres, &dir.cantidadHuecos);
        }
        for (uint32_t i = (uint32_t)existente; i < dir.cantidadArchivos - 1; i++)
            dir.archivos[i] = dir.archivos[i + 1];
        dir.cantidadArchivos--;
    }

    if (dir.cantidadArchivos >= MAX_ARCHIVOS) { fclose(arch); return BUCKET_LLENO; }

    int huecoIdx = -1;
    for (uint32_t i = 0; i < dir.cantidadHuecos; i++) {
        if (dir.espaciosLibres[i].tamano >= tamano) { huecoIdx = (int)i; break; }
    }

    if (huecoIdx >= 0) {
        EspacioLibre* h = &dir.espaciosLibres[huecoIdx];
        offset = h->offset;
        uint32_t restante = h->tamano - tamano;
        for (uint32_t i = (uint32_t)huecoIdx; i < dir.cantidadHuecos - 1; i++)
            dir.espaciosLibres[i] = dir.espaciosLibres[i + 1];
        dir.cantidadHuecos--;
        if (restante > 0 && dir.cantidadHuecos < MAX_ARCHIVOS) {
            dir.espaciosLibres[dir.cantidadHuecos].offset = offset + tamano;
            dir.espaciosLibres[dir.cantidadHuecos].tamano = restante;
            dir.cantidadHuecos++;
            mergeHuecos(dir.espaciosLibres, &dir.cantidadHuecos);
        }
    } else {
        fseek(arch, 0, SEEK_END);
        long fin = ftell(arch);
        if (fin < 0) { fclose(arch); return BUCKET_ERROR; }
        offset = (uint32_t)fin;
    }

    fseek(arch, offset, SEEK_SET);
    if (fwrite(datos, 1, tamano, arch) != tamano) { fclose(arch); return BUCKET_ERROR; }

    strncpy(dir.archivos[dir.cantidadArchivos].ruta, clave, MAX_RUTA - 1);
    dir.archivos[dir.cantidadArchivos].ruta[MAX_RUTA - 1] = '\0';
    dir.archivos[dir.cantidadArchivos].tamano = tamano;
    dir.archivos[dir.cantidadArchivos].offset = offset;
    dir.cantidadArchivos++;

    rewind(arch);
    if (fwrite(&dir, sizeof(dir), 1, arch) != 1) { fclose(arch); return BUCKET_ERROR; }
    fclose(arch);
    return BUCKET_OK;
}

int descargarArchivo(const char* nombre, const char* clave, void** datos, uint32_t* tamano) {
    if (existeBucket(nombre) != 0) return BUCKET_NO_ENCONTRADO;

    char ruta[256];
    snprintf(ruta, sizeof(ruta), "buckets/%s", nombre);
    FILE* arch = fopen(ruta, "rb");
    if (!arch) return BUCKET_ERROR;

    BloqueDirectorio dir;
    if (fread(&dir, sizeof(dir), 1, arch) != 1) { fclose(arch); return BUCKET_ERROR; }

    for (uint32_t i = 0; i < dir.cantidadArchivos; i++) {
        if (strcmp(dir.archivos[i].ruta, clave) == 0) {
            ArchivoMetadata* meta = &dir.archivos[i];
            *tamano = meta->tamano;
            *datos = malloc(meta->tamano);
            if (!*datos) { fclose(arch); return BUCKET_ERROR; }
            fseek(arch, meta->offset, SEEK_SET);
            if (fread(*datos, 1, meta->tamano, arch) != meta->tamano) {
                free(*datos); *datos = NULL; fclose(arch); return BUCKET_ERROR;
            }
            fclose(arch);
            return BUCKET_OK;
        }
    }

    fclose(arch);
    return BUCKET_NO_ENCONTRADO;
}

int eliminarArchivo(const char* nombre, const char* clave, int recursivo) {
    if (existeBucket(nombre) != 0) return BUCKET_NO_ENCONTRADO;

    char ruta[256];
    snprintf(ruta, sizeof(ruta), "buckets/%s", nombre);
    FILE* arch = fopen(ruta, "rb+");
    if (!arch) return BUCKET_ERROR;

    BloqueDirectorio dir;
    if (fread(&dir, sizeof(dir), 1, arch) != 1) { fclose(arch); return BUCKET_ERROR; }

    uint32_t elim = 0;
    size_t longClave = strlen(clave);

    for (uint32_t i = 0; i < dir.cantidadArchivos; ) {
        int coincide = recursivo
            ? (strncmp(dir.archivos[i].ruta, clave, longClave) == 0)
            : (strcmp(dir.archivos[i].ruta, clave) == 0);

        if (coincide) {
            if (dir.cantidadHuecos < MAX_ARCHIVOS) {
                dir.espaciosLibres[dir.cantidadHuecos].offset = dir.archivos[i].offset;
                dir.espaciosLibres[dir.cantidadHuecos].tamano = dir.archivos[i].tamano;
                dir.cantidadHuecos++;
            }
            for (uint32_t j = i; j < dir.cantidadArchivos - 1; j++)
                dir.archivos[j] = dir.archivos[j + 1];
            dir.cantidadArchivos--;
            elim++;
        } else {
            i++;
        }
    }

    if (elim == 0) { fclose(arch); return BUCKET_NO_ENCONTRADO; }

    mergeHuecos(dir.espaciosLibres, &dir.cantidadHuecos);

    rewind(arch);
    if (fwrite(&dir, sizeof(dir), 1, arch) != 1) { fclose(arch); return BUCKET_ERROR; }
    fclose(arch);
    return BUCKET_OK;
}

int eliminarBucket(const char* nombre, int forzar) {
    if (existeBucket(nombre) != 0) return BUCKET_NO_ENCONTRADO;

    if (!forzar) {
        char ruta[256];
        snprintf(ruta, sizeof(ruta), "buckets/%s", nombre);
        FILE* arch = fopen(ruta, "rb");
        if (!arch) return BUCKET_ERROR;
        BloqueDirectorio dir;
        if (fread(&dir, sizeof(dir), 1, arch) != 1) { fclose(arch); return BUCKET_ERROR; }
        fclose(arch);
        if (dir.cantidadArchivos > 0) return BUCKET_ERROR;
    }

    char ruta[256];
    snprintf(ruta, sizeof(ruta), "buckets/%s", nombre);
    return (remove(ruta) == 0) ? BUCKET_OK : BUCKET_ERROR;
}

int listarContenido(const char* bucket, const char* prefijo, char* salida, size_t* tamSalida) {
    size_t pos = 0;

    if (bucket == NULL) {
        DIR* d = opendir("buckets");
        if (d) {
            struct dirent* entrada;
            while ((entrada = readdir(d)) != NULL) {
                if (entrada->d_name[0] == '.') continue;
                struct stat st;
                char rc[512];
                snprintf(rc, sizeof(rc), "buckets/%s", entrada->d_name);
                if (stat(rc, &st) != 0 || !S_ISREG(st.st_mode)) continue;
                int n = snprintf(salida + pos, *tamSalida - pos, "%s\n", entrada->d_name);
                if (n < 0 || (size_t)n >= *tamSalida - pos) break;
                pos += (size_t)n;
            }
            closedir(d);
        }
        snprintf(salida + pos, *tamSalida - pos, ".\n");
        pos += 2;
        *tamSalida = pos;
        return BUCKET_OK;
    }

    if (existeBucket(bucket) != 0) return BUCKET_NO_ENCONTRADO;

    char ruta[256];
    snprintf(ruta, sizeof(ruta), "buckets/%s", bucket);
    FILE* arch = fopen(ruta, "rb");
    if (!arch) return BUCKET_ERROR;

    BloqueDirectorio dir;
    if (fread(&dir, sizeof(dir), 1, arch) != 1) { fclose(arch); return BUCKET_ERROR; }
    fclose(arch);

    size_t longPrefijo = prefijo ? strlen(prefijo) : 0;

    for (uint32_t i = 0; i < dir.cantidadArchivos; i++) {
        if (prefijo && strncmp(dir.archivos[i].ruta, prefijo, longPrefijo) != 0) continue;
        int n = snprintf(salida + pos, *tamSalida - pos, "%s|%u\n",
                         dir.archivos[i].ruta, dir.archivos[i].tamano);
        if (n < 0 || (size_t)n >= *tamSalida - pos) break;
        pos += (size_t)n;
    }

    snprintf(salida + pos, *tamSalida - pos, ".\n");
    pos += 2;
    *tamSalida = pos;
    return BUCKET_OK;
}
