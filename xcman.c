/* See LICENSE file for copyright and license details. */
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>

#define eprintf(...) (fprintf(stderr, __VA_ARGS__), exit(1))
#define erealloc(P, N) ((tmp_ = realloc((P), (N))) ? tmp_ : (eprintf("realloc: out of memory\n"), NULL))
#define ecalloc(N, M) ((tmp_ = calloc((N), (M))) ? tmp_ : (eprintf("calloc: out of memory\n"), NULL))
static void *tmp_;

#define OPAQUE (~(uint32_t)0)

#define COPY_AREA(DEST, SRC)\
	((DEST)->x = (SRC)->x,\
	 (DEST)->y = (SRC)->y,\
	 (DEST)->width = (SRC)->width,\
	 (DEST)->height = (SRC)->height)

struct window {
	struct window *next;
	Window id;
	Pixmap pixmap;
	XWindowAttributes a;
	int solid;
	int damaged;
	Damage damage;
	Picture picture;
	Picture alpha_picture;
	XserverRegion border_size;
	XserverRegion extents;
	uint32_t opacity;

	/* for drawing translucent windows */
	XserverRegion border_clip;
	struct window *prev_trans;
};

static unsigned long int *ignores = NULL;
static size_t n_ignores = 0, size_ignores = 0;
static Display *dpy;
static int screen;
static Window root;
static XRenderPictFormat *visual_format;
static int root_height, root_width;
static int damage_error, xfixes_error, render_error;
static int damage_event;
static int composite_opcode;
static Atom opacity_atom, background_atom1, background_atom2, pixmap_atom;
static Atom *background_atoms[] = {&background_atom1, &background_atom2};
static XRenderColor alpha_colour = {.red = 0, .green = 0, .blue = 0};

static struct window *window_list;
static Picture root_picture;
static Picture root_buffer;
static Picture root_tile;
static XserverRegion all_damage;
static int clip_changed;

static void
usage(const char *program)
{
	fprintf(stderr, "usage: %s\n", program);
	exit(1);
}

static Picture
solid_picture(double a)
{
	Pixmap pixmap;
	Picture picture;
	XRenderPictureAttributes pa;
	pixmap = XCreatePixmap(dpy, root, 1, 1, 8);
	if (!pixmap)
		return None;
	pa.repeat = 1;
	picture = XRenderCreatePicture(dpy, pixmap, XRenderFindStandardFormat(dpy, PictStandardA8), CPRepeat, &pa);
	if (picture) {
		alpha_colour.alpha = a * 0xFFFF;
		XRenderFillRectangle(dpy, PictOpSrc, picture, &alpha_colour, 0, 0, 1, 1);
	}
	XFreePixmap(dpy, pixmap);
	return picture;
}

static void
discard_ignore(unsigned long int sequence)
{
	size_t i;
	for (i = 0; i < n_ignores && sequence > ignores[i]; i++);
	memmove(ignores, &ignores[i], (n_ignores -= i) * sizeof(*ignores));
}

static void
set_ignore(unsigned long int sequence)
{
	if (n_ignores == size_ignores)
		ignores = erealloc(ignores, (size_ignores += 64) * sizeof(*ignores));
	ignores[n_ignores++] = sequence;
}

static int
should_ignore(unsigned long int sequence)
{
	discard_ignore(sequence);
	return n_ignores && ignores[0] == sequence;
}

static struct window *
find_window(Window id)
{
	struct window *w;
	for (w = window_list; w; w = w->next)
		if (w->id == id)
			return w;
	return NULL;
}

