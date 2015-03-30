#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <stddef.h>

#define FATAL 1
#define ERROR 1
#define LOG 1
#define elog(level, format, ...) fprintf(stderr, format "\n", __VA_ARGS__)
#define false 0
#define true 1
typedef char bool;

static size_t get_dirent_size(int dirfd);
static unsigned long long du(int dirfd, const char *path, dev_t dev, bool track_device);

static size_t get_dirent_size(int dirfd)
{
	size_t dirent_size;
	long name_max = -1;
#if defined(HAVE_FPATHCONF) && defined(HAVE_DIRFD) \
			&& defined(_PC_NAME_MAX)
	name_max = fpathconf(dirfd, _PC_NAME_MAX);
#endif
	if (name_max == -1)
#if defined(NAME_MAX)
		name_max = NAME_MAX;
#else
		name_max = 255;
#endif

	if (name_max < 255) name_max = 255;
	dirent_size = offsetof(struct dirent, d_name) + name_max + 1;

	return dirent_size < sizeof(struct dirent) ? sizeof(struct dirent) : dirent_size;
}

/******************************************************
 * implementation of du -s and du -sx functionality
 * works recursively, returns total size in kilobytes
 ******************************************************/
static unsigned long long
du(int dirfd, const char *path, dev_t dev, bool track_device)
{
	struct stat st;
	unsigned long long ret = 0;
	struct dirent *buf, *e;
	size_t dirent_size;
	DIR *dir;

	if (fstatat(dirfd, path, &st, dirfd == AT_FDCWD ? 0 : AT_SYMLINK_NOFOLLOW))
		return ret;

	if (track_device && !dev) dev = st.st_dev;
	if (track_device && st.st_dev != dev) return ret;

	ret += st.st_blocks/2;

	if ((st.st_mode & S_IFMT) != S_IFDIR
			|| (dirfd = openat(dirfd, path, O_RDONLY|O_NOCTTY|O_DIRECTORY|O_NOFOLLOW)) == -1)
		return ret;

	if ((dir = fdopendir(dirfd)) == NULL) {
		close(dirfd);
		return ret;
	}

	dirent_size = get_dirent_size(dirfd);
	if ((buf = malloc(dirent_size))) {
		while (readdir_r(dir, buf, &e) == 0 && e != NULL)
			// skip "." and ".."
			if (e->d_name[0] != '.'
					|| (e->d_name[1] && (e->d_name[1] != '.' || e->d_name[2]))
					|| !strncmp(e->d_name, "lost+found", 11))
				ret += du(dirfd, e->d_name, dev, track_device);
		free(buf);
	} else elog(ERROR, "Can not allocate %lu bytes for dirent", dirent_size);

	closedir(dir); // close dirfd as well
	return ret;
}

int main(int argc, char **argv)
{
	const char path[] = ".";
	printf("%llu\t%s\n", du(AT_FDCWD, path, 0, true), path);
	return 0;
}
