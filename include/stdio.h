#ifndef ORTHOX_KERNEL_STDIO_H
#define ORTHOX_KERNEL_STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef struct _IO_FILE FILE;
extern FILE* stdout;
extern FILE* stderr;

int printf(const char* fmt, ...);
int fprintf(FILE* stream, const char* fmt, ...);
int puts(const char* s);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);
int fflush(FILE* stream);
void perror(const char* s);
/*
 * Kernel-local minimal snprintf. Supports %% %c %s %d %i %u %x %X %p
 * and l/ll integer variants. Decimal field width, '-' left alignment, and
 * '0' zero padding are supported. Precision, floating point, and locale
 * formatting are intentionally unsupported.
 */
int snprintf(char* dst, size_t size, const char* fmt, ...);
int vsnprintf(char* dst, size_t size, const char* fmt, va_list ap);

#endif
