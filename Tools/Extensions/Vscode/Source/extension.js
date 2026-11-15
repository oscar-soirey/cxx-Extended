'use strict';

/*
 * C++Extended for VS Code.
 *
 * The native language server (Tools/Ls, built on the compiler modules) does
 * the analysis; this file syncs documents, adapts LSP results to the VS Code
 * API, shows the generated C++ and runs/builds programs.
 */

const vscode = require('vscode');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const { LspClient } = require('./lsp-client');

const LANGUAGE_ID = 'cxxe';
const CXXE_EXTENSIONS = ['.cppe', '.hppe', '.he'];
const SERVER_NAME = 'C++Extended language server';
const SERVER_EXECUTABLE = process.platform === 'win32' ? 'cxxe-language-server.exe' : 'cxxe-language-server';
const GENERATED_SCHEME = 'cxxe-generated';
const CHANGE_DELAY_MS = 120;
/* Must match SEMANTIC_TOKEN_TYPES / MODIFIERS in Tools/Ls/Src/Requests.c. */
const TOKEN_TYPES = ['namespace', 'class', 'enum', 'struct', 'type', 'parameter', 'variable', 'property',
  'enumMember', 'function', 'method', 'keyword', 'decorator'];
const TOKEN_MODIFIERS = ['declaration'];

let extensionContext = null;
let client = null;
let starting = null;
let output = null;
let diagnostics = null;
let statusItem = null;
const openedDocuments = new Set();
const pendingChanges = new Map();      // uri -> timer
const generatedViews = new Map();      // source uri -> generated uri
const generatedChanged = new vscode.EventEmitter();
const generatedTimers = new Map();

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

function config() {
  return vscode.workspace.getConfiguration('cxxe');
}

function logLevel() {
  return config().get('languageServer.logLevel', 'info');
}

function log(message) {
  if (output && logLevel() !== 'silent') output.appendLine(`[CXXE] ${message}`);
}

function isCxxeFile(fileName) {
  const lower = fileName.toLowerCase();
  return CXXE_EXTENSIONS.some(ext => lower.endsWith(ext));
}

function isCxxeDocument(document) {
  return Boolean(document) && document.uri.scheme === 'file' &&
    (document.languageId === LANGUAGE_ID || isCxxeFile(document.fileName));
}

function workspaceFolderFor(uri) {
  return (uri && vscode.workspace.getWorkspaceFolder(uri)) || vscode.workspace.workspaceFolders?.[0];
}

/* Settings forwarded to the server (initializationOptions and
   workspace/didChangeConfiguration). */
function serverSettings() {
  const c = config();
  return {
    includePaths: c.get('includePaths', []),
    compilerPath: c.get('compilerPath', ''),
    cppCompiler: c.get('cppCompiler', 'g++'),
    cppStandard: c.get('cppStandard', 'c++20'),
    cppCheck: c.get('cppCheck', 'onSave')
  };
}

function setStatus(state, detail) {
  if (!statusItem) return;
  const icons = { starting: '$(sync~spin)', running: '$(check)', stopped: '$(circle-slash)', error: '$(error)' };
  statusItem.text = `${icons[state] || ''} CXXE`;
  statusItem.tooltip = detail || SERVER_NAME;
  statusItem.backgroundColor = state === 'error' ? new vscode.ThemeColor('statusBarItem.errorBackground') : undefined;
  statusItem.show();
}

async function normalizeLanguage(document) {
  if (!isCxxeDocument(document) || document.languageId === LANGUAGE_ID) return document;
  try {
    return await vscode.languages.setTextDocumentLanguage(document, LANGUAGE_ID);
  } catch (error) {
    log(`Cannot switch ${document.fileName} to C++Extended: ${error.message}`);
    return document;
  }
}

/* ------------------------------------------------------------------------ */
/* Server lifecycle                                                          */
/* ------------------------------------------------------------------------ */

function serverCandidates() {
  const configured = config().get('languageServer.path', '');
  const list = [];
  if (configured) list.push(configured);
  const bundled = `${process.platform}-${process.arch}`;
  list.push(extensionContext.asAbsolutePath(path.join('server', bundled, SERVER_EXECUTABLE)));
  list.push(SERVER_EXECUTABLE); /* PATH */
  return [...new Set(list)];
}

