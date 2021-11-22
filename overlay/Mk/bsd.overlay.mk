.if !defined(DISABLE_PARFETCH) && !make(fetch-list) && !make(fetch-url-list-int) && \
	!make(fetch-urlall-list) && !make(fetch-url-list)

.if !defined(PARFETCH)
# Try to use /usr/ports/work/bin/parfetch if it exists. Users can
# make it available to Poudriere by deploying it and enabling
# this overlay without any further setup.
# /work is ignored by git via the ports tree .gitignore.
.  if exists(${PORTSDIR}/work/bin/parfetch)
PARFETCH=	${PORTSDIR}/work/bin/parfetch
.  else
PARFETCH=	parfetch
.  endif
.endif

_DO_PARFETCH=	${SETENV} ${_DO_FETCH_ENV} ${_MASTER_SITES_ENV} \
		${_PATCH_SITES_ENV} ${PARFETCH} ${DISTFILES:C/.*/-d '&'/} \
		${PATCHFILES:C/:-p[0-9]//:C/.*/-p '&'/}

.if !target(do-fetch)
do-fetch:
	@${_DO_PARFETCH}
.endif

.if !target(checksum)
checksum:
.  if !defined(NO_CHECKSUM)
	@${_DO_PARFETCH}
.  endif
.endif

.endif
