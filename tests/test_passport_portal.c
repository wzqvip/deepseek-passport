// Host tests for the provisioning portal's request logic.
#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "passport_portal.h"
#include "passport_config.h"

int main(void)
{
    // --- routing ------------------------------------------------------------
    assert(passport_portal_route("/") == PASSPORT_PORTAL_ROUTE_ROOT);
    assert(passport_portal_route("/index.html") == PASSPORT_PORTAL_ROUTE_ROOT);
    assert(passport_portal_route("/save") == PASSPORT_PORTAL_ROUTE_SAVE);
    assert(passport_portal_route("/api/status") == PASSPORT_PORTAL_ROUTE_STATUS);
    assert(passport_portal_route("/api/config") == PASSPORT_PORTAL_ROUTE_CONFIG);

    // A query string does not change the handler.
    assert(passport_portal_route("/api/status?t=1") == PASSPORT_PORTAL_ROUTE_STATUS);
    assert(passport_portal_route("/?next=/save") == PASSPORT_PORTAL_ROUTE_ROOT);
    // A single trailing slash is not significant.
    assert(passport_portal_route("/api/status/") == PASSPORT_PORTAL_ROUTE_STATUS);
    assert(passport_portal_route("/save/") == PASSPORT_PORTAL_ROUTE_SAVE);

    // Captive-portal probes, one per platform family.
    assert(passport_portal_route("/generate_204") == PASSPORT_PORTAL_ROUTE_REDIRECT);
    assert(passport_portal_route("/gen_204") == PASSPORT_PORTAL_ROUTE_REDIRECT);
    assert(passport_portal_route("/hotspot-detect.html") == PASSPORT_PORTAL_ROUTE_REDIRECT);
    assert(passport_portal_route("/library/test/success.html") == PASSPORT_PORTAL_ROUTE_REDIRECT);
    assert(passport_portal_route("/ncsi.txt") == PASSPORT_PORTAL_ROUTE_REDIRECT);
    assert(passport_portal_route("/connecttest.txt") == PASSPORT_PORTAL_ROUTE_REDIRECT);
    assert(passport_portal_route("/redirect") == PASSPORT_PORTAL_ROUTE_REDIRECT);

    // Everything else, including anything that does not start with a slash.
    assert(passport_portal_route("/nope") == PASSPORT_PORTAL_ROUTE_NOT_FOUND);
    assert(passport_portal_route("/api") == PASSPORT_PORTAL_ROUTE_NOT_FOUND);
    assert(passport_portal_route("/apistatus") == PASSPORT_PORTAL_ROUTE_NOT_FOUND);
    assert(passport_portal_route("api/status") == PASSPORT_PORTAL_ROUTE_NOT_FOUND);
    assert(passport_portal_route("") == PASSPORT_PORTAL_ROUTE_NOT_FOUND);
    assert(passport_portal_route(NULL) == PASSPORT_PORTAL_ROUTE_NOT_FOUND);
    // Case matters: these paths are compared exactly.
    assert(passport_portal_route("/API/STATUS") == PASSPORT_PORTAL_ROUTE_NOT_FOUND);

    // --- route names --------------------------------------------------------
    assert(strcmp(passport_portal_route_name(PASSPORT_PORTAL_ROUTE_ROOT), "root") == 0);
    assert(strcmp(passport_portal_route_name(PASSPORT_PORTAL_ROUTE_NOT_FOUND), "not-found") == 0);
    assert(strlen(passport_portal_route_name((passport_portal_route_t)99)) > 0);

    // --- form decoding ------------------------------------------------------
    char out[128];

    assert(passport_portal_form_get("ssid=MyNet&pass=secret", "ssid", out, sizeof(out)));
    assert(strcmp(out, "MyNet") == 0);
    assert(passport_portal_form_get("ssid=MyNet&pass=secret", "pass", out, sizeof(out)));
    assert(strcmp(out, "secret") == 0);

    // Absent keys are reported as absent, not as an empty value.
    assert(!passport_portal_form_get("ssid=MyNet", "key", out, sizeof(out)));
    assert(!passport_portal_form_get("", "ssid", out, sizeof(out)));
    // A field with no '=' is not a pair.
    assert(!passport_portal_form_get("ssid", "ssid", out, sizeof(out)));
    // A prefix of a key must not match.
    assert(!passport_portal_form_get("ssidname=x", "ssid", out, sizeof(out)));

    // A present but empty value is distinguishable from an absent one.
    assert(passport_portal_form_get("ssid=&pass=x", "ssid", out, sizeof(out)));
    assert(out[0] == '\0');
    assert(passport_portal_form_get("pass=x&ssid=", "ssid", out, sizeof(out)));
    assert(out[0] == '\0');

    // '+' is a space, and %XX is decoded.
    assert(passport_portal_form_get("name=My+Pet", "name", out, sizeof(out)));
    assert(strcmp(out, "My Pet") == 0);
    assert(passport_portal_form_get("p=a%2Bb%26c", "p", out, sizeof(out)));
    assert(strcmp(out, "a+b&c") == 0);
    assert(passport_portal_form_get("p=%41%42%43", "p", out, sizeof(out)));
    assert(strcmp(out, "ABC") == 0);
    // Lowercase hex works too.
    assert(passport_portal_form_get("p=%2f", "p", out, sizeof(out)));
    assert(strcmp(out, "/") == 0);

    // A percent-encoded UTF-8 value survives: this is a Chinese pet name arriving from a
    // browser form. E4 B8 AD is U+4E2D.
    assert(passport_portal_form_get("name=%E4%B8%AD%E6%96%87", "name", out, sizeof(out)));
    assert(strcmp(out, "\xE4\xB8\xAD\xE6\x96\x87") == 0);

    // A malformed escape is passed through rather than swallowing the following bytes.
    assert(passport_portal_form_get("p=%zz", "p", out, sizeof(out)));
    assert(strcmp(out, "%zz") == 0);
    assert(passport_portal_form_get("p=%4", "p", out, sizeof(out)));
    assert(strcmp(out, "%4") == 0);

    // The key itself may be encoded.
    assert(passport_portal_form_get("%73sid=x", "ssid", out, sizeof(out)));
    assert(strcmp(out, "x") == 0);

    // A key in a later field is still found.
    assert(passport_portal_form_get("a=1&b=2&c=3", "c", out, sizeof(out)));
    assert(strcmp(out, "3") == 0);

    // A value containing '=' keeps the later ones.
    assert(passport_portal_form_get("token=abc=def", "token", out, sizeof(out)));
    assert(strcmp(out, "abc=def") == 0);

    // Truncation is bounded and the string stays terminated.
    char small[4];
    assert(passport_portal_form_get("ssid=abcdefgh", "ssid", small, sizeof(small)));
    assert(strlen(small) == 3u);
    assert(small[3] == '\0');

    // Invalid arguments.
    assert(!passport_portal_form_get(NULL, "a", out, sizeof(out)));
    assert(!passport_portal_form_get("a=1", NULL, out, sizeof(out)));
    assert(!passport_portal_form_get("a=1", "a", NULL, sizeof(out)));
    assert(!passport_portal_form_get("a=1", "a", out, 0u));

    // --- HTML escaping ------------------------------------------------------
    char esc[256];

    passport_portal_escape("plain text", esc, sizeof(esc));
    assert(strcmp(esc, "plain text") == 0);

    // The characters that would let a value change the page's structure.
    passport_portal_escape("<script>alert('x')</script>", esc, sizeof(esc));
    assert(strcmp(esc, "&lt;script&gt;alert(&#39;x&#39;)&lt;/script&gt;") == 0);
    assert(strstr(esc, "<") == NULL);

    passport_portal_escape("a&b\"c", esc, sizeof(esc));
    assert(strcmp(esc, "a&amp;b&quot;c") == 0);

    // A value that closes an attribute and opens a tag.
    passport_portal_escape("\" onload=\"evil()", esc, sizeof(esc));
    assert(strstr(esc, "\"") == NULL);
    assert(strcmp(esc, "&quot; onload=&quot;evil()") == 0);

    // Escaping is bounded and stays terminated.
    char tiny[8];
    passport_portal_escape("<<<<<<<<", tiny, sizeof(tiny));
    assert(strlen(tiny) == 7u);
    assert(tiny[7] == '\0');
    assert(strstr(tiny, "<") == NULL);

    // Invalid arguments.
    assert(passport_portal_escape(NULL, esc, sizeof(esc)) == 0u);
    assert(passport_portal_escape("x", NULL, sizeof(esc)) == 0u);
    assert(passport_portal_escape("x", esc, 0u) == 0u);

    // --- the form-to-configuration mapping ----------------------------------
    // Every field the portal accepts must name a configuration field that exists. A typo
    // here would only be discovered by a user whose setting silently did not save.
    assert(passport_portal_field_count() > 0);
    assert(passport_portal_field(-1) == NULL);
    assert(passport_portal_field(passport_portal_field_count()) == NULL);

    bool saw_wifi = false;
    bool saw_key = false;
    for (int i = 0; i < passport_portal_field_count(); i++) {
        const passport_portal_field_t *f = passport_portal_field(i);
        assert(f != NULL);
        assert(f->form_key != NULL && f->form_key[0] != '\0');
        assert(f->config_field != NULL);
        // The page is generated from this table, so a missing label or heading would render as a
        // nameless input or a crash in the formatter.
        assert(f->label != NULL && f->label[0] != '\0');
        assert(f->group != NULL && f->group[0] != '\0');
        const passport_cfg_field_t *cfg = passport_cfg_find(f->config_field);
        assert(cfg != NULL);   // the schema knows this field
        // The form key must survive its own parser: a key containing '&' or '=' could
        // never arrive from a browser form.
        assert(strchr(f->form_key, '&') == NULL);
        assert(strchr(f->form_key, '=') == NULL);
        assert(strchr(f->form_key, '%') == NULL);
        // Two fields writing the same setting would silently overwrite each other.
        for (int j = i + 1; j < passport_portal_field_count(); j++) {
            const passport_portal_field_t *other = passport_portal_field(j);
            assert(strcmp(f->config_field, other->config_field) != 0);
            assert(strcmp(f->form_key, other->form_key) != 0);
        }
        if (strcmp(f->form_key, "wifi_ssid") == 0) {
            saw_wifi = true;
        }
        if (strcmp(f->form_key, "llm_api_key") == 0) {
            saw_key = true;
        }
    }
    // The two fields a first-time setup cannot do without are actually accepted.
    assert(saw_wifi);
    assert(saw_key);

    // Every settable configuration field should be reachable from the page, so a phone can do
    // whatever the console can. Counting them catches a field added to the schema and forgotten here.
    int writable = 0;
    for (int i = 0; i < passport_cfg_field_count(); i++) {
        const passport_cfg_field_t *cfg = passport_cfg_field(i);
        if (strcmp(cfg->name, "version") == 0) {
            continue;   // not a setting
        }
        writable++;
        bool listed = false;
        for (int j = 0; j < passport_portal_field_count() && !listed; j++) {
            listed = strcmp(passport_portal_field(j)->config_field, cfg->name) == 0;
        }
        assert(listed);
    }
    assert(passport_portal_field_count() == writable);

    return 0;
}