function findOnPath(name) {
  const dirs = (process.env.PATH || '').split(path.delimiter);
  for (const dir of dirs) {
    const candidate = path.join(dir, name);
    try { if (fs.statSync(candidate).isFile()) return candidate; } catch (_) { /* next */ }
  }
  return null;
}

function resolveServer() {
  for (const candidate of serverCandidates()) {
    if (candidate === SERVER_EXECUTABLE) {
      const found = findOnPath(candidate);
      if (found) return found;
      continue;
    }
    try { if (fs.statSync(candidate).isFile()) return candidate; } catch (_) { /* next */ }
  }
  return null;
}

async function ensureServer() {
  if (client?.initialized) return client;
  if (starting) return starting;
  if (!config().get('languageServer.autoStart', true)) return null;

  starting = (async () => {
    const executable = resolveServer();
    if (!executable) {
      const message = `${SERVER_NAME} not found. Build it with Tools/Ls/Build.bat (or build.sh) or set "cxxe.languageServer.path".`;
      setStatus('error', message);
      log(message);
      vscode.window.showErrorMessage(message, 'Set Path').then(choice => {
        if (choice === 'Set Path') vscode.commands.executeCommand('cxxe.setLanguageServerPath');
      });
      return null;
    }
    setStatus('starting', `Starting ${executable}`);
    const c = new LspClient(output, logLevel());
    c.on('error', error => {
      log(`Server process error: ${error.message}`);
      setStatus('error', error.message);
    });
    c.on('exit', code => {
      if (client === c) {
        client = null;
        openedDocuments.clear();
        if (code !== 0) {
          setStatus('error', `${SERVER_NAME} stopped (code ${code}). Click to see the log.`);
          vscode.window.showWarningMessage(`${SERVER_NAME} stopped unexpectedly.`, 'Restart').then(choice => {
            if (choice === 'Restart') restartServer();
          });
        } else {
          setStatus('stopped', `${SERVER_NAME} stopped`);
        }
      }
    });
    c.on('notification', (method, params) => {
      if (method === 'textDocument/publishDiagnostics' && params?.uri) {
        diagnostics.set(vscode.Uri.parse(params.uri), (params.diagnostics || []).map(toDiagnostic));
      } else if (method === 'window/logMessage' && params?.message) {
        log(params.message);
      }
    });
    const folder = vscode.workspace.workspaceFolders?.[0];
    try {
      await c.start(executable, [], folder?.uri.fsPath, {
        rootUri: folder?.uri.toString() || null,
        workspaceFolders: folder ? [{ uri: folder.uri.toString(), name: folder.name }] : null,
        settings: serverSettings()
      });
    } catch (error) {
      c.dispose();
      throw error;
    }
    client = c;
    setStatus('running', `${SERVER_NAME} ${c.serverInfo?.version || ''}\n${executable}`);
    for (const document of vscode.workspace.textDocuments) {
      if (isCxxeDocument(document)) await openDocument(document);
    }
    return client;
  })();

  try {
    return await starting;
  } catch (error) {
    log(`Unable to start the server: ${error.message}`);
    setStatus('error', error.message);
    return null;
  } finally {
    starting = null;
  }
}

async function restartServer() {
  for (const timer of pendingChanges.values()) clearTimeout(timer);
  pendingChanges.clear();
  openedDocuments.clear();
  const old = client;
  client = null;
  if (old) await old.shutdown();
  diagnostics.clear();
  await ensureServer();
}

/* ------------------------------------------------------------------------ */
/* Document synchronisation                                                  */
/* ------------------------------------------------------------------------ */

async function openDocument(document) {
  if (!isCxxeDocument(document)) return;
  document = await normalizeLanguage(document);
  const c = await ensureServer();
  const key = document.uri.toString();
  if (!c?.initialized || openedDocuments.has(key)) return;
  openedDocuments.add(key);
  c.sendNotification('textDocument/didOpen', {
    textDocument: { uri: key, languageId: LANGUAGE_ID, version: document.version, text: document.getText() }
  });
}

function sendChange(document) {
  const key = document.uri.toString();
  pendingChanges.delete(key);
  if (!client?.initialized || !openedDocuments.has(key)) return;
  client.sendNotification('textDocument/didChange', {
    textDocument: { uri: key, version: document.version },
    contentChanges: [{ text: document.getText() }]
  });
  refreshGeneratedView(key);
}

