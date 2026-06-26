# SPDX-FileCopyrightText: 2026 TY ESP32-S3 project
# SPDX-License-Identifier: Apache-2.0
"""idf.py signing-profile / signing-key-gen — unified Secure Boot workflow."""

from __future__ import annotations

import os
import subprocess
import sys
from typing import Any, Dict

import click
from click.core import Context
from idf_py_actions.constants import GENERATORS
from idf_py_actions.errors import FatalError
from idf_py_actions.global_options import global_options
from idf_py_actions.tools import PropertyDict, ensure_build_directory, run_target

from signing_common import (
    VALID_SIGNING_PROFILES,
    apply_signing_profile,
    dev_signing_key_path,
    sdkconfig_defaults_for_profile,
)


def _set_cache_entry(args: PropertyDict, key: str, value: str) -> None:
    entries = list(getattr(args, 'define_cache_entry', None) or [])
    entries = [e for e in entries if e.split('=', 1)[0] != key]
    entries.append('%s=%s' % (key, value))
    args.define_cache_entry = entries


def signing_profile(
    target_name: str,
    ctx: Context,
    args: PropertyDict,
    profile: str,
    build: bool,
) -> None:
    """Switch project signing profile (none / signed_ota / secure_boot)."""
    if isinstance(profile, (list, tuple)):
        if len(profile) != 1:
            raise FatalError('Expected exactly one profile argument')
        profile = profile[0]

    if profile not in VALID_SIGNING_PROFILES:
        raise FatalError(
            'Invalid profile %r — use none, signed_ota, or secure_boot' % profile
        )

    project_dir = os.path.realpath(args.project_dir)
    internal = None if profile == 'none' else profile

    if internal:
        key_path = dev_signing_key_path(project_dir)
        if not os.path.isfile(key_path):
            raise FatalError(
                'Missing dev signing key: %s\nRun: idf signing-key-gen' % key_path
            )

    apply_signing_profile(project_dir, internal)
    defaults = sdkconfig_defaults_for_profile(profile)
    print('SDKCONFIG_DEFAULTS=%s' % defaults)

    if not build:
        print('Next: idf build   (or: idf signing-profile %s --build)' % profile)
        return

    _set_cache_entry(args, 'SDKCONFIG_DEFAULTS', defaults)
    ensure_build_directory(args, ctx.info_name)
    force = GENERATORS[args.generator].get('force_progression', False)
    run_target('all', args, force_progression=force)
    print('Build OK (%s profile)' % profile)


def signing_key_gen(
    target_name: str,
    ctx: Context,
    args: PropertyDict,
    force: bool,
) -> None:
    """Generate RSA-3072 dev signing key under keys/dev/."""
    project_dir = os.path.realpath(args.project_dir)
    key_path = dev_signing_key_path(project_dir)
    key_dir = os.path.dirname(key_path)

    if os.path.isfile(key_path) and not force:
        print('Dev signing key already exists: %s' % key_path)
        print('Use --force to regenerate.')
        return

    os.makedirs(key_dir, exist_ok=True)
    cmd = [
        sys.executable, '-m', 'espsecure', 'generate_signing_key',
        '--version', '2', '--scheme', 'rsa3072', key_path,
    ]
    print('Running: %s' % ' '.join(cmd))
    subprocess.check_call(cmd, cwd=project_dir)
    print('Dev key written: %s' % key_path)
    print('Next: idf signing-profile signed_ota --build')


def action_extensions(base_actions: Dict, project_path: str) -> Dict[str, Any]:
    return {
        'actions': {
            'signing-profile': {
                'callback': signing_profile,
                'short_help': 'Switch signing profile (none / signed_ota / secure_boot).',
                'help': (
                    'Apply OTA signing sdkconfig profile for the current -C project.\n'
                    '  none        — unsigned (default development)\n'
                    '  signed_ota  — OTA signature verify, no eFuse burn\n'
                    '  secure_boot — hardware Secure Boot v2 (eFuse, irreversible)\n'
                    'Wipes sdkconfig + build/. Use --build to configure and compile.\n'
                    'See doc/secure_boot_production.md.'
                ),
                'arguments': [
                    {
                        'names': ['profile'],
                        'nargs': 1,
                        'required': True,
                    },
                ],
                'options': global_options + [
                    {
                        'names': ['--build'],
                        'is_flag': True,
                        'help': 'Run idf build after applying profile.',
                    },
                ],
            },
            'signing-key-gen': {
                'callback': signing_key_gen,
                'short_help': 'Generate dev Secure Boot signing key (keys/dev/).',
                'help': (
                    'Create keys/dev/secure_boot_signing_key.pem (RSA-3072, SB v2).\n'
                    'Private key is gitignored. Run once before signed_ota / secure_boot.'
                ),
                'options': global_options + [
                    {
                        'names': ['--force'],
                        'is_flag': True,
                        'help': 'Regenerate even if key file exists.',
                    },
                ],
            },
        },
    }
