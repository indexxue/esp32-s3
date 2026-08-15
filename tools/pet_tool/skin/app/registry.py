"""Register feature modules. New tools: subclass FeatureModule + append here."""

from __future__ import annotations

from app.base_feature import FeatureModule
from app.context import ProjectContext
from app.features.body_feature import BodyFeature
from app.features.design_feature import DesignFeature
from app.features.pack_feature import PackFeature
from app.features.reserved_feature import ReservedFeature
from app.features.splash_feature import SplashFeature
from app.features.theme_feature import ThemeFeature


def built_in_features(ctx: ProjectContext) -> list[FeatureModule]:
    """Sidebar order. boot/anim deferred; splash stays static single frame."""
    mods: list[FeatureModule] = [
        SplashFeature(ctx),
        DesignFeature(ctx),
        PackFeature(ctx),
        BodyFeature(ctx),
        ReservedFeature(ctx, "sfx", "SFX", "sfx/", 50),
        ReservedFeature(ctx, "font", "Font", "font/", 60),
        ThemeFeature(ctx),
    ]
    mods.sort(key=lambda m: m.order)
    return mods
