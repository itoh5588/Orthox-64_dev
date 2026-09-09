#ifndef _UNISTD_H_
#define _UNISTD_H_

# include <sys/unistd.h>

#ifndef _FCHDIR_DECLARED
#define _FCHDIR_DECLARED
int fchdir (int __fildes);
#endif

#ifndef _READLINK_DECLARED
#define _READLINK_DECLARED
ssize_t readlink (const char *__restrict __path, char *__restrict __buf, size_t __buflen);
#endif

#endif /* _UNISTD_H_ */
