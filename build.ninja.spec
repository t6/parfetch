CSTD = gnu11
LDFLAGS += -pthread
LDADD += $LDADD_EXECINFO

pkg-config
	libcrypto
	libevent
	libssl
	zlib

bundle libcurl.a
	subdir = $srcdir/vendor/libcurl
	CFLAGS += -I$srcdir/vendor/curl/include -I$srcdir/vendor/curl/lib -I$srcdir/vendor/nghttp2/lib/includes -I$srcdir/vendor/include $CFLAGS_libcrypto $CFLAGS_libssl $CFLAGS_zlib -include $srcdir/vendor/include/curl-config.h
	$srcdir/vendor/include/curl-config.h
	vendor/curl/lib/asyn-thread.c
	vendor/curl/lib/altsvc.c
	vendor/curl/lib/base64.c
	vendor/curl/lib/bufref.c
	vendor/curl/lib/conncache.c
	vendor/curl/lib/connect.c
	vendor/curl/lib/content_encoding.c
	vendor/curl/lib/cookie.c
	vendor/curl/lib/curl_addrinfo.c
	vendor/curl/lib/curl_ctype.c
	vendor/curl/lib/curl_fnmatch.c
	vendor/curl/lib/curl_get_line.c
	vendor/curl/lib/curl_memrchr.c
	vendor/curl/lib/curl_range.c
	vendor/curl/lib/curl_threads.c
	vendor/curl/lib/dotdot.c
	vendor/curl/lib/doh.c
	vendor/curl/lib/dynbuf.c
	vendor/curl/lib/easy.c
	vendor/curl/lib/escape.c
	vendor/curl/lib/file.c
	vendor/curl/lib/fileinfo.c
	vendor/curl/lib/formdata.c
	vendor/curl/lib/ftp.c
	vendor/curl/lib/ftplistparser.c
	vendor/curl/lib/getenv.c
	vendor/curl/lib/getinfo.c
	vendor/curl/lib/h2h3.c
	vendor/curl/lib/hash.c
	vendor/curl/lib/hmac.c
	vendor/curl/lib/hostasyn.c
	vendor/curl/lib/hostip.c
	vendor/curl/lib/hostip6.c
	vendor/curl/lib/hsts.c
	vendor/curl/lib/http.c
	vendor/curl/lib/http2.c
	vendor/curl/lib/http_aws_sigv4.c
	vendor/curl/lib/http_chunks.c
	vendor/curl/lib/http_digest.c
	vendor/curl/lib/http_proxy.c
	vendor/curl/lib/if2ip.c
	vendor/curl/lib/inet_ntop.c
	vendor/curl/lib/inet_pton.c
	vendor/curl/lib/llist.c
	vendor/curl/lib/md5.c
	vendor/curl/lib/mime.c
	vendor/curl/lib/mprintf.c
	vendor/curl/lib/mqtt.c
	vendor/curl/lib/multi.c
	vendor/curl/lib/netrc.c
	vendor/curl/lib/nonblock.c
	vendor/curl/lib/parsedate.c
	vendor/curl/lib/pingpong.c
	vendor/curl/lib/progress.c
	vendor/curl/lib/rand.c
	vendor/curl/lib/rename.c
	vendor/curl/lib/select.c
	vendor/curl/lib/sendf.c
	vendor/curl/lib/setopt.c
	vendor/curl/lib/sha256.c
	vendor/curl/lib/share.c
	vendor/curl/lib/slist.c
	vendor/curl/lib/socks.c
	vendor/curl/lib/socketpair.c
	vendor/curl/lib/speedcheck.c
	vendor/curl/lib/splay.c
	vendor/curl/lib/strcase.c
	vendor/curl/lib/strdup.c
	vendor/curl/lib/strerror.c
	vendor/curl/lib/strtok.c
	vendor/curl/lib/strtoofft.c
	vendor/curl/lib/timeval.c
	vendor/curl/lib/transfer.c
	vendor/curl/lib/url.c
	vendor/curl/lib/urlapi.c
	vendor/curl/lib/version.c
	vendor/curl/lib/vauth/digest.c
	vendor/curl/lib/vauth/vauth.c
	vendor/curl/lib/vtls/hostcheck.c
	vendor/curl/lib/vtls/keylog.c
	vendor/curl/lib/vtls/openssl.c
	vendor/curl/lib/vtls/vtls.c
	vendor/curl/lib/warnless.c
	vendor/curl/lib/wildcard.c

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
	libias/workqueue.c

bundle libnghttp2.a
	subdir = $srcdir/vendor/nghttp2
	CFLAGS += -I$srcdir/vendor/nghttp2/lib/includes -I$srcdir/vendor/include
	CPPFLAGS += -DNGHTTP2_STATICLIB -DHAVE_NETINET_IN -DHAVE_ARPA_INET_H
	vendor/nghttp2/lib/nghttp2_buf.c
	vendor/nghttp2/lib/nghttp2_callbacks.c
	vendor/nghttp2/lib/nghttp2_debug.c
	vendor/nghttp2/lib/nghttp2_frame.c
	vendor/nghttp2/lib/nghttp2_hd.c
	vendor/nghttp2/lib/nghttp2_hd_huffman.c
	vendor/nghttp2/lib/nghttp2_hd_huffman_data.c
	vendor/nghttp2/lib/nghttp2_helper.c
	vendor/nghttp2/lib/nghttp2_http.c
	vendor/nghttp2/lib/nghttp2_map.c
	vendor/nghttp2/lib/nghttp2_mem.c
	vendor/nghttp2/lib/nghttp2_npn.c
	vendor/nghttp2/lib/nghttp2_option.c
	vendor/nghttp2/lib/nghttp2_outbound_item.c
	vendor/nghttp2/lib/nghttp2_pq.c
	vendor/nghttp2/lib/nghttp2_priority_spec.c
	vendor/nghttp2/lib/nghttp2_queue.c
	vendor/nghttp2/lib/nghttp2_rcbuf.c
	vendor/nghttp2/lib/nghttp2_session.c
	vendor/nghttp2/lib/nghttp2_stream.c
	vendor/nghttp2/lib/nghttp2_submit.c
	vendor/nghttp2/lib/nghttp2_version.c

bundle libparfetch.a
	CFLAGS += -I$srcdir/vendor/curl/include $CFLAGS_libcrypto $CFLAGS_libevent
	loop.c
	parfetch.c
	progress.c

bin parfetch
	LDADD += $LDADD_libcrypto $LDADD_libevent $LDADD_libssl $LDADD_zlib
	libcurl.a
	libias.a
	libnghttp2.a
	libparfetch.a

bin parfetch-static
	LDADD += -static -Wl,--push-state -Wl,--static $LDADD_static_libcrypto $LDADD_static_libevent $LDADD_static_libssl $LDADD_static_zlib -Wl,--pop-state
	libcurl.a
	libias.a
	libnghttp2.a
	libparfetch.a

install-data parfetch
	overlay/Mk/bsd.overlay.mk

ninja
	rule hardlink-binary
	  command = ln $in $out || cp $in $out
	  description = LN $in $out
	build $DESTDIR$SHAREDIR/parfetch/overlay/bin/parfetch-static: hardlink-binary $DESTDIR$BINDIR/parfetch-static

default-install $DESTDIR$SHAREDIR/parfetch/overlay/bin/parfetch-static
