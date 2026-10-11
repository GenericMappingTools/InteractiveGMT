/*======================================================================================
 * mbna_motif.c -- the C half of the Motif shim of the mbnavadjust port, mbna_motif.h: an XmString is
 * a plain C string, made and freed here. Everything a widget does is the window's,
 * mbnavadjust_window.cpp.
 *====================================================================================*/

#include <stdlib.h>
#include <string.h>

#include "mbna_motif.h"

XmString mbna_XmStringCreateLocalized(const char *text) {
	if (text == NULL)
		text = "";
	const size_t n = strlen(text) + 1;
	char *s = (char *)malloc(n);
	if (s != NULL)
		memcpy(s, text, n);
	return s;
}

void mbna_XmStringFree(XmString s) {
	free(s);
}

void mbna_XtFree(void *p) {
	free(p);
}
