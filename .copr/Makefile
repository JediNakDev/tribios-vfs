# Copr builds this repository directly: the Copr project's package is set to
# the "Custom" / "Make SRPM" method, which clones the repository and runs
# `make -f .copr/Makefile srpm outdir=<dir>`.
#
# Source0 is the published release archive, not the checked-out tree, so a Copr
# build of a commit still packages the last tagged release. Cut the tag first,
# then build. docs/packaging/copr.md has the full procedure.

srpm:
	dnf -y install rpm-build rpmdevtools
	spectool -g -R -D "_sourcedir $(PWD)" packaging/rpm/tribios-vfs.spec
	rpmbuild -bs \
		-D "_sourcedir $(PWD)" \
		-D "_srcrpmdir $(outdir)" \
		packaging/rpm/tribios-vfs.spec

.PHONY: srpm
