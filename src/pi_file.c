/*
 * cps-pi - POSIX-style file access for the NJEMU core on the SD card (FatFs)
 *
 * The core reads ROM zips and its ROM database with open/read/lseek/close and
 * fopen/fread/fseek/ftell (unzip.c), and writes EEPROM contents with
 * fopen/fwrite. Paths are passed straight to FatFs ("SD:/cpspi/roms/x.zip").
 */
#include <fatfs/ff.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAX_FD		8
#define FD_BASE		3
#define MAX_STREAMS	8

static FIL s_Files[MAX_FD];
static unsigned char s_Used[MAX_FD];

// newlib's feof/ferror/clearerr are macros reading FILE::_flags, so every
// stream starts with a real FILE header whose flags are kept up to date
typedef struct
{
	FILE hdr;
	FIL fil;
	int used;
} TStream;

static TStream s_Streams[MAX_STREAMS];

#define STREAM_ERR(s)	((s)->hdr._flags |= __SERR)
#define STREAM_EOF(s)	((s)->hdr._flags |= __SEOF)
#define STREAM_CLEAR(s)	((s)->hdr._flags &= ~(__SERR | __SEOF))

/* ---- descriptors --------------------------------------------------------- */

int open (const char *path, int flags, ...)
{
	int i;
	for (i = 0; i < MAX_FD; i++)
	{
		if (!s_Used[i]) break;
	}
	if (i == MAX_FD) return -1;

	BYTE mode;
	switch (flags & O_ACCMODE)
	{
	case O_WRONLY: mode = FA_WRITE; break;
	case O_RDWR:   mode = FA_READ | FA_WRITE; break;
	default:       mode = FA_READ; break;
	}
	if (flags & O_CREAT)
	{
		mode |= (flags & O_TRUNC) ? FA_CREATE_ALWAYS : FA_OPEN_ALWAYS;
		if (!(flags & O_TRUNC) && (flags & O_ACCMODE) == O_WRONLY) mode |= FA_CREATE_ALWAYS;
	}
	else if (flags & O_TRUNC)
	{
		mode |= FA_CREATE_ALWAYS;
	}
	if (flags & O_APPEND) mode |= FA_OPEN_APPEND;

	if (f_open (&s_Files[i], path, mode) != FR_OK) return -1;
	s_Used[i] = 1;
	return i + FD_BASE;
}

static FIL *fil_of (int fd)
{
	fd -= FD_BASE;
	if (fd < 0 || fd >= MAX_FD || !s_Used[fd]) return 0;
	return &s_Files[fd];
}

_READ_WRITE_RETURN_TYPE read (int fd, void *buf, size_t n)
{
	FIL *f = fil_of (fd);
	if (!f) return -1;
	size_t done = 0;
	while (done < n)
	{
		UINT want = (n - done) > 0x40000000u ? 0x40000000u : (UINT) (n - done);
		UINT got = 0;
		if (f_read (f, (char *) buf + done, want, &got) != FR_OK) return done ? (_READ_WRITE_RETURN_TYPE) done : -1;
		done += got;
		if (got < want) break;
	}
	return (_READ_WRITE_RETURN_TYPE) done;
}

_READ_WRITE_RETURN_TYPE write (int fd, const void *buf, size_t n)
{
	FIL *f = fil_of (fd);
	if (!f) return -1;
	UINT wr = 0;
	if (f_write (f, buf, (UINT) n, &wr) != FR_OK) return -1;
	return (_READ_WRITE_RETURN_TYPE) wr;
}

off_t lseek (int fd, off_t off, int whence)
{
	FIL *f = fil_of (fd);
	if (!f) return -1;
	FSIZE_t base = whence == SEEK_CUR ? f_tell (f) : whence == SEEK_END ? f_size (f) : 0;
	if (f_lseek (f, base + off) != FR_OK) return -1;
	return (off_t) f_tell (f);
}

int close (int fd)
{
	FIL *f = fil_of (fd);
	if (!f) return -1;
	f_close (f);
	s_Used[fd - FD_BASE] = 0;
	return 0;
}

int fstat (int fd, struct stat *st)
{
	FIL *f = fil_of (fd);
	if (!f) return -1;
	memset (st, 0, sizeof *st);
	st->st_size = (off_t) f_size (f);
	st->st_mode = S_IFREG | 0666;
	return 0;
}

int stat (const char *path, struct stat *st)
{
	FILINFO info;
	if (f_stat (path, &info) != FR_OK) return -1;
	memset (st, 0, sizeof *st);
	st->st_size = (off_t) info.fsize;
	st->st_mode = ((info.fattrib & AM_DIR) ? S_IFDIR : S_IFREG) | 0666;
	return 0;
}

