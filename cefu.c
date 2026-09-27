/*
 * Cefu — Cat Editor For Unix
 * C99, no ncurses, no legacy, no fat. Built-in fcurses (screen lib)
 * without <termios.h>. Syntax highlight: C, C++, Python, Lua, FASM.
 *
 * Yuunold 2026 (c) Cefu
 * BSD-2-Clause License
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <time.h>
#include <ctype.h>

/*
 *  //-rawterm
 *  Terminal raw mode without <termios.h>. We declare our own
 *  termios-compatible struct and hit ioctl() directly:
 *    - Linux:        TCGETS / TCSETS
 *    - macOS/BSD:    TIOCGETA / TIOCSETA
 *  No root needed — same privilege level as any terminal program.
 */

#if defined(__linux__)
#  define CEFU_LINUX 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__) || defined(__DragonFly__)
#  define CEFU_BSD 1
#else
#  error "cefu: unsupported platform (need Linux, macOS, or a BSD)"
#endif

#define CEFU_NCCS 32

#if defined(CEFU_LINUX)

struct cefu_termios {
    unsigned int  c_iflag;
    unsigned int  c_oflag;
    unsigned int  c_cflag;
    unsigned int  c_lflag;
    unsigned char c_line;
    unsigned char c_cc[CEFU_NCCS];
    unsigned int  c_ispeed;
    unsigned int  c_ospeed;
};
#define CEFU_TCGET 0x5401 /* TCGETS */
#define CEFU_TCSET 0x5402 /* TCSETS */
#define CEFU_ECHO   0000010
#define CEFU_ICANON 0000002
#define CEFU_ISIG   0000001
#define CEFU_IEXTEN 0100000
#define CEFU_IXON   0002000
#define CEFU_ICRNL  0000400
#define CEFU_BRKINT 0000002
#define CEFU_INPCK  0000020
#define CEFU_ISTRIP 0000040
#define CEFU_OPOST  0000001
#define CEFU_CS8    0000060
#define CEFU_VMIN   6
#define CEFU_VTIME  5

#elif defined(CEFU_BSD)

struct cefu_termios {
    unsigned long c_iflag;
    unsigned long c_oflag;
    unsigned long c_cflag;
    unsigned long c_lflag;
    unsigned char c_cc[CEFU_NCCS];
    unsigned long c_ispeed;
    unsigned long c_ospeed;
};
#  if defined(__APPLE__)
#    define CEFU_TCGET 0x40487413 /* TIOCGETA */
#    define CEFU_TCSET 0x80487414 /* TIOCSETA */
#  else
#    define CEFU_TCGET 0x402c7413
#    define CEFU_TCSET 0x802c7414
#  endif
#define CEFU_ECHO   0x00000008
#define CEFU_ICANON 0x00000100
#define CEFU_ISIG   0x00000080
#define CEFU_IEXTEN 0x00000400
#define CEFU_IXON   0x00000200
#define CEFU_ICRNL  0x00000100
#define CEFU_BRKINT 0x00000002
#define CEFU_INPCK  0x00000010
#define CEFU_ISTRIP 0x00000020
#define CEFU_OPOST  0x00000001
#define CEFU_CS8    0x00000300
#define CEFU_VMIN   16
#define CEFU_VTIME  17

#endif

static struct cefu_termios g_orig_termios;
static int g_raw_active = 0;

static int cefu_term_get(int fd, struct cefu_termios *out) {
    return ioctl(fd, CEFU_TCGET, out);
}
static int cefu_term_set(int fd, const struct cefu_termios *in) {
    return ioctl(fd, CEFU_TCSET, in);
}

static void cefu_raw_disable(void) {
    if (g_raw_active) {
        cefu_term_set(STDIN_FILENO, &g_orig_termios);
        g_raw_active = 0;
    }
}

static int cefu_raw_enable(void) {
    if (cefu_term_get(STDIN_FILENO, &g_orig_termios) == -1) return -1;
    struct cefu_termios raw = g_orig_termios;

    raw.c_iflag &= ~(unsigned long)(CEFU_IXON | CEFU_ICRNL | CEFU_BRKINT | CEFU_INPCK | CEFU_ISTRIP);
    raw.c_oflag &= ~(unsigned long)(CEFU_OPOST);
    raw.c_cflag |= (unsigned long)(CEFU_CS8);
    raw.c_lflag &= ~(unsigned long)(CEFU_ECHO | CEFU_ICANON | CEFU_ISIG | CEFU_IEXTEN);
    raw.c_cc[CEFU_VMIN]  = 0;
    raw.c_cc[CEFU_VTIME] = 1; /* 100ms read timeout, lets us poll */

    if (cefu_term_set(STDIN_FILENO, &raw) == -1) return -1;
    g_raw_active = 1;
    return 0;
}

/* Tiny wrapper so we can explicitly discard write()'s return value
 * without -Wunused-result complaining; these are best-effort tty
 * control sequences, nothing sane to do if they short-write. */
static void write_ignore(int fd, const void *buf, size_t n) {
    ssize_t r = write(fd, buf, n);
    (void)r;
}

static int cefu_get_window_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        return -1;
    }
    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return 0;
}

/*
 *  //-fcurses
 *  Minimal screen library: a back-buffer of cells (char + color),
 *  flushed to the terminal via a single ANSI write per frame.
 *  No ncurses. Colors via SGR escapes.
 */

