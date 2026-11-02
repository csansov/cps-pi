/*
 * cps-pi - minimal C library shim for the NJEMU core on Circle (bare metal)
 *
 * Circle provides memcpy, memset, the str... functions, malloc; newlib provides the headers
 * and libm. Everything else the core references is implemented here or in
 * pi_file.c (files on the SD card).
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "cpspi.h"

/* ---- vsnprintf: flags - + space 0 #, width (*), precision, h hh l ll z t,
        conversions d i u x X o c s p % f F e g (fixed point) ------------- */

static void out_ch (char *buf, size_t size, size_t *pos, char c)
{
	if (*pos + 1 < size) buf[*pos] = c;
	(*pos)++;
}

static void out_padded (char *buf, size_t size, size_t *pos, const char *s, int len,
			int width, int left, char pad)
{
	int n = width > len ? width - len : 0;
	if (!left) while (n-- > 0) out_ch (buf, size, pos, pad);
	for (int i = 0; i < len; i++) out_ch (buf, size, pos, s[i]);
	if (left) while (n-- > 0) out_ch (buf, size, pos, ' ');
}

int vsnprintf (char *buf, size_t size, const char *fmt, va_list ap)
{
	size_t pos = 0;
	for (; *fmt; ++fmt)
	{
		if (*fmt != '%') { out_ch (buf, size, &pos, *fmt); continue; }
		++fmt;
		int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
		for (;; ++fmt)
		{
			if (*fmt == '-') left = 1;
			else if (*fmt == '0') zero = 1;
			else if (*fmt == '+') plus = 1;
			else if (*fmt == ' ') space = 1;
			else if (*fmt == '#') alt = 1;
			else break;
		}
		int width = 0;
		if (*fmt == '*') { width = va_arg (ap, int); if (width < 0) { left = 1; width = -width; } ++fmt; }
		else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
		int prec = -1;
		if (*fmt == '.')
		{
			++fmt;
			prec = 0;
			if (*fmt == '*') { prec = va_arg (ap, int); ++fmt; }
			else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
		}
		int lng = 0;
		for (;; ++fmt)
		{
			if (*fmt == 'l') lng++;
			else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') lng = 2;
			else if (*fmt == 'h') { }
			else break;
		}
		char tmp[64];
		switch (*fmt)
		{
		case 's': {
			const char *s = va_arg (ap, const char *);
			if (!s) s = "(null)";
			int len = (int) strlen (s);
			if (prec >= 0 && len > prec) len = prec;
			out_padded (buf, size, &pos, s, len, width, left, ' ');
			break; }
		case 'c':
			tmp[0] = (char) va_arg (ap, int);
			out_padded (buf, size, &pos, tmp, 1, width, left, ' ');
			break;
		case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
			unsigned long long v;
			int neg = 0;
			unsigned base = (*fmt == 'x' || *fmt == 'X' || *fmt == 'p') ? 16 : (*fmt == 'o' ? 8 : 10);
			if (*fmt == 'p') { v = (uintptr_t) va_arg (ap, void *); alt = 1; }
			else if (*fmt == 'd' || *fmt == 'i')
			{
				long long sv = lng >= 2 ? va_arg (ap, long long)
					     : lng == 1 ? va_arg (ap, long) : va_arg (ap, int);
				if (sv < 0) { neg = 1; v = (unsigned long long) (-(sv + 1)) + 1; }
				else v = (unsigned long long) sv;
			}
			else
			{
				v = lng >= 2 ? va_arg (ap, unsigned long long)
				  : lng == 1 ? va_arg (ap, unsigned long) : va_arg (ap, unsigned);
			}
			const char *dig = *fmt == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
			char rev[24];
			int n = 0;
			do { rev[n++] = dig[v % base]; v /= base; } while (v);
			while (prec > n && n < 23) rev[n++] = '0';
			int len = 0;
			if (neg) tmp[len++] = '-';
			else if (plus) tmp[len++] = '+';
			else if (space) tmp[len++] = ' ';
			if (alt && base == 16) { tmp[len++] = '0'; tmp[len++] = 'x'; }
			if (zero && !left && prec < 0)
			{
				/* zero padding goes between the sign and the digits */
				int n0 = width - len - n;
				if (n0 > 40) n0 = 40;
				while (n0-- > 0) tmp[len++] = '0';
			}
			while (n) tmp[len++] = rev[--n];
			out_padded (buf, size, &pos, tmp, len, width, left, ' ');
			break; }
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
			double d = va_arg (ap, double);
			if (prec < 0) prec = 6;
			if (prec > 9) prec = 9;
			int len = 0;
			if (d < 0) { tmp[len++] = '-'; d = -d; }
			else if (plus) tmp[len++] = '+';
			if (d > 9.0e18) { memcpy (tmp + len, "inf", 3); len += 3; }
			else
			{
				unsigned long long scale = 1;
				for (int i = 0; i < prec; i++) scale *= 10;
				unsigned long long ip = (unsigned long long) d;
				unsigned long long fp = (unsigned long long) ((d - (double) ip) * (double) scale + 0.5);
				if (fp >= scale) { ip++; fp -= scale; }
				char rev[24];
				int n = 0;
				do { rev[n++] = (char) ('0' + ip % 10); ip /= 10; } while (ip);
				while (n) tmp[len++] = rev[--n];
				if (prec > 0)
				{
					tmp[len++] = '.';
					for (int i = prec - 1; i >= 0; i--)
					{
						tmp[len + i] = (char) ('0' + fp % 10);
						fp /= 10;
					}
					len += prec;
				}
			}
			out_padded (buf, size, &pos, tmp, len, width, left, zero && !left ? '0' : ' ');
			break; }
		case '%':
			out_ch (buf, size, &pos, '%');
			break;
		case 0:
			--fmt;
			continue;
		default:
			out_ch (buf, size, &pos, '%');
			out_ch (buf, size, &pos, *fmt);
			break;
		}
	}
	if (size) buf[pos < size ? pos : size - 1] = 0;
	return (int) pos;
}

