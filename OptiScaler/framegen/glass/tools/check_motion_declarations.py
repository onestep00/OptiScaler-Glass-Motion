"""Build and check declaration tables, relocation and the startup adapter offline.

Never installs files, attaches to Cyberpunk, or executes the game's instructions.
The plugin's negative startup test runs in this Python process with mock SDK hooks.
"""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import argparse
import ctypes as c
import hashlib
import json
import subprocess
import sys
import time


def foreign_startup(folder):
    class Sdk(c.Structure):
        _fields_ = [(name, c.c_void_p) for name in ('runtime', 'logger', 'hooking', 'states', 'scripts')]
    calls = []
    attach_type = c.WINFUNCTYPE(c.c_bool, c.c_void_p, c.c_void_p, c.c_void_p, c.c_void_p)
    detach_type = c.WINFUNCTYPE(c.c_bool, c.c_void_p, c.c_void_p)
    attach = attach_type(lambda *args: calls.append('attach') or False)
    detach = detach_type(lambda *args: calls.append('detach') or True)
    hooks = (c.c_void_p * 2)(c.cast(attach, c.c_void_p), c.cast(detach, c.c_void_p))
    sdk = Sdk(hooking=c.addressof(hooks))
    plugin = c.WinDLL(str(folder / 'GlassMotion.dll'))
    plugin.Supports.restype = c.c_uint32
    plugin.Main.argtypes = [c.c_void_p, c.c_uint8, c.POINTER(Sdk)]
    plugin.Main.restype = c.c_bool
    assert plugin.Supports() == 1
    assert not plugin.Main(1, 0, c.byref(sdk)) and not calls
    statuses = list(folder.glob('startup-*/status.json'))
    assert len(statuses) == 1
    status = json.loads(statuses[0].read_text())
    assert status['startup_status'] == 'native_layout_rejected'
    assert not status['enabled'] and not status['native_routes_resolved']
    assert plugin.Main(1, 1, c.byref(sdk)) and not calls
    return dict(foreign_executable_rejected=True, hook_attempts=len(calls), rejection_status_saved=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--sdk', type=Path)
    parser.add_argument('--game-exe', type=Path,
                        default=Path('C:/Program Files (x86)/Steam/steamapps/common/Cyberpunk 2077/bin/x64/Cyberpunk2077.exe'))
    args = parser.parse_args()
    p = args.workspace.resolve(strict=True)
    work = p.parent
    module = Path(__file__).resolve().parent.parent
    sdk = (args.sdk or work / 'glass-red4ext-sdk').resolve(strict=True)
    output = p / ('declaration-adapter-' + str(time.time_ns()))
    output.mkdir()
    common = [sys.executable, str(work / 'run-msvc.py'), 'cl.exe', '/nologo', '/std:c++20',
              '/EHsc', '/O2', '/MD', '/W4', '/WX', '/DNOMINMAX', '/I' + str(module)]
    detours = module.parents[1] / 'library/detours/detours.lib'
    include = module.parents[1] / 'include'
    jobs = [
        ('table', module / 'tests/MotionDeclarations.cpp', [], []),
        ('scope', module / 'tests/MotionShaderScope.cpp', [], []),
        ('compatibility', module / 'tests/DeclarationCompatibility.cpp', [], []),
        ('callback', module / 'CyberpunkDeclarationProbe.cpp',
         ['/DGLASS_DECLARATION_SELFTEST', '/I' + str(include)], [str(detours)]),
        ('GlassMotion', module / 'CyberpunkDeclarationProbe.cpp',
         ['/LD', '/DGLASS_RED4EXT_STARTUP', '/DRED4EXT_HEADER_ONLY', '/external:I' + str(sdk / 'include'),
          '/external:W0'], ['/IMPLIB:' + str(output / 'GlassMotion.lib')]),
    ]

    def build(job):
        name, source, flags, link = job
        artifact = output / (name + ('.dll' if name == 'GlassMotion' else '.exe'))
        command = common + flags + [str(source), '/Fe' + str(artifact), '/Fo' + str(output / (name + '.obj'))]
        if link:
            command += ['/link'] + link
        log_file = output / (name + '-build.log')
        with log_file.open('w', encoding='utf-8') as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError(name + ': ' + '\n'.join(log_file.read_text(errors='replace').splitlines()[-10:]))
        return name, artifact

    with ThreadPoolExecutor(max_workers=4) as pool:
        artifacts = dict(pool.map(build, jobs))

    def check(name, arguments):
        result = subprocess.run([str(artifacts[name])] + list(map(str, arguments)), capture_output=True, text=True)
        (output / (name + '-check.log')).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(name + ': exit ' + str(result.returncode) + ' ' + result.stderr[-600:])
        return json.loads(result.stdout.strip().splitlines()[-1])

    checks = dict(table=check('table', []), scope=check('scope', []), compatibility=check('compatibility', [args.game_exe]),
                  callback=check('callback', [p / 'pending-motion-declarations']))
    checks['startup'] = foreign_startup(output)
    sources = ['MotionDeclarationTable.h', 'CyberpunkDeclarationProbe.cpp', 'CyberpunkDeclarationProfile.h',
               'RelocatableCode.h', 'MotionShaderScope.h', 'tests/MotionShaderScope.cpp',
               'tests/MotionDeclarations.cpp', 'tests/DeclarationCompatibility.cpp']
    result = dict(folder=str(output), dll_sha256=hashlib.sha256(artifacts['GlassMotion'].read_bytes()).hexdigest(),
                  sources={name: hashlib.sha256((module / name).read_bytes()).hexdigest() for name in sources},
                  checks=checks, deployed=False, live_supply_verified=False, fg_input_changed=False)
    (output / 'result.json').write_text(json.dumps(result, indent=2))
    (p / 'declaration-adapter-latest.json').write_text(json.dumps(result, indent=2))
    print(json.dumps({k: v for k, v in result.items() if k != 'sources'}))


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(json.dumps(dict(error=type(error).__name__, detail=str(error)[-1500:])))
        sys.exit(1)
