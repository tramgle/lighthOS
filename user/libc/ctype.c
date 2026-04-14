/* 256-entry ASCII classification table. Each entry is a bitmask of the
   character classes, so `isalpha(c)` is just `table[c] & _ALPHA`. */

#define _UPPER   0x01
#define _LOWER   0x02
#define _DIGIT   0x04
#define _SPACE   0x08  /* ' '/'\t'/'\n'/'\v'/'\f'/'\r' */
#define _PUNCT   0x10
#define _CNTRL   0x20
#define _XDIGIT  0x40
#define _BLANK   0x80  /* ' ' / '\t' */

static const unsigned char ctype_table[256] = {
    /* 0x00-0x08 */ _CNTRL, _CNTRL, _CNTRL, _CNTRL, _CNTRL, _CNTRL, _CNTRL, _CNTRL, _CNTRL,
    /* 0x09-0x0d */ _CNTRL|_SPACE|_BLANK, _CNTRL|_SPACE, _CNTRL|_SPACE, _CNTRL|_SPACE, _CNTRL|_SPACE,
    /* 0x0e-0x1f */ _CNTRL, _CNTRL, _CNTRL, _CNTRL, _CNTRL, _CNTRL, _CNTRL, _CNTRL,
                    _CNTRL, _CNTRL, _CNTRL, _CNTRL, _CNTRL, _CNTRL, _CNTRL, _CNTRL, _CNTRL, _CNTRL,
    /* 0x20 ' '  */ _SPACE|_BLANK,
    /* 0x21-0x2f */ _PUNCT, _PUNCT, _PUNCT, _PUNCT, _PUNCT, _PUNCT, _PUNCT, _PUNCT,
                    _PUNCT, _PUNCT, _PUNCT, _PUNCT, _PUNCT, _PUNCT, _PUNCT,
    /* 0x30-0x39 */ _DIGIT|_XDIGIT, _DIGIT|_XDIGIT, _DIGIT|_XDIGIT, _DIGIT|_XDIGIT,
                    _DIGIT|_XDIGIT, _DIGIT|_XDIGIT, _DIGIT|_XDIGIT, _DIGIT|_XDIGIT,
                    _DIGIT|_XDIGIT, _DIGIT|_XDIGIT,
    /* 0x3a-0x40 */ _PUNCT, _PUNCT, _PUNCT, _PUNCT, _PUNCT, _PUNCT, _PUNCT,
    /* 0x41-0x46 A-F */ _UPPER|_XDIGIT, _UPPER|_XDIGIT, _UPPER|_XDIGIT, _UPPER|_XDIGIT, _UPPER|_XDIGIT, _UPPER|_XDIGIT,
    /* 0x47-0x5a G-Z */ _UPPER, _UPPER, _UPPER, _UPPER, _UPPER, _UPPER, _UPPER, _UPPER, _UPPER,
                        _UPPER, _UPPER, _UPPER, _UPPER, _UPPER, _UPPER, _UPPER, _UPPER, _UPPER, _UPPER, _UPPER,
    /* 0x5b-0x60 */ _PUNCT, _PUNCT, _PUNCT, _PUNCT, _PUNCT, _PUNCT,
    /* 0x61-0x66 a-f */ _LOWER|_XDIGIT, _LOWER|_XDIGIT, _LOWER|_XDIGIT, _LOWER|_XDIGIT, _LOWER|_XDIGIT, _LOWER|_XDIGIT,
    /* 0x67-0x7a g-z */ _LOWER, _LOWER, _LOWER, _LOWER, _LOWER, _LOWER, _LOWER, _LOWER, _LOWER,
                        _LOWER, _LOWER, _LOWER, _LOWER, _LOWER, _LOWER, _LOWER, _LOWER, _LOWER, _LOWER, _LOWER,
    /* 0x7b-0x7e */ _PUNCT, _PUNCT, _PUNCT, _PUNCT,
    /* 0x7f      */ _CNTRL,
    /* rest zeroed */
};

static inline int mask(int c, int m) {
    return (c >= 0 && c < 256) ? (ctype_table[c] & m) : 0;
}

int isdigit(int c)  { return mask(c, _DIGIT); }
int isalpha(int c)  { return mask(c, _UPPER|_LOWER); }
int isalnum(int c)  { return mask(c, _UPPER|_LOWER|_DIGIT); }
int isspace(int c)  { return mask(c, _SPACE); }
int isxdigit(int c) { return mask(c, _XDIGIT); }
int isupper(int c)  { return mask(c, _UPPER); }
int islower(int c)  { return mask(c, _LOWER); }
int ispunct(int c)  { return mask(c, _PUNCT); }
int iscntrl(int c)  { return mask(c, _CNTRL); }
int isprint(int c)  { return c >= 0x20 && c < 0x7f; }
int isgraph(int c)  { return isprint(c) && c != ' '; }
int isblank(int c)  { return mask(c, _BLANK); }

int tolower(int c) { return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c; }
int toupper(int c) { return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c; }
