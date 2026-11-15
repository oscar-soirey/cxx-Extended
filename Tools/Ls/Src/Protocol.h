/*
 * Protocol.h
 *
 * LSP base protocol over stdio: Content-Length framed JSON-RPC messages.
 */

#ifndef CXXE_LS_PROTOCOL_H
#define CXXE_LS_PROTOCOL_H

#include "Core/Common.h"
#include "Core/StrBuf.h"
#include "Json.h"

void protocol_init(void);
/* Next message body (malloc'd), or NULL at end of input. */
char *protocol_read(size_t *len_out);
void protocol_send(const Str *json);
void protocol_respond(const Json *id, const char *result_json);
void protocol_respond_error(const Json *id, int code, const char *message);
void protocol_notify(const char *method, const char *params_json);
/* window/logMessage (type: 1 error, 2 warning, 3 info, 4 log). */
void protocol_log(int type, const char *fmt, ...);

#endif
