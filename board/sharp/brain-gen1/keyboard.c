// SPDX-License-Identifier: GPL-2.0+
/*
 * Sharp Brain Gen1 keyboard matrix polling driver.
 *
 * The PW-GC610 8x8 matrix mapping comes from the following work-in-progress
 * page (retrieved 2026-08-27):
 * https://scrapbox.io/brain-hackers/PW-GC610
 *
 * The modifier assignments follow the Brainux Wiki Gxxxx/Axxxx table
 * (retrieved 2026-08-27):
 * https://wiki.brainux.org/beginners/get-started/
 * Copyright Brainux Wiki contributors, licensed under CC BY-SA 4.0.
 *
 * The Port A/B register layout follows the TMPA910CRA hardware manual,
 * sections 3.9.1 and 3.9.2 (pages 126-134).  Register names and addresses
 * were cross-checked against the MuCross TMPA9xx U-Boot source release:
 * https://mucross.com/downloads/tx09-linux/Release-20110309/src/
 *
 * The following copyright notice is retained from the MuCross TMPA9xx
 * register header used for that cross-check:
 *
 * (C) Copyright 2009,2010
 * Kernel Concepts <www.kernelconcepts.de>
 * Florian Boor (florian.boor@kernelconcepts.de)
 *
 */

#include <common.h>
#include <stdio_dev.h>
#include <watchdog.h>
#include <asm/io.h>
#include <linux/delay.h>

#define TMPA910_GPIOADATA	0xf08003fc
#define TMPA910_GPIOBDATA	0xf08013fc
#define TMPA910_GPIOBODE	0xf0801c00

#define BRAIN_KBD_ROWS		8
#define BRAIN_KBD_COLS		8
#define BRAIN_KBD_NO_KEY	(-1)
#define BRAIN_KBD_DEBOUNCE_MS	20
#define BRAIN_KBD_QUEUE_SIZE	8

#define BRAIN_KBD_SHIFT		(1ULL << (5 * BRAIN_KBD_COLS + 0))
#define BRAIN_KBD_CTRL		(1ULL << (5 * BRAIN_KBD_COLS + 1))
#define BRAIN_KBD_ALT		(1ULL << (6 * BRAIN_KBD_COLS + 1))
#define BRAIN_KBD_SYMBOL		(1ULL << (7 * BRAIN_KBD_COLS + 1))
#define BRAIN_KBD_MODIFIERS	(BRAIN_KBD_SHIFT | BRAIN_KBD_CTRL | \
				 BRAIN_KBD_ALT | BRAIN_KBD_SYMBOL)

/*
 * Printable keys use lowercase ASCII.  Cursor keys use the ANSI sequences
 * understood by U-Boot's command-line editor.  Home and Clear map to the
 * editor's Ctrl-A and Ctrl-U commands respectively.
 */
static const char *const brain_keymap[BRAIN_KBD_ROWS][BRAIN_KBD_COLS] = {
	[0] = { NULL, NULL, NULL, NULL, NULL, NULL, "\x01", NULL },
	[2] = { "q", "w", "e", "r", "t", "y", "u", "i" },
	[3] = { "a", "s", "d", "f", "g", "h", "o", "p" },
	[4] = { "z", "x", "c", "v", "b", "j", "k", "l" },
	[5] = { NULL, NULL, NULL, NULL, NULL, "n", "m", "-" },
	[6] = { NULL, NULL, NULL, NULL, "\x1b[D", "\x1b[A",
		"\x1b[B", "\x1b[C" },
	[7] = { " ", NULL, NULL, NULL, NULL, "\r", "\b", "\x15" },
};

static unsigned char brain_kbd_queue[BRAIN_KBD_QUEUE_SIZE];
static unsigned int brain_kbd_queue_head;
static unsigned int brain_kbd_queue_tail;
static u64 brain_kbd_candidate;
static u64 brain_kbd_stable;
static int brain_kbd_reported_key = BRAIN_KBD_NO_KEY;
static ulong brain_kbd_candidate_since;

static bool brain_kbd_queue_empty(void)
{
	return brain_kbd_queue_head == brain_kbd_queue_tail;
}

static void brain_kbd_queue_put(unsigned char ch)
{
	unsigned int next = (brain_kbd_queue_head + 1) % BRAIN_KBD_QUEUE_SIZE;

	if (next == brain_kbd_queue_tail)
		return;

	brain_kbd_queue[brain_kbd_queue_head] = ch;
	brain_kbd_queue_head = next;
}

static void brain_kbd_queue_sequence(const char *sequence)
{
	while (sequence && *sequence)
		brain_kbd_queue_put(*sequence++);
}

