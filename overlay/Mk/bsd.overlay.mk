.if !defined(_INCLUDE_PARFETCH_OVERLAY) && !defined(DISABLE_PARFETCH) && \
	!make(fetch-list) && !make(fetch-url-list-int) && \
	!make(fetch-urlall-list) && !make(fetch-url-list)
_INCLUDE_PARFETCH_OVERLAY=	yes

.if !defined(PARFETCH)
.  if exists(${LOCALBASE}/bin/parfetch)
PARFETCH?=	${LOCALBASE}/bin/parfetch
.  endif
# Try to use bin/parfetch-static from the overlay if parfetch
# is not available from LOCALBASE. This makes it available to
# Poudriere without any further setup.
.  for odir in ${OVERLAYS}
.    if exists(${odir}/bin/parfetch-static)
PARFETCH?=	${odir}/bin/parfetch-static
.    endif
.  endfor
PARFETCH?=	parfetch
.endif

# Global connection limit (see CURLMOPT_MAX_TOTAL_CONNECTIONS(3))
PARFETCH_MAX_TOTAL_CONNECTIONS?=	4

# Max connections per host (see CURLMOPT_MAXCONNECTS(3))
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