typedef enum {
    FC_DEFAULT = 0,
    FC_KEYWORD,
    FC_STRING,
    FC_COMMENT,
    FC_NUMBER,
    FC_TYPE,
    FC_PREPROC,
    FC_STATUSBAR,
    FC_LINENO,
} fc_color_t;

typedef struct {
    char       ch;
    fc_color_t color;
} fc_cell_t;

static int g_scr_rows = 24, g_scr_cols = 80;
static fc_cell_t *g_back = NULL; /* g_scr_rows * g_scr_cols */

static const char *fc_sgr(fc_color_t c) {
    switch (c) {
        case FC_KEYWORD:   return "\x1b[38;5;39m";  /* blue */
        case FC_STRING:    return "\x1b[38;5;150m"; /* green */
        case FC_COMMENT:   return "\x1b[38;5;244m"; /* gray */
        case FC_NUMBER:    return "\x1b[38;5;208m"; /* orange */
        case FC_TYPE:      return "\x1b[38;5;213m"; /* pink */
        case FC_PREPROC:   return "\x1b[38;5;178m"; /* gold */
        case FC_STATUSBAR: return "\x1b[48;5;236m\x1b[38;5;252m";
        case FC_LINENO:    return "\x1b[38;5;240m";
        default:           return "\x1b[39m";
    }
}

static int fc_init(void) {
    if (cefu_get_window_size(&g_scr_rows, &g_scr_cols) == -1) {
        g_scr_rows = 24; g_scr_cols = 80;
    }
    g_back = calloc((size_t)g_scr_rows * (size_t)g_scr_cols, sizeof(fc_cell_t));
    if (!g_back) return -1;
    for (int i = 0; i < g_scr_rows * g_scr_cols; i++) g_back[i].ch = ' ';
    write_ignore(STDOUT_FILENO, "\x1b[?1049h", 8); /* alt screen */
    write_ignore(STDOUT_FILENO, "\x1b[?25l", 6);   /* hide cursor while drawing */
    return 0;
}

static void fc_shutdown(void) {
    write_ignore(STDOUT_FILENO, "\x1b[?25h", 6);
    write_ignore(STDOUT_FILENO, "\x1b[?1049l", 8);
    free(g_back);
    g_back = NULL;
}

static void fc_resize_check(void) {
    int r, c;
    if (cefu_get_window_size(&r, &c) == 0 && (r != g_scr_rows || c != g_scr_cols)) {
        free(g_back);
        g_scr_rows = r; g_scr_cols = c;
        g_back = calloc((size_t)g_scr_rows * (size_t)g_scr_cols, sizeof(fc_cell_t));
        for (int i = 0; i < g_scr_rows * g_scr_cols; i++) g_back[i].ch = ' ';
    }
}

static void fc_clear(void) {
    for (int i = 0; i < g_scr_rows * g_scr_cols; i++) {
        g_back[i].ch = ' ';
        g_back[i].color = FC_DEFAULT;
    }
}

static void fc_put(int row, int col, char ch, fc_color_t color) {
    if (row < 0 || row >= g_scr_rows || col < 0 || col >= g_scr_cols) return;
    g_back[row * g_scr_cols + col].ch = ch;
    g_back[row * g_scr_cols + col].color = color;
}

static void fc_puts(int row, int col, const char *s, fc_color_t color) {
    while (*s && col < g_scr_cols) {
        fc_put(row, col, *s, color);
        s++; col++;
    }
}

/* Flush back-buffer to terminal in one write(), grouping runs of
 * same color to minimize escape sequences. cx/cy = cursor pos to
 * park at after drawing (-1 to leave hidden). */
static char g_flush_buf[1 << 20];
static void fc_flush(int cx, int cy) {
    size_t len = 0;
    fc_color_t last = (fc_color_t)-1;
    len += (size_t)snprintf(g_flush_buf + len, sizeof(g_flush_buf) - len, "\x1b[H");
    for (int r = 0; r < g_scr_rows; r++) {
        for (int c = 0; c < g_scr_cols; c++) {
            fc_cell_t cell = g_back[r * g_scr_cols + c];
            if (cell.color != last) {
                len += (size_t)snprintf(g_flush_buf + len, sizeof(g_flush_buf) - len,
                                        "\x1b[0m%s", fc_sgr(cell.color));
                last = cell.color;
            }
            if (len < sizeof(g_flush_buf) - 1) g_flush_buf[len++] = cell.ch;
        }
        if (r != g_scr_rows - 1) {
            len += (size_t)snprintf(g_flush_buf + len, sizeof(g_flush_buf) - len, "\r\n");
        }
    }
    len += (size_t)snprintf(g_flush_buf + len, sizeof(g_flush_buf) - len, "\x1b[0m");
    if (cx >= 0 && cy >= 0) {
        len += (size_t)snprintf(g_flush_buf + len, sizeof(g_flush_buf) - len,
                                 "\x1b[%d;%dH\x1b[?25h", cy + 1, cx + 1);
    }
    write_ignore(STDOUT_FILENO, g_flush_buf, len);
}

/* ---- raw key reading ------------------------------------------- */

#define FC_KEY_ESC       27
#define FC_KEY_UP        1000
#define FC_KEY_DOWN      1001
#define FC_KEY_LEFT      1002
#define FC_KEY_RIGHT     1003
#define FC_KEY_DEL       1004
#define FC_KEY_HOME      1005
#define FC_KEY_END       1006
#define FC_KEY_PGUP      1007
#define FC_KEY_PGDN      1008
#define FC_KEY_CTRL_PGUP 1009
#define FC_KEY_CTRL_PGDN 1010
#define FC_KEY_NONE      -1