function scheduleChange(document) {
  if (!isCxxeDocument(document)) return;
  const key = document.uri.toString();
  clearTimeout(pendingChanges.get(key));
  pendingChanges.set(key, setTimeout(() => sendChange(document), CHANGE_DELAY_MS));
}

/* Requests must see the latest text, or positions would be off. */
function flushChanges(document) {
  const key = document.uri.toString();
  if (pendingChanges.has(key)) {
    clearTimeout(pendingChanges.get(key));
    sendChange(document);
  }
}

async function request(document, method, params, fallback) {
  try {
    const c = await ensureServer();
    if (!c?.initialized) return fallback;
    await openDocument(document);
    flushChanges(document);
    const result = await c.request(method, { textDocument: { uri: document.uri.toString() }, ...params });
    return result ?? fallback;
  } catch (error) {
    log(`${method} failed: ${error.message}`);
    return fallback;
  }
}

/* ------------------------------------------------------------------------ */
/* LSP -> VS Code conversions                                                */
/* ------------------------------------------------------------------------ */

const toPosition = p => new vscode.Position(p?.line || 0, p?.character || 0);
const toRange = r => new vscode.Range(toPosition(r?.start), toPosition(r?.end));
const fromPosition = p => ({ line: p.line, character: p.character });

function toLocation(item) {
  if (!item?.uri || !item.range) return null;
  return new vscode.Location(vscode.Uri.parse(item.uri), toRange(item.range));
}

function toMarkdown(value) {
  if (value == null) return undefined;
  const text = typeof value === 'string' ? value : value.value;
  return text === undefined ? undefined : new vscode.MarkdownString(text);
}

function toDiagnostic(d) {
  const severities = [undefined, vscode.DiagnosticSeverity.Error, vscode.DiagnosticSeverity.Warning,
    vscode.DiagnosticSeverity.Information, vscode.DiagnosticSeverity.Hint];
  const result = new vscode.Diagnostic(toRange(d.range), d.message || '', severities[d.severity] ?? vscode.DiagnosticSeverity.Error);
  result.source = d.source || 'cxxe';
  if (d.code !== undefined) result.code = String(d.code);
  return result;
}

function toCompletionItem(raw) {
  const kind = Number.isInteger(raw.kind) && raw.kind > 0 ? raw.kind - 1 : vscode.CompletionItemKind.Text;
  const item = new vscode.CompletionItem(raw.label || '', kind);
  if (raw.detail) item.detail = raw.detail;
  if (raw.documentation) item.documentation = toMarkdown(raw.documentation);
  if (raw.sortText) item.sortText = raw.sortText;
  if (raw.filterText) item.filterText = raw.filterText;
  if (raw.insertText !== undefined) {
    item.insertText = raw.insertTextFormat === 2 ? new vscode.SnippetString(raw.insertText) : raw.insertText;
  }
  return item;
}

function toSymbol(s) {
  const kind = Number.isInteger(s.kind) && s.kind > 0 ? s.kind - 1 : vscode.SymbolKind.Object;
  if (s.location) {
    return new vscode.SymbolInformation(s.name || '', kind, s.containerName || '', toLocation(s.location));
  }
  const symbol = new vscode.DocumentSymbol(s.name || '', s.detail || '', kind, toRange(s.range), toRange(s.selectionRange || s.range));
  symbol.children = (s.children || []).map(toSymbol);
  return symbol;
}

function toWorkspaceEdit(raw) {
  const edit = new vscode.WorkspaceEdit();
  for (const [uri, edits] of Object.entries(raw?.changes || {})) {
    for (const e of edits) edit.replace(vscode.Uri.parse(uri), toRange(e.range), e.newText);
  }
  return edit;
}

/* ------------------------------------------------------------------------ */
/* Providers                                                                 */
/* ------------------------------------------------------------------------ */

