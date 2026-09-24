"""Export validated native grafts for the module's Glass/grafts catalog.

Reads <workspace>/native-grafted/index.json and verification.json and writes
<out>/grafts/index.bin plus one <sha256>.dxil per exported root graft and, when
its camera-only variant is verified, <sha256>.camera.dxil. index.bin is
"GGRAFT02", u32 count, u32 reserved (0), then count 52-byte records sorted by sha:
{ u8 sha256[32]; u32 current_output; u32 previous_output; u32 supply_class;
  u32 camera_current_output; u32 camera_previous_output }.
sha256 is the original VS DXBC container hash the module compares against.
The camera outputs are 0xFFFFFFFF when the camera variant is not exported; the
root outputs are 0xFFFFFFFF for a camera-only record (no <sha256>.dxil).

The camera-only variant applies the native previous camera rows to the target's
own current world position; it reads no b7 row. It is exported when
camera_status is "validated" and its camera verification row has all checks
true, beside its root graft or alone:
  - a matched shader whose root graft is not exported (status "unsupported",
    or "factory_mismatch": a skinned-factory target whose root-only twins of
    another vertex factory graft_native_motion.py refused and whose other
    twins did not validate);
  - a generic_camera shader (no native current-position twin: the canonical
    native previous view-projection multiply on its own world position).
A camera-only record's supply_class is the target's own current position class
(camera_supply_class), since no previous graph is copied from a native VS.

Vehicle targets (index.json field "vehicle", graft_native_motion.py) never get a
camera-only record: the vehicle moves with its own transform, whose engine
supply (MotionMatrix) such a VS does not receive, and camera motion alone is
wrong on a moving vehicle. Their draws keep the engine's motion. Each refused
VS is listed in <out>/grafts/refused.bin: "GGREFS01", u32 count, u32 reserved
(0), then count 36-byte records sorted by sha: { u8 sha256[32]; u32 reason },
reason 1 = vehicle object motion without engine supply. A vehicle root graft
keeps its camera variant: array and multi-instance draws follow the engine's
array convention (previous view-projection on each element's current world
position).

A graft is exported only when its status is "validated", its verification row
has all four checks true and it requires engine motion rows [24, 25, 26].

supply_class comes from <workspace>/native-previous-supply-union.json, keyed by
the graft's native_sha256 (the original velocity VS whose previous-clip graph
the graft copied). The union's previous.dependencies and loads[] describe that
previous-clip graph only:
  bit 1 skinning   - previous inputs BLENDINDICES/BLENDWEIGHT/
                     INSTANCE_SKINNING_DATA/BONEINDEX, or a load from SRV t10
                     (bone buffer)
  bit 2 preskinned - a load from SRV t9 or constant buffer b3 (preskinned
                     previous vertices)
  bit 0 root-only  - neither: the previous graph reads the MotionMatrix and
                     camera rows plus the draw's own constants and vertex
                     attributes only
A root graft without a union entry is refused.

--skinned-previous-world current reads <workspace>/native-grafted-skinned-current
(graft_native_motion.py and verify_native_grafts.py with the same flag). There a
class-2 root graft may carry previous_world "current": it reads the target's own
current INSTANCE_TRANSFORM rows instead of the MotionMatrix and is exported with
required engine motion rows [] instead of [24, 25, 26]; its supply class must be
2. Vehicle targets keep the MotionMatrix previous world there too. All other
records follow the rules above.

Light pixel shaders: <out>/grafts/light-ps.bin lists every pixel shader all of
whose technique passes in the shader-cache census <workspace>/
all-cache-techniques.json (techniques[].pass; programs[] of kind "ps" by
sha256) are light passes. The packed capture adds the brightness term of its
record opacity, saturate(luma(F) * scale) for the colour F the draw adds, only
for these (RewriteMaterialMotion lightTarget); every other PS records its
material opacity alone. Light passes are renderstage_ plus
  transparent, transparent_notxaa, transparent_back_face,
  transparent_depth_write, transparent_background,
  transparent_notxaa_background, unlit, screenspace_vfx
These write lit or emitted colour into the scene colour the display shows.
The other passes of blended draws do not: distortion writes refraction offsets,
and the mark_rt, hologram_depth, hair_alpha_accum, highlights, wireframe,
gbuffer, cascade, overdraw and water-depth passes write marks, depth, hair
coverage, highlight masks, G-buffer or shadow values or debug colour. A PS
that also appears in any other pass is not listed (8 PS of screenspace_vfx and
vision in the 2026-09 census): the module knows a pipeline's PS, not its pass.
light-ps.bin is "GGLTPS01", u32 count, u32 reserved (0), then count sorted,
unique 32-byte SHA-256 digests of the PS DXBC containers.

Background pixel shaders: <out>/grafts/background-ps.bin lists every pixel
shader whose displayed pixel is background content rather than the surface it
is drawn on (same census):
  - every PS of a renderstage_distortion technique: it writes the screen-space
    offset by which the engine later resamples the scene behind the surface;
  - every PS of a renderstage_screenspace_vfx technique of a cloak* or
    optical_camouflage* material: it samples the scene colour at SV_Position
    plus a normal-map offset (8477c369);
  - every PS of a global_water_patch or water_plane technique, any pass: the
    transparent PS shows the bottom through a depth read at a refracted
    offset and an environment map sampled by the reflected vector (231949c6).
The surface's motion is wrong inside such a draw, so the module compiles its
packed variants coverage-only (GeometryPipeline.cpp createTarget
backgroundTarget): record opacity 0 and no brightness term, light-pass PS
included; only the boundary band takes the object's motion. The distortion
draws are blended and reach the packed capture (21 distortion PS in the
2026-09-24 per-pipeline reports), so the blend-equation rejection does not keep
them out and they are listed by pass. The 2026-09 census gives 152 PS: 129
distortion, 12 cloak screen-space VFX and 15 water PS (4 of them distortion).
One water PS (67645e0a, planar reflection and water depth) also draws the
hologram_depth pass of two cloak materials; the module knows a pipeline's PS,
not its material. light-ps.bin is unchanged: 16 listed PS (the 12 cloak
screen-space VFX PS and the 4 water transparent PS) are also light-pass PS, and
the coverage-only variant drops their brightness term.
background-ps.bin has the layout of light-ps.bin with the magic "GGBGPS01".

The output contains extracted game shader code. Write it to an ignored local
directory;
never commit or publish it. GlassFg.props copies <repo>/artifacts/glass-grafts/
Glass/grafts beside the built DLL, so the usual call is
  --workspace ..\\glass-native-material-v1 --out artifacts/glass-grafts/Glass
"""
from pathlib import Path
from collections import Counter
import argparse
import hashlib
import json
import shutil
import struct

