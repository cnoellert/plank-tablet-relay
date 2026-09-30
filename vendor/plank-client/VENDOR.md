# PLANK Client raw Wacom worker snapshot

Source repository: `instinctual/plank-client` (GPL-3.0-or-later)

Source commit: `4b0b569b847a708d0b55535c715f8e16ef906b65`

Files copied without edits:

| Vendored file | Upstream path |
| --- | --- |
| `linuxrawwacom.cpp` | `app/streaming/input/linuxrawwacom.cpp` |
| `linuxrawwacom.h` | `app/streaming/input/linuxrawwacom.h` |
| `wacomidentity.h` | `app/streaming/input/wacomidentity.h` |
| `plank.h` | `moonlight-common-c/moonlight-common-c/src/plank.h` |

CMake checks each file's SHA-256 digest against this snapshot before compiling
the Linux worker. Update the Client first, then deliberately update these
files, hashes and commit together.
