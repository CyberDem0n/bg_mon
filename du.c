#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stddef.h>

#define FATAL 1
#define ERROR 1
#define LOG 1
#define elog(level, format, ...) fprintf(stderr, format "\n", __VA_ARGS__)
#define false 0
#define true 1
typedef char bool;

static size_t get_name_max(DIR * dirp);
static unsigned long long du(const char *start, dev_t dev, bool track_device);

static size_t get_name_max(DIR * dirp)
{
	long name_max = -1;
#if defined(HAVE_FPATHCONF) && defined(HAVE_DIRFD) \
			&& defined(_PC_NAME_MAX)
	name_max = fpathconf(dirfd(dirp), _PC_NAME_MAX);
#endif
	if (name_max == -1)
#if defined(NAME_MAX)
		name_max = NAME_MAX;
#else
		name_max = 255;
#endif
	return (name_max < 255) ? 255 : name_max;
}

/******************************************************
 * implementation of du -s and du -sx functionality   *
 * works recursively, returns total size in kilobytes *
 ******************************************************/
static unsigned long long du(const char *start, dev_t dev, bool track_device)
{
	struct stat st;
	char *path;
	struct dirent *buf, *e;
	unsigned long long ret = 0;
	DIR *d;
	size_t path_len, path_max, name_max, dirent_size;

	if (lstat(start, &st)) return ret;
	if (track_device && !dev) dev = st.st_dev;
	if (track_device && st.st_dev != dev) return ret;

	ret += st.st_blocks/2;

	if ((st.st_mode & S_IFMT) != S_IFDIR || (d = opendir(start)) == NULL) return ret;

	name_max = get_name_max(d);

	if ((dirent_size = offsetof(struct dirent, d_name) + name_max + 1) < sizeof(struct dirent));
		dirent_size = sizeof(struct dirent);

	if ((buf = malloc(dirent_size)) == NULL) {
		elog(ERROR, "Can not allocate %lu bytes for dirent", dirent_size);
		goto du_ret;
	}

	path_len = strlen(start);
	path_len += start[path_len - 1] != '/';

	path_max = name_max + path_len + 1;
	if ((path = malloc(path_max)) == NULL) {
		elog(ERROR, "Can not allocate %lu bytes for path", path_max);
		free(buf);
		goto du_ret;
	}

	strcpy(path, start);
	path[path_len - 1] = '/';

	while (readdir_r(d, buf, &e) == 0 && e != NULL) {
		if (e->d_name[0] == '.' && (e->d_name[1] == '\0' || (e->d_name[1] == '.' && e->d_name[2] == '\0')))
			continue;
		if (path_len + strlen(e->d_name) >= path_max)
			continue;
		strcpy(path + path_len, e->d_name);
		ret += du(path, dev, track_device);
	}
	free(path);
	free(buf);
du_ret:
	closedir(d);
	return ret;
}

int main(int argc, char **argv)
{
	const char path[] = ".";
	printf("%llu\t%s\n", du(path, 0, true), path);
	return 0;
}