function registerProviders(context) {
  const selector = [{ scheme: 'file', language: LANGUAGE_ID }];
  const at = pos => ({ position: fromPosition(pos) });
  const push = d => context.subscriptions.push(d);

  push(vscode.languages.registerCompletionItemProvider(selector, {
    async provideCompletionItems(document, pos) {
      const result = await request(document, 'textDocument/completion', at(pos), null);
      if (!result) return undefined;
      const items = Array.isArray(result) ? result : (result.items || []);
      return new vscode.CompletionList(items.map(toCompletionItem), Boolean(result.isIncomplete));
    }
  }, '.', '>', ':', '@', '(', ','));

  push(vscode.languages.registerHoverProvider(selector, {
    async provideHover(document, pos) {
      const result = await request(document, 'textDocument/hover', at(pos), null);
      if (!result?.contents) return undefined;
      return new vscode.Hover(toMarkdown(result.contents), result.range ? toRange(result.range) : undefined);
    }
  }));

  push(vscode.languages.registerDefinitionProvider(selector, {
    async provideDefinition(document, pos) {
      const result = await request(document, 'textDocument/definition', at(pos), null);
      return (Array.isArray(result) ? result : [result]).map(toLocation).filter(Boolean);
    }
  }));

  push(vscode.languages.registerReferenceProvider(selector, {
    async provideReferences(document, pos, options) {
      const result = await request(document, 'textDocument/references',
        { ...at(pos), context: { includeDeclaration: options.includeDeclaration } }, []);
      return result.map(toLocation).filter(Boolean);
    }
  }));

  push(vscode.languages.registerDocumentHighlightProvider(selector, {
    async provideDocumentHighlights(document, pos) {
      const result = await request(document, 'textDocument/documentHighlight', at(pos), []);
      return result.map(h => new vscode.DocumentHighlight(toRange(h.range), (h.kind || 1) - 1));
    }
  }));

  push(vscode.languages.registerRenameProvider(selector, {
    async prepareRename(document, pos) {
      const result = await request(document, 'textDocument/prepareRename', at(pos), null);
      if (!result) throw new Error('This element cannot be renamed.');
      return { range: toRange(result.range), placeholder: result.placeholder };
    },
    async provideRenameEdits(document, pos, newName) {
      const c = await ensureServer();
      if (!c?.initialized) return undefined;
      flushChanges(document);
      try {
        const result = await c.request('textDocument/rename', {
          textDocument: { uri: document.uri.toString() }, position: fromPosition(pos), newName
        });
        return toWorkspaceEdit(result);
      } catch (error) {
        throw new Error(error.message || 'Rename failed.');
      }
    }
  }));

  push(vscode.languages.registerSignatureHelpProvider(selector, {
    async provideSignatureHelp(document, pos) {
      const raw = await request(document, 'textDocument/signatureHelp', at(pos), null);
      if (!raw?.signatures?.length) return undefined;
      const help = new vscode.SignatureHelp();
      help.activeSignature = raw.activeSignature || 0;
      help.signatures = raw.signatures.map(sig => {
        const info = new vscode.SignatureInformation(sig.label || '', toMarkdown(sig.documentation));
        info.parameters = (sig.parameters || []).map(p => new vscode.ParameterInformation(p.label || '', toMarkdown(p.documentation)));
        info.activeParameter = sig.activeParameter ?? raw.activeParameter ?? 0;
        return info;
      });
      help.activeParameter = help.signatures[help.activeSignature]?.activeParameter ?? 0;
      return help;
    }
  }, { triggerCharacters: ['(', ','], retriggerCharacters: ['='] }));

  push(vscode.languages.registerDocumentSymbolProvider(selector, {
    async provideDocumentSymbols(document) {
      const result = await request(document, 'textDocument/documentSymbol', {}, []);
      return result.map(toSymbol);
    }
  }));

  push(vscode.languages.registerWorkspaceSymbolProvider({
    async provideWorkspaceSymbols(query) {
      const c = await ensureServer();
      if (!c?.initialized) return [];
      try {
        return ((await c.request('workspace/symbol', { query })) || []).map(toSymbol);
      } catch (error) {
        log(`workspace/symbol failed: ${error.message}`);
        return [];
      }
    }
  }));

  push(vscode.languages.registerDocumentLinkProvider(selector, {
    async provideDocumentLinks(document) {
      const result = await request(document, 'textDocument/documentLink', {}, []);
      return result.map(l => new vscode.DocumentLink(toRange(l.range), vscode.Uri.parse(l.target)));
    }
  }));

  const legend = new vscode.SemanticTokensLegend(TOKEN_TYPES, TOKEN_MODIFIERS);
  push(vscode.languages.registerDocumentSemanticTokensProvider(selector, {
    async provideDocumentSemanticTokens(document) {
      const result = await request(document, 'textDocument/semanticTokens/full', {}, null);
      return new vscode.SemanticTokens(Uint32Array.from(result?.data || []));
    }
  }, legend));
}

