/*
 * iob.c
 *
 * simple CADR i/o board simulation
 * support for mouse, keyboard, clock
 *
 * $Id$
 */

#include <stdio.h>
#include <string.h>
#include "ucode.h"
#include <signal.h>

#ifdef linux
#include <sys/time.h>
#endif

#ifdef _WIN32
#include <SDL/SDL_keysym.h>
#else
#include "SDL/SDL_keysym.h"
#endif

int iob_key_scan;
int iob_kbd_csr;

int mouse_x, mouse_y;
int mouse_head, mouse_middle, mouse_tail;
int mouse_rawx, mouse_rawy;

extern int u_pc;

void tv_post_60hz_interrupt(void);
void chaos_xmit_pkt(void);

/*
 CADR i/o board

 interrupt vectors:
 260 kdb/mouse
 264 serial
 270 chaos int
 274 clock
 400 ether xmit done
 404 ether rcv done
 410 ether collision

764100
0	0 read kbd
2	1 read kbd
4	2 read mouse y (12 bits)
6	3 read mouse x (12 bits)
10	4 click audio
12	5 kbd/mouse csr

csr - write
0 remote mouse enable
1 mouse int enable
2 kbd int enable
3 clock int enable

csr - read
0 remote mouse eable
1 mouse int enable
2 kbd int enable
3 clock int enable
4 mouse ready
5 kbd ready
6 clock ready
7 ser int enable

keyboard
; 		5-0	keycode
; 		7-6	shift
; 		9-8	top
; 		11-10	control
; 		13-12	meta
; 		14	shift-lock
; 		15	unused

*/

/* keycodes produced by LM keyboard */
#define LM_K_BREAK	0201
#define LM_K_CLEAR	0202
#define LM_K_CALL	0203
#define LM_K_ESC	0204
#define LM_K_BACK	0205
#define LM_K_HELP	0206
#define LM_K_RUBOUT	0207
#define LM_K_CR		0215

#define LM_K_NETWORK 0
#define LM_K_SYSTEM 0
#define LM_K_ABORT 0
#define LM_K_END 0

/* ****
;KEYBOARD TRANSLATE TABLE IS A 3 X 64 ARRAY.
;3 ENTRIES FOR EACH OF 100 KEYS.  FIRST IS VANILLA, SECOND SHIFT, THIRD TOP.
;THE FUNCTION KBD-INITIALIZE IS ONLY CALLED ONCE, IN ORDER TO SET UP THIS ARRAY.
**** */
unsigned char kb_old_table[64][3] = {
	/* none,shift,top */
	0201,	0201,	LM_K_NETWORK,	//BREAK,BREAK,NETWORK
	0204,	0204,	LM_K_SYSTEM,	//ESC,ESC,SYSTEM
	'1',	'!',	'!',
	'2',	'"',	'"',
	'3',	'#',	'#',
	'4',	'$',	'$',
	'5',	'%',	'%',
	'6',	'&',	'&',
	'7',	'\'',	'\'',
	'8',	'(',	'(',
	'9',	')',	')',
	'0',	'_',	'_',
	'-',	'=',	'=',
	'@',	'`',	'`',
	'^',	'~',	'~',
	0210,	0210,	0210,	//BS,BS,BS
	0203,	0203, 	LM_K_ABORT,	//CALL,CALL,ABORT
	0202,	0202,	0202,	//CLEAR,CLEAR,CLEAR
	0211,	0211,	0211,	//TAB,TAB,TAB
	'',	'',	'',
	'q',	'Q',	'',
	'w',	'W',	'',
	'e',	'E',	'',
	'r',	'R',	'',
	't',	'T',	'',
	'y',	'Y',	'',
	'u',	'U',	'',
	'i',	'I',	'',
	'o',	'O',	'',
	'p',	'P',	'',
	'[',	'{',	'{',
	']',	'}',	'}',
	'\\',	'|',	'|',
	'/',	'',	'',
	'',	0215,	0,	//^L,cr,^@
	0215,	0211,	0211,
	0214,	0214,	0214,	//FORM,FORM,FORM
	0213,	0213,	0213,	//VT,VT,VT
	0207,	0207,	0207,	//RUBOUT,RUBOUT,RUBOUT
	'a',	'A',	'',
	's',	'S',	'',
	'd',	'D',	'',
	'f',	'F',	'',
	'g',	'G',	'\032',
	'h',	'H',	0206,	//h,H,HELP
	'j',	'J',	'',
	'k',	'K',	'',
	'l',	'L',	'',
	';',	'+',	'+',
	':',	'*',	'*',
	0215,	0215,	LM_K_END,	//CR,CR,END
	0212,	0212,	0212,	//LINE,LINE,LINE
	0205,	0205,	0205,	//BACK-NEXT,BACK-NEXT,BACK-NEXT
	'z',	'Z',	'',
	'x',	'X',	'',
	'c',	'C',	'',
	'v',	'V',	'',
	'b',	'B',	'',
	'n',	'N',	'',
	'm',	'M',	'',
	',',	'<',	'<',
	'.',	'>',	'>',
	'/',	'?',	'?',
	' ',	' ',	' '
};

