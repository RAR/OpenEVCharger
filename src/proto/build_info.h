#ifndef OPENEVCHARGER_PROTO_BUILD_INFO_H
#define OPENEVCHARGER_PROTO_BUILD_INFO_H

/* Set OPENEVCHARGER_VERSION via cmake -DOPENEVCHARGER_VERSION="x.y.z" or
 * leave as the default. Likewise OPENEVCHARGER_GIT_SHA can come from
 * `git rev-parse --short HEAD` at build time. */
#ifndef OPENEVCHARGER_VERSION
#define OPENEVCHARGER_VERSION "0.1.0-dev"
#endif
#ifndef OPENEVCHARGER_GIT_SHA
#define OPENEVCHARGER_GIT_SHA "unset"
#endif

#endif /* OPENEVCHARGER_PROTO_BUILD_INFO_H */
