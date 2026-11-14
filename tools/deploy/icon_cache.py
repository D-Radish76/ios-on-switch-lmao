"""IconServices' rendered-icon cache on Data, made stale when a deploy stages another GPU build.

iconservicesagent keeps the icons it renders in the system group container systemgroup.com.apple.lsd.iconscache,
a folder named by its UUID or by the group itself (the agent's own sandbox exception names the latter):

    <Data>/containers/Shared/SystemGroup/<UUID or group>/Library/Caches/com.apple.IconsCache/[<salt>/]{store.index,
    __cache_info__, ...}

24B119's IconServices adds <salt>, a UUID of its own build time, the BuildVersion and the UniqueDeviceID
(-[ISDefaults cacheURL]); a cache at the folder's top is still let go where one is there.

__cache_info__ records the ProductBuildVersion the icons were rendered under. At launch the agent empties the cache
unless that value is the system's (/System/Library/CoreServices/SystemVersion.plist), the cache directory belongs to
_iconservices and it carries no ACL (-[ISMutableIconCache initWithURL:] in 21H461's and 24B119's iconservicesagent,
"Resetting icon cache at %@ ..."); it then writes a new __cache_info__. On Apple's devices the GPU driver is part of the
OS build, so an update that changes how icons draw also changes ProductBuildVersion. Here the GPU build changes alone,
and the icons it drew before stay in the cache. So a deploy whose staged GPU build (the stager's whole-list digest, the
first line of ngpu/stage.tsv.gz) differs from the one the cache was last let go for removes ProductBuildVersion from
__cache_info__, then records that digest in <Data>/Switch/icon-cache-gpu-2: the agent's own reset runs at its next
launch, and a deploy of the same build changes nothing. The cache directory itself is never touched: its owner and
ACL are the other two conditions. icon-cache-gpu, the record of a step that looked for __cache_info__ only at the cache
folder's top, says nothing about a 24B119 cache and is not read.
"""

import gzip
import plistlib
from collections.abc import Callable
from pathlib import Path

from tools.install import renamed

GROUP = 'systemgroup.com.apple.lsd.iconscache'
SYSTEM_GROUPS = 'containers/Shared/SystemGroup'
METADATA = '.com.apple.mobile_container_manager.metadata.plist'
CACHE = 'Library/Caches/com.apple.IconsCache'
INFO = '__cache_info__'
BUILD_KEY = 'ProductBuildVersion'
STAGE_LIST = 'ngpu/stage.tsv.gz'  # drivers/TegraGM20BMetal/stage-tegra-gm20b-metal.py STATE
RECORD = 'icon-cache-gpu-2'


def _names_group(metadata: Path) -> bool:
    try:
        with open(metadata, 'rb') as handle:
            return plistlib.load(handle).get('MCMMetadataIdentifier') == GROUP
    except (OSError, plistlib.InvalidFileException, ValueError):
        return False


def cache_directories(data_root: Path) -> list[Path]:
    """Return the agent's cache directories on a mounted Data volume, none where the group container is not made yet."""
    groups = Path(data_root) / SYSTEM_GROUPS
    if not groups.is_dir():
        return []
    containers = [groups / GROUP] if (groups / GROUP).is_dir() else []
    containers += [m.parent for m in sorted(groups.glob('*/' + METADATA)) if m.parent.name != GROUP and _names_group(m)]
    return [c / CACHE for c in containers]


def cache_infos(directory: Path) -> list[Path]:
    """Return each __cache_info__ of a cache directory: at its top and in each salt folder."""
    found = [directory / INFO] if (directory / INFO).is_file() else []
    if directory.is_dir():
        found += sorted(p for p in directory.glob('*/' + INFO) if p.is_file())
    return found


def staged_gpu(data_root: Path) -> str:
    """Return the GPU build the card holds: the stager's whole-list digest, '' without a staged build."""
    try:
        with gzip.open(Path(data_root) / STAGE_LIST, 'rt') as handle:
            first = handle.readline().strip()
    except (OSError, EOFError):
        return ''
    return first[2:] if first.startswith('# ') else ''


def _forget_build(info: Path) -> bool:
    """Remove ProductBuildVersion from __cache_info__ in place, in its own format; whether it held one."""
    data = info.read_bytes()
    values = plistlib.loads(data)
    if not isinstance(values, dict) or BUILD_KEY not in values:
        return False
    del values[BUILD_KEY]
    form = plistlib.FMT_BINARY if data.startswith(b'bplist') else plistlib.FMT_XML
    with open(info, 'r+b') as handle:
        handle.write(plistlib.dumps(values, fmt=form))
        handle.truncate()
    return True


def _let_go(info: Path) -> str:
    try:
        return 'reset at the next launch' if _forget_build(info) else 'already set to reset'
    except (OSError, plistlib.InvalidFileException, ValueError) as error:
        return f'unreadable __cache_info__ ({error}): the agent resets it'


def present(data_root: Path, log: Callable[..., None]) -> str:
    """Let the cache go where the staged GPU build is not the one it was last let go for; return what was done."""
    folder = renamed.data_folder(data_root)
    record = folder / RECORD
    gpu = staged_gpu(data_root)
    previous = record.read_text().strip() if record.is_file() else None
    if previous == gpu:
        return 'unchanged'
    infos = [info for directory in cache_directories(data_root) for info in cache_infos(directory)]
    outcomes = [_let_go(info) for info in infos]
    done = 'no cache yet'
    if outcomes:
        done = 'reset at the next launch' if 'reset at the next launch' in outcomes else outcomes[0]
    folder.mkdir(parents=True, exist_ok=True)
    record.write_text(gpu + '\n')
    log(f'icon cache: GPU build {gpu[:12] or "none"} (was {(previous or "unrecorded")[:12]}): {done}')
    return done
