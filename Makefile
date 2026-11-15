# install.command and uninstall.command are generated from querycave.s and checked
# against the driver installed on this machine by build_patch.py, which
# also leaves a patched copy of the driver (GeForceGLDriverWeb.patched) for
# patchtest.dylib.  The patched copy contains NVIDIA's code and is not part
# of the source.

all: install.command patchtest.dylib repro

querycave.bin: querycave.s extract.py
	clang -c -arch x86_64 -o querycave.o querycave.s
	python3 -I extract.py querycave.o querycave.bin 0x123440000d30

install.command uninstall.command GeForceGLDriverWeb.patched: querycave.bin build_patch.py
	python3 -I build_patch.py

repro: repro.c
	clang -O2 -Wall -Wno-deprecated-declarations -o $@ repro.c -framework OpenGL

patchtest.dylib: patchtest.c
	clang -O2 -Wall -arch x86_64 -dynamiclib -o $@ patchtest.c

clean:
	rm -f querycave.o querycave.bin patchtest.dylib repro GeForceGLDriverWeb.patched
