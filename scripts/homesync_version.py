# PlatformIO pre-script for x4pro-homesync: version = 1.6.<100 + commits since
# the fork point>, so every committed build is "newer" to OtaUpdater's
# three-segment compare. A dirty tree gets a "-dev" suffix (never published).
import subprocess

Import("env")  # noqa: F821  (PlatformIO injects Import)

FORK_BASE = "707625a"


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, text=True, cwd=env["PROJECT_DIR"]).stdout.strip()  # noqa: F821


count = git("rev-list", "--count", f"{FORK_BASE}..HEAD") or "0"
dirty = git("status", "--porcelain", "--untracked-files=no")
version = f"1.6.{100 + int(count)}"
full = f"{version}{'-dev' if dirty else ''}-x4pro-hs"

# The version changes on every commit. As a global define it would change the
# command line of all ~600 translation units and force a full rebuild (~5 min);
# only the handful of files that use CROSSPOINT_VERSION get it, so everything
# else stays cached.
_uses = {}


def _with_version(env_, node):
    path = node.srcnode().get_abspath()
    if path not in _uses:
        try:
            with open(path, errors="ignore") as f:
                _uses[path] = "CROSSPOINT_VERSION" in f.read()
        except OSError:
            _uses[path] = False
    if not _uses[path]:
        return node
    local = env_.Clone()
    local.Append(CPPDEFINES=[("CROSSPOINT_VERSION", env_.StringifyMacro(full))])
    return local.Object(node)


env.AddBuildMiddleware(_with_version)  # noqa: F821
print(f"homesync version: {full}")
