NAME=		parfetch
AWK?=		awk

all:

clean:
	@rm -r _build

tag:
	@[ -z "${V}" ] && echo "must set V" && exit 1; \
	date=$$(git log -1 --pretty=format:%cd --date=format:%Y-%m-%d HEAD); \
	title="== [${V}] - $${date}"; \
	if ! grep -Fq "$${title}" CHANGELOG.adoc; then \
		echo "# ${NAME} ${V}"; \
		${AWK} '/^== Unreleased$$/{x=1;next}x{if($$1=="=="){exit}else if($$1=="==="){$$1="=="};print}' \
			CHANGELOG.adoc >RELNOTES.adoc.new; \
		${AWK} "/^== Unreleased$$/{print;printf\"\n$${title}\n\";next}{print}" \
			CHANGELOG.adoc >CHANGELOG.adoc.new; \
		mv CHANGELOG.adoc.new CHANGELOG.adoc; \
		cat RELNOTES.adoc.new >>RELNOTES.adoc; \
		asciidoctor -b html5 -o - RELNOTES.adoc | pandoc -f html -t gfm - -o RELNOTES.md.new; \
		printf "${NAME} ${V}\n\n" >RELNOTES.md; \
		cat RELNOTES.md.new >>RELNOTES.md; \
		rm -f RELNOTES.adoc.new RELNOTES.adoc RELNOTES.md.new; \
	fi; \
	git commit -m "Release ${V}" CHANGELOG.adoc; \
	git tag -F RELNOTES.md v${V}

release:
	@tag=$$(git tag --points-at HEAD); \
	if [ -z "$$tag" ]; then echo "create a tag first"; exit 1; fi; \
	V=$$(echo $${tag} | sed 's,^v,,'); \
	git ls-files --recurse-submodules . ':!:libias/tests' | \
		bsdtar --files-from=- -s ",^,${NAME}-$${V}/," --options lzip:compression-level=9 \
			--uid 0 --gid 0 -caf ${NAME}-$${V}.tar.lz; \
	sha256 ${NAME}-$${V}.tar.lz >${NAME}-$${V}.tar.lz.SHA256 || \
	sha256sum --tag ${NAME}-$${V}.tar.lz >${NAME}-$${V}.tar.lz.SHA256; \
	printf "SIZE (%s) = %s\n" ${NAME}-$${V}.tar.lz $$(wc -c <${NAME}-$${V}.tar.lz) \
		>>${NAME}-$${V}.tar.lz.SHA256

publish:
	@tag=$$(git tag --points-at HEAD); \
	if [ -z "$$tag" ]; then echo "create a tag first"; exit 1; fi; \
	V=$$(echo $${tag} | sed 's,^v,,'); \
	git push --follow-tags origin; \
	gh release create $${tag} -F RELNOTES.md \
		${NAME}-$${V}.tar.lz \
		${NAME}-$${V}.tar.lz.SHA256

.PHONY: all clean publish release tag
