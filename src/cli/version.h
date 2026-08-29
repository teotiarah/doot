#ifndef DOOT_VERSION_H
#define DOOT_VERSION_H

#define DOOT_VERSION "0.1.0-dev"

/* The build injects the git revision with -DDOOT_BUILD_REV=... so a binary can
 * always be traced to a commit. Amalgamated builds without the define report
 * "unknown", which is honest rather than misleading. */
#ifndef DOOT_BUILD_REV
#define DOOT_BUILD_REV "unknown"
#endif

#endif /* DOOT_VERSION_H */
