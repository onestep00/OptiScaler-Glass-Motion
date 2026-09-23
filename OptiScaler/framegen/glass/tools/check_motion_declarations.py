"""Build and check declaration tables, shader-pair scope and relocation offline.

Never installs files, attaches to Cyberpunk, or executes the game's instructions.
The hook itself is NativeMotionDeclarations.cpp in the product DLL; its
installation is reported live by the status DECL line.
"""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import argparse
import hashlib
import json
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--game-exe', type=Path,
                        default=Path('C:/Program Files (x86)/Steam/steamapps/common/Cyberpunk 2077/bin/x64/Cyberpunk2077.exe'))
    args = parser.parse_args()
    p = args.workspace.resolve(strict=True)
    work = p.parent
    module = Path(__file__).resolve().parent.parent
    output = p / ('declaration-adapter-' + str(time.time_ns()))
    output.mkdir()
    common = [sys.executable, str(work / 'run-msvc.py'), 'cl.exe', '/nologo', '/std:c++20',
              '/EHsc', '/O2', '/MD', '/W4', '/WX', '/DNOMINMAX', '/I' + str(module)]
    jobs = [
        ('table', module / 'tests/MotionDeclarations.cpp', [], []),
        ('scope', module / 'tests/MotionShaderScope.cpp', [], []),
        ('compatibility', module / 'tests/DeclarationCompatibility.cpp', [], []),
    ]

    def build(job):
        name, source, flags, link = job
        artifact = output / (name + '.exe')
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

    checks = dict(table=check('table', []), scope=check('scope', []), compatibility=check('compatibility', [args.game_exe]))
    sources = ['MotionDeclarationTable.h', 'CyberpunkDeclarationProfile.h',
               'RelocatableCode.h', 'MotionShaderScope.h', 'tests/MotionShaderScope.cpp',
               'tests/MotionDeclarations.cpp', 'tests/DeclarationCompatibility.cpp']
    result = dict(folder=str(output),
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
