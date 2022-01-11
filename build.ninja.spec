CSTD = gnu99
LDFLAGS += -pthread
LDADD += $LDADD_EXECINFO

pkg-config
	libcrypto
	libcurl
	libevent

bundle libias.a
	subdir = $srcdir/libias
	libias/array.c
	libias/compats.c
	libias/diff.c
	libias/distinfo.c
	libias/flow.c
	libias/io.c
	libias/io/mkdirp.c
	libias/libgrapheme/character.c
	libias/libgrapheme/utf8.c
	libias/libgrapheme/util.c
	libias/map.c
	libias/mem.c
	libias/mempool.c
	libias/mempool/file.c
	libias/peg.c
	libias/peg/distinfo.c
	libias/queue.c
	libias/set.c
	libias/stack.c
	libias/str.c
	libias/util.c

bundle libparfetch.a
	CFLAGS += $CFLAGS_libcrypto $CFLAGS_libcurl $CFLAGS_libevent
	loop.c
	parfetch.c
	progress.c

bin parfetch
	LDADD += $LDADD_libcrypto $LDADD_libcurl $LDADD_libevent
	libias.a
	libparfetch.a

bin parfetch-static
	LDADD += -static -Wl,--push-state -Wl,--static $LDADD_static_libcrypto $LDADD_static_libcurl $LDADD_static_libevent -Wl,--pop-state
	libias.a
	libparfetch.a

install-data parfetch
	overlay/Mk/bsd.overlay.mk

ninja
	rule hardlink-binary
	  command = ln $in $out || cp $in $out
	  description = LN $in $out
	build $DESTDIR$SHAREDIR/parfetch/overlay/bin/parfetch-static: hardlink-binary $DESTDIR$BINDIR/parfetch-static

default-install $DESTDIR$SHAREDIR/parfetch/overlay/bin/parfetch-static