/* ------------------------------------------------------------------------ */
/* Generated C++ view                                                        */
/* ------------------------------------------------------------------------ */

function generatedUriFor(sourceUri) {
  const name = path.basename(sourceUri.fsPath).replace(/\.(cppe|hppe|he)$/i, m => (m.toLowerCase() === '.cppe' ? '.cpp' : '.hpp'));
  return vscode.Uri.from({ scheme: GENERATED_SCHEME, path: `/${name}`, query: sourceUri.toString() });
}

function refreshGeneratedView(sourceKey) {
  const generated = generatedViews.get(sourceKey);
  if (!generated) return;
  clearTimeout(generatedTimers.get(sourceKey));
  generatedTimers.set(sourceKey, setTimeout(() => generatedChanged.fire(generated), 300));
}

const generatedContentProvider = {
  onDidChange: generatedChanged.event,
  async provideTextDocumentContent(uri) {
    const source = vscode.Uri.parse(uri.query);
    const document = vscode.workspace.textDocuments.find(d => d.uri.toString() === source.toString()) ||
      await vscode.workspace.openTextDocument(source);
    const result = await request(document, 'cxxe/generatedCpp', {}, null);
    return result?.text ?? '// The C++Extended language server is not running.';
  }
};

async function showGeneratedCpp() {
  const editor = vscode.window.activeTextEditor;
  if (!editor || !isCxxeDocument(editor.document)) {
    vscode.window.showInformationMessage('Open a C++Extended file (.cppe / .hppe) first.');
    return;
  }
  const generated = generatedUriFor(editor.document.uri);
  generatedViews.set(editor.document.uri.toString(), generated);
  const doc = await vscode.workspace.openTextDocument(generated);
  await vscode.languages.setTextDocumentLanguage(doc, 'cpp');
  await vscode.window.showTextDocument(doc, { viewColumn: vscode.ViewColumn.Beside, preserveFocus: true, preview: false });
}

/* ------------------------------------------------------------------------ */
/* Run / build                                                               */
/* ------------------------------------------------------------------------ */

const quote = s => `"${s}"`;

function includeFlags(cwd) {
  return config().get('includePaths', []).map(p => `-I${quote(path.isAbsolute(p) ? p : path.join(cwd, p))}`).join(' ');
}

function writeScript(dir, name, lines) {
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, process.platform === 'win32' ? `${name}.bat` : `${name}.sh`);
  fs.writeFileSync(file, lines.join(process.platform === 'win32' ? '\r\n' : '\n') + '\n', { mode: 0o755 });
  return file;
}

function executeScript(label, script, cwd, folder) {
  const execution = process.platform === 'win32'
    ? new vscode.ProcessExecution('cmd.exe', ['/d', '/c', script], { cwd })
    : new vscode.ProcessExecution('/bin/sh', [script], { cwd });
  if (folder) {
    const task = new vscode.Task({ type: 'cxxe', label }, folder, label, 'C++Extended', execution, ['$cxxe']);
    task.presentationOptions = { reveal: vscode.TaskRevealKind.Always, clear: true, panel: vscode.TaskPanelKind.Dedicated };
    return vscode.tasks.executeTask(task);
  }
  const terminal = vscode.window.createTerminal({
    name: label, cwd,
    shellPath: process.platform === 'win32' ? 'cmd.exe' : '/bin/sh',
    shellArgs: process.platform === 'win32' ? ['/d', '/c', script] : [script]
  });
  terminal.show();
  return undefined;
}

