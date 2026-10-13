# blittrace

Diagnostic LD_PRELOAD for rbp: logs DirectFB `dfb_gfxcard_*` operations
(blit, stretchblit, batchblit, tileblit, fill, drawrectangle, drawlines) whose
destination right edge falls on chosen columns, to `/tmp/blittrace.log` inside
the runtime. Edit `hot()` for other columns. Used to find the colorkey blit
off-by-one (docs/rk3399/00-port.md, "Garbage column right of the deck frames").

Build (from work/src/directfb, for its internal headers):

```sh
R=$REPO/work/runtime/rx3
arm-linux-gnueabi-gcc -O2 -fPIC -shared -march=armv5t -mfloat-abi=soft \
  -fno-stack-protector -D_GNU_SOURCE -D_REENTRANT -std=gnu99 \
  -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32 -I. -Iinclude -Ilib -Isrc \
  -o blittrace.so $REPO/tools/blittrace/blittrace.c \
  $R/lib/libdl.so.2 -lc -L$REPO/scripts/shims/compat -L$R/lib -L$R/usr/lib \
  -Wl,-rpath-link,$R/lib:$R/usr/lib
```

Linking `dlsym` against the vendor libdl matters: linked against the host libc
it binds `dlsym@GLIBC_2.4` to libc.so.6 and rbp dies with a relocation error.

Run: install as `usr/lib/rbpi4b/blittrace.so` in the runtime, then
`RX3_EXTRA_PRELOAD=/usr/lib/rbpi4b/blittrace.so` on the launcher.