unsigned short kb_sdl_to_scancode[256][4];

/*
keys we need to map

meta <- alt

top
 network
 system
 abort
 help
 end

call
clear

*/

/*
764100
0 read kbd
1 read kbd
2 read mouse y (12 bits)
3 read mouse x (12 bits)
4 click audio
5 kbd/mouse csr

csr - write
0 remote mouse enable
1 mouse int enable
2 kbd int enable
3 clock int enable

csr - read
0 remote mouse eable
1 mouse int enable
2 kbd int enable
3 clock int enable
4 mouse ready
5 kbd ready
6 clock ready
7 ser int enable
*/

#define US_CLOCK_IS_WALL_CLOCK
#ifdef linux
#define USE_SIGVTARLM_FOR_60HZ
#endif

#ifdef _WIN32
#define USE_US_CLOCK_FOR_60HZ
#endif

unsigned long
get_us_clock()
{
	unsigned long v;
#ifdef US_CLOCK_IS_WALL_CLOCK
	static unsigned long last_hz60;
	static struct timeval tv;
	struct timeval tv2;
	unsigned long ds, du, hz60;

	if (tv.tv_sec == 0) {
		gettimeofday(&tv, 0);
		v = 0;
		last_hz60 = 0;
	} else {
		unsigned int newsec;
		gettimeofday(&tv2, 0);

		if (tv2.tv_usec < tv.tv_usec) {
			tv2.tv_sec--;
			tv2.tv_usec += 1000*1000;
		}
		ds = tv2.tv_sec - tv.tv_sec;
		du = tv2.tv_usec - tv.tv_usec;

//		v = (ds * 100) + (du / 10000);
		v = (ds * 1000*1000) + du;
		if (0) printf("delta %lu\n", v);

#ifdef USE_US_CLOCK_FOR_60HZ
		hz60 = v / 16000;
		if (hz60 > last_hz60) {
			last_hz60 = hz60;
			tv_post_60hz_interrupt();
		}
#endif
	}
#else
	/* assume 200ns cycle, we want 1us */
	extern long cycles;
	v = cycles * (1000/200);
#endif

	return v;
}

static unsigned long cv;

unsigned int
get_us_clock_low(void)
{
	cv = get_us_clock();
	return cv & 0xffff;
}

unsigned int
get_us_clock_high(void)
{
	return cv >> 16;
}

unsigned int get_60hz_clock(void)
{
	return 0;
}


