#ifndef ORTHOX_KERNEL_STDLIB_H
#define ORTHOX_KERNEL_STDLIB_H

#include <stddef.h>

#ifndef EXIT_SUCCESS
#define EXIT_SUCCESS 0
#endif
#ifndef EXIT_FAILURE
#define EXIT_FAILURE 1
#endif

int atoi(const char* s);
char* getenv(const char* name);
int putenv(char* string);
int clearenv(void);
void free(void* ptr);
unsigned long strtoul(const char* nptr, char** endptr, int base);
_Noreturn void exit(int status);

#endif
