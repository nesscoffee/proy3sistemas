#include "../include/bucket_handler.h"
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>

int crear_bucket(char* nombre) {
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