int snprintf (char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	int n = vsnprintf (buf, size, fmt, ap);
	va_end (ap);
	return n;
}

int vsprintf (char *buf, const char *fmt, va_list ap)
{
	return vsnprintf (buf, 0x7FFFFFFF, fmt, ap);
}

int sprintf (char *buf, const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	int n = vsnprintf (buf, 0x7FFFFFFF, fmt, ap);
	va_end (ap);
	return n;
}

/* printf and friends: the core's messages go to the log */
static void log_line (const char *buf)
{
	char line[256];
	strncpy (line, buf, sizeof line - 1);
	line[sizeof line - 1] = 0;
	size_t l = strlen (line);
	while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
	if (l) cpspi_log ("core: %s", line);
}

int printf (const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start (ap, fmt);
	int n = vsnprintf (buf, sizeof buf, fmt, ap);
	va_end (ap);
	log_line (buf);
	return n;
}

int vprintf (const char *fmt, va_list ap)
{
	char buf[256];
	int n = vsnprintf (buf, sizeof buf, fmt, ap);
	log_line (buf);
	return n;
}

int fprintf (FILE *stream, const char *fmt, ...)
{
	(void) stream;
	char buf[256];
	va_list ap;
	va_start (ap, fmt);
	int n = vsnprintf (buf, sizeof buf, fmt, ap);
	va_end (ap);
	log_line (buf);
	return n;
}

int puts (const char *s)
{
	log_line (s);
	return 1;
}

int fputs (const char *s, FILE *stream)
{
	(void) stream;
	log_line (s);
	return 1;
}

int putchar (int c)
{
	return c;
}

/* ---- process ---------------------------------------------------------- */

void exit (int code)
{
	cpspi_fatal (CPSPI_ERR_CORE, "core exit(%d)", code);
	for (;;) { }
}

void abort (void)
{
	cpspi_fatal (CPSPI_ERR_CORE, "core abort()");
	for (;;) { }
}

void __assert_func (const char *file, int line, const char *func, const char *expr)
{
	cpspi_fatal (CPSPI_ERR_CORE, "assertion %s failed: %s:%d (%s)", expr, file, line, func ? func : "");
	for (;;) { }
}

/* ---- time --------------------------------------------------------------- */

uint64_t cpspi_ticks_us (void);
void cpspi_delay_us (unsigned us);

time_t time (time_t *t)
{
	time_t now = (time_t) (cpspi_ticks_us () / 1000000);
	if (t) *t = now;
	return now;
}

int usleep (useconds_t us)
{
	cpspi_delay_us ((unsigned) us);
	return 0;
}

unsigned sleep (unsigned s)
{
	cpspi_delay_us (s * 1000000u);
	return 0;
}

/* ---- strings ------------------------------------------------------------ */

char *strrchr (const char *s, int c)
{
	const char *last = NULL;
	do
	{
		if (*s == (char) c) last = s;
	} while (*s++);
	return (char *) last;
}

char *strtok_r (char *s, const char *delim, char **save);

char *strtok (char *s, const char *delim)
{
	static char *save;
	return strtok_r (s, delim, &save);
}

long strtol (const char *s, char **end, int base)
{
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
	int neg = 0;
	if (*s == '-') { neg = 1; s++; }
	else if (*s == '+') s++;
	unsigned long v = strtoul (s, end, base);
	return neg ? -(long) v : (long) v;
}

/* sscanf: %d %i %u %x %X %s %c %n with an optional width (the core's ROM
   database parser uses "%d" and "%x") */
