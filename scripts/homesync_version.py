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
env.Append(CPPDEFINES=[("CROSSPOINT_VERSION", env.StringifyMacro(full))])  # noqa: F821
print(f"homesync version: {full}")