#define CTRL_KEY(k) ((k) & 0x1f)

/* Reads one logical keypress (blocking-ish; VTIME gives 100ms
 * granularity so we loop). Decodes arrow/nav escape sequences.
 * Returns FC_KEY_NONE if no input within timeout. */
static int fc_read_key(void) {
    char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return FC_KEY_NONE;

    if (c == FC_KEY_ESC) {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return FC_KEY_ESC;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return FC_KEY_ESC;
        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                char seq2;
                if (read(STDIN_FILENO, &seq2, 1) != 1) return FC_KEY_ESC;

                /* Modified variant: ESC [ digit ; modifier ~
                 * e.g. Ctrl+PageUp = \x1b[5;5~, Ctrl+PageDown = \x1b[6;5~
                 * (modifier 5 = Ctrl in the standard xterm encoding). */
                if (seq2 == ';') {
                    char modifier, tilde;
                    if (read(STDIN_FILENO, &modifier, 1) != 1) return FC_KEY_ESC;
                    if (read(STDIN_FILENO, &tilde, 1) != 1) return FC_KEY_ESC;
                    if (tilde == '~' && modifier == '5') {
                        if (seq[1] == '5') return FC_KEY_CTRL_PGUP;
                        if (seq[1] == '6') return FC_KEY_CTRL_PGDN;
                    }
                    return FC_KEY_ESC; /* unrecognized modified sequence */
                }

                if (seq2 == '~') {
                    switch (seq[1]) {
                        case '1': return FC_KEY_HOME;
                        case '3': return FC_KEY_DEL;
                        case '4': return FC_KEY_END;
                        case '5': return FC_KEY_PGUP;
                        case '6': return FC_KEY_PGDN;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return FC_KEY_UP;
                    case 'B': return FC_KEY_DOWN;
                    case 'C': return FC_KEY_RIGHT;
                    case 'D': return FC_KEY_LEFT;
                    case 'H': return FC_KEY_HOME;
                    case 'F': return FC_KEY_END;
                }
            }
        }
        return FC_KEY_ESC;
    }
    return (unsigned char)c;
}

/*
 *  //-highlight
 *  Small table-driven tokenizers, one per language. No regex.
 *  Each highlighter fills a color-per-column array for one line,
 *  given a bit of persistent state for multi-line comments/strings.
 */

typedef enum { LANG_NONE, LANG_C, LANG_CPP, LANG_PY, LANG_LUA, LANG_FASM } lang_t;

typedef struct {
    int in_block_comment; /* carries across lines, for C-style block comments */
} hl_state_t;

static const char *c_keywords[] = {
    "if","else","for","while","do","switch","case","break","continue","return",
    "goto","struct","union","enum","typedef","sizeof","static","const","void",
    "int","char","float","double","long","short","unsigned","signed","volatile",
    "extern","inline","auto","register","default", NULL
};
static const char *cpp_keywords[] = {
    "if","else","for","while","do","switch","case","break","continue","return",
    "goto","struct","union","enum","typedef","sizeof","static","const","void",
    "int","char","float","double","long","short","unsigned","signed","volatile",
    "extern","inline","auto","register","default","class","public","private",
    "protected","virtual","namespace","template","typename","new","delete",
    "this","try","catch","throw","using","operator","friend","explicit",
    "constexpr","nullptr","override","final", NULL
};
static const char *py_keywords[] = {
    "if","elif","else","for","while","break","continue","return","def","class",
    "import","from","as","try","except","finally","raise","with","lambda",
    "pass","yield","global","nonlocal","assert","del","in","is","not","and",
    "or","True","False","None","async","await", NULL
};
static const char *lua_keywords[] = {
    "if","then","elseif","else","end","for","while","repeat","until","do",
    "function","local","return","break","in","and","or","not","nil","true",
    "false", NULL
};
static const char *fasm_keywords[] = {
    "mov","add","sub","mul","div","jmp","je","jne","jg","jl","jge","jle","cmp",
    "push","pop","call","ret","lea","xor","and","or","not","shl","shr","nop",
    "int","syscall","db","dw","dd","dq","times","dup","org","use32","use64",
    "section","segment","format","entry","include","macro","endm","if","else",
    "end","rb","rw","rd", NULL
};

static int hl_is_keyword(const char *word, const char **table) {
    for (int i = 0; table[i]; i++) if (strcmp(word, table[i]) == 0) return 1;
    return 0;
}

static lang_t hl_detect_lang(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return LANG_NONE;
    if (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0) return LANG_C;
    if (strcmp(dot, ".cpp") == 0 || strcmp(dot, ".hpp") == 0 ||
        strcmp(dot, ".cc") == 0 || strcmp(dot, ".cxx") == 0) return LANG_CPP;
    if (strcmp(dot, ".py") == 0) return LANG_PY;
    if (strcmp(dot, ".lua") == 0) return LANG_LUA;
    if (strcmp(dot, ".asm") == 0 || strcmp(dot, ".fasm") == 0 || strcmp(dot, ".inc") == 0) return LANG_FASM;
    return LANG_NONE;
}

/* Fills `colors[len]` with a color per character of `line`.
 * Handles: line comments, block comments (C/C++ only, stateful),
 * strings ("...", '...'), numbers, keywords, #preproc (C/C++). */
