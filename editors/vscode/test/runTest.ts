// Runs the end-to-end suite in a downloaded VS Code against a copy of the
// `inferred` conformance fixture: C++ modules with no build system.
//
//   LSP_MCPP_PAYLOAD=<assembled payload>   (or a payload/ directory in this extension)
//   LSP_MCPP_SERVER=<lsp-mcpp executable>  (optional; overrides payload/bin)
//   VSCODE_TEST_VERSION=stable             (optional)
//   npm run compile && npm test            (Linux without a display: xvfb-run -a npm test)

import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import { runTests } from '@vscode/test-electron';

const BUILD_FILES = ['mcpp.toml', 'mcpp.lock', 'CMakeLists.txt', 'compile_commands.json', 'build_database.json', 'target', 'build', '.cache', 'scenario.json'];

function prepareWorkspace(extensionDevelopmentPath: string): string {
    const repositoryRoot = path.resolve(extensionDevelopmentPath, '..', '..');
    const candidates = [
        path.join(repositoryRoot, 'conformance', 'fixtures', 'inferred'),
        path.join(repositoryRoot, '.agents', 'docs', 'assets', '2026-09-13-modules-lsp-spike', 'fixture'),
    ];
    const fixture = candidates.find((candidate) => fs.existsSync(path.join(candidate, 'src', 'main.cpp')));
    if (!fixture) {
        throw new Error(`No fixture found; looked in:\n${candidates.join('\n')}`);
    }
    const workspace = fs.mkdtempSync(path.join(os.tmpdir(), 'lsp-mcpp-e2e-'));
    fs.cpSync(fixture, workspace, { recursive: true });
    // The inferred fixture has no build system; remove anything that would make it another kind.
    for (const name of BUILD_FILES) {
        fs.rmSync(path.join(workspace, name), { recursive: true, force: true });
    }
    return workspace;
}

async function main(): Promise<void> {
    const extensionDevelopmentPath = path.resolve(__dirname, '..', '..');
    const extensionTestsPath = path.resolve(__dirname, 'suite', 'index');
    const workspace = prepareWorkspace(extensionDevelopmentPath);
    const cacheDirectory = process.env.LSP_MCPP_CACHE_DIR ?? fs.mkdtempSync(path.join(os.tmpdir(), 'lsp-mcpp-e2e-cache-'));
    console.log(`workspace: ${workspace}`);
    console.log(`server cache: ${cacheDirectory}`);

    const exitCode = await runTests({
        version: process.env.VSCODE_TEST_VERSION ?? 'stable',
        extensionDevelopmentPath,
        extensionTestsPath,
        launchArgs: [
            workspace,
            '--disable-extensions',
            '--disable-workspace-trust',
            '--skip-welcome',
            '--skip-release-notes',
            '--disable-telemetry',
        ],
        extensionTestsEnv: {
            LSP_MCPP_TEST: '1',
            LSP_MCPP_E2E_WORKSPACE: workspace,
            LSP_MCPP_CACHE_DIR: cacheDirectory,
        },
    });
    if (exitCode !== 0) {
        throw new Error(`The end-to-end suite exited with ${exitCode}.`);
    }
}

main().catch((error: unknown) => {
    console.error(error instanceof Error ? error.stack ?? error.message : error);
    process.exit(1);
});