static Picture
make_root_tile(void)
{
	Picture picture, pixmap = None;
	Atom actual_type;
	int i, actual_format, fill;
	unsigned long int nitems, bytes_after;
	unsigned char *prop;
	XRenderPictureAttributes pa = {.repeat = 1};

	for (i = 0; i < 2; i++) {
		if (!XGetWindowProperty(dpy, root, *(background_atoms[i]), 0, 4, 0, AnyPropertyType,
				        &actual_type, &actual_format, &nitems, &bytes_after, &prop) &&
		    actual_type == pixmap_atom && actual_format == 32 && nitems == 1) {
			memcpy(&pixmap, prop, 4);
			XFree(prop);
			fill = 0;
			break;
		}
	}
	if (!pixmap) {
		pixmap = XCreatePixmap(dpy, root, 1, 1, DefaultDepth(dpy, screen));
		fill = 1;
	}
	picture = XRenderCreatePicture(dpy, pixmap, visual_format, CPRepeat, &pa);
	if (fill) {
		alpha_colour.alpha = 0xFFFF;
		XRenderFillRectangle(dpy, PictOpSrc, picture, &alpha_colour, 0, 0, 1, 1);
	}
	return picture;
}

static XserverRegion
window_extents(struct window *w)
{
	XRectangle r;
	COPY_AREA(&r, &w->a);
	r.width  += w->a.border_width * 2;
	r.height += w->a.border_width * 2;
	return XFixesCreateRegion(dpy, &r, 1);
}

static XserverRegion
border_size(struct window *w)
{
	XserverRegion border;
	set_ignore(NextRequest(dpy));
	border = XFixesCreateRegionFromWindow(dpy, w->id, WindowRegionBounding);
	set_ignore(NextRequest(dpy));
	XFixesTranslateRegion(dpy, border, w->a.x + w->a.border_width, w->a.y + w->a.border_width);
	return border;
}

static void
paint_all(XserverRegion region)
{
	struct window *w, *t = NULL;
	XRectangle r;
	int x, y, wid, hei;
	Pixmap rootPixmap;
	XRenderPictureAttributes pa;
	XRenderPictFormat *format;
	Drawable draw;

	if (!region) {
		r.x = r.y = 0;
		r.width = root_width;
		r.height = root_height;
		region = XFixesCreateRegion(dpy, &r, 1);
	}
	if (!root_buffer) {
		rootPixmap = XCreatePixmap(dpy, root, root_width, root_height, DefaultDepth(dpy, screen));
		root_buffer = XRenderCreatePicture(dpy, rootPixmap, visual_format, 0, NULL);
		XFreePixmap(dpy, rootPixmap);
	}
	XFixesSetPictureClipRegion(dpy, root_picture, 0, 0, region);
	for (w = window_list; w; w = w->next) {
		if (!w->damaged)
			continue;
		if (w->a.x + w->a.width < 1 || w->a.y + w->a.height < 1 || w->a.x >= root_width || w->a.y >= root_height)
			continue;
		if (!w->picture) {
			draw = w->id;
			if (w->pixmap)
				draw = w->pixmap;
			else
				w->pixmap = XCompositeNameWindowPixmap(dpy, w->id);
			format = XRenderFindVisualFormat(dpy, w->a.visual);
			pa.subwindow_mode = IncludeInferiors;
			w->picture = XRenderCreatePicture(dpy, draw, format, CPSubwindowMode, &pa);
		}
		if (clip_changed) {
			if (w->border_size) {
				set_ignore(NextRequest(dpy));
				XFixesDestroyRegion(dpy, w->border_size);
				w->border_size = None;
			}
			if (w->extents) {
				XFixesDestroyRegion(dpy, w->extents);
				w->extents = None;
			}
			if (w->border_clip) {
				XFixesDestroyRegion(dpy, w->border_clip);
				w->border_clip = None;
			}
		}
		if (!w->border_size)
			w->border_size = border_size(w);
		if (!w->extents)
			w->extents = window_extents(w);
		if (w->solid) {
			x = w->a.x;
			y = w->a.y;
			wid = w->a.width + w->a.border_width * 2;
			hei = w->a.height + w->a.border_width * 2;
			XFixesSetPictureClipRegion(dpy, root_buffer, 0, 0, region);
			set_ignore(NextRequest(dpy));
			XFixesSubtractRegion(dpy, region, region, w->border_size);
			set_ignore(NextRequest(dpy));
			XRenderComposite(dpy, PictOpSrc, w->picture, None, root_buffer, 0, 0, 0, 0, x, y, wid, hei);
		}
		if (!w->border_clip) {
			w->border_clip = XFixesCreateRegion(dpy, NULL, 0);
			XFixesCopyRegion(dpy, w->border_clip, region);
			XFixesIntersectRegion(dpy, w->border_clip, w->border_clip, w->border_size);
		}
		w->prev_trans = t;
		t = w;
	}
	XFixesSetPictureClipRegion(dpy, root_buffer, 0, 0, region);
	if (!root_tile)
		root_tile = make_root_tile();
	XRenderComposite(dpy, PictOpSrc, root_tile, None, root_buffer, 0, 0, 0, 0, 0, 0, root_width, root_height);
	for (w = t; w; w = w->prev_trans) {
		XFixesSetPictureClipRegion(dpy, root_buffer, 0, 0, w->border_clip);
		if (w->opacity != OPAQUE && !w->alpha_picture)
			w->alpha_picture = solid_picture((double)w->opacity / OPAQUE);
		if (!w->solid) {
			x = w->a.x;
			y = w->a.y;
			wid = w->a.width + w->a.border_width * 2;
			hei = w->a.height + w->a.border_width * 2;
			set_ignore(NextRequest(dpy));
			XRenderComposite(dpy, PictOpOver, w->picture, w->alpha_picture, root_buffer, 0, 0, 0, 0, x, y, wid, hei);
		}
		XFixesDestroyRegion(dpy, w->border_clip);
		w->border_clip = None;
	}
	XFixesDestroyRegion(dpy, region);
	if (root_buffer != root_picture) {
		XFixesSetPictureClipRegion(dpy, root_buffer, 0, 0, None);
		XRenderComposite(dpy, PictOpSrc, root_buffer, None, root_picture, 0, 0, 0, 0, 0, 0, root_width, root_height);
	}
}

