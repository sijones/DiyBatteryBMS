Import("env")

env.Append(CPPDEFINES=[
  ("PLATFORMIO_BOARD", env["BOARD"])
])