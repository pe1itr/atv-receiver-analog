#ifndef ATV_VERSION_H
#define ATV_VERSION_H
/* Application release number; update when publishing a new version. */
#define ATV_VERSION "0.1.1"
/* The Makefile embeds the UTC build time in each receiver compilation. */
#ifndef ATV_BUILD_UTC
#define ATV_BUILD_UTC "UNKNOWN"
#endif
#endif
