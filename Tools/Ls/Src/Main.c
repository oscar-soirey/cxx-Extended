/*
 * Main.c
 *
 * CXXE language server: JSON-RPC loop, document lifecycle and dispatch.
 * The analysis uses the compiler modules of Compiler/Src, so diagnostics
 * are exactly those of `cxxe`.
 */

#include "Server.h"
#include "Requests.h"
#include "Protocol.h"
#include "Core/Memory.h"
#include "Core/Path.h"

#define CXXE_LS_VERSION "0.3.0"

static void initialize_result(Str *out) {
    size_t i;
    str_put(out, "{\"serverInfo\":{\"name\":\"cxxe-language-server\",\"version\":\"" CXXE_LS_VERSION "\"},\"capabilities\":{"
                 "\"positionEncoding\":\"utf-16\","
                 "\"textDocumentSync\":{\"openClose\":true,\"change\":1,\"save\":{\"includeText\":false}},"
                 "\"hoverProvider\":true,\"definitionProvider\":true,\"referencesProvider\":true,"
                 "\"documentHighlightProvider\":true,\"renameProvider\":{\"prepareProvider\":true},"
                 "\"documentSymbolProvider\":true,\"workspaceSymbolProvider\":true,"
                 "\"documentLinkProvider\":{\"resolveProvider\":false},"
                 "\"completionProvider\":{\"triggerCharacters\":[\".\",\">\",\":\",\"@\",\"(\",\",\"]},"
                 "\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"],\"retriggerCharacters\":[\"=\"]},"
                 "\"semanticTokensProvider\":{\"full\":true,\"legend\":{\"tokenTypes\":[");
    for (i = 0; SEMANTIC_TOKEN_TYPES[i]; ++i) { if (i) str_ch(out, ','); json_put_string(out, SEMANTIC_TOKEN_TYPES[i]); }
    str_put(out, "],\"tokenModifiers\":[");
    for (i = 0; SEMANTIC_TOKEN_MODIFIERS[i]; ++i) { if (i) str_ch(out, ','); json_put_string(out, SEMANTIC_TOKEN_MODIFIERS[i]); }
    str_put(out, "]}}}}");
}

static char *default_temp_dir(void) {
    const char *base = getenv("TEMP");
    char buf[1024];
    if (!base || !*base) base = getenv("TMPDIR");
    if (!base || !*base) base = getenv("TMP");
#ifdef _WIN32
    if (!base || !*base) base = ".";
#else
    if (!base || !*base) base = "/tmp";
#endif
    snprintf(buf, sizeof(buf), "%s/cxxe-language-server", base);
    return xstrdup0(buf);
}

/* Standard CXXE headers live in <compiler dir>/stde: follow the configured
   compiler when there is one, otherwise the server's own directory. */
static void use_compiler_path(const Json *settings) {
    const char *c = json_str(settings, "compilerPath");
    if (c && *c) {
        free(g_cxxe_executable_dir);
        g_cxxe_executable_dir = path_dirname0(c);
    }
}

static void doc_free(LsDocument *d) {
    unit_free(&d->unit);
    diag_list_clear(&d->diags);
    diag_list_clear(&d->cpp_diags);
    free(d->generated);
    free(d->text);
    free(d);
}

static void reanalyze_all(LsServer *s) {
    size_t i;
    for (i = 0; i < s->doc_n; ++i) { analyze_document(s, s->docs[i]); publish_diagnostics(s, s->docs[i]); }
}