static void hl_line(lang_t lang, const char *line, size_t len,
                     fc_color_t *colors, hl_state_t *state) {
    for (size_t i = 0; i < len; i++) colors[i] = FC_DEFAULT;
    if (lang == LANG_NONE) return;

    const char **kw = NULL;
    switch (lang) {
        case LANG_C:   kw = c_keywords; break;
        case LANG_CPP: kw = cpp_keywords; break;
        case LANG_PY:  kw = py_keywords; break;
        case LANG_LUA: kw = lua_keywords; break;
        case LANG_FASM:kw = fasm_keywords; break;
        default: break;
    }

    size_t i = 0;

    if (state->in_block_comment) {
        while (i < len) {
            colors[i] = FC_COMMENT;
            if (i + 1 < len && line[i] == '*' && line[i+1] == '/') {
                colors[i+1] = FC_COMMENT;
                i += 2;
                state->in_block_comment = 0;
                break;
            }
            i++;
        }
    }

    while (i < len) {
        char c = line[i];

        /* line comments */
        if ((lang == LANG_C || lang == LANG_CPP) && c == '/' && i+1 < len && line[i+1] == '/') {
            for (; i < len; i++) colors[i] = FC_COMMENT;
            break;
        }
        if ((lang == LANG_PY) && c == '#') {
            for (; i < len; i++) colors[i] = FC_COMMENT;
            break;
        }
        if (lang == LANG_LUA && c == '-' && i+1 < len && line[i+1] == '-') {
            for (; i < len; i++) colors[i] = FC_COMMENT;
            break;
        }
        if (lang == LANG_FASM && c == ';') {
            for (; i < len; i++) colors[i] = FC_COMMENT;
            break;
        }

        /* block comments (C/C++) */
        if ((lang == LANG_C || lang == LANG_CPP) && c == '/' && i+1 < len && line[i+1] == '*') {
            colors[i] = FC_COMMENT; colors[i+1] = FC_COMMENT;
            i += 2;
            state->in_block_comment = 1;
            while (i < len) {
                colors[i] = FC_COMMENT;
                if (i + 1 < len && line[i] == '*' && line[i+1] == '/') {
                    colors[i+1] = FC_COMMENT;
                    i += 2;
                    state->in_block_comment = 0;
                    break;
                }
                i++;
            }
            continue;
        }

        /* preprocessor */
        if ((lang == LANG_C || lang == LANG_CPP) && c == '#' && i == 0) {
            for (; i < len; i++) colors[i] = FC_PREPROC;
            break;
        }

        /* strings */
        if (c == '"' || c == '\'') {
            char quote = c;
            colors[i++] = FC_STRING;
            while (i < len) {
                colors[i] = FC_STRING;
                if (line[i] == '\\' && i+1 < len) { colors[i+1] = FC_STRING; i += 2; continue; }
                if (line[i] == quote) { i++; break; }
                i++;
            }
            continue;
        }

        /* numbers */
        if ((c >= '0' && c <= '9') ||
            (c == '0' && i+1 < len && (line[i+1] == 'x' || line[i+1] == 'X'))) {
            while (i < len && (isalnum((unsigned char)line[i]) || line[i] == '.')) {
                colors[i] = FC_NUMBER;
                i++;
            }
            continue;
        }

        /* identifiers / keywords */
        if (isalpha((unsigned char)c) || c == '_' || c == '$' || c == '%') {
            size_t start = i;
            while (i < len && (isalnum((unsigned char)line[i]) || line[i] == '_')) i++;
            size_t wlen = i - start;
            if (wlen < 64) {
                char word[64];
                memcpy(word, line + start, wlen);
                word[wlen] = '\0';
                if (kw && hl_is_keyword(word, kw)) {
                    for (size_t j = start; j < i; j++) colors[j] = FC_KEYWORD;
                } else if (i < len && line[i] == '(') {
                    for (size_t j = start; j < i; j++) colors[j] = FC_TYPE;
                }
            }
            continue;
        }

        i++;
    }
}

/*
 *  //-editor
 *  Line buffer (dynamic array of strings), tabs (= open files),
 *  cursor, viewport scrolling, status bar, file I/O.
 */

#include <ctype.h>

typedef struct {
    char  *text;
    size_t len;
    size_t cap;
} eline_t;

typedef struct {
    eline_t *lines;
    size_t   nlines;
    size_t   cap;

    char     filename[256];
    int      has_filename;
    int      dirty;
    lang_t   lang;

    size_t   cy, cx;       /* cursor line/col */
    size_t   rowoff, coloff; /* viewport scroll */
} tab_t;

#define MAX_TABS 32
static tab_t g_tabs[MAX_TABS];
static int   g_ntabs = 0;
static int   g_curtab = 0;

#define TAB_WIDTH 4
#define GUTTER_WIDTH 5 /* line-number column */

static eline_t eline_new(const char *s, size_t len) {
    eline_t l;
    l.cap = len + 16;
    l.text = malloc(l.cap);
    l.len = len;
    if (len) memcpy(l.text, s, len);
    l.text[len] = '\0';
    return l;
}

static void tab_ensure_cap(tab_t *t, size_t need) {
    if (need <= t->cap) return;
    size_t newcap = t->cap ? t->cap * 2 : 16;
    while (newcap < need) newcap *= 2;
    t->lines = realloc(t->lines, newcap * sizeof(eline_t));
    t->cap = newcap;
}

static void tab_insert_line(tab_t *t, size_t at, const char *s, size_t len) {
    tab_ensure_cap(t, t->nlines + 1);
    memmove(&t->lines[at+1], &t->lines[at], (t->nlines - at) * sizeof(eline_t));
    t->lines[at] = eline_new(s, len);
    t->nlines++;
}