void
iob_unibus_read(int offset, int *pv)
{
	/* default, for now */
	*pv = 0;

	switch (offset) {
	case 0100:
		*pv = iob_key_scan & 0177777;
		traceio("unibus: kbd low %011o\n", *pv);
		iob_kbd_csr &= ~(1 << 5);
		break;
	case 0102:
//		*pv = (iob_key_scan >> 16) & 0177777;
		*pv = 0177777;
		iob_kbd_csr &= ~(1 << 5);
		traceio("unibus: kbd high %011o\n", *pv);
		break;
	case 0104:
		traceio("unibus: mouse y\n");
		 *pv = (mouse_tail << 12) |
			 (mouse_middle << 13) |
			 (mouse_head << 14) |
			 (mouse_y & 07777); 

		 mouse_tail = 0;
		 mouse_middle = 0;
		 mouse_head = 0;

		 iob_kbd_csr &= ~(1 << 4);
		break;
	case 0106:
		traceio("unibus: mouse x\n");
		 *pv = (mouse_rawx << 12) | (mouse_rawy << 14) |
			 (mouse_x & 07777); 
		break;
	case 0110:
		traceio("unibus: beep\n");
		fprintf(stderr,"\a"); /* alert - beep */
		break;
	case 0112:
		*pv = iob_kbd_csr;
		traceio("unibus: kbd csr %011o\n", *pv);
		break;
	case 0120:
		traceio("unibus: usec clock low\n");
		*pv = get_us_clock_low();
		break;
	case 0122:
		traceio("unibus: usec clock high\n");
		*pv = get_us_clock_high();
		break;
	case 0124:
		printf("unibus: 60hz clock\n");
		*pv = get_60hz_clock();
		break;
	case 0140:
		traceio("unibus: chaos read\n");
		*pv = chaos_get_csr();
		break;
	case 0142:
		printf/*traceio*/("unibus: chaos read my-number\n");
		*pv = chaos_get_addr();
		break;
	case 0144:
		*pv = chaos_get_rcv_buffer();
		traceio("unibus: chaos read rcv buffer %06o\n", *pv);
		break;
	case 0146:
		*pv = chaos_get_bit_count();
		printf/*traceio*/("unibus: chaos read bit-count 0%o\n", *pv);
		break;
	case 0152:
		*pv = chaos_get_addr();
		printf/*traceio*/("unibus: chaos read xmt => %o\n", *pv);
		chaos_xmit_pkt();
		break;
	default:
		if (offset > 0140 && offset <= 0153)
			printf/*traceio*/("unibus: chaos read other %o\n",
					  offset);
		chaos_xmit_pkt();
		break;
	}
}

void
iob_unibus_write(int offset, int v)
{
	switch (offset) {
	case 0100:
		traceio("unibus: kbd low\n");
		break;
	case 0102:
		traceio("unibus: kbd high\n");
		break;
	case 0104:
		traceio("unibus: mouse y\n");
		break;
	case 0106:
		traceio("unibus: mouse x\n");
		break;
	case 0110:
		traceio("unibus: beep\n");
		break;
	case 0112:
		traceio("unibus: kbd csr\n");
		iob_kbd_csr = 
			(iob_kbd_csr & ~017) | (v & 017);
		break;
	case 0120:
		traceio("unibus: usec clock\n");
		break;
	case 0122:
		traceio("unibus: usec clock\n");
		break;
	case 0124:
		printf("unibus: START 60hz clock\n");
		break;
	case 0140:
		printf/*traceio*/("unibus: chaos write %011o, u_pc %011o ",
				  v, u_pc);
		show_label_closest(u_pc);
		printf("\n");

		chaos_set_csr(v);
		break;
	case 0142:
		traceio("unibus: chaos write-buffer write %011o, u_pc %011o\n",
			v, u_pc);
		chaos_put_xmit_buffer(v);
		break;
	default:
		if (offset > 0140 && offset <= 0152)
			printf/*traceio*/("unibus: chaos write other\n");
		break;
	}
}

int
iob_get_key_scan(void)
{
	return iob_key_scan;
}

