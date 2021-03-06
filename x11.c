// x11.c --- X11 routines used by the TV and KBD interfaces

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <err.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xos.h>
#include <X11/keysym.h>

#include "usim.h"
#include "utrace.h"
#include "ucode.h"
#include "tv.h"
#include "kbd.h"
#include "mouse.h"

typedef struct DisplayState {
	unsigned char *data;
	int linesize;
	int depth;
	int width;
	int height;
} DisplayState;

static Display *display;
static Window window;
static int bitmap_order;
static int color_depth;
static Visual *visual = NULL;
static GC gc;
static XImage *ximage;

#define USIM_EVENT_MASK ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask | KeyReleaseMask

#define MOUSE_EVENT_LBUTTON 1
#define MOUSE_EVENT_MBUTTON 2
#define MOUSE_EVENT_RBUTTON 4

#define X_SHIFT 1
#define X_CAPS 2
#define X_CTRL 4
#define X_ALT 8
#define X_META 16

static unsigned long Black;
static unsigned long White;

static int old_run_state;

static XComposeStatus status;

// Store modifier bitmasks for Alt and Meta: Shift, Caps Lock, and
// Control are all constant, so they don't need to be stored.
static unsigned int x_alt = X_ALT;
static unsigned int x_meta = X_META;

// Takes E, converts it into a LM (hardware) keycode and sends it to
// the IOB KBD.
static void
process_key(XEvent *e, int keydown)
{
	KeySym keysym;
	unsigned char buffer[5];
	int extra;
	int lmcode;

	extra = 0;
	if (e->xkey.state & x_meta)
		extra |= 3 << 12;

	if (e->xkey.state & x_alt)
		extra |= 3 << 12;

	if (e->xkey.state & X_SHIFT)
		extra |= 3 << 6;

	if (e->xkey.state & X_CAPS)
		extra ^= 3 << 6;

	if (e->xkey.state & X_CTRL)
		extra |= 3 << 10;

	if (keydown) {
		XLookupString(&e->xkey, (char *) buffer, 5, &keysym, &status);

		if (keysym == XK_Shift_L ||
		    keysym == XK_Shift_R ||
		    keysym == XK_Control_L ||
		    keysym == XK_Control_R ||
		    keysym == XK_Alt_L ||
		    keysym == XK_Alt_R)
			return;

		switch (keysym) {
		case XK_F1:        lmcode = 1;             break; // Terminal.
		case XK_F2:        lmcode = 1 | (3 << 8);  break; // System.
		case XK_F3:        lmcode = 0 | (3 << 8);  break; // Network.
		case XK_F4:        lmcode = 16 | (3 << 8); break; // Abort.
		case XK_F5:        lmcode = 17;		   break; // Clear.
		case XK_F6:        lmcode = 44 | (3 << 8); break; // Help.
		case XK_F11:       lmcode = 50 | (3 << 8); break; // End.
		case XK_F7:        lmcode = 16;		   break; // Call.
		case XK_F12:       lmcode = 0;             break; // Break.
		case XK_Break:     lmcode = 0;             break; // Break.
		case XK_BackSpace: lmcode = 046;           break; // Rubout.
		case XK_Return:    lmcode = 50;            break; // Return.
		case XK_Tab:       lmcode = 18;            break; // Tab
		case XK_Escape:    lmcode = 1;             break; // Escape
		default:
			if (keysym > 255) {
				WARNING(TRACE_MISC, "unknown keycode: %lu", keysym);
				return;
			}
			lmcode = kbd_translate_table[(extra & (3 << 6)) ? 1 : 0][keysym];
			break;
		}

		// Keep Control and Meta bits, Shift is in the scancode table.
		lmcode |= extra & ~(3 << 6);
		// ... but if Control or Meta, add in Shift.
		if (extra & (17 << 10))
			lmcode |= extra;

		lmcode |= 0xffff0000;

		kbd_key_event(lmcode, keydown);
	}
}

static int u_minh = 0x7fffffff;
static int u_maxh;
static int u_minv = 0x7fffffff;
static int u_maxv;

void
accumulate_update(int h, int v, int hs, int vs)
{
	if (h < u_minh)
		u_minh = h;
	if (h + hs > u_maxh)
		u_maxh = h + hs;
	if (v < u_minv)
		u_minv = v;
	if (v + vs > u_maxv)
		u_maxv = v + vs;
}

void
send_accumulated_updates(void)
{
	int hs;
	int vs;

	hs = u_maxh - u_minh;
	vs = u_maxv - u_minv;
	if (u_minh != 0x7fffffff && u_minv != 0x7fffffff && u_maxh && u_maxv) {
		XPutImage(display, window, gc, ximage, u_minh, u_minv, u_minh, u_minv, hs, vs);
		XFlush(display);
	}

	u_minh = 0x7fffffff;
	u_maxh = 0;
	u_minv = 0x7fffffff;
	u_maxv = 0;
}

