# Prototype runs on macOS and Linux through the FUSE 2.x API

The prototype's filesystem adapter targets the FUSE 2.x API, which macFUSE
implements on macOS and libfuse 2.x implements on Linux, so one adapter builds
and mounts on both.

This reopens a decision recorded in issue #1, which lists Linux and a
cross-platform filesystem adapter as out of scope. That scope was written to
stop the experiment paying for a portability layer. No such layer exists here:
the Workspace engine is ordinary POSIX code, and the adapter is a single file
whose platform differences are the extended-attribute callback signatures, two
macFUSE-only mount options and the unmount command. Linux support costs three
`__APPLE__` guards and gains a platform to develop and test on.

macOS with macFUSE stays the platform whose measurements decide the verdict for
issue #1. Linux results are reported separately and never substituted for it.

FUSE 3 is not supported. libfuse 3 does not offer the FUSE 2 API, so it would
need a second adapter, and no part of the verdict depends on it.