static void
add_damage(XserverRegion damage)
{
	if (all_damage) {
		XFixesUnionRegion(dpy, all_damage, all_damage, damage);
		XFixesDestroyRegion(dpy, damage);
	} else {
		all_damage = damage;
	}
}

static unsigned int
get_opacity_prop(struct window *w, unsigned int def)
{
	uint32_t i;
	Atom actual;
	int format;
	unsigned long int n, left;
	unsigned char *data;
	int err = XGetWindowProperty(dpy, w->id, opacity_atom, 0, 1, 0, XA_CARDINAL, &actual, &format, &n, &left, &data);
	if (err || !data)
		return def;
	i = *(uint32_t *)data;
	XFree(data);
	return i;
}

static void
determine_mode(struct window *w)
{
	XRenderPictFormat *format;
	XserverRegion damage;
	if (w->alpha_picture) {
		XRenderFreePicture(dpy, w->alpha_picture);
		w->alpha_picture = None;
	}
	w->opacity = get_opacity_prop(w, OPAQUE);
	w->solid = (w->opacity == OPAQUE &&
		    ((format = w->a.class == InputOnly ? NULL : XRenderFindVisualFormat(dpy, w->a.visual)),
		     (!format || format->type != PictTypeDirect || !format->direct.alphaMask)));
	if (w->extents) {
		damage = XFixesCreateRegion(dpy, NULL, 0);
		XFixesCopyRegion(dpy, damage, w->extents);
		add_damage(damage);
	}
}

static void
map_window(Window id)
{
	struct window *w = find_window(id);
	if (!w)
		return;
	w->a.map_state = IsViewable;
	XSelectInput(dpy, id, PropertyChangeMask); /* subscribe to transparency changes */
	determine_mode(w); /* This needs to be here since we don't get PropertyNotify when unmapped */
	w->damaged = 0;
}

