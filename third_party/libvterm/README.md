# libvterm 0.3.3 (scrollback-patched subset)

This directory contains the C11 source and header subset of libvterm 0.3.3 used by the `mcpp` build of `Lib-Terminal`:

- Upstream archives:
  - `https://deb.debian.org/debian/pool/main/libv/libvterm/libvterm_0.3.3.orig.tar.gz`
  - `https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz`
- Archive SHA-256: `09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0`
- License: MIT (see `LICENSE`)

The scrollback continuation metadata patches applied to `include/vterm.h`, `src/vterm_internal.h`, `src/state.c`, and `src/screen.c` match `cmake/LibVTermScrollback.cmake`.