CHECKS = ('original_outputs_unchanged', 'original_branches_unchanged',
          'native_previous_expression_identical', 'current_clip_convention_verified')
CAMERA_CHECKS = ('camera_original_outputs_unchanged', 'camera_no_b7',
                 'camera_previous_is_current_world_with_previous_rows',
                 'camera_current_clip_convention_verified')
ROWS = [24, 25, 26]
NO_OUTPUT = 0xFFFFFFFF
REFUSED_VEHICLE = 1  # refused.bin reason: vehicle object motion without engine supply
SKINNING_INPUTS = {'BLENDINDICES', 'BLENDWEIGHT', 'INSTANCE_SKINNING_DATA', 'BONEINDEX'}
SRV, CBV = 0, 2
CLASS_NAMES = {1: 'root-only', 2: 'skinning', 4: 'preskinned', 6: 'skinning+preskinned'}
# Technique passes whose pixel shaders draw light into the scene colour (docstring).
LIGHT_PASSES = frozenset('renderstage_' + name for name in (
    'transparent', 'transparent_notxaa', 'transparent_back_face', 'transparent_depth_write',
    'transparent_background', 'transparent_notxaa_background', 'unlit', 'screenspace_vfx'))
# Techniques whose pixel shaders show background content (docstring), by rule.
BACKGROUND_RULES = {
    'distortion': lambda technique: technique['pass'] == 'renderstage_distortion',
    'cloak screenspace_vfx': lambda technique: (technique['pass'] == 'renderstage_screenspace_vfx'
                                                and technique['material'].startswith(('cloak', 'optical_camouflage'))),
    'water': lambda technique: technique['material'] in ('global_water_patch', 'water_plane'),
}


def supply_class(entry):
    dependencies = entry['previous']['dependencies']
    loads = [(load['binding'][0], load['binding'][2]) for load in entry['loads']]
    skinning = (any(str(name).upper() in SKINNING_INPUTS for name, *_ in dependencies['inputs'])
                or (SRV, 10) in loads)
    preskinned = (SRV, 9) in loads or (CBV, 3) in loads or any(row[0] == 3 for row in dependencies['cb_rows'])
    value = (2 if skinning else 0) | (4 if preskinned else 0)
    return value or 1


