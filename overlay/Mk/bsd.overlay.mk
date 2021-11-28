.if !defined(_INCLUDE_PARFETCH_OVERLAY) && !defined(NO_PARFETCH) && \
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

# Per host connection limit (see CURLMOPT_MAX_HOST_CONNECTIONS(3))
PARFETCH_MAX_HOST_CONNECTIONS?=		1
# Global connection limit (see CURLMOPT_MAX_TOTAL_CONNECTIONS(3))
PARFETCH_MAX_TOTAL_CONNECTIONS?=	4

_PARFETCH_ENV=	${_DO_FETCH_ENV} \
		${_MASTER_SITES_ENV} \
		${_PATCH_SITES_ENV} \
		dp__PARFETCH_MAKESUM='${_PARFETCH_MAKESUM}' \
		dp_CHECKSUM_ALGORITHMS='${CHECKSUM_ALGORITHMS:tu}' \
		dp_PARFETCH_MAX_HOST_CONNECTIONS=${PARFETCH_MAX_HOST_CONNECTIONS} \
		dp_PARFETCH_MAX_TOTAL_CONNECTIONS=${PARFETCH_MAX_TOTAL_CONNECTIONS}
_DO_PARFETCH=	${SETENV} ${_PARFETCH_ENV} ${PARFETCH} \
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

.if !target(makesum)
makesum: check-sanity
	@${MAKE} fetch _PARFETCH_MAKESUM=yes NO_CHECKSUM=yes DISABLE_SIZE=yes \
		DISTFILES="${DISTFILES}" MASTER_SITES="${MASTER_SITES}" \
		PATCH_SITES="${PATCH_SITES}"
.endif

.endif
