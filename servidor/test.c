#include "include/bucket_handler.h"
#include <assert.h>
#include <stdio.h>


int main() {
    int result = crear_bucket("test");
    printf("Result: %d\n", result);
    assert(result == 0);
}
