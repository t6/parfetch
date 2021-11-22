PARFETCH?=	/home/tobias/src/github.com/t6/parfetch/_build/.bin/parfetch

_DO_PARFETCH=	${SETENV} ${_DO_FETCH_ENV} ${_MASTER_SITES_ENV} ${_PATCH_SITES_ENV} dp_REAL_DISTDIR='${DISTDIR}' \
			${PARFETCH} ${DISTFILES:C/.*/-d '&'/} ${PATCHFILES:C/:-p[0-9]//:C/.*/-p '&'/}

.if !target(do-fetch)
do-fetch:
	@${_DO_PARFETCH}
.endif

# The following aren't really supported.

.if !target(fetch-list)
fetch-list:
	@${_DO_PARFETCH}
.endif

.if !target(fetch-url-list-int)
fetch-url-list-int:
	@${_DO_PARFETCH}
.endif

.if !target(fetch-urlall-list)
fetch-urlall-list:
	@${_DO_PARFETCH}
.endif

.if !target(fetch-url-list)
fetch-url-list:
	@${_DO_PARFETCH}
.endif
