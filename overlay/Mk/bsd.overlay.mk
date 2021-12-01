# The following options are supported:
#
# PARFETCH_MAKESUM_EPHEMERAL
# When defined during makesum, distinfo is created/updated but
# no distfiles are saved to disk. Note that the files are still
# downloaded completely to checksum them but DISTDIR is left
# untouched.
#
# PARFETCH_MAKESUM_KEEP_TIMESTAMP
# When defined during makesum, retain the previous TIMESTAMP in
# distinfo. This can be useful when refreshing patches that have
# no code changes and thus do not warrant a TIMESTAMP bump.
#
# PARFETCH_MAX_HOST_CONNECTIONS
# Sets the per host connection limit. Also see
# CURLMOPT_MAX_HOST_CONNECTIONS(3).
#
# PARFETCH_MAX_TOTAL_CONNECTIONS
# Sets the global connection limit. Also see
# CURLMOPT_MAX_TOTAL_CONNECTIONS(3).
#
.if !defined(BEFOREPORTMK) && !defined(INOPTIONSMK) && \
	!defined(_INCLUDE_PARFETCH_OVERLAY) && !defined(NO_PARFETCH) && \
	!make(fetch-list) && !make(fetch-url-list-int) && \
	!make(fetch-urlall-list) && !make(fetch-url-list)
_INCLUDE_PARFETCH_OVERLAY=	yes

.if !defined(PARFETCH)
.  if exists(${LOCALBASE}/bin/parfetch-static)
PARFETCH?=	${LOCALBASE}/bin/parfetch-static
.  elif exists(${LOCALBASE}/bin/parfetch)
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

PARFETCH_MAX_HOST_CONNECTIONS?=		1
PARFETCH_MAX_TOTAL_CONNECTIONS?=	4

_PARFETCH_ENV=	${_DO_FETCH_ENV} \
		${_MASTER_SITES_ENV} \
		${_PATCH_SITES_ENV} \
		dp__PARFETCH_MAKESUM='${_PARFETCH_MAKESUM}' \
		dp_CHECKSUM_ALGORITHMS='${CHECKSUM_ALGORITHMS:tu}' \
		dp_PARFETCH_MAKESUM_EPHEMERAL='${PARFETCH_MAKESUM_EPHEMERAL:Dyes}' \
		dp_PARFETCH_MAKESUM_KEEP_TIMESTAMP='${PARFETCH_MAKESUM_KEEP_TIMESTAMP:Dyes}' \
		dp_PARFETCH_MAX_HOST_CONNECTIONS=${PARFETCH_MAX_HOST_CONNECTIONS} \
		dp_PARFETCH_MAX_TOTAL_CONNECTIONS=${PARFETCH_MAX_TOTAL_CONNECTIONS}
_DO_PARFETCH=	${SETENV} ${_PARFETCH_ENV} ${PARFETCH} \
		${empty(DISTFILES):?:${DISTFILES:C/.*/-d '&'/}} \
		${empty(PATCHFILES):?:${PATCHFILES:C/:-p[0-9]//:C/.*/-p '&'/}}

.if !target(do-fetch)
do-fetch:
	@${_DO_PARFETCH}

.  if !target(checksum)
checksum: fetch
.  endif

.  if !target(makesum)
makesum:
	@${MAKE} fetch _PARFETCH_MAKESUM=yes NO_CHECKSUM=yes DISABLE_SIZE=yes \
		DISTFILES="${DISTFILES}" MASTER_SITES="${MASTER_SITES}" \
		PATCH_SITES="${PATCH_SITES}"
.  endif

.endif

.endif