static void brain_kbd_queue_key(const char *sequence, u64 modifiers)
{
	unsigned char ch;

	if (!sequence)
		return;
	/* No sourced PW-GC610 symbol layer is available yet. */
	if (modifiers & BRAIN_KBD_SYMBOL)
		return;

	/* Alt prefixes both printable keys and command-line escape sequences. */
	if (modifiers & BRAIN_KBD_ALT)
		brain_kbd_queue_put('\x1b');

	if (sequence[0] && !sequence[1]) {
		ch = sequence[0];
		if ((modifiers & BRAIN_KBD_SHIFT) && ch >= 'a' && ch <= 'z')
			ch -= 'a' - 'A';
		if ((modifiers & BRAIN_KBD_CTRL) &&
		    ((ch >= '@' && ch <= '_') || (ch >= 'a' && ch <= 'z')))
			ch &= 0x1f;
		brain_kbd_queue_put(ch);
		return;
	}

	brain_kbd_queue_sequence(sequence);
}

static u64 brain_kbd_scan(void)
{
	int col;
	u64 keys = 0;

	for (col = 0; col < BRAIN_KBD_COLS; col++) {
		u32 rows;
		int row;

		/* Open-drain outputs are active low; release every other KO. */
		writel(0xff & ~BIT(col), (void *)TMPA910_GPIOBDATA);
		udelay(5);
		rows = ~readl((void *)TMPA910_GPIOADATA) & 0xff;

		for (row = 0; row < BRAIN_KBD_ROWS; row++) {
			if (rows & BIT(row))
				keys |= 1ULL << (row * BRAIN_KBD_COLS + col);
		}
	}

	/* Leave all columns released between scans. */
	writel(0xff, (void *)TMPA910_GPIOBDATA);

	return keys;
}

static void brain_kbd_poll(void)
{
	u64 raw = brain_kbd_scan();
	u64 normal;
	u64 modifiers;
	int key = BRAIN_KBD_NO_KEY;
	int bit;

	if (raw != brain_kbd_candidate) {
		brain_kbd_candidate = raw;
		brain_kbd_candidate_since = get_timer(0);
		return;
	}

	if (raw == brain_kbd_stable ||
	    get_timer(brain_kbd_candidate_since) < BRAIN_KBD_DEBOUNCE_MS)
		return;

	brain_kbd_stable = raw;
	modifiers = raw & BRAIN_KBD_MODIFIERS;
	normal = raw & ~BRAIN_KBD_MODIFIERS;

	/* Accept one ordinary key plus modifiers; reject ambiguous chords. */
	for (bit = 0; bit < BRAIN_KBD_ROWS * BRAIN_KBD_COLS; bit++) {
		if (!(normal & (1ULL << bit)))
			continue;
		if (key != BRAIN_KBD_NO_KEY) {
			key = BRAIN_KBD_NO_KEY;
			break;
		}
		key = bit;
	}

	if (key == brain_kbd_reported_key)
		return;

	brain_kbd_reported_key = key;
	if (key != BRAIN_KBD_NO_KEY)
		brain_kbd_queue_key(brain_keymap[key / BRAIN_KBD_COLS]
					 [key % BRAIN_KBD_COLS], modifiers);
}

static int brain_kbd_tstc(struct stdio_dev *dev)
{
	brain_kbd_poll();
	return !brain_kbd_queue_empty();
}

static int brain_kbd_getc(struct stdio_dev *dev)
{
	unsigned char ch;

	while (!brain_kbd_tstc(dev))
		WATCHDOG_RESET();

	ch = brain_kbd_queue[brain_kbd_queue_tail];
	brain_kbd_queue_tail = (brain_kbd_queue_tail + 1) %
				 BRAIN_KBD_QUEUE_SIZE;
	return ch;
}

static int brain_kbd_start(struct stdio_dev *dev)
{
	/* Release every KO before changing the outputs to open-drain mode. */
	writel(0xff, (void *)TMPA910_GPIOBDATA);
	writel(0xff, (void *)TMPA910_GPIOBODE);

	brain_kbd_queue_head = 0;
	brain_kbd_queue_tail = 0;
	brain_kbd_candidate = 0;
	brain_kbd_stable = 0;
	brain_kbd_reported_key = BRAIN_KBD_NO_KEY;
	brain_kbd_candidate_since = get_timer(0);

	return 0;
}

static int brain_kbd_stop(struct stdio_dev *dev)
{
	/* Never leave a column asserted when the console is detached. */
	writel(0xff, (void *)TMPA910_GPIOBDATA);
	return 0;
}

int drv_keyboard_init(void)
{
	struct stdio_dev dev = {
		.name = "brain-kbd",
		.flags = DEV_FLAGS_INPUT,
		.start = brain_kbd_start,
		.stop = brain_kbd_stop,
		.getc = brain_kbd_getc,
		.tstc = brain_kbd_tstc,
	};

	return stdio_register(&dev);
}
