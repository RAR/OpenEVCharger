/* Reconnect backoff: double from 1s, cap at 60s. Header-only (pure). */
#ifndef BACKOFF_H
#define BACKOFF_H

#define BACKOFF_MAX_S 60

static inline int backoff_next(int cur_s)
{
    int n = (cur_s < 1) ? 1 : cur_s * 2;
    return n > BACKOFF_MAX_S ? BACKOFF_MAX_S : n;
}

#endif /* BACKOFF_H */
