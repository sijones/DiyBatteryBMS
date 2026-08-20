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

name = str(env["PIOENV"])

# StringifyMacro handles the quoting, which is otherwise easy to get wrong in a
# way that only shows up as a compile error in an unrelated file.
env.Append(CPPDEFINES=[("PIO_ENV", env.StringifyMacro(name))])

print("Build env: %s" % name)
