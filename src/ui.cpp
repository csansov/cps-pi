/*
 * cps-pi - menus at CPS1 resolution (derived from neo-pi's ui.cpp)
 */
#include "fontdata.h"
#include "ui.h"
#include "cpspi.h"
#include <circle/font.h>
#include <circle/util.h>
#include <circle/timer.h>

#define OX	GAME_SRC_X
#define OY	GAME_SRC_Y

// 0xRRGGBB <-> the core's BGR555
static inline u16 To555 (u32 c)
{
	return BGR555 ((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

static inline u32 From555 (u16 p)
{
	unsigned r5 = p & 0x1F, g5 = (p >> 5) & 0x1F, b5 = (p >> 10) & 0x1F;
	return (((r5 << 3) | (r5 >> 2)) << 16) | (((g5 << 3) | (g5 >> 2)) << 8) | ((b5 << 3) | (b5 >> 2));
}

CUI::CUI (void)
{
	memset (m_Buf, 0, sizeof m_Buf);
	memset (m_Snapshot, 0, sizeof m_Snapshot);
}

void CUI::Pixel (int x, int y, u32 c)
{
	if (x >= 0 && x < UI_W && y >= 0 && y < UI_H)
	{
		m_Buf[(y + OY) * GAME_SRC_PITCH + x + OX] = To555 (c);
	}
}

void CUI::Fill (int x, int y, int w, int h, u32 c)
{
	for (int j = y; j < y + h; j++)
	{
		for (int i = x; i < x + w; i++)
		{
			Pixel (i, j, c);
		}
	}
}

// cps-pi logo, 30x15: N "CPS", P "PI", W border, . plate, space = transparent
static const char *const s_Logo[] =
{
	" WWWWWWWWWWWWWWWWWWWWWWWWWWWW ",
	"W............................W",
	"W.NNNNNNNN.NNNNNNNN.NNNNNNNN.W",
	"W.NNNN.....NNNN..NN.NNNN.....W",
	"W.NNNN.....NNNNNNNN.NNNNNNNN.W",
	"W.NNNN.....NNNN.........NNNN.W",
	"W.NNNNNNNN.NNNN.....NNNNNNNN.W",
	"W............................W",
	"W.PPPPPPPP.PPPP..............W",
	"W.PPPP..PP.PPPP..............W",
	"W.PPPPPPPP.PPPP..............W",
	"W.PPPP.....PPPP..............W",
	"W.PPPP.....PPPP.....WWW..WWW.W",
	"W...................W W..W W.W",
	" WWWWWWWWWWWWWWWWWWWW WWWW WW ",
};
#define LOGO_W		30
#define LOGO_H		15

// Static logo on the game list: in colour, twice the size, above the list
#define LOGO_SCALE	2
#define LOGO_Y		7		// in the band above the list (y 0-40)
#define LOGO_YELLOW	0xF0C81Eu
#define LOGO_BLUE	0x4678FFu

// Background: the logo in grey, tiled every 48x24 (odd rows shifted by 24),
// faint, scrolling diagonally; the pattern repeats every 48 pixels
#define TILE_CELL_W	48
#define TILE_CELL_H	24
#define TILE_GREY	0x96A0AFu	// = UI_DIM
#define TILE_DIM	24		// % of the full colour over the background
#define TILE_US_PER_PX	33333		// 30 pixels per second

static u32 Mix (u32 nBack, u32 nFore, int nPercent)
{
	u32 nResult = 0;
	for (int s = 0; s < 24; s += 8)
	{
		int b = (int) ((nBack >> s) & 0xFF), f = (int) ((nFore >> s) & 0xFF);
		nResult |= (u32) (b + (f - b) * nPercent / 100) << s;
	}
	return nResult;
}

// Screens without a game behind them
void CUI::Background (void)
{
	const u16 nGrey = To555 (Mix (UI_BG1, TILE_GREY, TILE_DIM));
	const u16 nPlate = To555 (Mix (UI_BG1, 0x000000, TILE_DIM));
	const u16 nBG = To555 (UI_BG1);
	// time based, so it moves at the same speed whatever draws the frames
	const unsigned nOff = (unsigned) (CTimer::GetClockTicks64 () / TILE_US_PER_PX) % TILE_CELL_W;
	for (int y = 0; y < UI_H; y++)
	{
		u16 *p = &m_Buf[(y + OY) * GAME_SRC_PITCH + OX];
		unsigned yy = ((unsigned) y + 2 * TILE_CELL_H - nOff) % (2 * TILE_CELL_H);
		unsigned nRow = yy / TILE_CELL_H, ly = yy % TILE_CELL_H;
		for (int x = 0; x < UI_W; x++)
		{
			u16 c = nBG;
			if (ly < LOGO_H)
			{
				unsigned lx = ((unsigned) x + 2 * TILE_CELL_W - nOff
					       - nRow * (TILE_CELL_W / 2)) % TILE_CELL_W;
				if (lx < LOGO_W)
				{
					char ch = s_Logo[ly][lx];
					if (ch == '.')
					{
						c = nPlate;
					}
					else if (ch != ' ')
					{
						c = nGrey;
					}
				}
			}
			p[x] = c;
		}
	}
}

void CUI::Logo (void)
{
	const int x0 = (UI_W - LOGO_W * LOGO_SCALE) / 2;
	for (int ly = 0; ly < LOGO_H; ly++)
	{
		for (int lx = 0; lx < LOGO_W; lx++)
		{
			u32 c;
			switch (s_Logo[ly][lx])
			{
			case 'N': c = LOGO_YELLOW; break;
			case 'P': c = LOGO_BLUE; break;
			case 'W': c = UI_WHITE; break;
			case '.': c = 0x000000; break;
			default:  continue;
			}
			Fill (x0 + lx * LOGO_SCALE, LOGO_Y + ly * LOGO_SCALE, LOGO_SCALE, LOGO_SCALE, c);
		}
	}
}

void CUI::Panel (int x0, int y0, int x1, int y1)
{
	Fill (x0, y0, x1 - x0 + 1, y1 - y0 + 1, UI_FRAME2);
	Fill (x0 + 1, y0 + 1, x1 - x0 - 1, y1 - y0 - 1, UI_FRAME1);
	Fill (x0 + 3, y0 + 3, x1 - x0 - 5, y1 - y0 - 5, UI_FRAME2);
	Fill (x0 + 4, y0 + 4, x1 - x0 - 7, y1 - y0 - 7, UI_PANEL);
}

// Menu font (Matinee, D.Guard): capitals only (its lower case is hard to read)
void CUI::Text (int x, int y, const char *s, u32 c, unsigned nTall)
{
	for (unsigned i = 0; s[i] != '\0'; i++)
	{
		unsigned ch = (unsigned char) s[i];
		if (ch < 32 || ch >= 128)
		{
			ch = '?';
		}
		if (ch >= 'a' && ch <= 'z')
		{
			ch -= 32;
		}
		for (int pass = 0; pass < 2; pass++)		// shadow, then glyph
		{
			int dx = pass == 0 ? 1 : 0;
			u32 col = pass == 0 ? 0x000000 : c;
			for (unsigned r = 0; r < 8; r++)
			{
				u8 nRow = g_MenuFont[ch - 32][r];
				for (unsigned k = 0; k < 8; k++)
				{
					if (nRow & (0x80 >> k))
					{
						for (unsigned t = 0; t < nTall; t++)
						{
							Pixel (x + i * 8 + k + dx, y + r * nTall + t + dx, col);
						}
					}
				}
			}
		}
	}
}

void CUI::CText (int y, const char *s, u32 c, unsigned nTall)
{
	Text ((UI_W - (int) strlen (s) * 8) / 2, y, s, c, nTall);
}

// Six numbered buttons: 13 px disc in the button colour with a 5x7 digit
static const u32 ButtonColor[6] = {0xDC2828, 0xF0C81E, 0x28B446, 0x325AE6, 0xB446DC, 0xF08228};
static const u32 ButtonInk[6]   = {0xFFFFFF, 0x281E00, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0x281E00};
static const char *DigitGlyph[6][7] =
{
	{"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."},
	{".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"},
	{"####.", "....#", "....#", ".###.", "....#", "....#", "####."},
	{"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."},
	{"#####", "#....", "####.", "....#", "....#", "#...#", ".###."},
	{".###.", "#....", "#....", "####.", "#...#", "#...#", ".###."}
};

void CUI::ButtonIcon (int x, int y, char cButton)
{
	unsigned b = (unsigned) (cButton - '1');
	if (b > 5)
	{
		return;
	}
	for (int j = 0; j < 13; j++)
	{
		for (int i = 0; i < 13; i++)
		{
			int dx = i - 6, dy = j - 6;
			int d2 = 4 * (dx * dx + dy * dy);	// (2d)^2, radius 6.5 -> 169, rim from 5.6 -> 125
			if (d2 <= 169)
			{
				Pixel (x + i, y + j, d2 > 125 ? 0x000000 : ButtonColor[b]);
			}
		}
	}
	for (int r = 0; r < 7; r++)
	{
		for (int k = 0; k < 5; k++)
		{
			if (DigitGlyph[b][r][k] == '#')
			{
				Pixel (x + 4 + k, y + 3 + r, ButtonInk[b]);
			}
		}
	}
}

u32 CUI::ButtonColorOf (char cButton)
{
	unsigned b = (unsigned) (cButton - '1');
	return b <= 5 ? ButtonColor[b] : UI_WHITE;
}

u32 CUI::Dim (u32 c, unsigned nPercent)
{
	return ((((c >> 16) & 0xFF) * nPercent / 100) << 16)
	     | ((((c >> 8) & 0xFF) * nPercent / 100) << 8)
	     | (((c & 0xFF) * nPercent / 100));
}

// Hint bar: button followed by its action
void CUI::Buttons (int y, const TButtonHint *pHints, unsigned nHints)
{
	const int nGap = 14;
	int nW = 0;
	for (unsigned i = 0; i < nHints; i++)
	{
		nW += 13 + 4 + (int) strlen (pHints[i].pText) * 8 + (i ? nGap : 0);
	}
	int x = (UI_W - nW) / 2;
	for (unsigned i = 0; i < nHints; i++)
	{
		ButtonIcon (x, y, pHints[i].cButton);
		Text (x + 13 + 4, y + 3, pHints[i].pText, 0xD2D2D2);
		x += 13 + 4 + (int) strlen (pHints[i].pText) * 8 + nGap;
	}
}

// The paused game (darkened snapshot) or, with no game loaded, the pattern
void CUI::Backdrop (boolean bOverGame)
{
	if (bOverGame)
	{
		for (int y = 0; y < UI_H; y++)
		{
			memcpy (&m_Buf[(y + OY) * GAME_SRC_PITCH + OX], &m_Snapshot[y * UI_W], UI_W * 2);
		}
	}
	else
	{
		Background ();
	}
}

void CUI::DrawList (const TGameEntry *pGames, unsigned nCount, unsigned nSel, const char *pMessage,
		    boolean bOverGame)
{
	Backdrop (bOverGame);
	Logo ();					// the big logo: only on the game list
	Panel (12, 44, UI_W - 13, 190);

	Fill (17, 104, UI_W - 34, 23, UI_BAND);
	Fill (17, 104, UI_W - 34, 1, UI_FRAME2);
	Fill (17, 126, UI_W - 34, 1, UI_FRAME2);

	for (int k = -4; k <= 4; k++)
	{
		int i = (int) nSel + k;
		if (i < 0 || i >= (int) nCount)
		{
			continue;
		}
		char Name[48];
		strncpy (Name, pGames[i].Name, 42);
		Name[42] = '\0';
		if (k == 0)
		{
			CText (107, Name, UI_WHITE, 2);
		}
		else
		{
			int y = 115 + k * 12 + (k > 0 ? 8 : -8) - 4;
			int nFade = 100 - (k < 0 ? -k : k) * 18;
			if (nFade < 35) nFade = 35;
			CText (y, Name, Dim (UI_DIM, (unsigned) nFade), 1);
		}
	}

	char Count[16];
	{
		unsigned a = nSel + 1, b = nCount;
		char *p = Count;
		if (a >= 100) *p++ = '0' + (a / 100) % 10;
		if (a >= 10) *p++ = '0' + (a / 10) % 10;
		*p++ = '0' + a % 10;
		*p++ = '/';
		if (b >= 100) *p++ = '0' + (b / 100) % 10;
		if (b >= 10) *p++ = '0' + (b / 10) % 10;
		*p++ = '0' + b % 10;
		*p = '\0';
	}
	Text (UI_W - 18 - (int) strlen (Count) * 8, 178, Count, 0x7A8290);
	char Info[32];
	strcpy (Info, pGames[nSel].File);
	strcat (Info, pGames[nSel].System == 2 ? ".zip   CPS2" : ".zip   CPS1");
	Text (18, 178, Info, 0x7A8290);

	if (pMessage && pMessage[0])
	{
		CText (32, pMessage, UI_ERR);
	}
	static const TButtonHint Hints[2] = {{'1', "START GAME"}, {'4', "OPTIONS"}};
	Buttons (198, Hints, 2);
}

void CUI::SnapshotGame (const u16 *pGameBuf, boolean bFlip)
{
	for (int y = 0; y < UI_H; y++)
	{
		const u16 *s = pGameBuf + ((bFlip ? UI_H - 1 - y : y) + OY) * GAME_SRC_PITCH + OX;
		for (int x = 0; x < UI_W; x++)
		{
			u16 p = s[bFlip ? UI_W - 1 - x : x];
			u32 r5 = (p & 0x1F) * 30 / 100;
			u32 g5 = ((p >> 5) & 0x1F) * 30 / 100;
			u32 b5 = ((p >> 10) & 0x1F) * 30 / 100;
			m_Snapshot[y * UI_W + x] = (u16) ((b5 << 10) | (g5 << 5) | r5);
		}
	}
}

void CUI::DrawPanelMenu (boolean bOverGame, const char *pTitle,
			 const TMenuItem *pItems, unsigned nItems, unsigned nSel,
			 const char *pMessage, u32 nMessageColor,
			 const TButtonHint *pHints, unsigned nHints, const char *pFooter)
{
	Backdrop (bOverGame);

	const int nStep = nItems > 8 ? 12 : 15;		// long lists (controls) a bit tighter
	int nInner = 22 + (int) nItems * nStep + 22;		// title + items + message line
	int y0 = (190 - nInner) / 2 + 4;
	if (y0 < 4) y0 = 4;
	int x0 = 64, x1 = UI_W - 65;
	Panel (x0, y0, x1, y0 + nInner + 8);

	// at most 30 characters fit inside the frame: cut long names at a word
	char Title[40];
	strncpy (Title, pTitle, 39);
	Title[39] = '\0';
	if (strlen (Title) > 30)
	{
		Title[30] = '\0';
		for (int i = 29; i > 8; i--)
		{
			if (Title[i] == ' ')
			{
				Title[i] = '\0';
				break;
			}
		}
		size_t n = strlen (Title);
		while (n > 0 && (Title[n - 1] == ' ' || Title[n - 1] == '-' || Title[n - 1] == ':'))
		{
			Title[--n] = '\0';
		}
	}
	CText (y0 + 10, Title, UI_TITLE);
	Fill (x0 + 8, y0 + 22, x1 - x0 - 15, 1, UI_FRAME2);

	for (unsigned n = 0; n < nItems; n++)
	{
		int y = y0 + 32 + (int) n * nStep;
		boolean bSel = n == nSel;
		if (bSel)
		{
			Fill (x0 + 8, y - 3, x1 - x0 - 15, 13, UI_BAND);
			for (int t = 0; t < 4; t++)		// cursor triangle
			{
				Fill (x0 + 12 + t, y + t, 1, 8 - 2 * t, UI_CURSOR);
			}
		}
		u32 nColor = pItems[n].bDisabled ? Dim (UI_DIM, 55) : (bSel ? UI_WHITE : UI_DIM);
		Text (x0 + 22, y, pItems[n].pLabel, nColor);
		if (pItems[n].pValue)
		{
			int nLen = (int) strlen (pItems[n].pValue);
			u32 nValColor = pItems[n].bDisabled ? Dim (UI_DIM, 45) : (bSel ? UI_VALUE : Dim (UI_DIM, 75));
			Text (x1 - 10 - nLen * 8, y, pItems[n].pValue, nValColor);
		}
	}

	if (pMessage && pMessage[0])
	{
		Text (x0 + 22, y0 + 32 + (int) nItems * nStep + 4, pMessage, nMessageColor);
	}

	if (pFooter && pFooter[0])
	{
		char Footer[48];
		strncpy (Footer, pFooter, 46);
		Footer[46] = '\0';
		CText (y0 + nInner + 14, Footer, Dim (UI_DIM, 60));
	}

	Buttons (209, pHints, nHints);
}

void CUI::DrawBox (const char *pLine1, const char *pLine2, const TButtonHint *pHint)
{
	// darken what is underneath, so the box stands out from the menu
	for (int y = 0; y < UI_H; y++)
	{
		u16 *p = &m_Buf[(y + OY) * GAME_SRC_PITCH + OX];
		for (int x = 0; x < UI_W; x++)
		{
			p[x] = To555 (Dim (From555 (p[x]), 40));
		}
	}
	Panel (84, 80, UI_W - 85, 150);
	if (pLine1) CText (92, pLine1, UI_WHITE);
	if (pLine2) CText (110, pLine2, UI_DIM);
	if (pHint) Buttons (126, pHint, 1);
}

void CUI::DrawBlack (void)
{
	for (int y = 0; y < UI_H; y++)
	{
		for (int x = 0; x < UI_W; x++)
		{
			m_Buf[(y + OY) * GAME_SRC_PITCH + x + OX] = 0;
		}
	}
}

void CUI::DrawLoading (const char *pName, unsigned nPercent)
{
	Background ();

	// long titles go on two lines of up to 34 characters, split at a space
	const unsigned nMax = 34;
	char Line1[48], Line2[48];
	size_t nLen = strlen (pName);
	Line2[0] = '\0';
	if (nLen <= nMax)
	{
		strncpy (Line1, pName, sizeof Line1 - 1);
		Line1[sizeof Line1 - 1] = '\0';
	}
	else
	{
		unsigned nCut = nMax;
		while (nCut > 0 && pName[nCut] != ' ')
		{
			nCut--;
		}
		if (nCut == 0)
		{
			nCut = nMax;			// no space: split inside the word
		}
		memcpy (Line1, pName, nCut);
		Line1[nCut] = '\0';
		const char *pRest = pName + nCut;
		while (*pRest == ' ')
		{
			pRest++;
		}
		strncpy (Line2, pRest, nMax);
		Line2[nMax] = '\0';
	}

	int dy = Line2[0] ? 6 : 0;			// the box grows by 6 lines up and down
	Panel (40, 84 - dy, UI_W - 41, 150 + dy);
	CText (98 - dy, "LOADING", UI_TITLE);
	CText (112 - dy, Line1, UI_WHITE);
	if (Line2[0])
	{
		CText (124 - dy, Line2, UI_WHITE);
	}
	int nBarW = UI_W - 112;
	int yBar = 130 + dy;
	Fill (56, yBar, nBarW, 8, UI_FRAME2);
	Fill (57, yBar + 1, nBarW - 2, 6, UI_PANEL);
	Fill (57, yBar + 1, (nBarW - 2) * (int) (nPercent > 100 ? 100 : nPercent) / 100, 6, UI_TITLE);
}

void CUI::DrawMessage (const char *pTitle, const char *pLine1, const char *pLine2)
{
	Background ();
	Panel (24, 70, UI_W - 25, 160);
	CText (86, pTitle, UI_ERR);
	if (pLine1) CText (108, pLine1, UI_WHITE);
	if (pLine2) CText (124, pLine2, UI_DIM);
}

// ---- controls set-up -------------------------------------------------------

void CUI::DrawMapping (boolean bOverGame, const char *pTitle, int nStep,
		       const char *const *pValues, const char *pFooter)
{
	Backdrop (bOverGame);

	const int x0 = 56, x1 = UI_W - 57;
	Panel (x0, 6, x1, 204);
	CText (16, pTitle, UI_TITLE);
	Fill (x0 + 8, 28, x1 - x0 - 15, 1, UI_FRAME2);

	// the request, big and in the middle: PRESS 1 / PRESS START ...
	static const char *Names[8] = {"1", "2", "3", "4", "5", "6", "START", "SELECT"};
	if (nStep >= 0 && nStep < 8)
	{
		const char *pName = Names[nStep];
		int nLen = 6 + (int) strlen (pName);
		int x = (UI_W - nLen * 8) / 2;
		Text (x, 38, "PRESS ", UI_WHITE, 2);
		Text (x + 48, 38, pName, nStep < 6 ? ButtonColorOf ((char) ('1' + nStep)) : UI_VALUE, 2);
	}
	else
	{
		CText (38, "SAVED", UI_OK, 2);
	}

	static const char *Labels[8] = {0, 0, 0, 0, 0, 0, "START", "SELECT / COIN"};
	for (int i = 0; i < 8; i++)
	{
		int y = 64 + i * 15;
		if (i == nStep)
		{
			Fill (x0 + 8, y - 1, x1 - x0 - 15, 15, UI_BAND);
		}
		if (i < 6)
		{
			ButtonIcon (x0 + 20, y, (char) ('1' + i));
			static const char *Roles[6] = {"JAB", "STRONG", "FIERCE", "SHORT", "FORWARD", "ROUNDHOUSE"};
			Text (x0 + 38, y + 3, Roles[i], i == nStep ? UI_WHITE : Dim (UI_DIM, 70));
		}
		else
		{
			Text (x0 + 20, y + 3, Labels[i], i == nStep ? UI_WHITE : UI_DIM);
		}
		const char *pValue = pValues[i];
		if (i == nStep)
		{
			pValue = "?";
		}
		if (pValue && pValue[0])
		{
			Text (x1 - 10 - (int) strlen (pValue) * 8, y + 3, pValue,
			      i == nStep ? UI_VALUE : Dim (UI_DIM, 85));
		}
	}

	if (pFooter && pFooter[0])
	{
		CText (188, pFooter, Dim (UI_DIM, 60));
	}
}

// ---- credits -----------------------------------------------------------------

void CUI::DrawCredits (const char *pVersion)
{
	Backdrop (TRUE);

	const int x0 = 30, x1 = UI_W - 31, y0 = 6;
	Panel (x0, y0, x1, y0 + 187);
	CText (y0 + 10, "CREDITS", UI_TITLE);
	Fill (x0 + 8, y0 + 22, x1 - x0 - 15, 1, UI_FRAME2);

	char Title[32] = "CPS-PI ";
	strncat (Title, pVersion, sizeof Title - 8);
	CText (y0 + 28, Title, UI_WHITE, 2);
	CText (y0 + 48, "BARE METAL " CPSPI_SYSTEM " FOR RASPBERRY PI", UI_VALUE);
	Fill (x0 + 8, y0 + 62, x1 - x0 - 15, 1, UI_FRAME2);

	static const char *Rows[8][2] =
	{
		{"CORE",   "NJEMU - NJ, FJTRUJY, HONKEYKONG"},
		{"68000",  "C68K - S.DALLAIRE"},
		{"Z80",    "CZ80 - S.DALLAIRE"},
		{"SOUND",  "YM2151 OKI QSOUND - MAME TEAM"},
		{"ZIP",    "MINIZ - R.GELDREICH"},
		{"SYSTEM", "CIRCLE - R.STANGE"},
		{"BASE",   "NEO-PI - A.SCANDEREBECH"},
		{"FONT",   "MATINEE - D.GUARD"}
	};
	for (int i = 0; i < 8; i++)
	{
		int y = y0 + 68 + i * 11;
		Text (x0 + 12, y, Rows[i][0], Dim (UI_DIM, 80));
		Text (x0 + 12 + 7 * 8, y, Rows[i][1], 0xEBEBEB);
	}
	Fill (x0 + 8, y0 + 157, x1 - x0 - 15, 1, UI_FRAME2);
	CText (y0 + 162, "CPS AND GAMES ARE TRADEMARKS OF CAPCOM.", 0x828796);
	CText (y0 + 173, "GAMES NOT INCLUDED.", 0x828796);

	static const TButtonHint Close = {'2', "CLOSE"};
	Buttons (201, &Close, 1);
}
