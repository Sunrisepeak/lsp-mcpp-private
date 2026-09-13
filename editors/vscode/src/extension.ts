// C++ Modules for VS Code: a thin client for the lsp-mcpp language server.
//
// The extension starts the server from the bundled payload, advertises the
// cxxModules protocol extension, shows one language status item, and otherwise
// stays out of sight: no notifications, no status bar items, no walkthroughs.

import * as vscode from 'vscode';
import {
    ClientCapabilities,
    CloseAction,
    CloseHandlerResult,
    ErrorAction,
    Executable,
    FeatureState,
    LanguageClient,
    LanguageClientOptions,
    MessageType,
    RevealOutputChannelOn,
    ServerOptions,
    ShowMessageNotification,
    ShowMessageRequest,
    State,
    StaticFeature,
    TransportKind,
} from 'vscode-languageclient/node';
import { registerCommands } from './commands';
import { checkConflicts, ConflictCheck } from './conflicts';
import { resolveLaunch } from './payload';
import { CxxModulesStatus, ModuleState, StatusController } from './status';

const CLIENT_ID = 'lspMcpp';
const CLIENT_NAME = 'C++ Modules';
const RESTART_WINDOW_MS = 3 * 60 * 1000;
const MAX_RESTARTS = 4;

export interface TestApi {
    waitForState(state: ModuleState | readonly ModuleState[], timeoutMs: number): Promise<CxxModulesStatus>;
    lastStatus(): CxxModulesStatus | undefined;
    // Notifications shown while LSP_MCPP_TEST=1; -1 when they could not be counted.
    notificationCount(): number;
    conflictCheck(): Promise<ConflictCheck>;
}

// Tells the server this client understands the cxxModules extension (S3).
class CxxModulesFeature implements StaticFeature {
    fillClientCapabilities(capabilities: ClientCapabilities): void {
        const experimental = (capabilities.experimental ?? {}) as Record<string, unknown>;
        experimental.cxxModules = { version: 1, status: true, graph: true, contexts: true };
        capabilities.experimental = experimental;
    }

    initialize(): void {
        // Nothing to register: the notification handler is installed on the client.
    }

    getState(): FeatureState {
        return { kind: 'static' };
    }

    clear(): void {
        // Stateless.
    }
}

function errorText(error: unknown): string {
    return error instanceof Error ? error.message : String(error);
}

function messageTypeName(type: MessageType): string {
    switch (type) {
        case MessageType.Error:
            return 'error';
        case MessageType.Warning:
            return 'warning';
        case MessageType.Info:
            return 'info';
        default:
            return 'log';
    }
}

class ServerHost implements vscode.Disposable {
    private client: LanguageClient | undefined;
    private channel: vscode.OutputChannel | undefined;
    private restarts: number[] = [];
    private queue: Promise<void> = Promise.resolve();

    constructor(private readonly context: vscode.ExtensionContext, private readonly status: StatusController) {}

    // Created on first use and kept across restarts, never revealed automatically.
    output(): vscode.OutputChannel {
        if (!this.channel) {
            this.channel = vscode.window.createOutputChannel(CLIENT_NAME);
        }
        return this.channel;
    }

    log(line: string): void {
        this.output().appendLine(`[${new Date().toLocaleTimeString()}] ${line}`);
    }

    runningClient(): LanguageClient | undefined {
        return this.client && this.client.state === State.Running ? this.client : undefined;
    }

    start(): Promise<void> {
        return this.enqueue(() => this.startNow());
    }

    stop(): Promise<void> {
        return this.enqueue(() => this.stopNow());
    }

    restart(): Promise<void> {
        return this.enqueue(async () => {
            this.restarts = [];
            await this.stopNow();
            await this.startNow();
        });
    }

    dispose(): void {
        // The client may still log while it stops, so the channel goes last.
        void this.stopNow().finally(() => {
            this.channel?.dispose();
            this.channel = undefined;
        });
    }

    private enqueue(operation: () => Promise<void>): Promise<void> {
        const next = this.queue.then(operation, operation);
        this.queue = next.catch(() => undefined);
        return next;
    }