static void tab_remove_line(tab_t *t, size_t at) {
    free(t->lines[at].text);
    memmove(&t->lines[at], &t->lines[at+1], (t->nlines - at - 1) * sizeof(eline_t));
    t->nlines--;
}

static void eline_grow(eline_t *l, size_t extra) {
    if (l->len + extra + 1 <= l->cap) return;
    while (l->len + extra + 1 > l->cap) l->cap = l->cap ? l->cap * 2 : 16;
    l->text = realloc(l->text, l->cap);
}

static void eline_insert_char(eline_t *l, size_t at, char c) {
    eline_grow(l, 1);
    memmove(l->text + at + 1, l->text + at, l->len - at + 1);
    l->text[at] = c;
    l->len++;
}

static void eline_delete_char(eline_t *l, size_t at) {
    if (at >= l->len) return;
    memmove(l->text + at, l->text + at + 1, l->len - at);
    l->len--;
}

static tab_t *cur_tab(void) { return &g_tabs[g_curtab]; }

static void tab_init_empty(tab_t *t, const char *filename) {
    memset(t, 0, sizeof(*t));
    tab_insert_line(t, 0, "", 0);
    if (filename && filename[0]) {
        strncpy(t->filename, filename, sizeof(t->filename) - 1);
        t->has_filename = 1;
        t->lang = hl_detect_lang(filename);
    }
}

static int tab_load_file(tab_t *t, const char *filename) {
    FILE *f = fopen(filename, "r");
    memset(t, 0, sizeof(*t));
    strncpy(t->filename, filename, sizeof(t->filename) - 1);
    t->has_filename = 1;
    t->lang = hl_detect_lang(filename);

    if (!f) {
        tab_insert_line(t, 0, "", 0);
        return -1; /* new file, doesn't exist yet */
    }

    char *linebuf = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&linebuf, &cap, f)) != -1) {
        while (n > 0 && (linebuf[n-1] == '\n' || linebuf[n-1] == '\r')) n--;
        tab_insert_line(t, t->nlines, linebuf, (size_t)n);
    }
    free(linebuf);
    fclose(f);
    if (t->nlines == 0) tab_insert_line(t, 0, "", 0);
    return 0;
}

static int tab_save_file(tab_t *t) {
    if (!t->has_filename) return -1;
    FILE *f = fopen(t->filename, "w");
    if (!f) return -1;
    for (size_t i = 0; i < t->nlines; i++) {
        fwrite(t->lines[i].text, 1, t->lines[i].len, f);
        fputc('\n', f);
    }
    fclose(f);
    t->dirty = 0;
    return 0;
}

/* ---- cursor motions --------------------------------------------- */

static void tab_move_cursor(tab_t *t, int key) {
    eline_t *line = &t->lines[t->cy];
    switch (key) {
        case FC_KEY_LEFT:
            if (t->cx > 0) t->cx--;
            else if (t->cy > 0) { t->cy--; t->cx = t->lines[t->cy].len; }
            break;
        case FC_KEY_RIGHT:
            if (t->cx < line->len) t->cx++;
            else if (t->cy + 1 < t->nlines) { t->cy++; t->cx = 0; }
            break;
        case FC_KEY_UP:
            if (t->cy > 0) t->cy--;
            break;
        case FC_KEY_DOWN:
            if (t->cy + 1 < t->nlines) t->cy++;
            break;
        case FC_KEY_HOME:
            t->cx = 0;
            break;
        case FC_KEY_END:
            t->cx = t->lines[t->cy].len;
            break;
        default: break;
    }
    if (t->cx > t->lines[t->cy].len) t->cx = t->lines[t->cy].len;
}

/* ---- editing ops -------------------------------------------------- */

static void tab_insert_char(tab_t *t, char c) {
    eline_insert_char(&t->lines[t->cy], t->cx, c);
    t->cx++;
    t->dirty = 1;
}

static void tab_insert_newline(tab_t *t) {
    eline_t *line = &t->lines[t->cy];
    size_t tail_len = line->len - t->cx;
    tab_insert_line(t, t->cy + 1, line->text + t->cx, tail_len);
    line = &t->lines[t->cy]; /* may have moved after realloc in tab_insert_line's memmove of array (not line itself) */
    line->len = t->cx;
    line->text[line->len] = '\0';
    t->cy++;
    t->cx = 0;
    t->dirty = 1;
}

static void tab_backspace(tab_t *t) {
    if (t->cx > 0) {
        eline_delete_char(&t->lines[t->cy], t->cx - 1);
        t->cx--;
    } else if (t->cy > 0) {
        eline_t *prev = &t->lines[t->cy - 1];
        eline_t *cur  = &t->lines[t->cy];
        size_t newcx = prev->len;
        eline_grow(prev, cur->len);
        memcpy(prev->text + prev->len, cur->text, cur->len + 1);
        prev->len += cur->len;
        tab_remove_line(t, t->cy);
        t->cy--;
        t->cx = newcx;
    }
    t->dirty = 1;
}

/*
 *  //-keymap
 *  Ctrl+S save, Ctrl+O open, Ctrl+N new file, Ctrl+F find, Ctrl+Q
 *  quit — all single combos, no sequences. Plain `q` is NOT a quit
 *  key (it's a normal letter you need to type).
 *
 *  The one remaining sequence is Tab then n => new tab, kept as a
 *  tiny two-key state machine (with a timeout) since Tab must also
 *  still work as a plain tab-character insertion on its own.
 */

