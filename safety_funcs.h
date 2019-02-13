#ifndef _SAFETY_FUNCS_H_
#define _SAFETY_FUNCS_H_

#include "postgres.h"

#define exit_with_error(...) \
	do { \
		exit_with_error_to_stderr(__FILE__, __LINE__, PG_FUNCNAME_MACRO, __VA_ARGS__); \
	} while (0)
void exit_with_error_to_stderr(const char *filename, int lineno, const char *funcname, const char *fmt, ...) __attribute__((format(PG_PRINTF_ATTRIBUTE, 4, 5)));
void *malloc_fn(size_t size);
void *realloc_fn(void *ptr, size_t size);

#endif /* _SAFETY_FUNCS_H_ */
