#pragma once

#include <stddef.h>

// Performs an HTTPS GET and stores the response body in a PSRAM buffer.
// Returns the HTTP status code, or -1 on a transport error.
// On HTTP 200 *out_buf/*out_len describe the body; the buffer must be released with heap_caps_free().
int http_get_body(const char* url, char** out_buf, size_t* out_len, size_t max_len);
