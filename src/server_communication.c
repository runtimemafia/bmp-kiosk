// server_communication.c
#define _POSIX_C_SOURCE 200809L
#include "server_communication.h"
#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *g_server_url = NULL;
static int g_curl_initialized = 0;

// Response buffer for capturing server response
struct response_buffer {
  char *data;
  size_t size;
};

// Callback for writing response data
static size_t write_callback(void *contents, size_t size, size_t nmemb,
                             void *userp) {
  size_t realsize = size * nmemb;
  struct response_buffer *mem = (struct response_buffer *)userp;

  char *ptr = realloc(mem->data, mem->size + realsize + 1);
  if (!ptr) {
    fprintf(stderr, "Not enough memory to store response\n");
    return 0;
  }

  mem->data = ptr;
  memcpy(&(mem->data[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->data[mem->size] = 0;

  return realsize;
}

// simple JSON string escaper (returns malloc'd string, caller must free)
// Escapes quotes, backslashes, and control characters into \uXXXX for safety.
static char *json_escape(const char *s) {
  if (!s)
    return NULL;
  size_t len = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
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
  if (!out)
    return NULL;
  char *q = out;
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    unsigned char c = *p;
    switch (c) {
    case '\"':
      *q++ = '\\';
      *q++ = '\"';
      break;
    case '\\':
      *q++ = '\\';
      *q++ = '\\';
      break;
    case '/':
      *q++ = '\\';
      *q++ = '/';
      break;
    case '\b':
      *q++ = '\\';
      *q++ = 'b';
      break;
    case '\f':
      *q++ = '\\';
      *q++ = 'f';
      break;
    case '\n':
      *q++ = '\\';
      *q++ = 'n';
      break;
    case '\r':
      *q++ = '\\';
      *q++ = 'r';
      break;
    case '\t':
      *q++ = '\\';
      *q++ = 't';
      break;
    default:
      if (c < 0x20) {
        // control -> \u00XX
        static const char hex[] = "0123456789abcdef";
        *q++ = '\\';
        *q++ = 'u';
        *q++ = '0';
        *q++ = '0';
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
  if (!server_url)
    return -1;
  if (g_curl_initialized)
    return 0; // already init

  g_server_url = strdup(server_url);
  if (!g_server_url)
    return -1;

  if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
    free(g_server_url);
    g_server_url = NULL;
    return -1;
  }
  g_curl_initialized = 1;
  return 0;
}

int send_qr_to_server(const char *qr_data) {
  if (!g_curl_initialized || !g_server_url || !qr_data)
    return -1;

  CURL *curl = curl_easy_init();
  if (!curl)
    return -1;

  int rc = -1;

  // Get templeId and kioskId from environment variables
  const char *temple_id = getenv("TEMPLE_ID");
  const char *kiosk_id = getenv("KIOSK_ID");

  if (!temple_id) {
    fprintf(stderr, "Error: TEMPLE_ID environment variable not set\n");
    curl_easy_cleanup(curl);
    return -1;
  }

  if (!kiosk_id) {
    fprintf(stderr, "Error: KIOSK_ID environment variable not set\n");
    curl_easy_cleanup(curl);
    return -1;
  }

  char *escaped_qr = json_escape(qr_data);
  if (!escaped_qr) {
    curl_easy_cleanup(curl);
    return -1;
  }

  char *escaped_temple = json_escape(temple_id);
  if (!escaped_temple) {
    free(escaped_qr);
    curl_easy_cleanup(curl);
    return -1;
  }

  char *escaped_kiosk = json_escape(kiosk_id);
  if (!escaped_kiosk) {
    free(escaped_qr);
    free(escaped_temple);
    curl_easy_cleanup(curl);
    return -1;
  }

  // Build JSON payload with qrData, templeId, and kioskId
  const char *template =
      "{ \"qrData\": \"%s\", \"templeId\": \"%s\", \"kioskId\": \"%s\" }";
  size_t payload_len = strlen(template) + strlen(escaped_qr) +
                       strlen(escaped_temple) + strlen(escaped_kiosk) + 1;
  char *payload = malloc(payload_len);
  if (!payload) {
    free(escaped_qr);
    free(escaped_temple);
    free(escaped_kiosk);
    curl_easy_cleanup(curl);
    return -1;
  }
  snprintf(payload, payload_len, template, escaped_qr, escaped_temple,
           escaped_kiosk);

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  // Initialize response buffer
  struct response_buffer response = {0};
  response.data = malloc(1);
  response.size = 0;
  if (!response.data) {
    free(payload);
    free(escaped_qr);
    free(escaped_temple);
    free(escaped_kiosk);
    curl_easy_cleanup(curl);
    return -1;
  }

  curl_easy_setopt(curl, CURLOPT_URL, g_server_url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(payload));
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L); // 5 sec timeout
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL,
                   1L); // safe for threaded apps / signals
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&response);

  CURLcode res = curl_easy_perform(curl);
  if (res == CURLE_OK) {
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code >= 200 && http_code < 300) {
      rc = 0; // success

      // Print the full response
      printf("Server response: %s\n", response.data);

      // Extract "data" field from JSON response
      // Simple parsing - look for "data":{...} or "data":...
      const char *data_key = "\"data\":";
      char *data_start = strstr(response.data, data_key);
      if (data_start) {
        data_start += strlen(data_key);
        // Skip whitespace
        while (*data_start && isspace(*data_start))
          data_start++;

        // Find the end of the data value
        char *data_end = NULL;
        if (*data_start == '{') {
          // Object - find matching }
          int depth = 0;
          data_end = data_start;
          do {
            if (*data_end == '{')
              depth++;
            else if (*data_end == '}')
              depth--;
            data_end++;
          } while (depth > 0 && *data_end);
        } else if (*data_start == '[') {
          // Array - find matching ]
          int depth = 0;
          data_end = data_start;
          do {
            if (*data_end == '[')
              depth++;
            else if (*data_end == ']')
              depth--;
            data_end++;
          } while (depth > 0 && *data_end);
        } else if (*data_start == '"') {
          // String - find closing quote (handling escapes)
          data_end = data_start + 1;
          while (*data_end && (*data_end != '"' || *(data_end - 1) == '\\')) {
            data_end++;
          }
          if (*data_end == '"')
            data_end++;
        } else {
          // Number, boolean, or null - find next comma or }
          data_end = data_start;
          while (*data_end && *data_end != ',' && *data_end != '}' &&
                 *data_end != ']') {
            data_end++;
          }
        }

        if (data_end && data_end > data_start) {
          size_t data_len = data_end - data_start;
          char *data_json = malloc(data_len + 1);
          if (data_json) {
            memcpy(data_json, data_start, data_len);
            data_json[data_len] = '\0';

            // Save to file
            FILE *f = fopen("qrscan_data.json", "w");
            if (f) {
              fprintf(f, "%s\n", data_json);
              fclose(f);
              printf("Data field saved to qrscan_data.json\n");

              // Call thermal printer if PRINTER_PATH is set
              const char *printer_path = getenv("PRINTER_PATH");
              if (printer_path) {
                printf("Calling printer: ./printer qrscan_data.json %s\n",
                       printer_path);

                // Build command: ./printer qrscan_data.json <printer_path>
                char cmd[512];
                snprintf(cmd, sizeof(cmd), "./printer qrscan_data.json %s",
                         printer_path);

                int print_result = system(cmd);
                if (print_result == 0) {
                  printf("✓ Print job completed successfully\n");
                } else {
                  fprintf(stderr, "✗ Print job failed with code %d\n",
                          print_result);
                }
              } else {
                printf("Note: PRINTER_PATH not set, skipping print\n");
              }
            } else {
              fprintf(stderr, "Failed to save data to file\n");
            }

            // Print the data field
            printf("Data field: %s\n", data_json);

            free(data_json);
          }
        } else {
          fprintf(stderr, "Failed to parse 'data' field from response\n");
        }
      } else {
        fprintf(stderr, "No 'data' field found in response\n");
      }
    } else {
      fprintf(stderr, "send_qr_to_server: HTTP %ld\n", http_code);
      rc = -1;
    }
  } else {
    fprintf(stderr, "send_qr_to_server: curl error: %s\n",
            curl_easy_strerror(res));
    rc = -1;
  }

  curl_slist_free_all(headers);
  free(payload);
  free(escaped_qr);
  free(escaped_temple);
  free(escaped_kiosk);
  free(response.data);
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
