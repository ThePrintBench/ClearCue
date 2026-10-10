# PlatformIO extra script for [env:clearcue] (runs as "pre:").
#
# Fetches a pinned wolfSSL release into .pio/deps/ (once), builds it as a static
# library with include/user_settings.h, and wires up the include paths/defines so
# src/clearcue/tls_*.cpp can use it.
#
# Pinned to an exact upstream tag + commit: the firmware is GPLv3 because of this
# library, so "corresponding source" is: this repo + wolfSSL at WOLFSSL_COMMIT.
# Bump both together and re-run test/tls_host/run.sh before releasing.

Import("env")  # noqa: F821
import os
import subprocess

WOLFSSL_TAG = "v5.9.4-stable"
WOLFSSL_COMMIT = "3c5eead44904df64e6a5a1f4ebdce377d35a849a"   # commit (the tag object is 25ada78b)
WOLFSSL_URL = "https://github.com/wolfSSL/wolfssl.git"

proj = env.subst("$PROJECT_DIR")
dest = os.path.join(proj, ".pio", "deps", f"wolfssl-{WOLFSSL_TAG}")


def git(*args, cwd=None):
    return subprocess.check_output(["git", *args], cwd=cwd, stderr=subprocess.STDOUT).decode().strip()


def ensure_source():
    if os.path.isdir(os.path.join(dest, ".git")):
        head = git("rev-parse", "HEAD", cwd=dest)
        if head == WOLFSSL_COMMIT:
            return
        print(f"[wolfssl] {dest} is at {head}, expected {WOLFSSL_COMMIT}; re-fetching")
        import shutil
        shutil.rmtree(dest)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    print(f"[wolfssl] fetching {WOLFSSL_URL} {WOLFSSL_TAG} (one time)")
    try:
        git("clone", "--quiet", "--depth", "1", "--branch", WOLFSSL_TAG, WOLFSSL_URL, dest)
    except FileNotFoundError:
        raise SystemExit("[wolfssl] git is not on PATH; install Git for Windows or add it to PATH")
    except subprocess.CalledProcessError as e:
        raise SystemExit("[wolfssl] clone failed:\n" + e.output.decode(errors="replace"))
    head = git("rev-parse", "HEAD", cwd=dest)
    if head != WOLFSSL_COMMIT:
        raise SystemExit(f"[wolfssl] tag {WOLFSSL_TAG} resolved to {head}, expected {WOLFSSL_COMMIT}. "
                         "Refusing to build an unpinned TLS library.")


ensure_source()

# Project-wide: wolfSSL headers, and our user_settings.h searched before any other
# (-iquote is consulted ahead of -I for #include "user_settings.h").
env.Append(
    CPPPATH=[dest],
    CPPDEFINES=["WOLFSSL_USER_SETTINGS"],
    CCFLAGS=["-iquote", os.path.join(proj, "include")],
)

# The library itself is compiled by tools/wolfssl_build.py (a post: script), once
# the platform has set up the RISC-V toolchain; doing it here would use host gcc.
env["BB_WOLFSSL_DIR"] = dest
