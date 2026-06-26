# SPDX-FileCopyrightText: 2026 TY ESP32-S3 project
# SPDX-License-Identifier: Apache-2.0
"""Shared Secure Boot / signed OTA profile helpers for idf.py extensions."""

from __future__ import annotations

import os
import shutil
import time
from typing import Dict, Optional

SIGNING_PROFILES = {
    'signed_ota': 'sdkconfig.defaults.signed_ota',
    'secure_boot': 'sdkconfig.defaults.secure_boot',
}

VALID_SIGNING_PROFILES = frozenset(('none', 'signed_ota', 'secure_boot'))


def repo_root_from_project(project_dir: str) -> str:
    return os.path.normpath(os.path.join(os.path.realpath(project_dir), '..'))


def dev_signing_key_path(project_dir: str) -> str:
    return os.path.join(repo_root_from_project(project_dir), 'keys', 'dev', 'secure_boot_signing_key.pem')


def sdkconfig_defaults_for_profile(profile: Optional[str]) -> str:
    if not profile or profile == 'none':
        return 'sdkconfig.defaults'
    fragment = SIGNING_PROFILES.get(profile)
    if fragment is None:
        raise ValueError('Unknown signing profile %r (use signed_ota or secure_boot)' % profile)
    return 'sdkconfig.defaults;%s' % fragment


def _remove_if_exists(path: str) -> None:
    if os.path.isfile(path):
        os.remove(path)


def reset_build_dir(project_dir: str, retries: int = 5) -> None:
    build_dir = os.path.join(project_dir, 'build')
    if not os.path.isdir(build_dir):
        return
    last_err: Optional[Exception] = None
    for attempt in range(1, retries + 1):
        try:
            shutil.rmtree(build_dir)
            print('Removed %s' % build_dir)
            return
        except OSError as exc:
            last_err = exc
            if attempt < retries:
                print('Build dir busy, retry %u/%u...' % (attempt, retries))
                time.sleep(1.0)
    raise OSError(
        'Could not remove %s (file locked). Close idf monitor (Ctrl+]), then retry.'
        % build_dir
    ) from last_err


def sync_bootloader_defaults(project_dir: str, profile: Optional[str]) -> None:
    boot_dir = os.path.join(project_dir, 'bootloader')
    if not os.path.isdir(boot_dir):
        return
    dest = os.path.join(boot_dir, 'sdkconfig.defaults')
    base = [
        '# Bootloader-only defaults. Slot selection uses otadata + CONFIG_BOOTLOADER_APP_* from ESP-IDF.',
        'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y',
    ]
    lines = list(base)
    if profile == 'secure_boot':
        fragment = os.path.join(boot_dir, 'sdkconfig.defaults.secure_boot')
        if os.path.isfile(fragment):
            with open(fragment, 'r', encoding='utf-8') as f:
                for line in f:
                    stripped = line.strip()
                    if not stripped or stripped.startswith('#'):
                        continue
                    if 'ROLLBACK' in stripped:
                        continue
                    lines.append(stripped)
    with open(dest, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))
        f.write('\n')
    print('Updated %s for profile %r' % (dest, profile or 'none'))


def apply_signing_profile(project_dir: str, profile: Optional[str]) -> None:
    """profile: None or 'none' = unsigned; else signed_ota / secure_boot."""
    if profile == 'none':
        profile = None

    for name in ('sdkconfig', 'sdkconfig.old'):
        _remove_if_exists(os.path.join(project_dir, name))

    boot_dir = os.path.join(project_dir, 'bootloader')
    if os.path.isdir(boot_dir):
        for name in ('sdkconfig', 'sdkconfig.old'):
            _remove_if_exists(os.path.join(boot_dir, name))

    reset_build_dir(project_dir)

    marker = os.path.join(project_dir, '.signing_profile')
    if profile:
        with open(marker, 'w', encoding='utf-8') as f:
            f.write(profile)
        print("Signing profile '%s' → %s" % (profile, marker))
    elif os.path.isfile(marker):
        os.remove(marker)
        print('Signing profile cleared (%s removed)' % marker)

    sync_bootloader_defaults(project_dir, profile)


def signing_profile_marker(project_dir: str) -> Optional[str]:
    path = os.path.join(project_dir, '.signing_profile')
    if not os.path.isfile(path):
        return None
    try:
        with open(path, 'r', encoding='utf-8') as f:
            value = f.read().strip()
    except OSError:
        return None
    return value if value in SIGNING_PROFILES else None


def read_sdkconfig_flag(project_dir: str, key: str) -> bool:
    path = os.path.join(project_dir, 'sdkconfig')
    if not os.path.isfile(path):
        return False
    needle = '%s=y' % key
    try:
        with open(path, 'r', encoding='utf-8') as f:
            for line in f:
                if line.strip() == needle:
                    return True
    except OSError:
        return False
    return False


def detect_signing_meta(project_dir: str) -> Optional[Dict[str, str]]:
    profile = signing_profile_marker(project_dir)
    if profile is None:
        if read_sdkconfig_flag(project_dir, 'CONFIG_SECURE_BOOT'):
            profile = 'secure_boot'
        elif read_sdkconfig_flag(project_dir, 'CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT'):
            profile = 'signed_ota'
        else:
            return None
    return {
        'profile': profile,
        'algorithm': 'rsa3072',
    }