    private async startNow(): Promise<void> {
        if (this.client) {
            return;
        }
        const resolution = resolveLaunch(this.context.extensionPath);
        if (!resolution.ok) {
            this.log(resolution.reason);
            this.status.showFailure(resolution.reason);
            return;
        }
        const launch = resolution.launch;
        const configuration = vscode.workspace.getConfiguration('lspMcpp');

        const args = ['serve'];
        if (launch.payloadDir) {
            args.push('--payload', launch.payloadDir);
        }
        if (!vscode.workspace.isTrusted) {
            args.push('--untrusted');
        }
        const logLevel = process.env.LSP_MCPP_LOG_LEVEL
            ?? (configuration.get<string>('trace.server') === 'verbose' ? 'debug' : undefined);
        if (logLevel) {
            args.push('--log-level', logLevel);
        }

        const folder = vscode.workspace.workspaceFolders?.find((candidate) => candidate.uri.scheme === 'file');
        const executable: Executable = {
            command: launch.executable,
            args,
            transport: TransportKind.stdio,
            options: { cwd: folder?.uri.fsPath, env: { ...process.env } },
        };
        const serverOptions: ServerOptions = { run: executable, debug: executable };

        const compiler = (configuration.get<string>('compiler') ?? '').trim();
        const clientOptions: LanguageClientOptions = {
            documentSelector: [
                { scheme: 'file', language: 'cpp' },
                { scheme: 'file', language: 'c' },
            ],
            outputChannel: this.output(),
            revealOutputChannelOn: RevealOutputChannelOn.Never,
            initializationOptions: {
                compiler: compiler.length > 0 ? compiler : null,
                semanticKit: configuration.get<string>('semanticKit') === 'off' ? 'off' : 'auto',
            },
            errorHandler: {
                error: () => ({ action: ErrorAction.Continue, handled: true }),
                closed: () => this.onClosed(),
            },
            initializationFailedHandler: (error) => {
                const reason = `The language server failed to initialize: ${errorText(error)}`;
                this.log(reason);
                this.status.showFailure(reason, true);
                return false;
            },
        };

        const client = new LanguageClient(CLIENT_ID, CLIENT_NAME, serverOptions, clientOptions);
        client.registerFeature(new CxxModulesFeature());
        client.onNotification('cxxModules/status', (params: CxxModulesStatus) => this.status.update(params));
        // Messages from the server go to the log, never to notifications.
        client.onNotification(ShowMessageNotification.type, (params) => {
            this.log(`server ${messageTypeName(params.type)}: ${params.message}`);
        });
        client.onRequest(ShowMessageRequest.type, (params) => {
            this.log(`server ${messageTypeName(params.type)}: ${params.message}`);
            return null;
        });

        this.client = client;
        this.status.showStarting();
        this.log(`Starting ${launch.executable} ${args.join(' ')}`);
        try {
            await client.start();
            if (!this.status.lastStatus()) {
                this.status.showRunning();
            }
        } catch (error) {
            const reason = `The language server could not be started: ${errorText(error)}`;
            this.log(reason);
            this.status.showFailure(reason, true);
        }
    }

    private async stopNow(): Promise<void> {
        const client = this.client;
        this.client = undefined;
        if (!client) {
            return;
        }
        try {
            await client.dispose(5000);
        } catch (error) {
            this.log(`Stopping the language server: ${errorText(error)}`);
        }
    }

    private onClosed(): CloseHandlerResult {
        const now = Date.now();
        this.restarts = this.restarts.filter((time) => now - time < RESTART_WINDOW_MS);
        this.restarts.push(now);
        if (this.restarts.length <= MAX_RESTARTS) {
            this.log('The language server stopped unexpectedly and is being restarted.');
            this.status.showStarting('Restarting');
            return { action: CloseAction.Restart, handled: true };
        }
        const reason = 'The language server stopped repeatedly and was not restarted.';
        this.log(reason);
        this.status.showFailure(reason, true);
        return { action: CloseAction.DoNotRestart, handled: true };
    }
}

// In extension tests, count every notification the extension (or the language
// client it bundles) shows. The API object is shared by both, so wrapping its
// methods observes them all.
function installNotificationCounter(): () => number {
    let count = 0;
    const window = vscode.window as unknown as Record<string, unknown>;
    let installed = true;
    for (const name of ['showInformationMessage', 'showWarningMessage', 'showErrorMessage']) {
        const original = window[name];
        if (typeof original !== 'function') {
            installed = false;
            continue;
        }
        const wrapped = (...args: unknown[]): unknown => {
            count += 1;
            return (original as (...inner: unknown[]) => unknown).apply(vscode.window, args);
        };
        try {
            window[name] = wrapped;
            installed = installed && window[name] === wrapped;
        } catch {
            installed = false;
        }
    }
    return () => (installed ? count : -1);
}

let activeHost: ServerHost | undefined;

export function activate(context: vscode.ExtensionContext): TestApi {
    const countNotifications = process.env.LSP_MCPP_TEST === '1' ? installNotificationCounter() : () => 0;

    const status = new StatusController();
    const host = new ServerHost(context, status);
    activeHost = host;
    context.subscriptions.push(status, host);

    registerCommands(context, {
        runningClient: () => host.runningClient(),
        restart: () => host.restart(),
        showLogs: () => host.output().show(true),
        log: (line) => host.log(line),
    });

    context.subscriptions.push(
        vscode.workspace.onDidChangeConfiguration((event) => {
            if (event.affectsConfiguration('lspMcpp.compiler') || event.affectsConfiguration('lspMcpp.semanticKit')) {
                void host.restart();
            }
        }),
        vscode.workspace.onDidGrantWorkspaceTrust(() => {
            void host.restart();
        }),
    );

    const conflicts = checkConflicts(context, (line) => host.log(line)).catch((error): ConflictCheck => {
        host.log(`Checking for conflicting extensions: ${errorText(error)}`);
        return 'none-found';
    });
    void host.start();

    return {
        waitForState: (state, timeoutMs) => status.waitForState(state, timeoutMs),
        lastStatus: () => status.lastStatus(),
        notificationCount: countNotifications,
        conflictCheck: () => conflicts,
    };
}

export async function deactivate(): Promise<void> {
    const host = activeHost;
    activeHost = undefined;
    await host?.stop();
}
