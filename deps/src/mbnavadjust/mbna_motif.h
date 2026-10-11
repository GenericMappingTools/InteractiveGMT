/*======================================================================================
 * mbna_motif.h -- the slice of Motif / Xt that mbnavadjust_callbacks.c uses, re-expressed for the
 * InteractiveGMT port: the C side of a by-name widget bridge whose other side is the Qt window,
 * mbnavadjust_window.cpp.
 *
 * A Widget is its NAME: the Motif widget name of mbnavadjust_creation.c, which is the objectName
 * of the same widget in the deps/ui/mbnavadjust*.ui files. Every Motif call the callbacks make --
 * XtVaSetValues for a sensitivity or a scale, a toggle state, a list's items, a label's text, the
 * message text, a dialog managed or unmanaged -- becomes a call of the same shape, forwarded by
 * name to the window, so the callbacks keep their upstream text. An XmString is a plain C string.
 *
 * Every function is mbna_-prefixed through the #defines below: the names can never bind to a real
 * Motif / Xt library, nor to another ported tool, in the same process.
 *====================================================================================*/

#ifndef MBNA_MOTIF_H_
#define MBNA_MOTIF_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef const char *Widget;
typedef Widget *WidgetList;
typedef void *XtPointer;
typedef char *String;
typedef char *XmString;
typedef unsigned char Boolean;
typedef unsigned int Cardinal;
#ifndef True
#	define True 1
#	define False 0
#endif
#ifndef TRUE
#	define TRUE 1
#	define FALSE 0
#endif

/* the resources the callbacks set and get, as the window understands them */
#define XmNsensitive     "sensitive"
#define XmNvalue         "value"
#define XmNminimum       "minimum"
#define XmNmaximum       "maximum"
#define XmNdecimalPoints "decimalPoints"
#define XmNwidth         "width"
#define XmNheight        "height"
#define XmNlabelString   "labelString"

/* XmStringUnparse, as the list callbacks call it: the item's text, to be XtFree'd */
#define XmCHARSET_TEXT 0
#define XmOUTPUT_ALL 0
#define XmStringUnparse(s, tag, tagtype, outtype, table, count, opt) mbna_XmStringCreateLocalized(s)

typedef unsigned short Dimension;

/* a drawing area's input, as the input callbacks read it from their call_data: only the members
   they read; the window fills them from its Qt mouse events */
#define ButtonPress 4
#define ButtonRelease 5
#define MotionNotify 6
#define XmCR_INPUT 3
typedef struct {
	int type;
} XAnyEvent;
typedef struct {
	int type;
	int x, y;
	unsigned int button;
} XButtonEvent;
typedef struct {
	int type;
	int x, y;
} XMotionEvent;
typedef struct {
	XAnyEvent xany;
	XButtonEvent xbutton;
	XMotionEvent xmotion;
} XEvent;
typedef struct {
	int reason;
	XEvent *event;
} XmAnyCallbackStruct;

/* a list's selection, as do_list_data_select reads it from its call_data */
typedef struct {
	int reason;
	int item_position;
	XmString item;
	int selected_item_count;
	XmString *selected_items;
} XmListCallbackStruct;

/* XtSetArg / XtSetValues, for the two places that set resources through an Arg list (the file
   selection box's pattern, the modelplot drawing area's size) */
#define XmNpattern "pattern"
#define XmRXmString "XmString"
typedef struct {
	String name;
	long long value;
} Arg;
#define XtSetArg(arg, n, v) ((arg).name = (n), (arg).value = (long long)(v))
/* BX_CONVERT of a string to an XmString: a copy of it, which the caller XmStringFree's */
#define BX_CONVERT(w, from, to, size, ok) ((*(ok) = 1), mbna_XmStringCreateLocalized(from))

/* X11 resources that went with the window: nothing to free */
#define XFreeGC(display, gc) ((void)0)
#define xg_free(xgid) ((void)0)

/* the window side: implemented in mbnavadjust_window.cpp ------------------------------------- */
/* each Arg: "pattern" a char * (the file dialog filter), "width"/"height" an int */
void mbna_XtSetValues(Widget w, Arg *args, Cardinal n);
/* name/value pairs until a NULL name; "labelString" takes a char *, every other resource an int */
void mbna_XtVaSetValues(Widget w, ...);
/* name/int* pairs until a NULL name */
void mbna_XtVaGetValues(Widget w, ...);
void mbna_XmToggleButtonSetState(Widget w, Boolean state, Boolean notify);
Boolean mbna_XmToggleButtonGetState(Widget w);
void mbna_XmListDeleteAllItems(Widget w);
void mbna_XmListAddItems(Widget w, XmString *items, int item_count, int position);
void mbna_XmListSelectPos(Widget w, int position, Boolean notify);
void mbna_XmListSetPos(Widget w, int position);
Boolean mbna_XmListGetSelectedPos(Widget w, int **position_list, int *position_count);
void mbna_XtManageChild(Widget w);
void mbna_XtUnmanageChild(Widget w);
void mbna_XmUpdateDisplay(Widget w);
void mbna_XmTextFieldSetString(Widget w, char *value);
void mbna_mb_get_text_string(Widget w, char *str, size_t len);
/* the message text: append at the end and show the end */
void mbna_text_append(Widget w, const char *text);
void mbna_set_label_string(Widget w, String str);
/* a label of BuilderXcessory's multi-line form :::t"line 1":t"line 2"... */
void mbna_set_label_multiline_string(Widget w, String str);
void mbna_XBell(int percent);

/* the C side: mbna_motif.c ------------------------------------------------------------------- */
XmString mbna_XmStringCreateLocalized(const char *text);
void mbna_XmStringFree(XmString s);
void mbna_XtFree(void *p);

#define XtVaSetValues            mbna_XtVaSetValues
#define XtVaGetValues            mbna_XtVaGetValues
#define XmToggleButtonSetState   mbna_XmToggleButtonSetState
#define XmToggleButtonGetState   mbna_XmToggleButtonGetState
#define XmListDeleteAllItems     mbna_XmListDeleteAllItems
#define XmListAddItems           mbna_XmListAddItems
#define XmListSelectPos          mbna_XmListSelectPos
#define XmListSetPos             mbna_XmListSetPos
#define XmListGetSelectedPos     mbna_XmListGetSelectedPos
#define XtManageChild            mbna_XtManageChild
#define XtUnmanageChild          mbna_XtUnmanageChild
#define XmUpdateDisplay          mbna_XmUpdateDisplay
#define XmTextFieldSetString     mbna_XmTextFieldSetString
#define mb_get_text_string       mbna_mb_get_text_string
#define set_label_string         mbna_set_label_string
#define set_label_multiline_string mbna_set_label_multiline_string
#define XmStringCreateLocalized  mbna_XmStringCreateLocalized
#define XmStringFree             mbna_XmStringFree
#define XtFree                   mbna_XtFree
#define XtSetValues              mbna_XtSetValues
#define MBV_SELECT_NONE          (-1)

#ifdef __cplusplus
}
#endif

#endif /* MBNA_MOTIF_H_ */
