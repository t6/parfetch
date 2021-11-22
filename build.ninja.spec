CFLAGS += -O0

bundle libias.a
	subdir = $srcdir/libias
	libias/array.c
	libias/compats.c
	libias/flow.c
	libias/io.c
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
	parfetch.c
	mkdirs.c

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