void
x11_event(void)
{
	XEvent e;

	send_accumulated_updates();
	kbd_dequeue_key_event();

	while (XCheckWindowEvent(display, window, USIM_EVENT_MASK, &e)) {
		switch (e.type) {
		case Expose:
			XPutImage(display, window, gc, ximage, 0, 0, 0, 0, tv_width, tv_height);
			XFlush(display);
			break;
		case KeyPress:
			process_key(&e, 1);
			break;
		case KeyRelease:
			process_key(&e, 0);
			break;
		case MotionNotify:
		case ButtonPress:
		case ButtonRelease:
			mouse_event(e.xbutton.x, e.xbutton.y, e.xbutton.button);
			break;
		default:
			break;
		}
	}

	if (old_run_state != run_ucode_flag)
		old_run_state = run_ucode_flag;
}

static void
init_mod_map(void)
{
	XModifierKeymap *map;
	int max_mod;

	map = XGetModifierMapping(display);
	max_mod = map->max_keypermod;

	NOTICE(TRACE_MISC, "Looking up X11 Modifier mappings...\n");

	// The modifiers at indices 0-2 have predefined meanings, but
	// those at indices 3-7 (Mod1 through Mod5) have their meaning
	// defined by the keys assigned to them, so loop through to
	// find Alt and Meta.
	for (int mod = 3; mod < 8; mod++) {
		bool is_alt;
		bool is_meta;

		is_alt = false;
		is_meta = false;

		// Get the keysyms matching this modifier.
		for (int i = 0; i < max_mod; i++) {
			int keysyms_per_code;
			KeyCode code;
			KeySym *syms;

			keysyms_per_code = 0;
			code = map->modifiermap[mod * max_mod + i];

			// Don't try to look up mappings for NoSymbol.
			if (code == NoSymbol)
				continue;

			syms = XGetKeyboardMapping(display, code, max_mod, &keysyms_per_code);
			if (keysyms_per_code == 0)
				WARNING(TRACE_MISC, "No keysyms for code %xu\n", code);

			for (int j = 0; j < keysyms_per_code; j++){
				switch(syms[j]){
				case XK_Meta_L: case XK_Meta_R:
					is_meta = true;
					break;
				case XK_Alt_L: case XK_Alt_R:
					is_alt = true;
					break;
				case NoSymbol:
					break;
				default:
					DEBUG(TRACE_MISC, "Sym %lx\n", syms[j]);
					break;
				}
			}
		}

		// Assign the modifer masks corresponding to this
		// modifier.
		if (is_alt)
			x_alt = 1 << mod;
		if (is_meta)
			x_meta = 1 << mod;

		// If both modifiers have already been found, then
		// we're done.
		if (x_alt && x_meta) {
			NOTICE(TRACE_MISC, "Found x_alt = %d, x_meta = %d\n", x_alt, x_meta);
			return;
		}
	}
}

void
x11_init(void)
{
	char *displayname;
	unsigned long bg_pixel = 0L;
	int xscreen;
	Window root;
	XEvent e;
	XGCValues gcvalues;
	XSetWindowAttributes attr;
	XSizeHints *size_hints;
	XTextProperty windowName;
	XTextProperty *pWindowName = &windowName;
	XTextProperty iconName;
	XTextProperty *pIconName = &iconName;
	XWMHints *wm_hints;
	char *window_name = (char *) "CADR";
	char *icon_name = (char *) "CADR";

	displayname = getenv("DISPLAY");
	display = XOpenDisplay(displayname);
	if (display == NULL)
		errx(1, "failed to open display");

	bitmap_order = BitmapBitOrder(display);
	xscreen = DefaultScreen(display);
	color_depth = DisplayPlanes(display, xscreen);

	Black = BlackPixel(display, xscreen);
	White = WhitePixel(display, xscreen);

	root = RootWindow(display, xscreen);
	attr.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
	window = XCreateWindow(display, root, 0, 0, tv_width, tv_height, 0, color_depth, InputOutput, visual, CWBorderPixel | CWEventMask, &attr);
	if (window == None)
		errx(1, "failed to open window");

	if (!XStringListToTextProperty(&window_name, 1, pWindowName))
		pWindowName = NULL;

	if (!XStringListToTextProperty(&icon_name, 1, pIconName))
		pIconName = NULL;

	size_hints = XAllocSizeHints();
	if (size_hints != NULL) {
		// The window will not be resizable.
		size_hints->flags = PMinSize | PMaxSize;
		size_hints->min_width = size_hints->max_width = tv_width;
		size_hints->min_height = size_hints->max_height = tv_height;
	}

	wm_hints = XAllocWMHints();
	if (wm_hints != NULL) {
		wm_hints->initial_state = NormalState;
		wm_hints->input = True;
		wm_hints->flags = StateHint | InputHint;
	}

	XSetWMProperties(display, window, pWindowName, pIconName, NULL, 0, size_hints, wm_hints, NULL);
	XMapWindow(display, window);

	gc = XCreateGC(display, window, 0, &gcvalues);

	// Fill window with the specified background color.
	bg_pixel = 0;
	XSetForeground(display, gc, bg_pixel);
	XFillRectangle(display, window, gc, 0, 0, tv_width, tv_height);

	// Wait for first Expose event to do any drawing, then flush.
	do
		XNextEvent(display, &e);
	while (e.type != Expose || e.xexpose.count);

	XFlush(display);
	ximage = XCreateImage(display, visual, (unsigned) color_depth, ZPixmap, 0, (char *) tv_bitmap, tv_width, tv_height, 32, 0);
	ximage->byte_order = LSBFirst;

	init_mod_map();
}
