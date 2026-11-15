GeForceGLDriverWeb 334.01.03f11 (OS X 10.9.5): query report buffer fix

Code licensed CC-0. Everything in this repository, INCLUDING THE CONTENTS OF THE README BELOW THIS LINE, was written by Claude Opus 5.5.

===

The defect: query results (occlusion counts, timestamps) are written by the GPU into a report buffer that Apple's libGPUSupportMercury grows when its slots run out (32, 64, 128, ...). On growth the driver's "query buffer changed" callback (0x123440309094) copies its per-slot records without rebasing them onto the new buffer, so a query that is active across the growth (for example a GL_TIME_ELAPSED query around 32 or more occlusion queries) writes its result into the destroyed buffer. The GPU channel faults (MMU Error 2), the result never arrives, and the application is aborted. repro.c reproduces it every time with 32 occlusion queries inside one GL_TIME_ELAPSED query (31 do not fault), and checks every query result when there is no fault. Seen in A Hat in Time.

The patch changes three regions of the x86_64 slice; the i386 slice is untouched:
  1. LC_CODE_SIGNATURE, the last load command, is dropped by decrementing ncmds and sizeofcmds in the header. Its bytes stay where they are.
  2. 200 bytes of code (querycave.s) go into the zero padding immediately after the load commands, at slice offset 0xd30 (address 0x123440000d30). The remaining 184 bytes of padding (0xdf8-0xeb0, up to __text) stay free for further patches. The code runs the original callback and then rebases every slot record that still refers to an old buffer: its pointers move to the new buffer and its mapping reference moves to the new buffer's mapping (taking a reference on the new one, releasing the old one with the driver's own release function at 0x1234402f711c).
  3. The first 6 bytes of the callback become a jump to that code.

Files:
  install.command            the Perl find/replace commands that install the patch
  uninstall.command          the reverse commands (restores the stock file byte for byte)
  repro.c                    self-contained repro and result check: ./repro [count] exits 0 (no fault, results correct), 1 (fault), 2 (wrong results); stock driver: ./repro faults, ./repro 31 does not; patched driver: ./repro and larger counts pass
  querycave.s                source of the patch code
  extract.py                 extracts the assembled code bytes (querycave.bin) from the object file
  build_patch.py             builds install.command and uninstall.command from querycave.bin, checks each pattern matches exactly once in the installed stock driver, applies install and uninstall to a copy and checks the results, and leaves the patched copy as GeForceGLDriverWeb.patched
  patchtest.c                runs the patched code in a process without installing it (DYLD_INSERT_LIBRARIES=patchtest.dylib PATCHTEST_FILE=GeForceGLDriverWeb.patched)
  Makefile                   make rebuilds everything from querycave.s against the installed driver, and builds repro and patchtest.dylib; make clean removes the generated files (GeForceGLDriverWeb.patched contains NVIDIA's code and is not distributed)

Verified with the patched code loaded via patchtest: repro with 32 and 300 queries (no fault, all results exact), 31, 32, 100 and 300 occlusion queries inside a timer query, 50 frames of 300 newly created queries each, exact occlusion results for 200 and 1000 queries with and without an active timer query, and A Hat in Time running for 120 seconds without a fault.

Applications that are already running keep the driver they loaded; others, including WindowServer, use the patched driver from their next launch (log out and back in, or restart).
