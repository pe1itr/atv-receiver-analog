#ifndef ATV_PLATFORM_H
#define ATV_PLATFORM_H
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#define ATV_DEFAULT_IMAGES "uhd-images"
#define fsync _commit
/* Create exclusively; never use an unchecked temporary filename. */
static inline int atv_mkstemp(char *path)
{
    size_t n=strlen(path);
    static unsigned sequence;
    for(unsigned attempt=0;attempt<256;++attempt) {
        unsigned v=(GetCurrentProcessId()*2654435761u+GetTickCount()+sequence++)&0xffffffu;
        snprintf(path+n-6,7,"%06x",v);
        int fd=_open(path,_O_CREAT|_O_EXCL|_O_RDWR|_O_BINARY,_S_IREAD|_S_IWRITE);
        if(fd>=0 || errno!=EEXIST) return fd;
    }
    errno=EEXIST; return -1;
}
#define mkstemp atv_mkstemp
static inline struct tm *atv_gmtime_r(const time_t *t,struct tm *out)
{ return gmtime_s(out,t)==0?out:NULL; }
#define gmtime_r atv_gmtime_r
static inline int atv_replace_file(const char *from,const char *to)
{
    if(MoveFileExA(from,to,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return 0;
    errno=EACCES; return -1;
}
#else
#define ATV_DEFAULT_IMAGES "uhd-images"
#define atv_replace_file rename
#endif
#endif
