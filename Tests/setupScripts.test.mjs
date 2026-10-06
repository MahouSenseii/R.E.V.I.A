import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

function runBuildTestPhase({ npmExit = 0, skipTests = false, nodeAvailable = true } = {}) {
  const directory = mkdtempSync(path.join(tmpdir(), 'revia-build-tests-'));
  try {
    const tools = path.join(directory, 'commands');
    mkdirSync(tools);
    const events = path.join(directory, 'events.txt');
    const argumentsPath = path.join(directory, 'npm-arguments.txt');
    const prepared = path.join(directory, 'prepared');
    writeFileSync(path.join(tools, 'npm.cmd'), [
      '@echo off',
      'echo npm>>"%REVIA_SETUP_EVENTS%"',
      ':arguments',
      'if "%~1"=="" goto install',
      'echo %~1>>"%REVIA_SETUP_ARGUMENTS%"',
      'shift',
      'goto arguments',
      ':install',
      `if ${npmExit}==0 echo ready>"%REVIA_SETUP_PREPARED%"`,
      `exit /b ${npmExit}`,
    ].join('\r\n'));
    const ctest = path.join(tools, 'ctest.cmd');
    writeFileSync(ctest, [
      '@echo off',
      'echo ctest>>"%REVIA_SETUP_EVENTS%"',
      ...(nodeAvailable ? ['if not exist "%REVIA_SETUP_PREPARED%" exit /b 74'] : []),
      'exit /b 0',
    ].join('\r\n'));
    const harness = String.raw`
$ErrorActionPreference = 'Stop'
$tokens = $null
$errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($env:REVIA_BUILD_SCRIPT, [ref]$tokens, [ref]$errors)
if ($errors.Count -ne 0) { throw 'Build script did not parse.' }
$phases = @($ast.FindAll({ param($entry)
    $entry -is [System.Management.Automation.Language.IfStatementAst] -and
        $entry.Clauses[0].Item1.Extent.Text -eq '-not $SkipTests'
}, $true))
if ($phases.Count -ne 1) { throw 'Build script must have one test phase.' }
$repoRoot = $env:REVIA_SETUP_ROOT
$ctestPath = $env:REVIA_SETUP_CTEST
$preset = 'debug'
$SkipTests = [bool]::Parse($env:REVIA_SETUP_SKIP_TESTS)
& ([scriptblock]::Create($phases[0].Extent.Text))
`;
    const powershell = path.join(process.env.SystemRoot, 'System32',
      'WindowsPowerShell', 'v1.0', 'powershell.exe');
    const result = spawnSync(powershell, ['-NoProfile', '-ExecutionPolicy', 'Bypass',
      '-EncodedCommand', Buffer.from(harness, 'utf16le').toString('base64')], {
      encoding: 'utf8', timeout: 15_000, windowsHide: true,
      env: {
        ...process.env,
        PATH: nodeAvailable ? `${tools};${process.env.PATH}` : tools,
        REVIA_BUILD_SCRIPT: fileURLToPath(new URL('../Tools/Build.ps1', import.meta.url)),
        REVIA_SETUP_ROOT: directory,
        REVIA_SETUP_CTEST: ctest,
        REVIA_SETUP_EVENTS: events,
        REVIA_SETUP_ARGUMENTS: argumentsPath,
        REVIA_SETUP_PREPARED: prepared,
        REVIA_SETUP_SKIP_TESTS: String(skipTests),
      },
    });
    assert.ifError(result.error);
    return {
      ...result,
      events: existsSync(events) ? readFileSync(events, 'utf8').trim().split(/\r?\n/) : [],
      npmArguments: existsSync(argumentsPath) ? readFileSync(argumentsPath, 'utf8').trim().split(/\r?\n/) : [],
      packageDirectory: path.join(directory, 'Tools', 'Presence', 'Live2D'),
    };
  } finally {
    assert.equal(path.dirname(directory), tmpdir());
    assert.match(path.basename(directory), /^revia-build-tests-/);
    rmSync(directory, { recursive: true, force: true });
  }
}

test('Windows build prepares pinned Live2D dependencies before CTest', {
  skip: process.platform !== 'win32',
}, () => {
  const result = runBuildTestPhase();
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.deepEqual(result.events, ['npm', 'ctest']);
  assert.deepEqual(result.npmArguments, [
    'ci', '--prefix', result.packageDirectory, '--ignore-scripts', '--no-audit', '--no-fund',
  ]);
});

test('Windows build stops before CTest when Live2D dependency installation fails', {
  skip: process.platform !== 'win32',
}, () => {
  const result = runBuildTestPhase({ npmExit: 37 });
  assert.equal(result.status, 1, result.stdout + result.stderr);
  assert.deepEqual(result.events, ['npm']);
  assert.match(result.stderr, /Live2D.*37/);
});

test('Windows build skips dependency preparation and CTest with SkipTests', {
  skip: process.platform !== 'win32',
}, () => {
  const result = runBuildTestPhase({ skipTests: true });
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.deepEqual(result.events, []);
});

test('Windows build leaves the Node dependency step optional without Node', {
  skip: process.platform !== 'win32',
}, () => {
  const result = runBuildTestPhase({ nodeAvailable: false });
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.deepEqual(result.events, ['ctest']);
});

test('Windows PowerShell health check reports missing prerequisites and exits normally', {
  skip: process.platform !== 'win32',
}, () => {
  const directory = mkdtempSync(path.join(tmpdir(), 'revia-health-check-'));
  try {
    for (const name of ['Tools', 'Config', 'Memory', 'RuntimeData']) {
      mkdirSync(path.join(directory, name));
    }
    const script = path.join(directory, 'Tools', 'HealthCheck.ps1');
    copyFileSync(fileURLToPath(new URL('../Tools/HealthCheck.ps1', import.meta.url)), script);
    writeFileSync(path.join(directory, 'Config', 'settings.json'), '{ invalid json');
    writeFileSync(path.join(directory, 'Config', 'model_manifest.json'),
      JSON.stringify({ profiles: { Full: [] }, artifacts: [] }));
    const powershell = path.join(process.env.SystemRoot, 'System32',
      'WindowsPowerShell', 'v1.0', 'powershell.exe');
    const result = spawnSync(powershell, ['-NoProfile', '-ExecutionPolicy', 'Bypass',
      '-File', script, '-Profile', 'Full', '-SkipVoice', '-SkipHashes'], {
      encoding: 'utf8', timeout: 15_000, windowsHide: true,
    });
    assert.ifError(result.error);
    assert.equal(result.status, 1, result.stdout + result.stderr);
    assert.match(result.stdout, /\[FAIL\] Configuration/);
    assert.match(result.stdout, /\[FAIL\] llama\.cpp runtime/);
    assert.match(result.stdout, /Health check failed with 5 blocking problem/);
    assert.equal(result.stderr, '', result.stderr);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});
