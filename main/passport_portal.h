// main/passport_portal.h
// The provisioning portal's request logic, kept pure so it can be host-tested.
//
// Three things live here rather than in the HTTP handler, because all three have failure
// modes worth testing without a browser:
//   * which handler a path selects, including the captive-portal probe paths that
//     operating systems use to decide whether a network needs a login page;
//   * decoding an application/x-www-form-urlencoded body, including %XX escapes and '+';
//   * escaping text before it is embedded in HTML, because the values in this portal come
//     from the network and end up in a page.
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    PASSPORT_PORTAL_ROUTE_ROOT = 0,
    PASSPORT_PORTAL_ROUTE_SAVE,
    PASSPORT_PORTAL_ROUTE_STATUS,
    PASSPORT_PORTAL_ROUTE_CONFIG,
    PASSPORT_PORTAL_ROUTE_REDIRECT,   // a captive-portal probe: answer with a redirect
    PASSPORT_PORTAL_ROUTE_NOT_FOUND,
} passport_portal_route_t;

// Select a handler for a request path. A query string is ignored, and a trailing slash is
// treated the same as none, so "/api/status?x=1" and "/api/status/" both work.
passport_portal_route_t passport_portal_route(const char *path);
const char *passport_portal_route_name(passport_portal_route_t route);

// Read one field from a form body, decoding %XX and '+'. Returns false when the field is
// absent. An empty but present field (for example "name=") returns true with an empty
// string, which is how a caller distinguishes "clear this" from "leave it alone".
bool passport_portal_form_get(const char *body, const char *key, char *out, size_t out_size);

// Escape `in` for HTML text or a double-quoted attribute. Returns the length written,
// excluding the terminator. A too-small buffer holds a truncated, still-escaped prefix.
size_t passport_portal_escape(const char *in, char *out, size_t out_size);

// The form fields the portal accepts, and the configuration field each one writes.
// Declared here rather than built inside the HTTP handler so a host test can prove that
// every accepted field really exists in the configuration schema — a typo would otherwise
// be discovered by a user whose password silently did not save.
//
// This one table drives both directions: the setup page is generated from it, and a submission is
// applied through it. A field added here appears in the form and becomes writable without a second
// list that can drift out of step. Whether a field is secret is asked of the schema rather than
// repeated here, so a key cannot become visible because someone forgot to mark it twice.
typedef struct {
    const char *form_key;      // the input's name attribute
    const char *config_field;  // the configuration field it writes
    const char *label;         // what the person reads
    const char *group;         // a heading; a change starts a new section
} passport_portal_field_t;

int passport_portal_field_count(void);
const passport_portal_field_t *passport_portal_field(int index);
