"""What the texture rounds' fixes leave in the refusal log of every process.

A refusal line the fixes removed names an old driver on the card. raw:tex.surf.pack names a pack made without the
RGB10A8_2P_XR10 merge and split states, which the shipped pack holds. raw:tex.surf.form names a texture over a surface
Metal makes that the raw path has no form for yet; HW Test's open-case tests ask for those on purpose. A copy across two
pixel formats with a texture held in planes is Metal's validation error the raw path copies as Apple's driver does.
"""

import re

from tools.devicerun.cut import REFUSAL, REFUSAL_SUBSYSTEM
from tools.devicerun.rules import REPORT, Check

OWNER = 'raw-textures'
SURFACES = 'drivers/TegraGM20BMetal/raw/runtime/RawMetalSurfaceTextures.mm'
BLIT_RULES = 'drivers/TegraGM20BMetal/raw/runtime/RawMetalBlitRules.mm'
# The texts of the refusals the fixes removed: a written XR texel buffer over a surface, a turned YCbCr target loaded,
# an XR texture whose words run into its alpha plane, XR planes of rows under 16 bytes
REMOVED = (
    'an RGB10A8_2P_XR10 texel buffer written over a surface',
    'a turned YCbCr target loaded',
    'written over planes whose words run on into its alpha plane',
    'RGB10A8_2P_XR10 over planes of rows under 16 bytes',
)
ACROSS = 'between textures of two pixel formats, one held through planes: its bytes copied'

TEXTURES = [
    Check(
        'textures.removed-refusal',
        OWNER,
        REMOVED,
        subsystem=REFUSAL_SUBSYSTEM,
        regex=REFUSAL.format(reasons=r'[\w.]+') + r'.*?(?P<text>' + '|'.join(map(re.escape, REMOVED)) + ')',
        show=('who', 'text'),
        source=SURFACES,
        note='the line is gone with the fixes: any is an old driver on the card',
    ),
    Check(
        'textures.surf-pack',
        OWNER,
        'tex.surf.pack: ',
        subsystem=REFUSAL_SUBSYSTEM,
        regex=REFUSAL.format(reasons=r'tex\.surf\.pack'),
        show=('who',),
        source=SURFACES,
        note='the card holds a pack without the RGB10A8_2P_XR10 merge and split states',
    ),
    Check(
        'textures.surf-form',
        OWNER,
        'tex.surf.form: ',
        REPORT,
        process='!SwitchHWTest',
        subsystem=REFUSAL_SUBSYSTEM,
        regex=REFUSAL.format(reasons=r'tex\.surf\.form')
        + r'no \S+ texture of format (?P<format>\d+) .*?: (?P<form>[^:]+): nil',
        show=('who', 'format', 'form'),
        source=SURFACES,
        note="a texture Metal makes that a client asked for and the raw path answered nil (HW Test's open cases "
        'ask on purpose): the forms to build next',
    ),
    Check(
        'textures.blit-across',
        OWNER,
        ACROSS,
        REPORT,
        process='!SwitchHWTest',
        subsystem=REFUSAL_SUBSYSTEM,
        regex=REFUSAL.format(reasons=r'blit\.invalid'),
        show=('who',),
        source=BLIT_RULES,
        note="a client copied across pixel formats with a texture held in planes, Metal's validation error (HW Test's "
        'planes tests do so on purpose)',
    ),
]