/****
;FORMAT OF DATA IN 764100 (IF USING OLD KEYBOARD):
; 00077   0006	  ;KEY CODE
; 00300   0602    ;SHIFT LEFT,RIGHT
; 01400   1002    ;TOP LEFT,RIGHT
; 06000   1202    ;CONTROL LEFT,RIGHT
; 30000   1402    ;META LEFT,RIGHT
; 40000   1601    ;SHIFT LOCK
****/
void
iob_sdl_key_event(int code, int extra)
{
	int s, c;

	if (0) printf("iob_sdl_key_event(code=%x,extra=%x)\n", code, extra);

	if (code == 0 ||
	    code == SDLK_LSHIFT ||
	    code == SDLK_RSHIFT ||
	    code == SDLK_LCTRL ||
	    code == SDLK_RCTRL ||
	    code == SDLK_LALT ||
	    code == SDLK_RALT)
		return;

	/*
	  network
	  system
	  abort
	  clear
	  help
	*/
	switch(code) {
	case SDLK_F1:
		iob_key_scan = 0 | (3 << 8);	/* network */
		break;
	case SDLK_F2:
		iob_key_scan = 1 | (3 << 8);	/* system */
		break;
	case SDLK_F3:
		iob_key_scan = 16 | (3 << 8);	/* abort */
		break;
	case SDLK_F4:
		iob_key_scan = 17;		/* clear */
		break;
	case SDLK_F5:
		iob_key_scan = 44 | (3 << 8); /* help */
		break;
	case SDLK_F6:
	case SDLK_END:
		iob_key_scan = 50 | (3 << 8); /* end */
		break;
	case SDLK_F7:
		iob_key_scan = 16; /* call */
		break;
	case SDLK_F12:
	case SDLK_BREAK:
		iob_key_scan = 0; /* break */
		break;
	case SDLK_BACKSPACE:
		iob_key_scan = 15; /* backspace */
		break;
	case SDLK_RETURN:
		iob_key_scan = 50; /* CR */
		break;
	default:
#if 1
	  iob_key_scan = kb_sdl_to_scancode[code][(extra & (3 << 6)) ? 1 : 0];
#else
		s = 0; /* unshifted */
		if (extra & (3 << 6))
			s = 1; /* shift */
		if (extra & (3 << 10))
			s = 2; /* control */
		if (extra & (3 << 12))
			s = 3; /* meta */

		c = kb_sdl_to_scancode[code][s];
		if (c == 0) {
			printf("code %x, s %d, c %x\n", code, s, c);
		}

		iob_key_scan = c;
#endif
		break;
	}
#if 1   /* This is also for Fx, Bsp, Del */
	iob_key_scan |= extra & ~(3 << 6);	     /* keep C/M bits, Shift in scancode tbl */
	/* but if Control/Meta, add in Shift */
	if (extra & (17 << 10))
	  iob_key_scan |= extra;
# if 0
	printf("code 0%o, extra 0%o, scan 0%o\n", code, extra, iob_key_scan);
# endif
#endif

	iob_kbd_csr |= 1 << 5;
	assert_unibus_interrupt(0260);
}

void
iob_warm_boot_key()
{
	iob_sdl_key_event(SDLK_RETURN, 0);
}

void
iob_sdl_mouse_event(int x, int y, int dx, int dy, int buttons)
{
	iob_kbd_csr |= 1 << 4;
	assert_unibus_interrupt(0264);

#if 0
	printf("iob_sdl_mouse_event(dx=%x,dy=%x,buttons=%x) x %o, y %o\n",
	       dx, dy, buttons, mouse_x, mouse_y);
	mouse_x += dx;
	mouse_y += dy;
#endif

	if (0)
		printf("iob_sdl_mouse_event(x=%x,y=%x,buttons=%x)\n",
		       x, y, buttons);
//	mouse_x = (x*3)/2;
//	mouse_y = (y*3)/2;
	mouse_x = (x*5)/3;
	mouse_y = (y*5)/3;

	if (buttons & 1)
		mouse_head = 1;
	if (buttons & 2)
		mouse_middle = 1;
	if (buttons & 4)
		mouse_tail = 1;
}

int tv_csr;

int
tv_xbus_read(int offset, unsigned int *pv)
{
	if (0) printf("tv register read, offset %o -> %o\n", offset, tv_csr);
	*pv = tv_csr;
	return 0;
}

int
tv_xbus_write(int offset, unsigned int v)
{
	if (0) printf("tv register write, offset %o, v %o\n", offset, v);
	if ((tv_csr & 4) != (v & 4))
		sdl_set_bow_mode((v & 4)>>2);
	tv_csr = v;
	tv_csr &= ~(1 << 4);
	deassert_xbus_interrupt();
	return 0;
}

//xxx tv interrupt
// tv csr @ base, 1<<4 = interrupt flag
// writing back clears int
// 60hz

void
tv_post_60hz_interrupt(void)
{
	tv_csr |= 1 << 4;
	assert_xbus_interrupt();
}

void
iob_sdl_clock_event()
{
	iob_kbd_csr |= 1 << 6;
	assert_unibus_interrupt(0274);
}

