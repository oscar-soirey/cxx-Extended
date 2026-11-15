#!/usr/bin/env python3
"""End-to-end tests of the CXXE language server over the LSP protocol.

    python3 Tests/language-server/run_lsp_tests.py [path/to/cxxe-language-server]

Without an argument the server is built from Tools/Ls with gcc. The tests
drive the server exactly like an editor would, on Tests/language-server/workspace.
"""
import json, os, pathlib, shutil, subprocess, sys, tempfile, urllib.parse

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
WS = HERE / "workspace"


def build_server():
    out = pathlib.Path(tempfile.mkdtemp()) / ("cxxe-language-server" + (".exe" if os.name == "nt" else ""))
    srcs = [str(p) for p in (ROOT / "Tools/Ls/Src").glob("*.c")]
    srcs += [str(p) for p in (ROOT / "Compiler/Src").rglob("*.c") if not str(p).replace("\\", "/").endswith("Cli/Main.c")]
    subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(ROOT / "Compiler/Src"), "-I" + str(ROOT / "Tools/Ls/Src"), *srcs, "-o", str(out)], check=True)
    return str(out)


def uri(path):
    p = pathlib.Path(path).resolve().as_posix()
    if not p.startswith("/"):
        p = "/" + p
    return "file://" + urllib.parse.quote(p, safe="/:")


class Client:
    def __init__(self, exe):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.next_id = 0
        self.notifications = []

    def send(self, obj):
        body = json.dumps(obj).encode()
        self.p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.p.stdin.flush()

    def read(self):
        length = None
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError("server closed the connection")
            line = line.strip()
            if not line:
                break
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":")[1])
        return json.loads(self.p.stdout.read(length))

    def request(self, method, params):
        self.next_id += 1
        rid = self.next_id
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        while True:
            msg = self.read()
            if msg.get("id") == rid:
                if "error" in msg:
                    return {"__error__": msg["error"]}
                return msg.get("result")
            self.notifications.append(msg)

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def diagnostics(self, u):
        # make sure everything sent so far was processed
        self.request("workspace/symbol", {"query": "__sync__"})
        found = [n for n in self.notifications if n.get("method") == "textDocument/publishDiagnostics" and n["params"]["uri"] == u]
        return found[-1]["params"]["diagnostics"] if found else None


failures = []


def check(name, cond, detail=""):
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else "  -> " + str(detail)[:400]))
    if not cond:
        failures.append(name)