def camera_outputs(shader, verified, source, refused):
    """(current, previous, dxil path) of a verified camera-only variant, else None."""
    if shader.get('camera_status') != 'validated':
        refused['status'] += 1
        return None
    if not verified or not all(verified.get(check) is True for check in CAMERA_CHECKS):
        refused['verification'] += 1
        return None
    current, previous = int(shader['camera_current_output']), int(shader['camera_previous_output'])
    if current == previous or not (0 <= current < 32 and 0 <= previous < 32):
        refused['outputs'] += 1
        return None
    if source.read_bytes()[:4] != b'DXBC':
        refused['container'] += 1
        return None
    return current, previous, source


def light_pixel_shaders(census):
    """Sorted digests of the PS whose census passes are all light passes, and the
    number of PS left out because they also appear in another pass."""
    passes = {}
    for technique in census['techniques']:
        for program in technique['programs']:
            if program['kind'] == 'ps':
                passes.setdefault(bytes.fromhex(program['sha256']), set()).add(technique['pass'])
    if any(len(digest) != 32 for digest in passes):
        raise ValueError('census pixel shader sha256 is not 32 bytes')
    light = sorted(digest for digest, names in passes.items() if names <= LIGHT_PASSES)
    mixed = sum(1 for names in passes.values() if names & LIGHT_PASSES and not names <= LIGHT_PASSES)
    return light, mixed


