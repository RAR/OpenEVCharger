/* Embedded SPA HTML/CSS/JS for the delta-bridge web UI.
 *
 * Inlined as a plain C string (rather than xxd-generated) so the source tree
 * has one fewer build-time dependency. The file IS the SPA — edit here.
 *
 * Kept under ~6 KB so the whole response fits comfortably inside RESP_CAP
 * (8 KB). Vanilla JS, no framework, no third-party assets, no icon set —
 * everything renders from the inlined CSS.
 */
#ifndef WEB_HTML_H
#define WEB_HTML_H

#include <stddef.h>

/* Placeholder; the real SPA lands in a later commit. */
static const char INDEX_HTML[] =
    "<!DOCTYPE html>"
    "<html><head><meta charset=\"utf-8\">"
    "<title>delta-bridge</title></head>"
    "<body><h1>delta-bridge</h1>"
    "<p>SPA scaffolding — full UI coming in v0.4 commit 4.</p>"
    "</body></html>";

#define INDEX_HTML_LEN (sizeof(INDEX_HTML) - 1)

#endif /* WEB_HTML_H */
