#ifndef OPENBHZD_PROTO_BUILD_INFO_H
#define OPENBHZD_PROTO_BUILD_INFO_H

/* Set OPENBHZD_VERSION via cmake -DOPENBHZD_VERSION="x.y.z" or
 * leave as the default. Likewise OPENBHZD_GIT_SHA can come from
 * `git rev-parse --short HEAD` at build time. */
#ifndef OPENBHZD_VERSION
#define OPENBHZD_VERSION "0.1.0-dev"
#endif
#ifndef OPENBHZD_GIT_SHA
#define OPENBHZD_GIT_SHA "unset"
#endif

#endif /* OPENBHZD_PROTO_BUILD_INFO_H */
