# SPDX-FileCopyrightText: 2026 TY ESP32-S3 project
# SPDX-License-Identifier: Apache-2.0
"""Shared idf.py release action — build with -DPROJECT_VER and archive to firmware/."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional, Tuple

from click.core import Context
from idf_py_actions.constants import GENERATORS
from idf_py_actions.errors import FatalError
from idf_py_actions.global_options import global_options
from idf_py_actions.tools import PropertyDict, ensure_build_directory, get_target, run_target

from git_version import clamp_release_version, git_max_semver_tag

_VERSION_RE = re.compile(r'^V?(?P<maj>\d+)\.(?P<min>\d+)\.(?P<pat>\d+)$', re.IGNORECASE)
_CMAKE_PROJECT_RE = re.compile(r'project\s*\(\s*(\w+)\s*\)')

# ESP-IDF 工程目录名（与 CMake project(...) 一致），release-all 默认顺序。
ALL_RELEASE_PRODUCTS = ('project', 'ble_demo', 'factory', 'ballot_guard')


def _normalize_version(raw: str) -> str:
    m = _VERSION_RE.match((raw or '').strip())
    if m is None:
        raise FatalError(
            'Invalid version %r — use three-part semver, e.g. 1.2.3 or V1.2.3' % raw
        )
    return '%s.%s.%s' % (m.group('maj'), m.group('min'), m.group('pat'))


def _cmake_project_name(project_dir: str) -> str:
    cmake_path = os.path.join(project_dir, 'CMakeLists.txt')
    try:
        with open(cmake_path, 'r', encoding='utf-8') as f:
            text = f.read()
    except OSError as e:
        raise FatalError('Cannot read %s: %s' % (cmake_path, e)) from e

    m = _CMAKE_PROJECT_RE.search(text)
    if m is None:
        raise FatalError('Cannot find project(...) in %s' % cmake_path)
    return m.group(1)


def _set_cache_entry(args: PropertyDict, key: str, value: str) -> None:
    entries = list(getattr(args, 'define_cache_entry', None) or [])
    entries = [e for e in entries if e.split('=', 1)[0] != key]
    entries.append('%s=%s' % (key, value))
    args.define_cache_entry = entries


def _ninja_executable() -> Optional[str]:
    make = os.environ.get('CMAKE_MAKE_PROGRAM')
    if make and os.path.isfile(make):
        return make
    found = shutil.which('ninja')
    if found and os.path.isfile(found):
        return found
    return None


def _ninja_version_ok(ninja_exe: str) -> bool:
    try:
        out = subprocess.check_output(
            [ninja_exe, '--version'],
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
        return bool(out)
    except (OSError, subprocess.CalledProcessError):
        return False


def _cmake_build_dir_needs_reset(build_dir: str) -> bool:
    """True when CMake/Ninja state is partial or points at a broken ninja.

    idf.py only deletes CMakeCache.txt after a failed cmake run; CMakeFiles/
    left behind reproduces: Configuring done (0.0s) + Ninja () version error.
    """
    if not os.path.isdir(build_dir):
        return False

    cache_path = os.path.join(build_dir, 'CMakeCache.txt')
    cmake_files = os.path.join(build_dir, 'CMakeFiles')
    has_cache = os.path.isfile(cache_path)
    has_cmake_files = os.path.isdir(cmake_files)

    if has_cmake_files and not has_cache:
        return True

    if not has_cache:
        return False

    cached_ninja: Optional[str] = None
    try:
        with open(cache_path, 'r', encoding='utf-8') as f:
            for line in f:
                if not line.startswith('CMAKE_MAKE_PROGRAM:FILEPATH='):
                    continue
                cached_ninja = line.split('=', 1)[1].strip()
                break
    except OSError:
        return True

    if not cached_ninja:
        return True
    if not os.path.isfile(cached_ninja):
        return True
    if not _ninja_version_ok(cached_ninja):
        return True

    return False


def _release_cmake_env() -> Dict[str, str]:
    env: Dict[str, str] = {}
    ninja_exe = _ninja_executable()
    if ninja_exe:
        env['CMAKE_MAKE_PROGRAM'] = ninja_exe
    return env


def _ensure_release_build_directory(
    ctx: Context,
    args: PropertyDict,
    project_dir: str,
) -> None:
    cmake_env = _release_cmake_env()
    try:
        ensure_build_directory(args, ctx.info_name, env=cmake_env)
    except FatalError:
        build_dir = os.path.join(project_dir, 'build')
        if not os.path.isdir(build_dir):
            raise
        print('CMake configure failed; wiping %s and retrying once' % build_dir)
        shutil.rmtree(build_dir, ignore_errors=True)
        ensure_build_directory(args, ctx.info_name, env=cmake_env)


def _reset_broken_build_dir(project_dir: str) -> None:
    build_dir = os.path.join(project_dir, 'build')
    if not _cmake_build_dir_needs_reset(build_dir):
        return
    print('Resetting %s (broken CMake/Ninja cache)' % build_dir)
    shutil.rmtree(build_dir, ignore_errors=True)


def _prepare_release_build(args: PropertyDict, project_dir: str, ver: str) -> None:
    _reset_broken_build_dir(project_dir)
    _set_cache_entry(args, 'PROJECT_VER', ver)
    ninja_exe = _ninja_executable()
    if ninja_exe:
        os.environ['CMAKE_MAKE_PROGRAM'] = ninja_exe
        _set_cache_entry(args, 'CMAKE_MAKE_PROGRAM', ninja_exe)


def _build_date_tag(when: datetime) -> str:
    """Compile date for filenames: YYYYMMDD (UTC)."""
    return when.strftime('%Y%m%d')


def _release_basename(
    product: str,
    version: str,
    build_date: str,
    role: str,
    ext: str,
) -> str:
    """{product}_{version}_{build_date}[_{role}].{ext} — app omits role segment."""
    if role == 'app':
        return '%s_%s_%s.%s' % (product, version, build_date, ext)
    return '%s_%s_%s_%s.%s' % (product, version, build_date, role, ext)


def _product_file_prefixes(product: str, version: str) -> List[str]:
    """Match current and legacy release filenames for cleanup."""
    return ['%s_%s_' % (product, version), '%s_v%s_' % (product, version)]


def _artifact_plan(product: str) -> List[Tuple[str, str, str]]:
    return [
        ('%s.bin' % product, 'app', 'bin'),
        ('bootloader/bootloader.bin', 'bootloader', 'bin'),
        ('partition_table/partition-table.bin', 'partition', 'bin'),
        ('ota_data_initial.bin', 'otadata', 'bin'),
        ('flash_args', 'flash_full', 'txt'),
        ('flash_%s_args' % product, 'flash_app', 'txt'),
        ('%s.elf' % product, 'debug', 'elf'),
        ('%s.map' % product, 'debug', 'map'),
    ]


def _flash_args_candidates(build_dir: str, product: str) -> List[str]:
    names = ['flash_%s_args' % product, 'flash_app_args', 'flash_project_args']
    return [os.path.join(build_dir, name) for name in names]


def _parse_app_flash_offset(build_dir: str, product: str) -> int:
    pattern = re.compile(r'^(0x[0-9A-Fa-f]+)\s+%s\.bin\s*$' % re.escape(product))
    for args_path in _flash_args_candidates(build_dir, product):
        if not os.path.isfile(args_path):
            continue
        with open(args_path, 'r', encoding='utf-8') as f:
            for line in f:
                m = pattern.match(line.strip())
                if m:
                    return int(m.group(1), 16)

    raise FatalError(
        'Could not find app flash offset for %s.bin under %s' % (product, build_dir)
    )


def _intel_hex_checksum(body: bytes) -> int:
    return (-sum(body)) & 0xFF


def _intel_hex_record(addr: int, data: bytes, record_type: int = 0) -> str:
    body = bytes([len(data), (addr >> 8) & 0xFF, addr & 0xFF, record_type]) + data
    return ':%s%02X' % (body.hex().upper(), _intel_hex_checksum(body))


def _bin_to_intel_hex(data: bytes, start_addr: int) -> str:
    lines: List[str] = []
    pos = 0
    chunk_size = 16
    last_upper: Optional[int] = None

    while pos < len(data):
        cur_addr = start_addr + pos
        upper = cur_addr >> 16
        if upper != last_upper:
            lines.append(_intel_hex_record(0, bytes([(upper >> 8) & 0xFF, upper & 0xFF]), 4))
            last_upper = upper
        seg_addr = cur_addr & 0xFFFF
        block = data[pos:pos + chunk_size]
        lines.append(_intel_hex_record(seg_addr, block, 0))
        pos += len(block)

    lines.append(':00000001FF')
    return '\n'.join(lines) + '\n'


def _write_app_hex(
    build_dir: str,
    release_dir: str,
    product: str,
    version: str,
    build_date: str,
) -> Dict[str, Any]:
    bin_src = os.path.join(build_dir, '%s.bin' % product)
    if not os.path.isfile(bin_src):
        raise FatalError('Build did not produce %s.bin in %s' % (product, build_dir))

    offset = _parse_app_flash_offset(build_dir, product)
    with open(bin_src, 'rb') as f:
        payload = f.read()

    out_name = _release_basename(product, version, build_date, 'app', 'hex')
    dst = os.path.join(release_dir, out_name)

    with open(dst, 'w', encoding='utf-8', newline='\n') as f:
        f.write(_bin_to_intel_hex(payload, offset))

    return {
        'file': out_name,
        'size': os.path.getsize(dst),
        'role': 'app',
        'format': 'intel_hex',
        'flash_offset': '0x%X' % offset,
    }


def _git_head_short(repo_root: str) -> Optional[str]:
    try:
        out = subprocess.check_output(
            ['git', '-C', repo_root, 'rev-parse', '--short', 'HEAD'],
            stderr=subprocess.DEVNULL,
            text=True,
        )
        return out.strip() or None
    except (OSError, subprocess.CalledProcessError):
        return None


def _copy_artifact(
    build_dir: str,
    release_dir: str,
    rel_path: str,
    out_name: str,
) -> Optional[Dict[str, Any]]:
    dst = os.path.join(release_dir, out_name)

    src = os.path.join(build_dir, rel_path.replace('/', os.sep))
    if not os.path.isfile(src):
        return None
    shutil.copy2(src, dst)
    entry: Dict[str, Any] = {'file': out_name, 'size': os.path.getsize(dst)}
    if out_name.endswith('.bin'):
        digest = hashlib.sha256()
        with open(dst, 'rb') as fh:
            for chunk in iter(lambda: fh.read(65536), b''):
                digest.update(chunk)
        entry['sha256'] = digest.hexdigest()
    return entry


def _copy_first_existing(
    build_dir: str,
    release_dir: str,
    rel_paths: List[str],
    out_name: str,
) -> Optional[Dict[str, Any]]:
    for rel_path in rel_paths:
        entry = _copy_artifact(build_dir, release_dir, rel_path, out_name)
        if entry is not None:
            return entry
    return None


def _collect_release_files(
    build_dir: str,
    release_dir: str,
    product: str,
    version: str,
    build_date: str,
    flash_bundle: bool,
    debug: bool,
) -> Tuple[List[Dict[str, Any]], str]:
    enabled_roles = {'app'}
    if flash_bundle:
        enabled_roles.update({'bootloader', 'partition', 'otadata', 'flash_full', 'flash_app'})
    if debug:
        enabled_roles.add('debug')

    artifacts: List[Dict[str, Any]] = []
    ota_image = ''

    for rel_path, role, ext in _artifact_plan(product):
        if role not in enabled_roles:
            continue
        out_name = _release_basename(product, version, build_date, role, ext)
        if role == 'flash_app':
            entry = _copy_first_existing(
                build_dir,
                release_dir,
                ['flash_%s_args' % product, 'flash_app_args', 'flash_project_args'],
                out_name,
            )
        else:
            entry = _copy_artifact(build_dir, release_dir, rel_path, out_name)
        if entry is None:
            if role == 'app' and ext == 'bin':
                raise FatalError('Build did not produce %s in %s' % (rel_path, build_dir))
            continue
        entry['role'] = role
        artifacts.append(entry)
        if role == 'app' and ext == 'bin':
            ota_image = out_name

    if not ota_image:
        raise FatalError('App image missing after release copy')

    hex_entry = _write_app_hex(build_dir, release_dir, product, version, build_date)
    artifacts.append(hex_entry)

    return artifacts, ota_image


def _remove_product_artifacts(
    release_dir: str,
    manifest: Dict[str, Any],
    product: str,
    version: str,
) -> None:
    """Remove prior release files for one product (used with --force)."""
    products = manifest.get('products', {})
    entry = products.get(product)
    if entry:
        for art in entry.get('artifacts', []):
            name = art.get('file')
            if not name:
                continue
            path = os.path.join(release_dir, name)
            if os.path.isfile(path):
                os.remove(path)
        flash = entry.get('flash') or {}
        for name in flash.values():
            path = os.path.join(release_dir, name)
            if os.path.isfile(path):
                os.remove(path)
        products.pop(product, None)

    for prefix in _product_file_prefixes(product, version):
        if not os.path.isdir(release_dir):
            continue
        for name in os.listdir(release_dir):
            if name.startswith(prefix):
                path = os.path.join(release_dir, name)
                if os.path.isfile(path):
                    os.remove(path)


def _load_manifest(manifest_path: str, version: str) -> Dict[str, Any]:
    if not os.path.isfile(manifest_path):
        return {'schema': 2, 'version': version, 'products': {}}

    with open(manifest_path, 'r', encoding='utf-8') as f:
        data = json.load(f)

    if data.get('schema') == 2 and 'products' in data:
        if data.get('version') != version:
            raise FatalError(
                'Manifest version mismatch in %s: %s vs %s'
                % (manifest_path, data.get('version'), version)
            )
        return data

    if data.get('schema') == 1 and 'product' in data:
        if data.get('version') != version:
            raise FatalError(
                'Manifest version mismatch in %s: %s vs %s'
                % (manifest_path, data.get('version'), version)
            )
        product = data['product']
        return {
            'schema': 2,
            'version': version,
            'target': data.get('target'),
            'git_commit': data.get('git_commit'),
            'products': {
                product: {
                    'built_at_utc': data.get('built_at_utc'),
                    'artifacts': data.get('artifacts', []),
                    'ota': data.get('ota'),
                },
            },
        }

    raise FatalError('Unsupported manifest format: %s' % manifest_path)


def _product_release_entry(
    product: str,
    ver: str,
    build_date: str,
    built_at: str,
    artifacts: List[Dict[str, Any]],
    ota_image: str,
    flash_bundle: bool,
) -> Dict[str, Any]:
    entry: Dict[str, Any] = {
        'build_date': build_date,
        'built_at_utc': built_at,
        'artifacts': artifacts,
    }
    if product == 'project':
        ota_entry: Dict[str, Any] = {
            'image': ota_image,
            'note': 'Upload this file at http://<device>/ota then Apply & Reboot',
        }
        for art in artifacts:
            if art.get('role') == 'app' and art.get('file') == ota_image and art.get('sha256'):
                ota_entry['sha256'] = art['sha256']
                break
        entry['ota'] = ota_entry
    if flash_bundle:
        entry['flash'] = {
            'bootloader': _release_basename(product, ver, build_date, 'bootloader', 'bin'),
            'partition': _release_basename(product, ver, build_date, 'partition', 'bin'),
            'otadata': _release_basename(product, ver, build_date, 'otadata', 'bin'),
            'full_script': _release_basename(product, ver, build_date, 'flash_full', 'txt'),
            'app_only_script': _release_basename(product, ver, build_date, 'flash_app', 'txt'),
        }
    return entry


def _write_readme(release_dir: str, ver: str, manifest: Dict[str, Any]) -> None:
    lines = ['Release %s (esp32s3)\n' % ver]
    for product, info in sorted(manifest.get('products', {}).items()):
        lines.append('[%s] built %s UTC' % (product, info.get('built_at_utc', '?')))
        for art in info.get('artifacts', []):
            if art.get('role') == 'app':
                lines.append('  %s' % art.get('file'))
        ota = info.get('ota')
        if ota:
            lines.append('  OTA: upload %s via http://<device>/ota' % ota.get('image'))
        lines.append('')
    with open(os.path.join(release_dir, 'README.txt'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines).rstrip() + '\n')


def _release_one_product(
    target_name: str,
    ctx: Context,
    args: PropertyDict,
    ver: str,
    repo_root: str,
    project_dir: str,
    flash_bundle: bool,
    debug: bool,
) -> None:
    product = _cmake_project_name(project_dir)
    release_dir = os.path.join(repo_root, 'firmware', ver)

    print('Release %s (%s) → firmware/%s/' % (ver, product, ver))
    print(
        'Build with -DPROJECT_VER=%s (esp_app_desc + NVS_APP_VERSION_STRING)'
        % ver
    )

    saved_project_dir = args.project_dir
    saved_build_dir = getattr(args, 'build_dir', None)
    args.project_dir = project_dir
    if saved_build_dir is not None:
        args.build_dir = os.path.join(project_dir, 'build')

    _prepare_release_build(args, project_dir, ver)

    prev_release_ver = os.environ.get('TY_RELEASE_PROJECT_VER')
    os.environ['TY_RELEASE_PROJECT_VER'] = ver
    try:
        _ensure_release_build_directory(ctx, args, project_dir)
        force = GENERATORS[args.generator].get('force_progression', False)
        run_target('all', args, force_progression=force)
    finally:
        if prev_release_ver is None:
            os.environ.pop('TY_RELEASE_PROJECT_VER', None)
        else:
            os.environ['TY_RELEASE_PROJECT_VER'] = prev_release_ver

    try:

        build_dir = os.path.realpath(args.build_dir)
        os.makedirs(release_dir, exist_ok=True)

        manifest_path = os.path.join(release_dir, 'manifest.json')
        manifest = _load_manifest(manifest_path, ver)
        existing_products = set(manifest.get('products', {}).keys())

        if product in existing_products:
            print(
                'Updating %s in firmware/%s/ (other products unchanged)'
                % (product, ver)
            )
        elif existing_products:
            print(
                'Adding %s to firmware/%s/ (already present: %s)'
                % (product, ver, ', '.join(sorted(existing_products)))
            )

        _remove_product_artifacts(release_dir, manifest, product, ver)

        built_at_dt = datetime.now(timezone.utc)
        build_date = _build_date_tag(built_at_dt)
        built_at = built_at_dt.strftime('%Y-%m-%dT%H:%M:%SZ')

        artifacts, ota_image = _collect_release_files(
            build_dir, release_dir, product, ver, build_date, flash_bundle, debug
        )

        manifest['schema'] = 2
        manifest['version'] = ver
        manifest['target'] = get_target(project_dir)
        manifest['git_commit'] = _git_head_short(repo_root)
        manifest['naming'] = {
            'pattern': '{product}_{version}_{build_date}.{ext}',
            'example': _release_basename(product, ver, build_date, 'app', 'bin'),
        }
        manifest.setdefault('products', {})[product] = _product_release_entry(
            product, ver, build_date, built_at, artifacts, ota_image, flash_bundle
        )

        with open(manifest_path, 'w', encoding='utf-8') as f:
            json.dump(manifest, f, indent=2)
            f.write('\n')

        _write_readme(release_dir, ver, manifest)

        print('Release artifacts written to: %s' % release_dir)
        for entry in artifacts:
            print('  %s [%s] (%u bytes)' % (entry['file'], entry['role'], entry['size']))
        print(
            'firmware/%s/ now contains: %s'
            % (ver, ', '.join(sorted(manifest['products'].keys())))
        )
    finally:
        args.project_dir = saved_project_dir
        if saved_build_dir is not None:
            args.build_dir = saved_build_dir


def release(
    target_name: str,
    ctx: Context,
    args: PropertyDict,
    version: str,
    flash_bundle: bool,
    debug: bool,
) -> None:
    """Build one product and archive under ../firmware/<version>/."""
    if isinstance(version, (list, tuple)):
        if len(version) != 1:
            raise FatalError('Expected exactly one version argument')
        version = version[0]

    requested = _normalize_version(version)
    project_dir = os.path.realpath(args.project_dir)
    repo_root = os.path.normpath(os.path.join(project_dir, '..'))
    tag_floor = git_max_semver_tag(repo_root)
    if tag_floor:
        print('Git tag floor: v%s' % tag_floor)
    ver = clamp_release_version(requested, repo_root)
    if ver == requested:
        print('Using release version %s (PROJECT_VER / NVS_APP_VERSION_STRING)' % ver)
    _release_one_product(
        target_name, ctx, args, ver, repo_root, project_dir,
        flash_bundle, debug,
    )


def release_all(
    target_name: str,
    ctx: Context,
    args: PropertyDict,
    version: str,
    flash_bundle: bool,
    debug: bool,
) -> None:
    """Build every ESP-IDF product into the same firmware/<version>/ directory."""
    if isinstance(version, (list, tuple)):
        if len(version) != 1:
            raise FatalError('Expected exactly one version argument')
        version = version[0]

    requested = _normalize_version(version)
    anchor_dir = os.path.realpath(args.project_dir)
    repo_root = os.path.normpath(os.path.join(anchor_dir, '..'))
    tag_floor = git_max_semver_tag(repo_root)
    if tag_floor:
        print('Git tag floor: v%s' % tag_floor)
    ver = clamp_release_version(requested, repo_root)
    if ver == requested:
        print('Using release version %s (PROJECT_VER / NVS_APP_VERSION_STRING)' % ver)

    print('Release-all %s → firmware/%s/' % (ver, ver))
    for name in ALL_RELEASE_PRODUCTS:
        project_dir = os.path.join(repo_root, name)
        if not os.path.isdir(project_dir):
            print('Skip %s (no directory %s)' % (name, project_dir))
            continue
        print('')
        print('=== %s ===' % name)
        _release_one_product(
            target_name, ctx, args, ver, repo_root, project_dir,
            flash_bundle, debug,
        )

    release_dir = os.path.join(repo_root, 'firmware', ver)
    manifest_path = os.path.join(release_dir, 'manifest.json')
    if os.path.isfile(manifest_path):
        with open(manifest_path, 'r', encoding='utf-8') as f:
            manifest = json.load(f)
        products = sorted(manifest.get('products', {}).keys())
        print('')
        print('Release-all complete. firmware/%s/: %s' % (ver, ', '.join(products)))


_RELEASE_OPTIONS = global_options + [
    {
        'names': ['--flash-bundle'],
        'is_flag': True,
        'help': 'Also pack bootloader, partition, otadata, and esptool flash_*.txt.',
    },
    {
        'names': ['--debug'],
        'is_flag': True,
        'help': 'Also pack .elf and .map (not for field OTA).',
    },
]


def action_extensions(base_actions: Dict, project_path: str) -> Dict[str, Any]:
    return {
        'actions': {
            'release': {
                'callback': release,
                'short_help': 'Build one product into ../firmware/<version>/.',
                'help': (
                    'Build with -DPROJECT_VER=<semver> and archive to firmware/<semver>/.\n'
                    'Different products share the same firmware/<semver>/ directory.\n'
                    'Default: app .bin + .hex + manifest.json.\n'
                    'Use release-all to build every product for one version.\n'
                    'Re-releasing the same product overwrites its files only.\n'
                    'See firmware/README.md.'
                ),
                'arguments': [
                    {
                        'names': ['version'],
                        'nargs': 1,
                        'required': True,
                    },
                ],
                'options': _RELEASE_OPTIONS,
            },
            'release-all': {
                'callback': release_all,
                'short_help': 'Build all products into ../firmware/<version>/.',
                'help': (
                    'Run release for project, ble_demo, factory, ballot_guard into one\n'
                    'firmware/<semver>/ folder (same version, multiple products).\n'
                    'Re-releasing overwrites each product\'s files in that folder.\n'
                    'See firmware/README.md.'
                ),
                'arguments': [
                    {
                        'names': ['version'],
                        'nargs': 1,
                        'required': True,
                    },
                ],
                'options': _RELEASE_OPTIONS,
            },
        },
    }