static void
finish_unmap_window(struct window *w)
{
	w->damaged = 0;
	if (w->extents) {
		add_damage(w->extents); /* destroys region */
		w->extents = None;
	}
	if (w->pixmap) {
		XFreePixmap(dpy, w->pixmap);
		w->pixmap = None;
	}
	if (w->picture) {
		set_ignore(NextRequest(dpy));
		XRenderFreePicture(dpy, w->picture);
		w->picture = None;
	}

	/* don't care about properties anymore */
	set_ignore(NextRequest(dpy));
	XSelectInput(dpy, w->id, 0);

	if (w->border_size) {
		set_ignore(NextRequest(dpy));
		XFixesDestroyRegion(dpy, w->border_size);
		w->border_size = None;
	}
	if (w->border_clip) {
		XFixesDestroyRegion(dpy, w->border_clip);
		w->border_clip = None;
	}

	clip_changed = 1;
}

static void
unmap_window(Window id)
{
	struct window *w = find_window(id);
	if (w) {
		w->a.map_state = IsUnmapped;
		finish_unmap_window(w);
	}
}

static void
add_window(Window id)
{
	struct window *w = ecalloc(1, sizeof(struct window));
	w->id = id;
	set_ignore(NextRequest(dpy));
	if (!XGetWindowAttributes(dpy, id, &w->a)) {
		free(w);
		return;
	}
	if (w->a.class != InputOnly)
		w->damage = XDamageCreate(dpy, id, XDamageReportNonEmpty);
	w->opacity = OPAQUE;
	w->next = window_list;
	window_list = w;
	if (w->a.map_state == IsViewable)
		map_window(id);
}

static void
restack_window(struct window *w, Window new_above)
{
	struct window **prev;
	Window old_above = w->next ? w->next->id : None;

	if (old_above != new_above) {
		/* unhook */
		for (prev = &window_list; *prev; prev = &(*prev)->next)
			if ((*prev) == w)
				break;
		*prev = w->next;

		/* rehook */
		for (prev = &window_list; *prev; prev = &(*prev)->next)
			if ((*prev)->id == new_above)
				break;
		w->next = *prev;
		*prev = w;
	}
}

static void
configure_window(XConfigureEvent *ce)
{
	struct window *w = find_window(ce->window);
	XserverRegion extents, damage = None;

	if (!w) {
		if (ce->window == root) {
			if (root_buffer) {
				XRenderFreePicture(dpy, root_buffer);
				root_buffer = None;
			}
			root_width = ce->width;
			root_height = ce->height;
		}
		return;
	}

	damage = XFixesCreateRegion(dpy, NULL, 0);
	if (w->extents != None)
		XFixesCopyRegion(dpy, damage, w->extents);
	if (w->a.width != ce->width || w->a.height != ce->height) {
		if (w->pixmap) {
			XFreePixmap(dpy, w->pixmap);
			w->pixmap = None;
			if (w->picture) {
				XRenderFreePicture(dpy, w->picture);
				w->picture = None;
			}
		}
	}
	COPY_AREA(&w->a, ce);
	w->a.border_width = ce->border_width;
	w->a.override_redirect = ce->override_redirect;
	restack_window(w, ce->above);
	if (damage) {
		extents = window_extents(w);
		XFixesUnionRegion(dpy, damage, damage, extents);
		XFixesDestroyRegion(dpy, extents);
		add_damage(damage);
	}

	clip_changed = 1;
}

static void
circulate_window(XCirculateEvent *ce)
{
	struct window *w = find_window(ce->window);
	if (w) {
		restack_window(w, ce->place == PlaceOnTop ? window_list->id : None);
		clip_changed = 1;
	}
}

static void
destroy_window(Window id, int gone)
{
	struct window **prev, *w;
	for (prev = &window_list; (w = *prev); prev = &w->next) {
		if (w->id == id) {
			if (gone)
				finish_unmap_window(w);
			*prev = w->next;
			if (w->picture) {
				set_ignore(NextRequest(dpy));
				XRenderFreePicture(dpy, w->picture);
				w->picture = None;
			}
			if (w->alpha_picture) {
				XRenderFreePicture(dpy, w->alpha_picture);
				w->alpha_picture = None;
			}
			if (w->damage != None) {
				set_ignore(NextRequest(dpy));
				XDamageDestroy(dpy, w->damage);
				w->damage = None;
			}
			free(w);
			break;
		}
	}
}

