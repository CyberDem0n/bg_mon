#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned long long du(const char *start);

/******************************************************
 * implementation of du -s functionality              *
 * works recursively, returns total size in kilobytes *
 * TODO: track device id                              *
 ******************************************************/
static unsigned long long du(const char *start)
{
	struct stat st;
	char path[256];
	struct dirent *e;
	unsigned long long ret = 0;
	DIR *d;
	size_t path_len;

	if (lstat(start, &st)) return ret;
	ret += st.st_blocks/2;
	if ((st.st_mode & S_IFMT) != S_IFDIR || (d = opendir(start)) == NULL) return ret;

	path_len = strlen(start);
	strcpy(path, start);
	if (path[path_len - 1] != '/')
		path[path_len++] = '/';

	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' && (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
			continue;
		if (path_len + strlen(e->d_name) >= sizeof(path))
			continue;
		strcpy(path + path_len, e->d_name);
		ret += du(path);
	}
	closedir(d);
	return ret;
}

int main(int argc, char **argv)
{
	const char path[] = ".";
	printf("%llu\t%s\n", du(path), path);
	return 0;
}