typedef enum {
    ACT_NONE, ACT_NEWTAB
} action_t;

static int    g_chord_len = 0;      /* 0 = idle, 1 = saw a lone Tab, waiting for 'n' */
static time_t g_chord_started = 0;
#define CHORD_TIMEOUT_SEC 2

/* Replay queue: a Tab that turned out NOT to start "Tab n" gets
 * bounced back here so it's still inserted as a literal tab. */
static int g_replay[2];
static int g_replay_len = 0;
static int g_replay_pos = 0;

/* Bounces a lone, unconsumed Tab back to the caller as a literal
 * tab-character insertion. `extra_key` is an additional key that
 * was already consumed while deciding the chord died (pass -1 if
 * there isn't one) — it must be replayed too, or it's lost. */
static void chord_queue_replay_tab(int extra_key) {
    g_replay[0] = '\t';
    if (extra_key >= 0) {
        g_replay[1] = extra_key;
        g_replay_len = 2;
    } else {
        g_replay_len = 1;
    }
    g_replay_pos = 0;
    g_chord_len = 0;
}

/* Pops a queued replay key, or FC_KEY_NONE if the queue is empty. */
static int replay_pop(void) {
    if (g_replay_pos < g_replay_len) return g_replay[g_replay_pos++];
    g_replay_len = 0;
    g_replay_pos = 0;
    return FC_KEY_NONE;
}

static int chord_starts_here(int key) {
    return key == '\t';
}

static action_t chord_feed(int key) {
    time_t now = time(NULL);
    if (g_chord_len > 0 && (now - g_chord_started) > CHORD_TIMEOUT_SEC) {
        /* Stale Tab timed out before `key` arrived: replay the tab,
         * then replay `key` too so the caller re-dispatches it fresh
         * next iteration (it may or may not start a new chord). */
        chord_queue_replay_tab(key);
        return ACT_NONE;
    }
    if (g_chord_len == 0) {
        g_chord_started = now;
        g_chord_len = 1; /* the Tab that got us here */
        return ACT_NONE;
    }
    /* g_chord_len == 1: this `key` is the one right after Tab */
    g_chord_len = 0;
    if (key == 'n') return ACT_NEWTAB;
    /* Not 'n': Tab wasn't a chord starter after all. Replay both the
     * literal tab AND this key, since `key` was already consumed
     * here and would otherwise be silently dropped. */
    chord_queue_replay_tab(key);
    return ACT_NONE;
}

static int chord_in_progress(void) {
    return g_chord_len > 0;
}

/*
 *  //-editor (rendering)
 */

static char g_status_msg[128] = "";

/* Visual column of char-index `cx` on `line`, expanding tabs by
 * TAB_WIDTH — used so horizontal scroll/cursor math is correct
 * even when the line contains real tab characters. */
static size_t line_vcol(const eline_t *line, size_t cx) {
    size_t vcol = 0;
    for (size_t c = 0; c < cx && c < line->len; c++) {
        if (line->text[c] == '\t') vcol = ((vcol / TAB_WIDTH) + 1) * TAB_WIDTH;
        else vcol++;
    }
    return vcol;
}

static void editor_scroll(tab_t *t, int text_rows) {
    if (t->cy < t->rowoff) t->rowoff = t->cy;
    if (t->cy >= t->rowoff + (size_t)text_rows) t->rowoff = t->cy - (size_t)text_rows + 1;

    int text_cols = g_scr_cols - GUTTER_WIDTH;
    size_t vcol = line_vcol(&t->lines[t->cy], t->cx);
    if (vcol < t->coloff) t->coloff = vcol;
    if (vcol >= t->coloff + (size_t)text_cols) t->coloff = vcol - (size_t)text_cols + 1;
}

static void draw_tabbar(void) {
    int col = 0;
    for (int i = 0; i < g_ntabs && col < g_scr_cols; i++) {
        const char *name = g_tabs[i].has_filename ? g_tabs[i].filename : "[No Name]";
        char label[64];
        snprintf(label, sizeof(label), " %d:%s%s ", i + 1, name, g_tabs[i].dirty ? "*" : "");
        fc_color_t color = (i == g_curtab) ? FC_STATUSBAR : FC_LINENO;
        for (const char *p = label; *p && col < g_scr_cols; p++, col++) {
            fc_put(0, col, *p, color);
        }
    }
    for (; col < g_scr_cols; col++) fc_put(0, col, ' ', FC_DEFAULT);
}

static void draw_statusbar(tab_t *t, int row) {
    char left[256], right[64];
    const char *name = t->has_filename ? t->filename : "[No Name]";
    snprintf(left, sizeof(left), " %s%s  Ln %zu, Col %zu",
             name, t->dirty ? " [+]" : "", t->cy + 1, t->cx + 1);
    snprintf(right, sizeof(right), "Yuunold 2026 (c) Cefu ");

    for (int c = 0; c < g_scr_cols; c++) fc_put(row, c, ' ', FC_STATUSBAR);
    fc_puts(row, 0, left, FC_STATUSBAR);
    int rlen = (int)strlen(right);
    fc_puts(row, g_scr_cols - rlen, right, FC_STATUSBAR);

    if (g_status_msg[0]) {
        fc_puts(row + 1, 0, g_status_msg, FC_DEFAULT);
    }
}

