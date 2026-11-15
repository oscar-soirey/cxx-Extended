'use strict';

const cp = require('node:child_process');
const { EventEmitter } = require('node:events');

class LspClient extends EventEmitter {
  constructor(output, logLevel = 'info') {
    super();
    this.output = output;
    this.logLevel = logLevel;
    this.proc = null;
    this.buffer = Buffer.alloc(0);
    this.nextId = 1;
    this.pending = new Map();
    this.initialized = false;
    this.closed = false;
  }

  log(message) {
    if (this.logLevel === 'silent') return;
    this.output.appendLine(`[client] ${message}`);
  }

  debug(message) {
    if (this.logLevel !== 'debug') return;
    this.output.appendLine(`[jsonrpc] ${message}`);
  }

  async start(command, args = [], cwd = undefined, initializeOptions = {}) {
    if (this.proc) return;

    this.log(`Starting ${command}${args.length ? ` ${args.join(' ')}` : ''}`);
    this.proc = cp.spawn(command, args, {
      cwd,
      stdio: ['pipe', 'pipe', 'pipe'],
      windowsHide: true,
      shell: false
    });

    this.proc.stdout.on('data', data => this.acceptData(data));
    this.proc.stderr.on('data', data => {
      const text = data.toString('utf8').trimEnd();
      if (text) this.output.appendLine(`[server] ${text}`);
    });
    this.proc.on('error', error => {
      this.emit('error', error);
    });
    this.proc.on('exit', (code, signal) => {
      this.log(`Server exited (code=${code}, signal=${signal || 'none'})`);
      const error = new Error(`C++Extended language server exited (code=${code}, signal=${signal || 'none'})`);
      for (const { reject } of this.pending.values()) reject(error);
      this.pending.clear();
      this.proc = null;
      this.initialized = false;
      this.emit('exit', code, signal);
    });

    const result = await this.request('initialize', {
      processId: process.pid,
      clientInfo: { name: 'C++Extended VS Code extension', version: '0.3.0' },
      rootUri: initializeOptions.rootUri ?? null,
      workspaceFolders: initializeOptions.workspaceFolders ?? null,
      initializationOptions: initializeOptions.settings ?? {},
      capabilities: {
        textDocument: {
          synchronization: { dynamicRegistration: false, willSave: false, willSaveWaitUntil: false, didSave: true },
          completion: { completionItem: { snippetSupport: true, documentationFormat: ['markdown', 'plaintext'] } },
          hover: { contentFormat: ['markdown', 'plaintext'] },
          definition: { linkSupport: false },
          references: {},
          documentHighlight: {},
          rename: { prepareSupport: true },
          documentLink: {},
          signatureHelp: { signatureInformation: { documentationFormat: ['markdown', 'plaintext'], activeParameterSupport: true } },
          documentSymbol: { hierarchicalDocumentSymbolSupport: true },
          semanticTokens: { requests: { full: true }, formats: ['relative'], tokenTypes: [], tokenModifiers: [] },
          publishDiagnostics: { versionSupport: true }
        },
        workspace: { workspaceSymbol: {}, configuration: false, didChangeConfiguration: {} },
        general: { positionEncodings: ['utf-16'] }
      }
    });
    this.serverCapabilities = result?.capabilities || {};
    this.serverInfo = result?.serverInfo || {};
    this.sendNotification('initialized', {});
    this.initialized = true;
    this.log(`Language server initialized (${this.serverInfo.name || 'server'} ${this.serverInfo.version || ''}).`);
  }

  acceptData(data) {
    this.buffer = Buffer.concat([this.buffer, Buffer.from(data)]);
    for (;;) {
      const headerEnd = this.buffer.indexOf(Buffer.from('\r\n\r\n'));
      if (headerEnd < 0) return;

      const header = this.buffer.subarray(0, headerEnd).toString('ascii');
      const match = /(?:^|\r\n)Content-Length\s*:\s*(\d+)(?:\r?\n|$)/i.exec(header);
      if (!match) {
        this.output.appendLine('[error] Invalid LSP message header: missing Content-Length.');
        this.buffer = this.buffer.subarray(headerEnd + 4);
        continue;
      }

      const length = Number(match[1]);
      const messageStart = headerEnd + 4;
      if (this.buffer.length < messageStart + length) return;

      const body = this.buffer.subarray(messageStart, messageStart + length).toString('utf8');
      this.buffer = this.buffer.subarray(messageStart + length);
      this.handleMessage(body);
    }
  }

  handleMessage(body) {
    let message;
    try {
      message = JSON.parse(body);
    } catch (error) {
      this.output.appendLine(`[error] Invalid JSON from language server: ${error.message}`);
      return;
    }
    this.debug(`<- ${body}`);

    if (Object.prototype.hasOwnProperty.call(message, 'id') &&
        (Object.prototype.hasOwnProperty.call(message, 'result') || Object.prototype.hasOwnProperty.call(message, 'error'))) {
      const pending = this.pending.get(message.id);
      if (!pending) return;
      this.pending.delete(message.id);
      if (message.error) {
        const error = new Error(message.error.message || 'Language server request failed');
        error.code = message.error.code;
        error.data = message.error.data;
        pending.reject(error);
      } else {
        pending.resolve(message.result);
      }
      return;
    }

    if (message.method) this.emit('notification', message.method, message.params);
  }

  write(message) {
    if (!this.proc || !this.proc.stdin.writable) throw new Error('C++Extended language server is not running.');
    const body = Buffer.from(JSON.stringify(message), 'utf8');
    const header = Buffer.from(`Content-Length: ${body.length}\r\n\r\n`, 'ascii');
    this.debug(`-> ${body.toString('utf8')}`);
    this.proc.stdin.write(Buffer.concat([header, body]));
  }

  request(method, params) {
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      try {
        this.write({ jsonrpc: '2.0', id, method, params });
      } catch (error) {
        this.pending.delete(id);
        reject(error);
      }
    });
  }

  sendNotification(method, params) {
    this.write({ jsonrpc: '2.0', method, params });
  }

  async shutdown() {
    if (!this.proc) return;
    try {
      if (this.initialized) await this.request('shutdown', null);
    } catch (_) {
      // Server may already have terminated.
    }
    try {
      this.sendNotification('exit', null);
    } catch (_) {
      // Ignore a closed pipe during extension shutdown.
    }
    await new Promise(resolve => {
      const proc = this.proc;
      if (!proc) return resolve();
      const timer = setTimeout(() => {
        try { proc.kill(); } catch (_) {}
        resolve();
      }, 1000);
      proc.once('exit', () => {
        clearTimeout(timer);
        resolve();
      });
    });
    this.dispose();
  }

  dispose() {
    if (this.proc) {
      try { this.proc.kill(); } catch (_) {}
    }
    this.proc = null;
    this.initialized = false;
    this.closed = true;
    for (const { reject } of this.pending.values()) reject(new Error('Language server disposed.'));
    this.pending.clear();
    this.removeAllListeners();
  }
}

module.exports = { LspClient };
