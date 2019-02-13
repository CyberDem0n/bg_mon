#include <stdlib.h>

#include "safety_funcs.h"
#include "storage/ipc.h"

void exit_with_error_to_stderr(const char *filename, int lineno, const char *funcname, const char *fmt, ...)
{
	va_list	ap;

	fprintf(stderr, "error occurred at %s:%d %s(): ",
			filename ? filename : "(unknown file)", lineno, funcname ? funcname : "unknownFunc");

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	fflush(stderr);

	proc_exit(1);
}

void *realloc_fn(void *ptr, size_t size)
{
	void *ret = realloc(ptr, size);

	if (ret == NULL)
		exit_with_error("Failed on request of size %zu in BgMon\n", size);

	return ret;
}

void *malloc_fn(size_t size)
{
	return realloc_fn(NULL, size);
}