static void
damage_window(XDamageNotifyEvent *de)
{
	XserverRegion parts;
	struct window *w = find_window(de->drawable);
	if (!w)
		return;
	if (!w->damaged) {
		parts = window_extents(w);
		set_ignore(NextRequest(dpy));
		XDamageSubtract(dpy, w->damage, None, None);
	} else {
		parts = XFixesCreateRegion(dpy, NULL, 0);
		set_ignore(NextRequest(dpy));
		XDamageSubtract(dpy, w->damage, None, parts);
		XFixesTranslateRegion(dpy, parts, w->a.x + w->a.border_width, w->a.y + w->a.border_width);
	}
	add_damage(parts);
	w->damaged = 1;
}

static int
error(Display *display, XErrorEvent *ev)
{
	const char *name = NULL;
	static char buffer[256];

	if (should_ignore(ev->serial))
		return 0;

	if (ev->request_code == composite_opcode && ev->minor_code == X_CompositeRedirectSubwindows)
		eprintf("another composite manager is already running\n");

	if (ev->error_code - xfixes_error == BadRegion) {
		name = "BadRegion";
	} else if (ev->error_code - damage_error == BadDamage) {
		name = "BadDamage";
	} else {
		switch (ev->error_code - render_error) {
		case BadPictFormat: name = "BadPictFormat"; break;
		case BadPicture:    name = "BadPicture";    break;
		case BadPictOp:     name = "BadPictOp";     break;
		case BadGlyphSet:   name = "BadGlyphSet";   break;
		case BadGlyph:      name = "BadGlyph";      break;
		default:
			buffer[0] = '\0';
			XGetErrorText(display, ev->error_code, buffer, sizeof(buffer));
			name = buffer;
			break;
		}
	}

	fprintf(stderr, "error %i: %s request %i minor %i serial %lu\n",
		ev->error_code, *name ? name : "unknown",
		ev->request_code, ev->minor_code, ev->serial);

	return 0;
}

static void
register_composite_manager(void)
{
	static char net_wm_cm[sizeof("_NET_WM_CM_S") + 3 * sizeof(screen)];
	Window w;
	Atom a, winNameAtom;
	XTextProperty tp;
	char **strs;
	int count;
	sprintf(net_wm_cm, "_NET_WM_CM_S%i", screen);
	a = XInternAtom(dpy, net_wm_cm, 0);
	w = XGetSelectionOwner(dpy, a);
	if (w) {
		winNameAtom = XInternAtom(dpy, "_NET_WM_NAME", 0);
		if (!XGetTextProperty(dpy, w, &tp, winNameAtom) &&
		    !XGetTextProperty(dpy, w, &tp, XA_WM_NAME)) {
			eprintf("another composite manager is already running (0x%lx)\n", (unsigned long int)w);
		}
		if (!XmbTextPropertyToTextList(dpy, &tp, &strs, &count)) {
			fprintf(stderr, "another composite manager is already running (%s)\n", strs[0]);
			XFreeStringList(strs);
		}
		XFree(tp.value);
		exit(1);
	}
	w = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, None, None);
	Xutf8SetWMProperties(dpy, w, "xcman", "xcman", NULL, 0, NULL, NULL, NULL);
	XSetSelectionOwner(dpy, a, w, 0);
}

