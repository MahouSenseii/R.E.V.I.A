import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { copyFileSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

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
