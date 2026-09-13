// The one piece of user interface this extension keeps visible: a language
// status item for C++ files, driven by the server's cxxModules/status notification.

import * as vscode from 'vscode';

export type ModuleState = 'starting' | 'loading' | 'preparing' | 'ready' | 'degraded' | 'error';

export interface SemanticProfile {
    kind: 'build-toolchain' | 'semantic-kit';
    compiler?: string;
    stdlib: string;
    target: string;
}

export interface IssueCommand {
    title: string;
    command: string;
    arguments?: unknown[];
}

export interface ModuleIssue {
    code: string;
    message: string;
    command?: IssueCommand;
}

export interface CxxModulesStatus {
    state: ModuleState;
    project: {
        root: string;
        source: 'mcpp' | 'cmake' | 'build-database' | 'compile-commands' | 'inferred';
        level?: number;
    };
    profile: SemanticProfile;
    engine: { name: 'clangd'; version: string };
    progress?: { done: number; total: number };
    issues?: ModuleIssue[];
}

const SHOW_LOGS: vscode.Command = { title: 'Show Logs', command: 'lspMcpp.showLogs' };
const RESTART: vscode.Command = { title: 'Restart', command: 'lspMcpp.restartServer' };
const BUSY_STATES: readonly ModuleState[] = ['starting', 'loading', 'preparing'];

interface Waiter {
    states: readonly ModuleState[];
    resolve: (status: CxxModulesStatus) => void;
    reject: (error: Error) => void;
    timer: NodeJS.Timeout;
}

export function describeProfile(profile: SemanticProfile | undefined): string {
    if (!profile) {
        return '';
    }
    return profile.compiler && profile.compiler.length > 0 ? profile.compiler : profile.stdlib ?? '';
}

function stateText(status: CxxModulesStatus): string | undefined {
    switch (status.state) {
        case 'starting':
            return 'Starting';
        case 'loading':
            return 'Loading the project';
        case 'preparing':
            return status.progress && status.progress.total > 0
                ? `Preparing modules ${status.progress.done}/${status.progress.total}`
                : 'Preparing modules';
        case 'ready':
            return undefined;
        case 'degraded':
            return 'Some features are limited';
        case 'error':
            return 'Only module-level features are available';
    }
}

export class StatusController implements vscode.Disposable {
    private readonly item: vscode.LanguageStatusItem;
    private current: CxxModulesStatus | undefined;
    private failure: string | undefined;
    private readonly waiters = new Set<Waiter>();

    constructor() {
        this.item = vscode.languages.createLanguageStatusItem('lspMcpp.status', { language: 'cpp' });
        this.item.name = 'C++ Modules';
        this.showStarting();
    }

    showStarting(detail = 'Starting'): void {
        this.failure = undefined;
        this.current = undefined;
        this.item.text = 'C++ Modules';
        this.item.detail = detail;
        this.item.busy = true;
        this.item.severity = vscode.LanguageStatusSeverity.Information;
        this.item.command = SHOW_LOGS;
    }

    // The server is up but has not described itself; it does not implement cxxModules/status.
    showRunning(): void {
        this.item.text = 'C++ Modules';
        this.item.detail = 'Running';
        this.item.busy = false;
        this.item.severity = vscode.LanguageStatusSeverity.Information;
        this.item.command = SHOW_LOGS;
    }

    // A failure seen by the extension itself: no payload, or a server that will not stay up.
    showFailure(message: string, offerRestart = false): void {
        this.failure = message;
        this.item.text = 'C++ Modules';
        this.item.detail = message;
        this.item.busy = false;
        this.item.severity = vscode.LanguageStatusSeverity.Error;
        this.item.command = offerRestart ? RESTART : SHOW_LOGS;
        for (const waiter of [...this.waiters]) {
            this.settle(waiter);
            waiter.reject(new Error(message));
        }
    }

    update(status: CxxModulesStatus): void {
        this.current = status;
        this.failure = undefined;
        const label = describeProfile(status.profile);
        this.item.text = label.length > 0 ? `C++ Modules · ${label}` : 'C++ Modules';

        const details: string[] = [];
        const state = stateText(status);
        if (state) {
            details.push(state);
        }
        if (status.project) {
            details.push(status.project.level !== undefined
                ? `${status.project.source} · level ${status.project.level}`
                : status.project.source);
        }
        if (status.engine) {
            details.push(`${status.engine.name} ${status.engine.version}`);
        }
        const issues = status.issues ?? [];
        if ((status.state === 'degraded' || status.state === 'error') && issues.length > 0) {
            details.push(issues[0].message);
        }
        this.item.detail = details.join(' · ');
        this.item.busy = BUSY_STATES.includes(status.state);
        this.item.severity = status.state === 'error'
            ? vscode.LanguageStatusSeverity.Error
            : status.state === 'degraded'
                ? vscode.LanguageStatusSeverity.Warning
                : vscode.LanguageStatusSeverity.Information;
        const withCommand = issues.find((issue) => issue.command !== undefined);
        this.item.command = withCommand?.command
            ? { title: withCommand.command.title, command: withCommand.command.command, arguments: withCommand.command.arguments }
            : SHOW_LOGS;

        for (const waiter of [...this.waiters]) {
            if (waiter.states.includes(status.state)) {
                this.settle(waiter);
                waiter.resolve(status);
            } else if (status.state === 'error') {
                this.settle(waiter);
                waiter.reject(new Error(`The server reported the error state: ${issues.map((issue) => issue.message).join('; ')}`));
            }
        }
    }

    lastStatus(): CxxModulesStatus | undefined {
        return this.current;
    }

    waitForState(state: ModuleState | readonly ModuleState[], timeoutMs: number): Promise<CxxModulesStatus> {
        const states: readonly ModuleState[] = typeof state === 'string' ? [state] : state;
        if (this.current && states.includes(this.current.state)) {
            return Promise.resolve(this.current);
        }
        if (this.failure) {
            return Promise.reject(new Error(this.failure));
        }
        return new Promise<CxxModulesStatus>((resolve, reject) => {
            const waiter: Waiter = {
                states,
                resolve,
                reject,
                timer: setTimeout(() => {
                    this.waiters.delete(waiter);
                    reject(new Error(`Timed out after ${timeoutMs} ms waiting for ${states.join(' or ')}; last status: ${JSON.stringify(this.current)}`));
                }, timeoutMs),
            };
            this.waiters.add(waiter);
        });
    }

    dispose(): void {
        for (const waiter of [...this.waiters]) {
            this.settle(waiter);
            waiter.reject(new Error('The extension was deactivated.'));
        }
        this.item.dispose();
    }

    private settle(waiter: Waiter): void {
        clearTimeout(waiter.timer);
        this.waiters.delete(waiter);
    }
}
