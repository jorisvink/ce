/*
 * Copyright (c) 2020 Joris Vink <joris@coders.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <Foundation/Foundation.h>
#include <Appkit/Appkit.h>

#include "ce.h"

void
ce_macos_set_pasteboard_contents(const void *data, size_t len)
{
	NSString	*str;
	NSPasteboard	*pb = [NSPasteboard generalPasteboard];

	[pb clearContents];

	str = [[NSString alloc] initWithBytes:data length:len
	    encoding:NSUTF8StringEncoding];

	[pb setString:str forType:NSPasteboardTypeString];
	[str release];

	ce_debug("added %zu bytes to macos pasteboard", len);
}

void
ce_macos_get_pasteboard_contents(u_int8_t **out, size_t *len)
{
	NSString	*res;
	size_t		slen;
	const char	*ptr;
	NSPasteboard	*pb = [NSPasteboard generalPasteboard];

	res = [pb stringForType:NSPasteboardTypeString];
	if (res == NULL)
		return;

	ptr = [res UTF8String];
	if (ptr == NULL)
		return;

	slen = strlen(ptr);
	if (slen > 1 && ptr[slen - 2] == '\n')
		slen--;

	ce_debug("obtained %zu bytes from macos pasteboard", slen);

	if ((*out = calloc(1, slen)) == NULL)
		fatal("%s: calloc(%zu): %s", __func__, slen, errno_s);

	memcpy(*out, ptr, slen);
	*len = slen;
}