def background_pixel_shaders(census):
    """Sorted digests of the PS of every technique a BACKGROUND_RULES rule selects,
    and the number of PS each rule selects (a PS may match several)."""
    selected = {rule: set() for rule in BACKGROUND_RULES}
    for technique in census['techniques']:
        for rule, matches in BACKGROUND_RULES.items():
            if matches(technique):
                selected[rule].update(bytes.fromhex(program['sha256'])
                                      for program in technique['programs'] if program['kind'] == 'ps')
    background = set().union(*selected.values())
    if any(len(digest) != 32 for digest in background):
        raise ValueError('census pixel shader sha256 is not 32 bytes')
    return sorted(background), {rule: len(digests) for rule, digests in selected.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--skinned-previous-world', choices=('motion', 'current'), default='motion',
                        help='current exports <workspace>/native-grafted-skinned-current')
    args = parser.parse_args()
    workspace = args.workspace.resolve(strict=True)
    variant = args.skinned_previous_world == 'current'
    grafted = workspace / ('native-grafted-skinned-current' if variant else 'native-grafted')
    index = json.loads((grafted / 'index.json').read_text(encoding='utf-8'))
    checked = json.loads((grafted / 'verification.json').read_text(encoding='utf-8'))
    verification = {row['sha256']: row for row in checked['results']}
    camera_verification = {row['sha256']: row for row in checked.get('camera_results', [])}
    generic_verification = {row['sha256']: row for row in checked.get('generic_results', [])}
    union = {row['sha256']: row for row in
             json.loads((workspace / 'native-previous-supply-union.json').read_text(encoding='utf-8'))['shaders']}
    census = json.loads((workspace / 'all-cache-techniques.json').read_text(encoding='utf-8'))
    light, mixed = light_pixel_shaders(census)
    background, background_rules = background_pixel_shaders(census)

    def root_outputs(shader):
        """(current, previous, supply class, dxil path) of an exportable root graft, else None."""
        sha = shader['sha256']
        if shader['status'] != 'validated':
            refused[shader['status']] += 1
            return None
        row = verification.get(sha)
        if not row or not all(row.get(check) is True for check in CHECKS):
            refused['verification'] += 1
            return None
        current_world = variant and shader.get('previous_world') == 'current'
        if shader['required_engine_motion_rows'] != ([] if current_world else ROWS):
            refused['rows'] += 1
            return None
        current, previous = int(shader['current_output']), int(shader['previous_output'])
        if current == previous or not (0 <= current < 32 and 0 <= previous < 32):
            refused['outputs'] += 1
            return None
        native = union.get(shader['native_sha256'])
        if native is None:
            refused['supply-union'] += 1
            return None
        if current_world and supply_class(native) != 2:
            refused['previous-world class'] += 1
            return None
        source = grafted / f'{sha}.dxil'
        if source.read_bytes()[:4] != b'DXBC':
            refused['container'] += 1
            return None
        return current, previous, supply_class(native), source

    index_rows = {}
    refused = Counter()
    camera_refused = Counter()
    records = []
    kinds = Counter()
    # Vehicle camera-only candidates: (digest, camera supply class, evidence).
    refusals = []

    def camera_only(shader, camera, kind):
        digest = bytes.fromhex(shader['sha256'])
        if shader.get('vehicle'):
            refusals.append((digest, int(shader['camera_supply_class']), tuple(shader['vehicle'])))
            kinds[f'refused vehicle camera only ({kind})'] += 1
            return
        records.append((digest, NO_OUTPUT, NO_OUTPUT, int(shader['camera_supply_class']), None, camera))
        kinds[f'camera only ({kind})'] += 1

    for shader in index['shaders']:
        sha = shader['sha256']
        index_rows[sha] = shader.get('camera_rows')
        root = root_outputs(shader)
        camera = camera_outputs(shader, camera_verification.get(sha), grafted / f'{sha}.camera.dxil', camera_refused)
        if root:
            records.append((bytes.fromhex(sha), *root, camera))
            kinds['root+camera' if camera else 'root only'] += 1
            if variant and shader.get('previous_world') == 'current':
                kinds['root with current previous world'] += 1
        elif camera:
            camera_only(shader, camera, 'native twin, root not exported')
    for shader in index.get('generic_camera', []):
        sha = shader['sha256']
        index_rows[sha] = shader.get('camera_rows')
        camera = camera_outputs(shader, generic_verification.get(sha), grafted / f'{sha}.camera.dxil', camera_refused)
        if camera:
            camera_only(shader, camera, 'generic template')

    records.sort(key=lambda record: record[0])
    refusals.sort()
    digests = [record[0] for record in records] + [digest for digest, *_ in refusals]
    if len(set(digests)) != len(digests):
        raise ValueError('duplicate graft sha256')
    out = args.out.resolve() / 'grafts'
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    blob = bytearray(struct.pack('<8sII', b'GGRAFT02', len(records), 0))
    for digest, current, previous, supply, source, camera in records:
        camera_current, camera_previous, camera_source = camera or (NO_OUTPUT, NO_OUTPUT, None)
        blob += struct.pack('<32sIIIII', digest, current, previous, supply, camera_current, camera_previous)
        if source:
            shutil.copyfile(source, out / f'{digest.hex()}.dxil')
        if camera_source:
            shutil.copyfile(camera_source, out / f'{digest.hex()}.camera.dxil')
    (out / 'index.bin').write_bytes(bytes(blob))
    refused_blob = struct.pack('<8sII', b'GGREFS01', len(refusals), 0) + b''.join(
        struct.pack('<32sI', digest, REFUSED_VEHICLE) for digest, *_ in refusals)
    (out / 'refused.bin').write_bytes(refused_blob)
    light_blob = struct.pack('<8sII', b'GGLTPS01', len(light), 0) + b''.join(light)
    (out / 'light-ps.bin').write_bytes(light_blob)
    background_blob = struct.pack('<8sII', b'GGBGPS01', len(background), 0) + b''.join(background)
    (out / 'background-ps.bin').write_bytes(background_blob)

    histogram = Counter(record[3] for record in records)
    print(f'grafts={len(records)} index_bytes={len(blob)} out={out}')
    for kind, count in sorted(kinds.items()):
        print(f'{kind}: {count}')
    print('root refused=' + (', '.join(f'{k}:{v}' for k, v in sorted(refused.items())) or 'none'))
    for value in sorted(histogram):
        print(f'class {value} ({CLASS_NAMES.get(value, "?")}): {histogram[value]}')
    print('camera refused=' + (', '.join(f'{k}:{v}' for k, v in sorted(camera_refused.items())) or 'none'))
    rows = Counter(str(index_rows[record[0].hex()]) for record in records if record[5])
    for value in sorted(rows):
        print(f'camera rows {value}: {rows[value]}')
    print(f'index_sha256={hashlib.sha256(bytes(blob)).hexdigest()}')
    by_class = Counter(supply for _, supply, _ in refusals)
    print(f'refused={len(refusals)} ('
          + ', '.join(f'class {value}: {by_class[value]}' for value in sorted(by_class))
          + f') refused_sha256={hashlib.sha256(refused_blob).hexdigest()}')
    for evidence, count in sorted(Counter(evidence for *_, evidence in refusals).items()):
        print(f'refused evidence {" + ".join(evidence)}: {count}')
    print(f'light_ps={len(light)} (left out, also in another pass: {mixed}) census={census["cache_sha256"][:16]} '
          f'light_sha256={hashlib.sha256(light_blob).hexdigest()}')
    print(f'background_ps={len(background)} ('
          + ', '.join(f'{rule}: {count}' for rule, count in background_rules.items())
          + f'; also light: {len(set(background) & set(light))}) '
          f'background_sha256={hashlib.sha256(background_blob).hexdigest()}')


if __name__ == '__main__':
    main()
