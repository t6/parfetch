CSTD = gnu99
LDADD += $LDADD_EXECINFO

bundle libias.a
	subdir = $srcdir/libias
	libias/array.c
	libias/compats.c
	libias/diff.c
	libias/distinfo.c
	libias/flow.c
	libias/io.c
	libias/io/mkdirp.c
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
	libias/utf8.c
	libias/util.c

bundle libparfetch.a
	CFLAGS += `pkg-config --cflags libcurl libevent librhash`
	loop.c
	parfetch.c

bin parfetch
	LDADD += `pkg-config --libs libcurl libevent librhash`
	libias.a
	libparfetch.a

bin parfetch-static
	LDADD += -static -Wl,--push-state -Wl,--static `pkg-config --static --libs libcurl libevent librhash` -Wl,--pop-state
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