int unlink (const char *path)
{
	return f_unlink (path) == FR_OK ? 0 : -1;
}

int remove (const char *path)
{
	return f_unlink (path) == FR_OK ? 0 : -1;
}

int mkdir (const char *path, mode_t mode)
{
	(void) mode;
	FRESULT r = f_mkdir (path);
	return (r == FR_OK || r == FR_EXIST) ? 0 : -1;
}

/* ---- streams --------------------------------------------------------------- */

static TStream *stream_of (FILE *fp)
{
	TStream *s = (TStream *) fp;
	if (s < s_Streams || s >= s_Streams + MAX_STREAMS || !s->used) return 0;
	return s;
}

FILE *fopen (const char *path, const char *mode)
{
	int i;
	for (i = 0; i < MAX_STREAMS; i++)
	{
		if (!s_Streams[i].used) break;
	}
	if (i == MAX_STREAMS) return 0;

	BYTE m;
	switch (mode[0])
	{
	case 'w': m = FA_WRITE | FA_CREATE_ALWAYS; break;
	case 'a': m = FA_WRITE | FA_OPEN_APPEND; break;
	default:  m = FA_READ; break;
	}
	if (strchr (mode, '+')) m |= FA_READ | FA_WRITE;

	TStream *s = &s_Streams[i];
	if (f_open (&s->fil, path, m) != FR_OK) return 0;
	memset (&s->hdr, 0, sizeof s->hdr);
	s->used = 1;
	return (FILE *) s;
}

int fclose (FILE *fp)
{
	TStream *s = stream_of (fp);
	if (!s) return -1;
	f_close (&s->fil);
	s->used = 0;
	return 0;
}

size_t fread (void *buf, size_t size, size_t n, FILE *fp)
{
	TStream *s = stream_of (fp);
	if (!s || size == 0 || n == 0) return 0;
	UINT want = (UINT) (size * n), got = 0;
	if (f_read (&s->fil, buf, want, &got) != FR_OK)
	{
		STREAM_ERR (s);
		return 0;
	}
	if (got < want) STREAM_EOF (s);
	return got / size;
}

size_t fwrite (const void *buf, size_t size, size_t n, FILE *fp)
{
	TStream *s = stream_of (fp);
	if (!s || size == 0 || n == 0) return 0;
	UINT want = (UINT) (size * n), wr = 0;
	if (f_write (&s->fil, buf, want, &wr) != FR_OK)
	{
		STREAM_ERR (s);
		return 0;
	}
	return wr / size;
}

int fseek (FILE *fp, long off, int whence)
{
	TStream *s = stream_of (fp);
	if (!s) return -1;
	FSIZE_t base = whence == SEEK_CUR ? f_tell (&s->fil) : whence == SEEK_END ? f_size (&s->fil) : 0;
	s->hdr._flags &= ~__SEOF;
	return f_lseek (&s->fil, base + off) == FR_OK ? 0 : -1;
}

long ftell (FILE *fp)
{
	TStream *s = stream_of (fp);
	if (!s) return -1;
	return (long) f_tell (&s->fil);
}

void rewind (FILE *fp)
{
	fseek (fp, 0, SEEK_SET);
}

int fgetc (FILE *fp)
{
	unsigned char c;
	return fread (&c, 1, 1, fp) == 1 ? c : EOF;
}

int getc (FILE *fp)
{
	return fgetc (fp);
}

char *fgets (char *buf, int n, FILE *fp)
{
	int i = 0;
	while (i < n - 1)
	{
		int c = fgetc (fp);
		if (c == EOF) break;
		buf[i++] = (char) c;
		if (c == '\n') break;
	}
	if (i == 0) return 0;
	buf[i] = 0;
	return buf;
}

int fputc (int c, FILE *fp)
{
	unsigned char ch = (unsigned char) c;
	return fwrite (&ch, 1, 1, fp) == 1 ? c : EOF;
}

// functions as well as the newlib macros (which read the FILE header)
#undef feof
#undef ferror
#undef clearerr

int feof (FILE *fp)
{
	TStream *s = stream_of (fp);
	return s ? (s->hdr._flags & __SEOF) != 0 : 1;
}

int ferror (FILE *fp)
{
	TStream *s = stream_of (fp);
	return s ? (s->hdr._flags & __SERR) != 0 : 1;
}

void clearerr (FILE *fp)
{
	TStream *s = stream_of (fp);
	if (s) STREAM_CLEAR (s);
}

int fflush (FILE *fp)
{
	TStream *s = stream_of (fp);
	if (!s) return 0;
	return f_sync (&s->fil) == FR_OK ? 0 : -1;
}
