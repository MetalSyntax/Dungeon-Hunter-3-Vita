#ifndef SOLOADER_VFILE_H
#define SOLOADER_VFILE_H

/* Virtual read-only FILE handles for the OBB + stdio dispatchers.
 * See vfile.c. */

#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int vf_is_virtual(FILE *f);
FILE *vf_fopen(const char *path, const char *mode);
int vf_fclose(FILE *f);
size_t vf_fread(void *ptr, size_t size, size_t count, FILE *f);
size_t vf_fwrite(const void *ptr, size_t size, size_t count, FILE *f);
int vf_fseek(FILE *f, long off, int whence);
long vf_ftell(FILE *f);
int vf_fseeko(FILE *f, off_t off, int whence);
off_t vf_ftello(FILE *f);
void vf_rewind(FILE *f);
int vf_feof(FILE *f);
int vf_ferror(FILE *f);
int vf_fflush(FILE *f);
int vf_setvbuf(FILE *f, char *buf, int mode, size_t size);
int vf_fileno(FILE *f);
int vf_fgetc(FILE *f);
int vf_getc(FILE *f);
int vf_ungetc(int c, FILE *f);
char *vf_fgets(char *s, int n, FILE *f);
int vf_fputc(int c, FILE *f);
int vf_putc(int c, FILE *f);
int vf_fputs(const char *s, FILE *f);
int vf_vfprintf(FILE *f, const char *fmt, va_list ap);
int vf_fprintf(FILE *f, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
