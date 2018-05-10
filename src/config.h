#ifndef __CONFIG_H
#define __CONFIG_H

#ifdef __APPLE__
#include <AvailabilityMacros.h>
#endif

#ifdef __linux__
#include <linux/version.h>
#include <features.h>
#endif

#define NDEBUG

/* Enable debugging zmalloc */
#define DEBUG_ZMALLOC

#endif /* __CONFIG_H */
