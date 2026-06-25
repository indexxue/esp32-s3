# SPDX-FileCopyrightText: 2026 TY ESP32-S3 project
# SPDX-License-Identifier: Apache-2.0
"""Git semver tag helpers for PROJECT_VER / idf.py release."""

from __future__ import annotations

import re
import subprocess
from typing import List, Optional, Tuple

_SEMVER_TAG_RE = re.compile(r'^v?(?P<maj>\d+)\.(?P<min>\d+)\.(?P<pat>\d+)$', re.IGNORECASE)


def parse_semver_tag(raw: str) -> Optional[str]:
    m = _SEMVER_TAG_RE.match((raw or '').strip())
    if m is None:
        return None
    return '%s.%s.%s' % (m.group('maj'), m.group('min'), m.group('pat'))


def semver_tuple(ver: str) -> Tuple[int, int, int]:
    parts = ver.split('.')
    return (int(parts[0]), int(parts[1]), int(parts[2]))


def semver_compare(a: str, b: str) -> int:
    ta = semver_tuple(a)
    tb = semver_tuple(b)
    if ta < tb:
        return -1
    if ta > tb:
        return 1
    return 0


def _git_output(repo_root: str, *args: str) -> Optional[str]:
    try:
        out = subprocess.check_output(
            ['git', '-C', repo_root] + list(args),
            stderr=subprocess.DEVNULL,
            text=True,
        )
        return out.strip() or None
    except (OSError, subprocess.CalledProcessError):
        return None


def git_list_semver_tags(repo_root: str) -> List[str]:
    raw = _git_output(repo_root, 'tag', '-l', '--sort=-version:refname')
    if not raw:
        return []
    tags: List[str] = []
    for line in raw.splitlines():
        ver = parse_semver_tag(line.strip())
        if ver:
            tags.append(ver)
    return tags


def git_max_semver_tag(repo_root: str) -> Optional[str]:
    tags = git_list_semver_tags(repo_root)
    return tags[0] if tags else None


def git_semver_for_build(repo_root: str) -> Optional[str]:
    """Version for firmware build: exact tag on HEAD, else nearest ancestor tag."""
    exact = _git_output(repo_root, 'describe', '--exact-match', '--tags', 'HEAD')
    if exact:
        ver = parse_semver_tag(exact)
        if ver:
            return ver

    nearest = _git_output(repo_root, 'describe', '--tags', '--abbrev=0')
    if nearest:
        ver = parse_semver_tag(nearest)
        if ver:
            return ver
    return None


def resolve_release_version(requested: str, repo_root: str) -> Tuple[str, bool]:
    """Pick effective release version; clamp below highest git semver tag.

    Returns (effective_version, was_clamped).
    """
    floor = git_max_semver_tag(repo_root)
    if floor and semver_compare(requested, floor) < 0:
        return floor, True
    return requested, False


def clamp_release_version(requested: str, repo_root: str) -> str:
    """Release version must not be lower than the highest git semver tag."""
    effective, was_clamped = resolve_release_version(requested, repo_root)
    if was_clamped:
        floor = effective
        print(
            'Release version %s is below git tag v%s — using %s (overwrite firmware/%s/)'
            % (requested, floor, floor, floor)
        )
    return effective
