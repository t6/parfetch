CSTD = gnu99
LDADD += $LDADD_EXECINFO $LDADD_SHA2

bundle libias.a
	subdir = $srcdir/libias
	libias/array.c
	libias/compats.c
	libias/flow.c
	libias/io.c
	libias/io/mkdirp.c
	libias/map.c
	libias/mem.c
	libias/mempool.c
	libias/mempool/file.c
	libias/queue.c
	libias/set.c
	libias/stack.c
	libias/str.c
	libias/util.c

bundle libparfetch.a
	CFLAGS += `pkg-config --cflags libcurl libevent`
	loop.c
	parfetch.c

bin parfetch
	LDADD += `pkg-config --libs libcurl libevent`
	libias.a
	libparfetch.a

bin parfetch-static
	LDADD += -static -Wl,--push-state -Wl,--static `pkg-config --static --libs libcurl libevent` -Wl,--pop-state
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
