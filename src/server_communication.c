// server_communication.c
#define _POSIX_C_SOURCE 200809L
#include "server_communication.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char *g_server_url = NULL;
static int   g_curl_initialized = 0;

// simple JSON string escaper (returns malloc'd string, caller must free)
// Escapes quotes, backslashes, and control characters into \uXXXX for safety.
static char *json_escape(const char *s) {
    if (!s) return NULL;
    size_t len = 0;
    for (const unsigned char *p = (const unsigned char*)s; *p; ++p) {
        unsigned char c = *p;
        if (c == '\"' || c == '\\' || c == '/' || c == '\b' || c == '\f' ||
            c == '\n' || c == '\r' || c == '\t') {
            len += 2; // escape sequences like \" or \\ or \n
        } else if (c < 0x20) {
            len += 6; // \u00XX
        } else {
            len += 1;
        }
    }
    char *out = malloc(len + 1);
    if (!out) return NULL;
    char *q = out;
    for (const unsigned char *p = (const unsigned char*)s; *p; ++p) {
        unsigned char c = *p;
        switch (c) {
        case '\"': *q++ = '\\'; *q++ = '\"'; break;
        case '\\': *q++ = '\\'; *q++ = '\\'; break;
        case '/':  *q++ = '\\'; *q++ = '/';  break;
        case '\b': *q++ = '\\'; *q++ = 'b';  break;
        case '\f': *q++ = '\\'; *q++ = 'f';  break;
        case '\n': *q++ = '\\'; *q++ = 'n';  break;
        case '\r': *q++ = '\\'; *q++ = 'r';  break;
        case '\t': *q++ = '\\'; *q++ = 't';  break;
        default:
            if (c < 0x20) {
                // control -> \u00XX
                static const char hex[] = "0123456789abcdef";
                *q++ = '\\'; *q++ = 'u'; *q++ = '0'; *q++ = '0';
                *q++ = hex[(c >> 4) & 0xF];
                *q++ = hex[c & 0xF];
            } else {
                *q++ = c;
            }
        }
    }
    *q = '\0';
    return out;
}

int server_comm_init(const char *server_url) {
    if (!server_url) return -1;
    if (g_curl_initialized) return 0; // already init

    g_server_url = strdup(server_url);
    if (!g_server_url) return -1;

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        free(g_server_url); g_server_url = NULL;
        return -1;
    }
    g_curl_initialized = 1;
    return 0;
}

int send_qr_to_server(const char *qr_data) {
    if (!g_curl_initialized || !g_server_url || !qr_data) return -1;

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    int rc = -1;
    char *escaped = json_escape(qr_data);
    if (!escaped) {
        curl_easy_cleanup(curl);
        return -1;
    }

    // build JSON payload (small, fixed fields). If you add more fields, adjust buffer.
    const char *template = "{ \"qr\": \"%s\" }";
    size_t payload_len = strlen(template) + strlen(escaped) + 1;
    char *payload = malloc(payload_len);
    if (!payload) {
        free(escaped);
        curl_easy_cleanup(curl);
        return -1;
    }
    snprintf(payload, payload_len, template, escaped);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, g_server_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(payload));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L); // 5 sec timeout
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // safe for threaded apps / signals

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code >= 200 && http_code < 300) {
            rc = 0; // success
        } else {
            fprintf(stderr, "send_qr_to_server: HTTP %ld\n", http_code);
            rc = -1;
        }
    } else {
        fprintf(stderr, "send_qr_to_server: curl error: %s\n", curl_easy_strerror(res));
        rc = -1;
    }

    curl_slist_free_all(headers);
    free(payload);
    free(escaped);
    curl_easy_cleanup(curl);
    return rc;
}

void server_comm_cleanup(void) {
    if (g_server_url) {
        free(g_server_url);
        g_server_url = NULL;
    }
    if (g_curl_initialized) {
        curl_global_cleanup();
        g_curl_initialized = 0;
    }
}
