/*
 * Requests.h
 *
 * LSP requests. Each writes the JSON `result` value into `out`.
 */

#ifndef CXXE_LS_REQUESTS_H
#define CXXE_LS_REQUESTS_H

#include "Server.h"

void feature_hover(LsServer *s, LsDocument *d, const Json *params, Str *out);
void feature_definition(LsServer *s, LsDocument *d, const Json *params, Str *out);
void feature_references(LsServer *s, LsDocument *d, const Json *params, Str *out);
void feature_highlight(LsServer *s, LsDocument *d, const Json *params, Str *out);
void feature_prepare_rename(LsServer *s, LsDocument *d, const Json *params, Str *out);
/* Returns 0 and sets *error when the rename is refused. */
int feature_rename(LsServer *s, LsDocument *d, const Json *params, Str *out, const char **error);
void feature_completion(LsServer *s, LsDocument *d, const Json *params, Str *out);
void feature_signature_help(LsServer *s, LsDocument *d, const Json *params, Str *out);
void feature_document_symbols(LsServer *s, LsDocument *d, const Json *params, Str *out);
void feature_workspace_symbols(LsServer *s, const Json *params, Str *out);
void feature_semantic_tokens(LsServer *s, LsDocument *d, const Json *params, Str *out);
void feature_document_links(LsServer *s, LsDocument *d, const Json *params, Str *out);
void feature_generated_cpp(LsServer *s, LsDocument *d, const Json *params, Str *out);

/* Legend shared with the initialize result. */
extern const char *const SEMANTIC_TOKEN_TYPES[];
extern const char *const SEMANTIC_TOKEN_MODIFIERS[];

#endif