static void draw_editor(void) {
    fc_resize_check();
    fc_clear();
    tab_t *t = cur_tab();

    int text_rows = g_scr_rows - 3; /* tabbar(1) + statusbar(1) + msgline(1) */
    if (text_rows < 1) text_rows = 1;
    editor_scroll(t, text_rows);

    draw_tabbar();

    static hl_state_t hstate;
    memset(&hstate, 0, sizeof(hstate)); /* recompute from top each frame: simple & correct */
    fc_color_t colors[1024];

    for (int r = 0; r < text_rows; r++) {
        size_t fileline = t->rowoff + (size_t)r;
        int screenrow = r + 1;
        if (fileline < t->nlines) {
            char numbuf[16];
            snprintf(numbuf, sizeof(numbuf), "%4zu", fileline + 1);
            fc_puts(screenrow, 0, numbuf, FC_LINENO);

            eline_t *line = &t->lines[fileline];
            size_t hlen = line->len < 1024 ? line->len : 1024;
            hl_line(t->lang, line->text, hlen, colors, &hstate);

            /* Walk characters, but track a separate *screen column*
             * (vcol) since a real tab character expands to several
             * columns. t->coloff/t->cx are char-index based, so we
             * skip drawing until vcol reaches coloff, then draw. */
            int text_cols = g_scr_cols - GUTTER_WIDTH;
            size_t vcol = 0;
            for (size_t c = 0; c < line->len && (int)(vcol - t->coloff) < text_cols; c++) {
                char ch = line->text[c];
                fc_color_t col = c < hlen ? colors[c] : FC_DEFAULT;
                if (ch == '\t') {
                    size_t next_stop = ((vcol / TAB_WIDTH) + 1) * TAB_WIDTH;
                    for (; vcol < next_stop && (int)(vcol - t->coloff) < text_cols; vcol++) {
                        if (vcol >= t->coloff) {
                            fc_put(screenrow, GUTTER_WIDTH + (int)(vcol - t->coloff), ' ', col);
                        }
                    }
                } else {
                    if (vcol >= t->coloff) {
                        fc_put(screenrow, GUTTER_WIDTH + (int)(vcol - t->coloff), ch, col);
                    }
                    vcol++;
                }
            }
        } else {
            fc_put(screenrow, 0, '~', FC_LINENO);
        }
    }

    draw_statusbar(t, g_scr_rows - 2);

    /* Cursor screen column must account for tabs before t->cx,
     * same expansion rule as the render loop above. */
    size_t cursor_vcol = line_vcol(&t->lines[t->cy], t->cx);

    int cy = (int)(t->cy - t->rowoff) + 1;
    int cx = GUTTER_WIDTH + (int)(cursor_vcol - t->coloff);
    fc_flush(cx, cy);
}

/*
 *  //-editor (prompts)
 */

/* Simple blocking line prompt drawn on the message line. Used for
 * "Name your file", open-file path, new-file name, etc. */
static int prompt_line(const char *label, char *out, size_t outsz) {
    size_t len = 0;
    out[0] = '\0';
    for (;;) {
        fc_resize_check();
        fc_clear();
        draw_tabbar();
        tab_t *t = cur_tab();
        for (int r = 0; r < g_scr_rows - 3; r++) { /* keep editor visible-ish */
            (void)t;
        }
        char line[300];
        snprintf(line, sizeof(line), "%s%s", label, out);
        fc_puts(g_scr_rows - 2, 0, line, FC_STATUSBAR);
        for (int c = (int)strlen(line); c < g_scr_cols; c++) fc_put(g_scr_rows - 2, c, ' ', FC_STATUSBAR);
        fc_flush((int)strlen(line), g_scr_rows - 2);

        int key = fc_read_key();
        if (key == FC_KEY_NONE) continue;
        if (key == '\r' || key == '\n') return 1;
        if (key == FC_KEY_ESC) return 0;
        if (key == 127 || key == 8) { if (len > 0) out[--len] = '\0'; continue; }
        if (key >= 32 && key < 127 && len + 1 < outsz) {
            out[len++] = (char)key;
            out[len] = '\0';
        }
    }
}

/*
 *  //-editor (search)
 *  Ctrl+F: prompts for a needle, jumps the cursor to the first
 *  match found scanning forward from the current position (wraps
 *  around to the top of the file if nothing is found below).
 */

static void search_forward(tab_t *t, const char *needle) {
    if (!needle[0]) return;
    size_t nlen = strlen(needle);

    /* search from just after current cursor, to end of file */
    for (size_t li = t->cy; li < t->nlines; li++) {
        eline_t *line = &t->lines[li];
        size_t start = (li == t->cy) ? t->cx + 1 : 0;
        if (start > line->len) continue;
        for (size_t c = start; c + nlen <= line->len; c++) {
            if (memcmp(line->text + c, needle, nlen) == 0) {
                t->cy = li; t->cx = c;
                snprintf(g_status_msg, sizeof(g_status_msg), "Found: %s", needle);
                return;
            }
        }
    }
    /* wrap: search from top down to original position */
    for (size_t li = 0; li <= t->cy; li++) {
        eline_t *line = &t->lines[li];
        size_t maxc = (li == t->cy) ? t->cx : line->len;
        for (size_t c = 0; c + nlen <= line->len && c <= maxc; c++) {
            if (memcmp(line->text + c, needle, nlen) == 0) {
                t->cy = li; t->cx = c;
                snprintf(g_status_msg, sizeof(g_status_msg), "Found (wrapped): %s", needle);
                return;
            }
        }
    }
    snprintf(g_status_msg, sizeof(g_status_msg), "Not found: %s", needle);
}