int
main(int argc, char **argv)
{
	XEvent ev;
	Window root_return, parent_return, *children;
	XRenderPictureAttributes pa;
	XRectangle *expose_rects = NULL;
	size_t n_expose = 0, size_expose = 0;
	unsigned int n;
	int more, composite_major, composite_minor;
	struct window *w;

	if (argc > 1)
		usage(argv[0]);

	dpy = XOpenDisplay(NULL);
	if (!dpy)
		eprintf("cannot open display\n");
	XSetErrorHandler(error);
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);

	if (!XRenderQueryExtension(dpy, &(int){0}, &render_error))
		eprintf("no render extension\n");
	if (!XQueryExtension(dpy, COMPOSITE_NAME, &composite_opcode, &(int){0}, &(int){0}))
		eprintf("no composite extension\n");
	XCompositeQueryVersion(dpy, &composite_major, &composite_minor);
	if (!composite_major && composite_minor < 2)
		eprintf("no composite extension version is too old\n");
	if (!XDamageQueryExtension(dpy, &damage_event, &damage_error))
		eprintf("no damage extension\n");
	if (!XFixesQueryExtension(dpy, &(int){0}, &xfixes_error))
		eprintf("no XFixes extension\n");

	register_composite_manager();
	opacity_atom      = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", 0);
	background_atom1  = XInternAtom(dpy, "_XROOTPMAP_ID", 0);
	background_atom2  = XInternAtom(dpy, "_XSETROOT_ID", 0);
	pixmap_atom       = XInternAtom(dpy, "PIXMAP", 0);
	pa.subwindow_mode = IncludeInferiors;
	root_width        = DisplayWidth(dpy, screen);
	root_height       = DisplayHeight(dpy, screen);
	visual_format     = XRenderFindVisualFormat(dpy, DefaultVisual(dpy, screen));
	root_picture      = XRenderCreatePicture(dpy, root, visual_format, CPSubwindowMode, &pa);
	all_damage        = None;
	clip_changed      = 1;
	XGrabServer(dpy);
	XCompositeRedirectSubwindows(dpy, root, CompositeRedirectManual);
	XSelectInput(dpy, root, SubstructureNotifyMask | ExposureMask | StructureNotifyMask | PropertyChangeMask);
	XQueryTree(dpy, root, &root_return, &parent_return, &children, &n);
	while (n--)
		add_window(children[n]);
	XFree(children);
	XUngrabServer(dpy);
	paint_all(None);

	for (;;) {
		XNextEvent(dpy, &ev);
		if ((ev.type & 0x7F) != KeymapNotify)
			discard_ignore(ev.xany.serial);
		switch (ev.type) {
		case CreateNotify:    add_window(ev.xcreatewindow.window);         break;
		case ConfigureNotify: configure_window(&ev.xconfigure);            break;
		case DestroyNotify:   destroy_window(ev.xdestroywindow.window, 1); break;
		case CirculateNotify: circulate_window(&ev.xcirculate);            break;
		case MapNotify:       map_window(ev.xmap.window);                  break;
		case UnmapNotify:     unmap_window(ev.xunmap.window);              break;
		case ReparentNotify:
			if (ev.xreparent.parent == root)
				add_window(ev.xreparent.window);
			else
				destroy_window(ev.xreparent.window, 0);
			break;
		case Expose:
			if (ev.xexpose.window == root) {
				more = ev.xexpose.count + 1;
				if (n_expose == size_expose)
					expose_rects = erealloc(expose_rects, (size_expose += more) * sizeof(XRectangle));
				COPY_AREA(&expose_rects[n_expose], &ev.xexpose);
				n_expose++;
				if (!ev.xexpose.count) {
					add_damage(XFixesCreateRegion(dpy, expose_rects, n_expose));
					n_expose = 0;
				}
			}
			break;
		case PropertyNotify:
			if (ev.xproperty.atom == opacity_atom) {
				if ((w = find_window(ev.xproperty.window)))
					determine_mode(w);
			} else if (root_tile && (ev.xproperty.atom == background_atom1 ||
						 ev.xproperty.atom == background_atom2)) {
				XClearArea(dpy, root, 0, 0, 0, 0, 1);
				XRenderFreePicture(dpy, root_tile);
				root_tile = None;
			}
			break;
		default:
			if (ev.type == damage_event + XDamageNotify)
				damage_window((XDamageNotifyEvent *)&ev);
			break;
		}
		if (!QLength(dpy) && all_damage) {
			paint_all(all_damage);
			XSync(dpy, 0);
			all_damage = None;
			clip_changed = 0;
		}
	}
}
