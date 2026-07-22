import subprocess

Import("env")

try:
    git_hash = subprocess.check_output(
        ["git", "describe", "--always", "--dirty"], stderr=subprocess.DEVNULL
    ).decode().strip()
except Exception:
    git_hash = "unknown"

env.Append(CPPDEFINES=[("BUILD_GIT_HASH", env.StringifyMacro(git_hash))])