static void search_interactive(tab_t *t) {
    char needle[128];
    if (prompt_line("Find: ", needle, sizeof(needle)) && needle[0]) {
        search_forward(t, needle);
    }
}

/*
 *  //-main
 *  Welcome screen, argv handling, main loop wiring it all together.
 */

static void print_welcome_and_exit_screen(void) {
    /* Printed to a normal (non-alt) screen, before/without raw mode,
     * as requested: `cefu` / `./cefu` with no args just shows this. */
    printf("cefu 2026 Yuunold Copyright\n");
    printf("BSD-2-Clause license\n");
    printf("welcome\n");
    printf("Name your file.\n");
}

static void die(const char *msg) {
    fc_shutdown();
    cefu_raw_disable();
    fprintf(stderr, "cefu: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static void cefu_cleanup(void) {
    fc_shutdown();
    cefu_raw_disable();
}

static void open_new_tab_interactive(void) {
    if (g_ntabs >= MAX_TABS) { snprintf(g_status_msg, sizeof(g_status_msg), "Max tabs reached"); return; }
    char name[256];
    if (prompt_line("New tab, file name: ", name, sizeof(name)) && name[0]) {
        tab_init_empty(&g_tabs[g_ntabs], name);
    } else {
        tab_init_empty(&g_tabs[g_ntabs], NULL);
    }
    g_curtab = g_ntabs;
    g_ntabs++;
}

static void open_file_interactive(void) {
    char name[256];
    if (prompt_line("Open file: ", name, sizeof(name)) && name[0]) {
        if (g_ntabs >= MAX_TABS) { snprintf(g_status_msg, sizeof(g_status_msg), "Max tabs reached"); return; }
        tab_load_file(&g_tabs[g_ntabs], name);
        g_curtab = g_ntabs;
        g_ntabs++;
    }
}

static void switch_tab(int direction) {
    if (g_ntabs <= 1) return;
    g_curtab = (g_curtab + direction + g_ntabs) % g_ntabs;
}

static void save_current_interactive(void) {
    tab_t *t = cur_tab();
    if (!t->has_filename) {
        char name[256];
        if (prompt_line("Name your file: ", name, sizeof(name)) && name[0]) {
            strncpy(t->filename, name, sizeof(t->filename) - 1);
            t->has_filename = 1;
            t->lang = hl_detect_lang(name);
        } else {
            return;
        }
    }
    if (tab_save_file(t) == 0) {
        snprintf(g_status_msg, sizeof(g_status_msg), "Saved: %s", t->filename);
    } else {
        snprintf(g_status_msg, sizeof(g_status_msg), "Save failed: %s", strerror(errno));
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_welcome_and_exit_screen();
        return 0;
    }

    if (cefu_raw_enable() == -1) die("failed to enable raw mode");
    atexit(cefu_cleanup);
    if (fc_init() == -1) die("failed to init screen");

    for (int i = 1; i < argc && g_ntabs < MAX_TABS; i++) {
        tab_load_file(&g_tabs[g_ntabs], argv[i]);
        g_ntabs++;
    }
    if (g_ntabs == 0) {
        tab_init_empty(&g_tabs[0], NULL);
        g_ntabs = 1;
    }
    g_curtab = 0;

    int running = 1;
    while (running) {
        draw_editor();

        int key;
        int from_replay = 0;
        if (g_replay_pos < g_replay_len) {
            key = replay_pop();
            from_replay = 1;
        } else {
            key = fc_read_key();
            if (key == FC_KEY_NONE) continue;
        }

        tab_t *t = cur_tab();

        /* Single-combo bindings, no sequences needed: */
        if (key == CTRL_KEY('q')) { running = 0; continue; }
        if (key == CTRL_KEY('f')) { g_status_msg[0] = '\0'; search_interactive(t); continue; }
        if (key == CTRL_KEY('s')) { g_status_msg[0] = '\0'; save_current_interactive(); continue; }
        if (key == CTRL_KEY('o')) { g_status_msg[0] = '\0'; open_file_interactive();    continue; }
        if (key == CTRL_KEY('n')) { g_status_msg[0] = '\0'; open_new_tab_interactive(); continue; }
        if (key == FC_KEY_CTRL_PGUP) { switch_tab(-1); continue; }
        if (key == FC_KEY_CTRL_PGDN) { switch_tab(+1); continue; }

        /* The only remaining sequence: Tab then n => new tab. Tab
         * alone still inserts a literal tab character (see keymap
         * section for the replay mechanism that makes that work). */
        if (!from_replay && (chord_in_progress() || chord_starts_here(key))) {
            action_t act = chord_feed(key);
            if (act == ACT_NEWTAB) {
                g_status_msg[0] = '\0';
                open_new_tab_interactive();
            }
            continue;
        }

        switch (key) {
            case FC_KEY_UP: case FC_KEY_DOWN: case FC_KEY_LEFT: case FC_KEY_RIGHT:
            case FC_KEY_HOME: case FC_KEY_END:
                tab_move_cursor(t, key);
                break;
            case '\r': case '\n':
                tab_insert_newline(t);
                break;
            case 127: case 8:
                tab_backspace(t);
                break;
            case '\t':
                tab_insert_char(t, '\t');
                break;
            default:
                if (key >= 32 && key < 127) {
                    tab_insert_char(t, (char)key);
                }
                break;
        }
    }

    for (int i = 0; i < g_ntabs; i++) {
        for (size_t l = 0; l < g_tabs[i].nlines; l++) free(g_tabs[i].lines[l].text);
        free(g_tabs[i].lines);
    }
    return 0;
}