int sscanf (const char *str, const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	const char *s = str;
	int count = 0;
	for (; *fmt; ++fmt)
	{
		if (*fmt == ' ' || *fmt == '\t' || *fmt == '\n')
		{
			while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
			continue;
		}
		if (*fmt != '%')
		{
			if (*s != *fmt) break;
			s++;
			continue;
		}
		++fmt;
		int suppress = 0, width = 0, lng = 0;
		if (*fmt == '*') { suppress = 1; ++fmt; }
		while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
		while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') { if (*fmt == 'l') lng++; ++fmt; }
		if (*fmt == 'n')
		{
			if (!suppress) *va_arg (ap, int *) = (int) (s - str);
			continue;
		}
		if (*fmt == 'c')
		{
			if (!*s) break;
			if (!suppress) *va_arg (ap, char *) = *s;
			s++;
			count++;
			continue;
		}
		while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
		if (!*s) break;
		if (*fmt == 's')
		{
			char *out = suppress ? 0 : va_arg (ap, char *);
			int n = 0;
			while (*s && *s != ' ' && *s != '\t' && *s != '\n' && *s != '\r' && (width == 0 || n < width))
			{
				if (out) out[n] = *s;
				n++;
				s++;
			}
			if (out) out[n] = 0;
			count++;
			continue;
		}
		if (*fmt == 'd' || *fmt == 'i' || *fmt == 'u' || *fmt == 'x' || *fmt == 'X')
		{
			int base = (*fmt == 'x' || *fmt == 'X') ? 16 : (*fmt == 'i' ? 0 : 10);
			int neg = 0;
			const char *start = s;
			if (*s == '-') { neg = 1; s++; }
			else if (*s == '+') s++;
			if ((base == 16 || base == 0) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
			if (base == 0) base = (*s == '0') ? 8 : 10;
			unsigned long long v = 0;
			int digits = 0;
			while (*s && (width == 0 || (s - start) < width))
			{
				int d;
				if (*s >= '0' && *s <= '9') d = *s - '0';
				else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
				else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
				else break;
				if (d >= base) break;
				v = v * base + d;
				digits++;
				s++;
			}
			if (digits == 0) break;
			if (neg) v = (unsigned long long) (-(long long) v);
			if (!suppress)
			{
				if (lng >= 2) *va_arg (ap, long long *) = (long long) v;
				else if (lng == 1) *va_arg (ap, long *) = (long) v;
				else *va_arg (ap, int *) = (int) v;
			}
			count++;
			continue;
		}
		break;
	}
	va_end (ap);
	return count;
}

char *strdup (const char *s)
{
	size_t n = strlen (s) + 1;
	char *p = (char *) malloc (n);
	if (p) memcpy (p, s, n);
	return p;
}

/* byte swap of 16 bit words (ROM loading) */
void swab (const void *src, void *dst, ssize_t n)
{
	const uint8_t *s = (const uint8_t *) src;
	uint8_t *d = (uint8_t *) dst;
	for (ssize_t i = 0; i + 1 < n; i += 2)
	{
		uint8_t a = s[i], b = s[i + 1];
		d[i] = b;
		d[i + 1] = a;
	}
}

/* newlib ctype table (index -1..255): _U 01 _L 02 _N 04 _S 010 _P 020 _C 040 _X 0100 _B 0200 */
#define _U 01
#define _L 02
#define _N 04
#define _S 010
#define _P 020
#define _C 040
#define _X 0100
#define _B 0200
const char _ctype_[1 + 256] = {
	0,
	_C,	_C,	_C,	_C,	_C,	_C,	_C,	_C,
	_C,	_C|_S,	_C|_S,	_C|_S,	_C|_S,	_C|_S,	_C,	_C,
	_C,	_C,	_C,	_C,	_C,	_C,	_C,	_C,
	_C,	_C,	_C,	_C,	_C,	_C,	_C,	_C,
	_S|_B,	_P,	_P,	_P,	_P,	_P,	_P,	_P,
	_P,	_P,	_P,	_P,	_P,	_P,	_P,	_P,
	_N,	_N,	_N,	_N,	_N,	_N,	_N,	_N,
	_N,	_N,	_P,	_P,	_P,	_P,	_P,	_P,
	_P,	_U|_X,	_U|_X,	_U|_X,	_U|_X,	_U|_X,	_U|_X,	_U,
	_U,	_U,	_U,	_U,	_U,	_U,	_U,	_U,
	_U,	_U,	_U,	_U,	_U,	_U,	_U,	_U,
	_U,	_U,	_U,	_P,	_P,	_P,	_P,	_P,
	_P,	_L|_X,	_L|_X,	_L|_X,	_L|_X,	_L|_X,	_L|_X,	_L,
	_L,	_L,	_L,	_L,	_L,	_L,	_L,	_L,
	_L,	_L,	_L,	_L,	_L,	_L,	_L,	_L,
	_L,	_L,	_L,	_P,	_P,	_P,	_P,	_C,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