async function runFile() {
  const editor = vscode.window.activeTextEditor;
  const document = editor?.document;
  if (!document || !isCxxeDocument(document) || !document.fileName.toLowerCase().endsWith('.cppe')) {
    vscode.window.showInformationMessage('Open the .cppe file to run.');
    return;
  }
  if (document.isDirty) await document.save();
  const cxxe = config().get('compilerPath', '') || 'cxxe';
  const cxx = config().get('cppCompiler', '') || 'g++';
  const std = config().get('cppStandard', 'c++20');
  const file = document.fileName;
  const dir = path.dirname(file);
  const folder = workspaceFolderFor(document.uri);
  const hash = crypto.createHash('sha1').update(file).digest('hex').slice(0, 10);
  const out = path.join(os.tmpdir(), 'cxxe-run', hash);
  const exe = path.join(out, process.platform === 'win32' ? 'program.exe' : 'program');
  const inc = includeFlags(folder?.uri.fsPath || dir);
  const win = process.platform === 'win32';
  const fail = win ? ' || exit /b 1' : ' || exit 1';
  const lines = win ? ['@echo off', 'setlocal'] : ['#!/bin/sh'];
  lines.push(`${quote(cxxe)} -I${quote(dir)} ${inc} roundtrip ${quote(file)} ${quote(path.join(out, 'main.cpp'))}${fail}`);
  /* CXXE headers next to the file are translated too (#include "x.hppe" -> x.hpp). */
  for (const name of fs.readdirSync(dir).filter(n => /\.hppe$/i.test(n))) {
    lines.push(`${quote(cxxe)} -I${quote(dir)} ${inc} roundtrip ${quote(path.join(dir, name))} ${quote(path.join(out, name.slice(0, -1)))}${fail}`);
  }
  /* Sibling .cppe files without a main() are part of the same program. */
  const units = [quote(path.join(out, 'main.cpp'))];
  for (const name of fs.readdirSync(dir).filter(n => /\.cppe$/i.test(n) && path.join(dir, n) !== file)) {
    let source = '';
    try { source = fs.readFileSync(path.join(dir, name), 'utf8'); } catch (_) { continue; }
    if (/\bmain\s*\(/.test(source)) continue;
    const generated = path.join(out, `${name.slice(0, -1)}`);
    lines.push(`${quote(cxxe)} -I${quote(dir)} ${inc} roundtrip ${quote(path.join(dir, name))} ${quote(generated)}${fail}`);
    units.push(quote(generated));
  }
  lines.push(`${quote(cxxe)} runtime export ${quote(path.join(out, 'runtime'))}${win ? ' >nul' : ' >/dev/null'}${fail}`);
  lines.push(`${quote(cxx)} -std=${std} -I${quote(out)} -I${quote(path.join(out, 'runtime'))} -I${quote(dir)} ${inc} ` +
    `${units.join(' ')} ${quote(path.join(out, 'runtime', 'cxxe_runtime.cpp'))} -o ${quote(exe)}${fail}`);
  if (win) {
    lines.push('echo.', `${quote(exe)}`, 'set CODE=%ERRORLEVEL%', 'echo.', 'echo [exit code %CODE%]');
  } else {
    lines.push('echo', `${quote(exe)}`, 'CODE=$?', 'echo', 'echo "[exit code $CODE]"');
  }
  const script = writeScript(out, 'run', lines);
  await executeScript(`Run ${path.basename(file)}`, script, dir, folder);
}

async function buildProject() {
  const folder = workspaceFolderFor(vscode.window.activeTextEditor?.document.uri);
  if (!folder || !fs.existsSync(path.join(folder.uri.fsPath, 'CMakeLists.txt'))) {
    vscode.window.showWarningMessage('No CMakeLists.txt at the root of the workspace folder (see CMake/CXXE.cmake).');
    return;
  }
  await vscode.workspace.saveAll(false);
  const root = folder.uri.fsPath;
  const build = path.resolve(root, config().get('build.directory', 'build'));
  const cxxe = config().get('compilerPath', '');
  const win = process.platform === 'win32';
  const fail = win ? ' || exit /b 1' : ' || exit 1';
  const lines = win ? ['@echo off'] : ['#!/bin/sh'];
  const configure = `cmake -S ${quote(root)} -B ${quote(build)}${cxxe ? ` -DCXXE_EXECUTABLE=${quote(cxxe)}` : ''}`;
  lines.push(win ? `if not exist ${quote(path.join(build, 'CMakeCache.txt'))} ${configure}${fail}`
                 : `[ -f ${quote(path.join(build, 'CMakeCache.txt'))} ] || ${configure}${fail}`);
  lines.push(`cmake --build ${quote(build)}${fail}`);
  const script = writeScript(path.join(os.tmpdir(), 'cxxe-run'), 'build', lines);
  await executeScript('Build (CMake)', script, root, folder);
}

/* ------------------------------------------------------------------------ */
/* Activation                                                                */
/* ------------------------------------------------------------------------ */

async function setServerPath() {
  const current = config().get('languageServer.path', '');
  const picked = await vscode.window.showOpenDialog({
    canSelectMany: false, openLabel: 'Use this language server',
    defaultUri: current ? vscode.Uri.file(current) : undefined,
    filters: process.platform === 'win32' ? { Executable: ['exe'] } : undefined
  });
  if (!picked?.length) return;
  await config().update('languageServer.path', picked[0].fsPath, vscode.ConfigurationTarget.Global);
  await restartServer();
}

function activate(context) {
  extensionContext = context;
  output = vscode.window.createOutputChannel('C++Extended');
  diagnostics = vscode.languages.createDiagnosticCollection('cxxe');
  statusItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
  statusItem.command = 'cxxe.showOutput';
  context.subscriptions.push(output, diagnostics, statusItem, generatedChanged);

  registerProviders(context);
  context.subscriptions.push(
    vscode.workspace.registerTextDocumentContentProvider(GENERATED_SCHEME, generatedContentProvider),
    vscode.commands.registerCommand('cxxe.showGeneratedCpp', showGeneratedCpp),
    vscode.commands.registerCommand('cxxe.runFile', runFile),
    vscode.commands.registerCommand('cxxe.buildProject', buildProject),
    vscode.commands.registerCommand('cxxe.restartLanguageServer', restartServer),
    vscode.commands.registerCommand('cxxe.setLanguageServerPath', setServerPath),
    vscode.commands.registerCommand('cxxe.showOutput', () => output.show(true)),

    vscode.workspace.onDidOpenTextDocument(document => {
      if (isCxxeDocument(document)) openDocument(document).catch(error => log(`didOpen failed: ${error.message}`));
    }),
    vscode.workspace.onDidChangeTextDocument(event => scheduleChange(event.document)),
    vscode.workspace.onDidSaveTextDocument(document => {
      if (!isCxxeDocument(document) || !client?.initialized) return;
      flushChanges(document);
      client.sendNotification('textDocument/didSave', { textDocument: { uri: document.uri.toString() } });
    }),
    vscode.workspace.onDidCloseTextDocument(document => {
      const key = document.uri.toString();
      if (document.uri.scheme === GENERATED_SCHEME) {
        for (const [source, generated] of generatedViews) if (generated.toString() === key) generatedViews.delete(source);
        return;
      }
      if (!isCxxeDocument(document)) return;
      clearTimeout(pendingChanges.get(key));
      pendingChanges.delete(key);
      if (client?.initialized && openedDocuments.has(key)) {
        client.sendNotification('textDocument/didClose', { textDocument: { uri: key } });
      }
      openedDocuments.delete(key);
      diagnostics.delete(document.uri);
    }),
    vscode.workspace.onDidChangeConfiguration(event => {
      if (!event.affectsConfiguration('cxxe')) return;
      if (event.affectsConfiguration('cxxe.languageServer.path')) {
        vscode.window.showInformationMessage('C++Extended: language server path changed.', 'Restart').then(choice => {
          if (choice === 'Restart') restartServer();
        });
        return;
      }
      if (client) client.logLevel = logLevel();
      if (client?.initialized) {
        client.sendNotification('workspace/didChangeConfiguration', { settings: { cxxe: serverSettings() } });
      }
    }),
    vscode.workspace.createFileSystemWatcher('**/*.{cppe,hppe,he}'),
    { dispose: () => { for (const t of pendingChanges.values()) clearTimeout(t); if (client) client.dispose(); client = null; } }
  );
  {
    const watcher = context.subscriptions[context.subscriptions.length - 2];
    const notifyWatched = () => client?.initialized && client.sendNotification('workspace/didChangeWatchedFiles', { changes: [] });
    watcher.onDidCreate(notifyWatched);
    watcher.onDidDelete(notifyWatched);
    watcher.onDidChange(notifyWatched);
  }

  if (vscode.workspace.textDocuments.some(isCxxeDocument)) {
    ensureServer().catch(error => log(`Startup failed: ${error.message}`));
  } else {
    setStatus('stopped', `${SERVER_NAME} starts when a C++Extended file is opened`);
    statusItem.hide();
  }
}

async function deactivate() {
  if (client) await client.shutdown();
  client = null;
}

module.exports = { activate, deactivate };
