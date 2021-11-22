CFLAGS += -O0

bundle libias.a
	subdir = $srcdir/libias
	libias/array.c
	libias/compats.c
	libias/diff.c
	libias/diffutil.c
	libias/flow.c
	libias/io.c
	libias/io/dir.c
	libias/json.c
	libias/map.c
	libias/mem.c
	libias/mempool.c
	libias/mempool/dir.c
	libias/mempool/file.c
	libias/mtree.c
	libias/path.c
	libias/peg.c
	libias/peg/clang.c
	libias/peg/json.c
	libias/peg/mtree.c
	libias/peg/objget.c
	libias/peg/toml.c
	libias/queue.c
	libias/set.c
	libias/stack.c
	libias/str.c
	libias/utf8.c
	libias/util.c

bundle libparfetch.a
	CFLAGS += `pkg-config --cflags libcurl libevent`
	parfetch.c
	mkdirs.c

bin parfetch
	LDADD += -static -Wl,--push-state -Wl,--static `pkg-config --static --libs libcurl libevent` -Wl,--pop-state
	libias.a
	libparfetch.a
