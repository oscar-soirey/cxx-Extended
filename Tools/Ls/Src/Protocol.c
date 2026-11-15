/*
 * Protocol.c
 *
 * Message framing. stdin/stdout are switched to binary mode on Windows so
 * the CRT does not translate CR/LF (the framing counts bytes).
 */

#include "Protocol.h"
#include "Core/Memory.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

void protocol_init(void) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

char *protocol_read(size_t *len_out) {
    char line[1024];
    size_t length = 0;
    int have_length = 0;
    for (;;) {
        size_t n;
        if (!fgets(line, sizeof(line), stdin)) return NULL;
        n = strlen(line);
        while (n && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = 0;
        if (n == 0) { if (have_length) break; continue; }
        if (!strncmp(line, "Content-Length:", 15)) { length = (size_t)strtoull(line + 15, NULL, 10); have_length = 1; }
    }
    {
        char *body = (char *)xmalloc(length + 1);
        if (fread(body, 1, length, stdin) != length) { free(body); return NULL; }
        body[length] = 0;
        *len_out = length;
        return body;
    }
}

void protocol_send(const Str *json) {
    fprintf(stdout, "Content-Length: %zu\r\n\r\n", json->len);
    fwrite(json->data, 1, json->len, stdout);
    fflush(stdout);
}

void protocol_respond(const Json *id, const char *result_json) {
    Str s;
    str_init(&s);
    str_put(&s, "{\"jsonrpc\":\"2.0\",\"id\":");
    json_put_raw(&s, id);
    str_put(&s, ",\"result\":");
    str_put(&s, result_json ? result_json : "null");
    str_ch(&s, '}');
    protocol_send(&s);
    str_free(&s);
}

void protocol_respond_error(const Json *id, int code, const char *message) {
    Str s;
    char num[32];
    str_init(&s);
    str_put(&s, "{\"jsonrpc\":\"2.0\",\"id\":");
    json_put_raw(&s, id);
    snprintf(num, sizeof(num), "%d", code);
    str_put(&s, ",\"error\":{\"code\":");
    str_put(&s, num);
    str_put(&s, ",\"message\":");
    json_put_string(&s, message);
    str_put(&s, "}}");
    protocol_send(&s);
    str_free(&s);
}

void protocol_notify(const char *method, const char *params_json) {
    Str s;
    str_init(&s);
    str_put(&s, "{\"jsonrpc\":\"2.0\",\"method\":");
    json_put_string(&s, method);
    str_put(&s, ",\"params\":");
    str_put(&s, params_json ? params_json : "{}");
    str_ch(&s, '}');
    protocol_send(&s);
    str_free(&s);
}

void protocol_log(int type, const char *fmt, ...) {
    char msg[2048];
    char num[16];
    va_list ap;
    Str p;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    str_init(&p);
    snprintf(num, sizeof(num), "%d", type);
    str_put(&p, "{\"type\":");
    str_put(&p, num);
    str_put(&p, ",\"message\":");
    json_put_string(&p, msg);
    str_ch(&p, '}');
    protocol_notify("window/logMessage", p.data);
    str_free(&p);
}