void
sigalrm_handler(int arg)
{
	if (0) printf("sigalrm_handler()\n");
	tv_post_60hz_interrupt();
}

void
iob_poll(unsigned long cycles)
{
#ifndef USE_SIGVTARLM_FOR_60HZ
	/* assume 200ns cycle, we want 16ms */
	if ((cycles % ((16*1000*1000)/200)) == 0) {
		tv_post_60hz_interrupt();
	}
#endif
}

int
iob_init(void)
{
	int i, j;

	/* #### bzzt! handle multiple modifiers!! */
	memset((char *)kb_sdl_to_scancode, 0, sizeof(kb_sdl_to_scancode));

	/* Walk unshifted old kbd table */
	for (i = 0; i < 64; i++) {
		char k;
		k = kb_old_table[i][0];
		kb_sdl_to_scancode[k][0] = i;
	}

	/* Modify mapping to match present-day US kbd */
	kb_sdl_to_scancode['`'][0] = 015 | (3 << 6); /* ` = Shift @ = ` */
	kb_sdl_to_scancode['`'][1] = 016 | (3 << 6); /* Sh-` = Sh-^ = ~*/
	
	kb_sdl_to_scancode['\''][0] = 010 | (3<<6);  /* ' = Sh-7 = ' */
	kb_sdl_to_scancode['\''][1] = 3 | (3<<6);    /* Sh-' = Sh-2 = " */
	kb_sdl_to_scancode['='][0] = 014 | (3<<6);   /* = = Sh-- = = */
	kb_sdl_to_scancode['2'][1] = 015;	     /* Sh-2 = @ (unshifted) */

	kb_sdl_to_scancode['6'][1] = 016;	     /* Sh-6 = ^ (unshifted) */
	kb_sdl_to_scancode['7'][1] = 7 | (3<<6);     /* Sh-7 = Sh-6 = & */
	kb_sdl_to_scancode['8'][1] = 061 | (3<<6);   /* Sh-8 = Sh-: = * */
	kb_sdl_to_scancode['9'][1] = 011 | (3<<6);   /* Sh-9 = Sh-8 = ( */
	kb_sdl_to_scancode['0'][1] = 012 | (3<<6);   /* Sh-0 = Sh-9 = ) */
	kb_sdl_to_scancode['-'][1] = 013 | (3<<6);   /* Sh-- = Sh-0 = _ */
	kb_sdl_to_scancode['='][1] = 060 | (3<<6);   /* Sh-= = Sh-; = + */

	kb_sdl_to_scancode[';'][1] = 061;	     /* Sh-; = : (unshifted) */

	/* map "Delete" to rubout */
	kb_sdl_to_scancode[0x7f][0] = 046;	     /* Delete = Rubout */

	/* map tab to tab */
	kb_sdl_to_scancode[9][0] = 022;		     /* Tab = Tab */

	/* esc = esc */
	kb_sdl_to_scancode[0x1b][0] = 1;	     /* Esc = Esc (Terminal) */

	/* Add shifts */
	for (i = 0; i < 256; i++) {
		if (kb_sdl_to_scancode[i][1] == 0)
			kb_sdl_to_scancode[i][1] = kb_sdl_to_scancode[i][0] |
				(3 << 6);
	}

#if 0   /* Don't do this */
	/* control keys */
	for (i = 0; i < 64; i++) {
		char k;
		k = kb_old_table[i][0];
		kb_sdl_to_scancode[k][2] = i | (3 << 10);
	}

	/* meta keys */
	for (i = 0; i < 64; i++) {
		char k;
		k = kb_old_table[i][0];
		kb_sdl_to_scancode[k][3] = i | (3 << 12);
	}
#endif

#ifdef USE_SIGVTARLM_FOR_60HZ
	{
		struct itimerval itimer;
		int usecs;

		signal(SIGVTALRM, sigalrm_handler);

		usecs = 16000;

		itimer.it_interval.tv_sec = 0;
		itimer.it_interval.tv_usec = usecs;
		itimer.it_value.tv_sec = 0;
		itimer.it_value.tv_usec = usecs;
		setitimer(ITIMER_VIRTUAL, &itimer, 0);
	}
#endif

	return 0;
}
