/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
 */

/*
 * The VirtualBox guest ring-0 libraries linked into this module call
 * bcmp(), which the amd64 kernel no longer exports (it is a macro on top
 * of memcmp() there).  Provide it so that the module links.
 */

#include <sys/param.h>
#include <sys/systm.h>

#undef bcmp
int bcmp(const void *, const void *, size_t);

int
bcmp(const void *b1, const void *b2, size_t len)
{

	return (memcmp(b1, b2, len) != 0);
}