def pos_of(text, needle, nth=0, offset=0):
    idx = -1
    for _ in range(nth + 1):
        idx = text.index(needle, idx + 1)
    idx += offset
    line = text.count("\n", 0, idx)
    col = idx - (text.rfind("\n", 0, idx) + 1)
    # UTF-16 column
    prefix = text[text.rfind("\n", 0, idx) + 1: idx]
    col = len(prefix.encode("utf-16-le")) // 2
    return {"line": line, "character": col}


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else build_server()
    c = Client(exe)
    init = c.request("initialize", {"processId": None, "rootUri": uri(WS), "capabilities": {},
                                    "initializationOptions": {"includePaths": [], "cppCompiler": "g++", "cppCheck": "onSave"}})
    check("initialize", init and "capabilities" in init, init)
    c.notify("initialized", {})

    texts = {}
    def open_doc(name, text=None):
        path = WS / name if text is None else pathlib.Path(tempfile.gettempdir()) / name
        if text is None:
            text = path.read_text()
        else:
            path.write_text(text)
        u = uri(path)
        texts[u] = text
        c.notify("textDocument/didOpen", {"textDocument": {"uri": u, "languageId": "cxxe", "version": 1, "text": text}})
        return u, text

    main_u, main_t = open_doc("Main.cppe")
    node_hu, node_ht = open_doc("Node.hppe")
    node_cu, node_ct = open_doc("Node.cppe")
    for u in (main_u, node_hu, node_cu):
        d = c.diagnostics(u)
        check("no diagnostics in " + u.rsplit("/", 1)[1], d == [], d)

    def at(u, needle, nth=0, offset=0):
        return {"textDocument": {"uri": u}, "position": pos_of(texts[u], needle, nth, offset)}

    h = c.request("textDocument/hover", at(main_u, "n.depth = -3", offset=2))
    check("hover property", h and "property depth" in h["contents"]["value"], h)
    h = c.request("textDocument/hover", at(main_u, "line=2", offset=0))
    check("hover named argument label", h and "int line" in h["contents"]["value"] and "make" in h["contents"]["value"], h)
    h = c.request("textDocument/hover", at(main_u, "@register", offset=1))
    check("hover @register", h and "@register" in h["contents"]["value"], h)
    h = c.request("textDocument/hover", at(node_hu, "value < 0"))
    check("hover setter value", h and "value being assigned" in h["contents"]["value"], h)

    d = c.request("textDocument/definition", at(main_u, "make(line"))
    check("definition across files (function)", d and d["uri"] in (node_hu, node_cu), d)
    d = c.request("textDocument/definition", at(main_u, "bump(times", offset=1))
    check("definition of a method from a header", d and d["uri"] == node_hu, d)
    d = c.request("textDocument/definition", at(main_u, "@trace", offset=2))
    check("definition of a custom decorator", d and d["uri"] == node_hu and d["range"]["start"]["line"] == 19, d)
    d = c.request("textDocument/definition", at(main_u, '"Node.hppe"', offset=2))
    check("definition on #include", d and d["uri"] == node_hu, d)
    d = c.request("textDocument/definition", at(main_u, "total);", offset=1))
    check("definition of a local variable", d and d["uri"] == main_u and d["range"]["start"]["line"] == 18, d)

    comp = c.request("textDocument/completion", at(main_u, "depth = -3"))
    labels = [i["label"] for i in comp["items"]]
    check("member completion (property, method, field)", {"depth", "bump", "name"} <= set(labels) and "get_depth" not in labels, labels)

    # @ completion on an empty line: only real decorators
    t = main_t.replace("@register\n", "@\n", 1)
    c.notify("textDocument/didChange", {"textDocument": {"uri": main_u, "version": 2}, "contentChanges": [{"text": t}]})
    texts[main_u] = t
    comp = c.request("textDocument/completion", at(main_u, "@\n", offset=1))
    labels = sorted(i["label"] for i in comp["items"])
    check("decorator completion lists only real decorators", labels == ["exposed", "register", "trace"], labels)
    c.notify("textDocument/didChange", {"textDocument": {"uri": main_u, "version": 3}, "contentChanges": [{"text": main_t}]})
    texts[main_u] = main_t

    comp = c.request("textDocument/completion", at(main_u, "line=2", offset=0))
    labels = [i["label"] for i in comp["items"]]
    check("named argument completion (unused parameters only)", "col=" in labels and "kind=" not in labels and "line=" not in labels, labels[:20])
    comp = c.request("textDocument/completion", at(main_u, "Engine::Node n", offset=8))
    labels = [i["label"] for i in comp["items"]]
    check("scope completion (namespace members)", "Node" in labels, labels)

    sh = c.request("textDocument/signatureHelp", at(main_u, "kind=1", offset=5))
    check("signature help with named argument", sh and sh["signatures"] and sh["signatures"][0]["activeParameter"] == 0 and "make" in sh["signatures"][0]["label"], sh)

    refs = c.request("textDocument/references", {**at(main_u, "n.depth = -3", offset=2), "context": {"includeDeclaration": True}})
    files = {r["uri"] for r in refs or []}
    check("property references across files", {main_u, node_hu, node_cu} <= files and len(refs) >= 8,
          sorted((r["uri"].rsplit("/", 1)[1], r["range"]["start"]["line"], r["range"]["start"]["character"]) for r in refs or []))

    pr = c.request("textDocument/prepareRename", at(main_u, "bump(times", offset=1))
    check("prepareRename", pr and pr["placeholder"] == "bump", pr)
    ren = c.request("textDocument/rename", {**at(main_u, "bump(times", offset=1), "newName": "advance"})
    ch = ren.get("changes", {}) if ren else {}
    check("rename a method in 3 files", set(ch) == {main_u, node_hu, node_cu} and all(len(v) == 1 for v in ch.values()), ren)
    ren = c.request("textDocument/rename", {**at(node_cu, "int kind", offset=4), "newName": "category"})
    ch = ren.get("changes", {}) if ren else {}
    check("rename a parameter also renames name= labels", node_cu in ch and main_u in ch and len(ch[node_cu]) == 2, ren)
    ren = c.request("textDocument/rename", {**at(main_u, "total", offset=1), "newName": "sum"})
    ch = ren.get("changes", {}) if ren else {}
    check("rename a local variable", list(ch) == [main_u] and len(ch[main_u]) == 2, ren)
    bad = c.request("textDocument/rename", {**at(main_u, "total", offset=1), "newName": "class"})
    check("rename refuses keywords", isinstance(bad, dict) and "__error__" in bad, bad)

    hl = c.request("textDocument/documentHighlight", at(main_u, "n.depth = -3", offset=0))
    check("document highlight", hl and len(hl) >= 5, hl)

    syms = c.request("textDocument/documentSymbol", {"textDocument": {"uri": node_hu}})
    def names(s):
        return [x["name"] for x in s] + [n for x in s for n in names(x.get("children", []))]
    all_names = names(syms or [])
    check("document symbols (outline)", {"Engine", "Node", "depth", "bump", "make", "@trace"} <= set(all_names), all_names)
    ws = c.request("workspace/symbol", {"query": "nod"})
    check("workspace symbols", any(s["name"] == "Node" for s in ws or []), ws)

    st = c.request("textDocument/semanticTokens/full", {"textDocument": {"uri": main_u}})
    check("semantic tokens", st and len(st["data"]) > 20 and len(st["data"]) % 5 == 0, st)
    links = c.request("textDocument/documentLink", {"textDocument": {"uri": main_u}})
    check("include links", links and links[0]["target"] == node_hu, links)
    gen = c.request("cxxe/generatedCpp", {"textDocument": {"uri": main_u}})
    check("generated C++", gen and gen["ok"] and "set_depth(-3)" in gen["text"] and '#include "Node.hpp"' in gen["text"], gen and gen["text"][:300])

    # CXXE errors are located
    err_u, _ = open_doc("Errors.cppe", "int f(int a, int b) { return a - b; }\nint main() {\n    return f(c=1, a=2);\n}\n")
    d = c.diagnostics(err_u)
    check("CXXE error at line:col", d and d[0]["range"]["start"] == {"line": 2, "character": 11} and "no parameter named 'c'" in d[0]["message"], d)

    # C++ errors of the generated code, mapped back through #line
    cpp_u, _ = open_doc("CppError.cppe", "class A { public: int x = 0; property x { get { return x; } set { x = value; } } };\nint main() {\n    A a; a.x = 1;\n    return a.x + undefined_thing;\n}\n")
    c.notify("textDocument/didSave", {"textDocument": {"uri": cpp_u}})
    d = c.diagnostics(cpp_u)
    cpp = [x for x in d or [] if x["source"] == "c++"]
    if shutil.which("g++"):
        check("C++ check on save mapped to the .cppe line", cpp and cpp[0]["range"]["start"]["line"] == 3 and "undefined_thing" in cpp[0]["message"], d)

    # UTF-16 columns
    utf_u, utf_t = open_doc("Utf.cppe", "int value = 1; // é𝄞\nint other = value; /* é */ int third = value;\n")
    h = c.request("textDocument/definition", {"textDocument": {"uri": utf_u}, "position": pos_of(utf_t, "value;", 1)})
    check("UTF-16 positions", h and h["range"]["start"] == {"line": 0, "character": 4}, h)

    # robustness: garbage and incomplete code never kill the server
    g_u, _ = open_doc("Garbage.cppe", "class { property ;; @ @@ match { int: } ))) template <<< \"unterminated\n")
    d = c.diagnostics(g_u)
    check("garbage input gives diagnostics, server alive", d is not None and len(d) >= 1, d)
    comp = c.request("textDocument/completion", {"textDocument": {"uri": g_u}, "position": {"line": 0, "character": 3}})
    check("requests still answered after errors", comp is not None, comp)

    c.request("shutdown", None)
    c.notify("exit", None)
    c.p.wait(timeout=10)
    check("clean exit", c.p.returncode == 0, c.p.returncode)
    print("== %d failure(s)" % len(failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