static void handle(LsServer *s, const Json *msg, int *running) {
    const char *method = json_str(msg, "method");
    const Json *id = json_get(msg, "id");
    const Json *params = json_get(msg, "params");
    const char *uri = json_str(params, "textDocument.uri");
    LsDocument *d = server_doc(s, uri);
    Str r;
    if (!method) return; /* responses to our own requests: none are sent */
    str_init(&r);

    if (!strcmp(method, "initialize")) {
        const char *root_uri = json_str(params, "rootUri");
        const Json *folders = json_get(params, "workspaceFolders");
        if (folders && folders->type == JSON_ARRAY && folders->child) root_uri = json_str(folders->child, "uri");
        if (root_uri) s->root = uri_to_path(root_uri);
        else if (json_str(params, "rootPath")) s->root = xstrdup0(json_str(params, "rootPath"));
        server_settings(s, json_get(params, "initializationOptions"));
        use_compiler_path(json_get(params, "initializationOptions"));
        initialize_result(&r);
        protocol_respond(id, r.data);
    } else if (!strcmp(method, "initialized")) {
        index_refresh(s);
    } else if (!strcmp(method, "shutdown")) {
        s->shutdown_requested = 1;
        protocol_respond(id, "null");
    } else if (!strcmp(method, "exit")) {
        *running = 0;
    } else if (!strcmp(method, "workspace/didChangeConfiguration")) {
        const Json *settings = json_get(params, "settings");
        const Json *cx = json_get(settings, "cxxe");
        server_settings(s, cx ? cx : settings);
        use_compiler_path(cx ? cx : settings);
        reanalyze_all(s);
    } else if (!strcmp(method, "workspace/didChangeWatchedFiles")) {
        index_refresh(s);
    } else if (!strcmp(method, "textDocument/didOpen")) {
        const char *text = json_str(params, "textDocument.text");
        if (!uri) goto done;
        if (!d) {
            d = (LsDocument *)xcalloc(1, sizeof(LsDocument));
            d->unit.uri = xstrdup0(uri);
            d->unit.path = uri_to_path(uri);
            s->docs = (LsDocument **)xrealloc(s->docs, (s->doc_n + 1) * sizeof(LsDocument *));
            s->docs[s->doc_n++] = d;
        }
        free(d->text);
        d->text = xstrdup0(text ? text : "");
        d->text_len = strlen(d->text);
        d->version = json_int(params, "textDocument.version", 0);
        analyze_document(s, d);
        publish_diagnostics(s, d);
    } else if (!strcmp(method, "textDocument/didChange")) {
        const Json *changes = json_get(params, "contentChanges"), *c;
        if (!d) goto done;
        for (c = changes ? changes->child : NULL; c; c = c->next) {
            const char *text = json_str(c, "text");
            if (!text || json_get(c, "range")) continue; /* full sync only */
            free(d->text);
            d->text = xstrdup0(text);
            d->text_len = strlen(d->text);
        }
        d->version = json_int(params, "textDocument.version", d->version);
        diag_list_clear(&d->cpp_diags); /* stale until the next save */
        analyze_document(s, d);
        publish_diagnostics(s, d);
    } else if (!strcmp(method, "textDocument/didSave")) {
        if (!d) goto done;
        cpp_check_document(s, d);
        publish_diagnostics(s, d);
    } else if (!strcmp(method, "textDocument/didClose")) {
        size_t i;
        if (!d) goto done;
        str_put(&r, "{\"uri\":");
        json_put_string(&r, uri);
        str_put(&r, ",\"diagnostics\":[]}");
        protocol_notify("textDocument/publishDiagnostics", r.data);
        for (i = 0; i < s->doc_n; ++i) if (s->docs[i] == d) { s->docs[i] = s->docs[--s->doc_n]; break; }
        doc_free(d);
    } else if (!strcmp(method, "workspace/symbol")) {
        feature_workspace_symbols(s, params, &r);
        protocol_respond(id, r.data);
    } else if (!strncmp(method, "textDocument/", 13) || !strncmp(method, "cxxe/", 5)) {
        const char *m = method;
        if (!id) goto done;
        if (!d) { protocol_respond(id, "null"); goto done; }
        if (!strcmp(m, "textDocument/hover")) feature_hover(s, d, params, &r);
        else if (!strcmp(m, "textDocument/definition")) feature_definition(s, d, params, &r);
        else if (!strcmp(m, "textDocument/references")) feature_references(s, d, params, &r);
        else if (!strcmp(m, "textDocument/documentHighlight")) feature_highlight(s, d, params, &r);
        else if (!strcmp(m, "textDocument/prepareRename")) feature_prepare_rename(s, d, params, &r);
        else if (!strcmp(m, "textDocument/rename")) {
            const char *err = NULL;
            if (!feature_rename(s, d, params, &r, &err)) { protocol_respond_error(id, -32803, err); goto done; }
        }
        else if (!strcmp(m, "textDocument/completion")) feature_completion(s, d, params, &r);
        else if (!strcmp(m, "textDocument/signatureHelp")) feature_signature_help(s, d, params, &r);
        else if (!strcmp(m, "textDocument/documentSymbol")) feature_document_symbols(s, d, params, &r);
        else if (!strcmp(m, "textDocument/semanticTokens/full")) feature_semantic_tokens(s, d, params, &r);
        else if (!strcmp(m, "textDocument/documentLink")) feature_document_links(s, d, params, &r);
        else if (!strcmp(m, "cxxe/generatedCpp")) feature_generated_cpp(s, d, params, &r);
        else { protocol_respond_error(id, -32601, "method not supported"); goto done; }
        protocol_respond(id, r.data ? r.data : "null");
    } else if (id) {
        protocol_respond_error(id, -32601, "method not supported");
    }
done:
    str_free(&r);
}

int main(int argc, char **argv) {
    LsServer server;
    int running = 1;
    memset(&server, 0, sizeof(server));
    g_cxxe_executable_dir = cxxe_executable_dir0(argc > 0 ? argv[0] : "cxxe-language-server");
    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        printf("cxxe-language-server %s\n", CXXE_LS_VERSION);
        return 0;
    }
    protocol_init();
    server.cpp_compiler = xstrdup0("g++");
    server.cpp_standard = xstrdup0("c++20");
    server.cpp_check_on_save = 1;
    server.temp_dir = default_temp_dir();
    while (running) {
        size_t len = 0;
        char *body = protocol_read(&len);
        Json *msg;
        if (!body) break;
        msg = json_parse(body, len);
        if (msg) handle(&server, msg, &running);
        json_free(msg);
        free(body);
    }
    return server.shutdown_requested ? 0 : 1;
}
