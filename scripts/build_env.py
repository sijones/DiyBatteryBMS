"""Tell the firmware which env it was built from.

The device knows a great deal about itself - chip, flash size, partition table,
whether PSRAM answered - but not which of the twenty-one builds it is running.
FW_BUILD comes closest and is only the wiring ("ESP32 TWAI"), which was enough
when that was the only thing that varied. It no longer is: a board can be any of
three flash sizes, with or without PSRAM, and the difference decides which file
it must be given to update.

So the env name goes in as a define, and the web UI puts it in the link to the
download page. That link is the whole reason this exists - the device cannot
fetch its own firmware over TLS with the internal heap it has, but it can say
precisely what it is and let the site work out the rest.

It is done here rather than in platformio.ini because ${this.__env__} only
resolves inside an [env:...] section, so the ini would need the same line
repeated in all twenty-one - which is exactly the duplication that file was
restructured to remove. PIOENV is available to every extra script for free.
"""

Import("env")

import os
import subprocess

name = str(env["PIOENV"])

# StringifyMacro handles the quoting, which is otherwise easy to get wrong in a
# way that only shows up as a compile error in an unrelated file.
env.Append(CPPDEFINES=[("PIO_ENV", env.StringifyMacro(name))])

print("Build env: %s" % name)


# The commit, for the boot log.
#
# FW_VERSION only moves when a beta is cut, and a dozen commits can go out under
# one number - so a field log saying BETA12 cannot say whether a given fix is on
# the board, and the answer had to be worked out from which log lines were
# missing. A short hash settles it, and "-dirty" marks a build made from
# uncommitted changes, which no hash describes.
#
# A header rather than a define. Every object's command line carries the
# defines, so a hash there would recompile the whole firmware on every commit; a
# header only rebuilds the files that include it, which is main.cpp. It is
# rewritten only when the text changes, so an unchanged tree does not even do
# that.

def git(*args):
    try:
        return subprocess.check_output(
            ["git"] + list(args), cwd=env.subst("$PROJECT_DIR"),
            stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return None

commit = git("rev-parse", "--short=7", "HEAD")
if commit is None:
    commit = "unknown"   # a source tarball, or no git on the path
elif git("status", "--porcelain", "--untracked-files=no"):
    commit += "-dirty"

gen_dir = os.path.join(env.subst("$BUILD_DIR"), "generated")
os.makedirs(gen_dir, exist_ok=True)
header = os.path.join(gen_dir, "build_info.h")
text = "#pragma once\n#define FW_COMMIT \"%s\"\n" % commit

try:
    with open(header) as fh:
        current = fh.read()
except OSError:
    current = None
if current != text:
    with open(header, "w") as fh:
        fh.write(text)

env.Append(CPPPATH=[gen_dir])

print("Build commit: %s" % commit)
