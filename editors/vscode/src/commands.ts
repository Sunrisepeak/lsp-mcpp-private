// The four commands: select context, show module graph, restart, show logs.

import * as vscode from 'vscode';
import type { LanguageClient } from 'vscode-languageclient/node';
import { describeProfile, SemanticProfile } from './status';

export interface ServerAccess {
    // The running client, or undefined when the server is not running.
    runningClient(): LanguageClient | undefined;
    restart(): Promise<void>;
    showLogs(): void;
    log(line: string): void;
}

interface ProtocolRange {
    start: { line: number; character: number };
    end: { line: number; character: number };
}

export type ModuleUnitRole =
    | 'module-interface'
    | 'module-partition-interface'
    | 'module-partition-implementation'
    | 'module-implementation'
    | 'non-module'
    | 'unknown';

export interface ModuleGraph {
    modules: { name: string; external: boolean; units: { uri: string; role: ModuleUnitRole }[] }[];
    imports: { from: string; module: string; range: ProtocolRange }[];
}

export interface ContextList {
    current: string;
    available: { id: string; label: string; profile: SemanticProfile }[];
}

interface ContextPick extends vscode.QuickPickItem {
    id?: string;
}

const CPP_LANGUAGES: readonly string[] = ['cpp', 'c'];

function errorText(error: unknown): string {
    return error instanceof Error ? error.message : String(error);
}

async function selectContext(access: ServerAccess): Promise<void> {
    const title = 'C++ Modules: Select Context';
    const client = access.runningClient();
    const editor = vscode.window.activeTextEditor;
    if (!client) {
        await vscode.window.showQuickPick([{ label: 'The C++ Modules language server is not running.' }], { title });
        return;
    }
    if (!editor || !CPP_LANGUAGES.includes(editor.document.languageId)) {
        await vscode.window.showQuickPick([{ label: 'Open a C++ file to choose the context it is analyzed in.' }], { title });
        return;
    }
    const textDocument = { uri: client.code2ProtocolConverter.asUri(editor.document.uri) };
    let contexts: ContextList;
    try {
        contexts = await client.sendRequest<ContextList>('cxxModules/contexts', { textDocument });
    } catch (error) {
        access.log(`cxxModules/contexts failed: ${errorText(error)}`);
        await vscode.window.showQuickPick([{ label: `The contexts could not be read: ${errorText(error)}` }], { title });
        return;
    }
    const available = contexts?.available ?? [];
    if (available.length === 0) {
        await vscode.window.showQuickPick([{ label: 'This file has a single context.' }], { title });
        return;
    }
    const items: ContextPick[] = available.map((context) => ({
        id: context.id,
        label: context.label,
        description: context.id === contexts.current ? `${describeProfile(context.profile)} · current` : describeProfile(context.profile),
        detail: context.profile?.target,
    }));
    const choice = await vscode.window.showQuickPick(items, { title, placeHolder: 'The build context this file is analyzed in' });
    if (!choice?.id || choice.id === contexts.current) {
        return;
    }
    try {
        await client.sendRequest('cxxModules/setContext', { textDocument, context: choice.id });
    } catch (error) {
        access.log(`cxxModules/setContext failed: ${errorText(error)}`);
    }
}

export function renderGraph(graph: ModuleGraph, toDisplayPath: (uri: string) => string): string {
    const lines: string[] = ['# C++ Module Graph', ''];
    const modules = [...(graph?.modules ?? [])].sort((a, b) => a.name.localeCompare(b.name));
    if (modules.length === 0) {
        lines.push('No modules were found in this workspace.');
        return `${lines.join('\n')}\n`;
    }
    const importers = new Map<string, string[]>();
    for (const edge of graph.imports ?? []) {
        const list = importers.get(edge.module) ?? [];
        list.push(`${toDisplayPath(edge.from)}:${edge.range.start.line + 1}`);
        importers.set(edge.module, list);
    }
    for (const module of modules) {
        lines.push(`## ${module.name}${module.external ? ' (external)' : ''}`, '');
        for (const unit of module.units) {
            lines.push(`- ${unit.role}: ${toDisplayPath(unit.uri)}`);
        }
        const users = importers.get(module.name) ?? [];
        if (users.length > 0) {
            lines.push('', 'Imported by:', '');
            for (const user of users.sort()) {
                lines.push(`- ${user}`);
            }
        }
        lines.push('');
    }
    return `${lines.join('\n')}\n`;
}

async function showModuleGraph(access: ServerAccess): Promise<void> {
    const client = access.runningClient();
    let content: string;
    if (!client) {
        content = '# C++ Module Graph\n\nThe C++ Modules language server is not running. Run **C++ Modules: Show Logs** for details.\n';
    } else {
        try {
            const graph = await client.sendRequest<ModuleGraph>('cxxModules/graph', {});
            content = renderGraph(graph, (uri) => vscode.workspace.asRelativePath(client.protocol2CodeConverter.asUri(uri), false));
        } catch (error) {
            access.log(`cxxModules/graph failed: ${errorText(error)}`);
            content = `# C++ Module Graph\n\nThe module graph could not be read: ${errorText(error)}\n`;
        }
    }
    const document = await vscode.workspace.openTextDocument({ language: 'markdown', content });
    await vscode.window.showTextDocument(document, { preview: true });
}

export function registerCommands(context: vscode.ExtensionContext, access: ServerAccess): void {
    context.subscriptions.push(
        vscode.commands.registerCommand('lspMcpp.selectContext', () => selectContext(access)),
        vscode.commands.registerCommand('lspMcpp.showModuleGraph', () => showModuleGraph(access)),
        vscode.commands.registerCommand('lspMcpp.restartServer', () => access.restart()),
        vscode.commands.registerCommand('lspMcpp.showLogs', () => access.showLogs()),
    );
}
