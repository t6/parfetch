.if !defined(DISABLE_PARFETCH) && !make(fetch-list) && !make(fetch-url-list-int) && \
	!make(fetch-urlall-list) && !make(fetch-url-list)

.if !defined(PARFETCH)
# Try to use /usr/ports/work/bin/parfetch if it exists. Users can
# make it available to Poudriere by deploying it and enabling
# this overlay without any further setup.
# /work is ignored by git via the ports tree .gitignore.
.  if exists(${PORTSDIR}/work/bin/parfetch-static)
PARFETCH=	${PORTSDIR}/work/bin/parfetch-static
.  elif exists(${PORTSDIR}/work/bin/parfetch)
PARFETCH=	${PORTSDIR}/work/bin/parfetch
.  else
PARFETCH=	parfetch
.  endif
.endif

# Global connection limit per CURLMOPT_MAX_TOTAL_CONNECTIONS(3)
PARFETCH_MAX_TOTAL_CONNECTIONS?=	4

# Max connections per host per CURLMOPT_MAXCONNECTS(3)
PARFETCH_MAX_CONNECTS_PER_HOST?=	1

_PARFETCH_ENV=	dp_PARFETCH_MAX_CONNECTS_PER_HOST=${PARFETCH_MAX_CONNECTS_PER_HOST} \
		dp_PARFETCH_MAX_TOTAL_CONNECTIONS=${PARFETCH_MAX_TOTAL_CONNECTIONS}

_DO_PARFETCH=	${SETENV} ${_DO_FETCH_ENV} ${_MASTER_SITES_ENV} \
		${_PATCH_SITES_ENV} ${_PARFETCH_ENV} ${PARFETCH} \
		${DISTFILES:C/.*/-d '&'/} \
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
