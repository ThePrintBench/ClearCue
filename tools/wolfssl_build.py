# PlatformIO extra script for [env:clearcue] (runs as "post:"). Compiles the wolfSSL
# checkout fetched by tools/wolfssl_config.py into a static library and links it.
Import("env")  # noqa: F821
import os

dest = env["BB_WOLFSSL_DIR"]

# Every top-level source; anything not enabled by include/user_settings.h compiles to
# an empty object. Quiet wolfSSL's own warnings so the project's stay visible.
wenv = env.Clone()
wenv.Append(CCFLAGS=["-w"])
# Also on CPPPATH so SCons tracks include/user_settings.h and rebuilds wolfSSL when it
# changes (it can't see through -iquote).
wenv.Append(CPPPATH=[os.path.join(env.subst("$PROJECT_DIR"), "include")])
lib = wenv.BuildLibrary(
    os.path.join("$BUILD_DIR", "wolfssl"),
    dest,
    src_filter=["-<*>", "+<src/*.c>", "+<wolfcrypt/src/*.c>", "-<src/sniffer.c>"],
)
env.Prepend(LIBS=[lib])
