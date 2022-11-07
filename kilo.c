/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

enum editorHighlight {
  HL_NORMAL = 0,
  HL_COMMENT,
  HL_STRING,
  HL_NUMBER,
  HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*** data ***/

struct editorSyntax {
  char *filetype;
  char **filematch;
  char *singleLineCommentStart;
  int flags;
};

typedef struct editorRow {
  int size;
  int renderSize;
  char *chars;
  char *render;
  unsigned char *highlight;
} editorRow;

struct editorConfig {
  int cursorX, cursorY;
  int renderX;
  int rowOffset, colOffset;
  int screenRows, screenCols;
  int numRows;
  editorRow *row;
  int dirty;
  char *filename;
  char statusMsg[80];
  time_t statusMsgTime;
  struct editorSyntax *syntax;
  struct termios orig_termios;
};

struct editorConfig E;

/*** filetypes ***/

char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL };

struct editorSyntax HLDB[] = {
  {
    "c",
    C_HL_extensions,
    "//",
    HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
  },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** terminal ***/

void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}

void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsettattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= ~(CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
    if (nread == -1 && errno != EAGAIN) die("read");

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
    if (seq[2] == '~') {
      switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
        case '5': return PAGE_UP;
        case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
      }
    }
      } else {
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
    case 'F': return END_KEY;
      }
    }

    return '\x1b';
  } else
    return c;
}

int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

  return 0;
}

int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** syntax highlighting ***/

int isSeparator(int c) {
  return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void editorUpdateSyntax(editorRow *row) {
  row->highlight = realloc(row->highlight, row->renderSize);
  memset(row->highlight, HL_NORMAL, row->renderSize);

  if (E.syntax == NULL) return;

  char *scs = E.syntax->singleLineCommentStart;
  int scsLen = scs ? strlen(scs) : 0;

  int prevSeparator = 1;
  int inString = 0;

  int i = 0;
  while (i < row->renderSize) {
    char c = row->render[i];
    unsigned char prevHighlight = (i > 0) ? row->highlight[i - 1] : HL_NORMAL;

    if (scsLen && !inString) {
      if (!strncmp(&row->render[i], scs, scsLen)) {
        memset(&row->highlight[i], HL_COMMENT, row->renderSize - i);
        break;
      }
    }

    if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
      if (inString) {
        row->highlight[i] = HL_STRING;
        if (c == '\\' && i + 1 < row->renderSize) {
          row->highlight[i + 1] = HL_STRING;
          i += 2;
          continue;
        }
        if (c == inString) inString = 0;
        i++;
        prevSeparator = 1;
        continue;
      } else {
        if (c == '"' || c == '\'') {
          inString = c;
          row->highlight[i] = HL_STRING;
          i++;
          continue;
        }
      }
    }

    if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
      if ((isdigit(c) && (prevSeparator || prevHighlight == HL_NUMBER)) ||
          (c == '.' && prevHighlight == HL_NUMBER)) {
        row->highlight[i] = HL_NUMBER;
        i++;
        prevSeparator = 0;
        continue;
      }
    }

    prevSeparator = isSeparator(c);
    i++;
  }
}

int editorSyntaxToColor(int highlight) {
  switch (highlight) {
    case HL_COMMENT: return 36;
    case HL_STRING: return 35;
    case HL_NUMBER: return 31;
    case HL_MATCH: return 34;
    default: return 37;
  }
}

void editorSelectSyntaxHighlight() {
  E.syntax = NULL;
  if (E.filename == NULL) return;

  char *ext = strrchr(E.filename, '.');

  for (unsigned int i = 0; i < HLDB_ENTRIES; i++) {
    struct editorSyntax *s = &HLDB[i];
    unsigned int j = 0;
    while (s->filematch[j]) {
      int isExt = (s->filematch[j][0] == '.');
      if ((isExt && ext && !strcmp(ext, s->filematch[j])) ||
          (!isExt && strstr(E.filename, s->filematch[j]))) {
        E.syntax = s;

        for (int fileRow = 0; fileRow < E.numRows; fileRow++)
          editorUpdateSyntax(&E.row[fileRow]);

        return;
      }
      j++;
    }
  }
}

/*** row operations ***/

int editorRowCxToRx(editorRow *row, int cursorX) {
  int renderX = 0;
  for (int j = 0; j < cursorX; j++) {
    if (row->chars[j] == '\t')
      renderX += (KILO_TAB_STOP - 1) - (renderX % KILO_TAB_STOP);
    renderX++;
  }
  return renderX;
}

int editorRowRxToCx(editorRow *row, int renderX) {
  int currentRenderX = 0;
  int cursorX;
  for (cursorX = 0; cursorX < row->size; cursorX++) {
    if (row->chars[cursorX] == '\t')
      currentRenderX += (KILO_TAB_STOP - 1) - (currentRenderX % KILO_TAB_STOP);
    currentRenderX++;

    if (currentRenderX > renderX) return cursorX;
  }
  return cursorX;
}

void editorUpdateRow(editorRow *row) {
  int tabs = 0;
  for (int j = 0; j < row->size; j++)
    if (row->chars[j] == '\t') tabs++;

  free(row->render);
  row->render = malloc(row->size + (tabs * (KILO_TAB_STOP - 1)) + 1);

  int idx = 0;
  for (int j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
    } else
      row->render[idx++] = row->chars[j];
  }

  row->render[idx] = '\0';
  row->renderSize = idx;

  editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numRows) return;

  E.row = realloc(E.row, sizeof(editorRow) * (E.numRows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(editorRow) * (E.numRows - at));

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].renderSize = 0;
  E.row[at].render = NULL;
  E.row[at].highlight = NULL;
  editorUpdateRow(&E.row[at]);

  E.numRows++;
  E.dirty++;
}

void editorFreeRow(editorRow *row) {
  free(row->render);
  free(row->chars);
  free(row->highlight);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numRows) return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(editorRow) * (E.numRows - at - 1));
  E.numRows--;
  E.dirty++;
}

void editorRowInsertChar(editorRow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowAppendString(editorRow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowDelChar(editorRow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c) {
  if (E.cursorY == E.numRows)
    editorInsertRow(E.numRows, "", 0);
  editorRowInsertChar(&E.row[E.cursorY], E.cursorX, c);
  E.cursorX++;
}

void editorInsertNewLine() {
  if (E.cursorX == 0)
    editorInsertRow(E.cursorY, "", 0);
  else {
    editorRow *row = &E.row[E.cursorY];
    editorInsertRow(E.cursorY + 1, &row->chars[E.cursorX], row->size - E.cursorX);
    row = &E.row[E.cursorY];
    row->size = E.cursorX;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }
  E.cursorY++;
  E.cursorX = 0;
}

void editorDelChar() {
  if (E.cursorY == E.numRows) return;
  if (E.cursorX == 0 && E.cursorY == 0) return;

  editorRow *row = &E.row[E.cursorY];
  if (E.cursorX > 0) {
    editorRowDelChar(row, E.cursorX - 1);
    E.cursorX--;
  } else {
    E.cursorX = E.row[E.cursorY - 1].size;
    editorRowAppendString(&E.row[E.cursorY - 1], row->chars, row->size);
    editorDelRow(E.cursorY);
    E.cursorY--;
  }
}

/*** file i/o ***/

char *editorRowsToString(int *bufLen) {
  int totLen = 0;
  for (int i = 0; i < E.numRows; i++)
    totLen += E.row[i].size + 1;
  *bufLen = totLen;

  char *buf = malloc(totLen);
  char *p = buf;
  for (int i = 0; i < E.numRows; i++) {
    memcpy(p, E.row[i].chars, E.row[i].size);
    p += E.row[i].size;
    *p = '\n';
    p++;
  }

  return buf;
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  editorSelectSyntaxHighlight();

  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' ||
                           line[linelen - 1] == '\r'))
      linelen--;
    editorInsertRow(E.numRows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editorSave() {
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
    if (E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
    editorSelectSyntaxHighlight();
  }

  int len;
  char *buf = editorRowsToString(&len);

  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }

  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

void editorFindCallback(char *query, int key) {
  static int last_match = -1, direction = 1;

  static int saved_highlight_line;
  static char *saved_highlight = NULL;

  if (saved_highlight) {
    memcpy(E.row[saved_highlight_line].highlight, saved_highlight,
        E.row[saved_highlight_line].renderSize);
    free(saved_highlight);
    saved_highlight = NULL;
  }

  if (key == '\r' || key == '\x1b') {
    last_match = -1;
    direction = 1;
    return;
  } else if (key == ARROW_RIGHT || key == ARROW_DOWN)
    direction = 1;
  else if (key == ARROW_LEFT || key == ARROW_UP)
    direction = -1;
  else {
    last_match = -1;
    direction = 1;
  }

  if (last_match == -1) direction = 1;
  int current = last_match;
  for (int i = 0; i < E.numRows; i++) {
    current += direction;
    if (current == -1) current = E.numRows - 1;
    else if (current == E.numRows) current = 0;

    editorRow *row = &E.row[current];
    char *match = strstr(row->render, query);
    if (match) {
      last_match = current;
      E.cursorY = current;
      E.cursorX = editorRowRxToCx(row, match - row->render);
      E.rowOffset = E.numRows;

      saved_highlight_line = current;
      saved_highlight = malloc(row->renderSize);
      memcpy(saved_highlight, row->highlight, row->renderSize);
      memset(&row->highlight[match - row->render], HL_MATCH, strlen(query));
      break;
    }
  }
}


void editorFind() {
  int savedCursorX = E.cursorX, savedCursorY = E.cursorY;
  int savedColOffset = E.colOffset, savedRowOffset = E.rowOffset;

  char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)",
                             editorFindCallback);

  if (query)
    free(query);
  else {
    E.cursorX = savedCursorX;
    E.cursorY = savedCursorY;
    E.colOffset = savedColOffset;
    E.rowOffset = savedRowOffset;
  }
}

/*** append buffer ***/

struct appendBuffer {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct appendBuffer *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

void abFree(struct appendBuffer *ab) {
  free(ab->b);
}

/*** output ***/

void editorScroll() {
  E.renderX = 0;
  if (E.cursorY < E.numRows)
    E.renderX = editorRowCxToRx(&E.row[E.cursorY], E.cursorX);

  if (E.cursorY < E.rowOffset)
    E.rowOffset = E.cursorY;
  if (E.cursorY >= E.rowOffset + E.screenRows)
    E.rowOffset = E.cursorY - E.screenRows + 1;
  if (E.renderX < E.colOffset)
    E.colOffset = E.renderX;
  if (E.renderX >= E.colOffset + E.screenCols)
    E.colOffset = E.renderX - E.screenCols + 1;
}

void editorDrawRows(struct appendBuffer *ab) {
  for (int y = 0; y < E.screenRows; y++) {
    int fileRow = y + E.rowOffset;
    if (fileRow >= E.numRows) {
      if (E.numRows == 0 && y == E.screenRows / 3) {
        char welcome[80];
        int welcomeLen = snprintf(welcome, sizeof(welcome),
          "Kilo editor -- version %s", KILO_VERSION);
        if (welcomeLen > E.screenCols) welcomeLen = E.screenCols;
        int padding = (E.screenCols - welcomeLen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
      padding --;
        }
        while (padding--) abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomeLen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int len = E.row[fileRow].renderSize - E.colOffset;
      if (len < 0) len = 0;
      if (len > E.screenCols) len = E.screenCols;
      char *c = &E.row[fileRow].render[E.colOffset];
      unsigned char *highlight = &E.row[fileRow].highlight[E.colOffset];
      int currentColor = -1;
      for (int i = 0; i < len; i++) {
        if (highlight[i] == HL_NORMAL) {
          if (currentColor != -1) {
            abAppend(ab, "\x1b[39m", 5);
            currentColor = -1;
          }
	  abAppend(ab, &c[i], 1);
        } else {
          int color = editorSyntaxToColor(highlight[i]);
          if (color != currentColor) {
            currentColor = color;
            char buf[16];
            int colorLen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
            abAppend(ab, buf, colorLen);
          }
          abAppend(ab, &c[i], 1);
        }
      }
      abAppend(ab, "\x1b[39m", 5);
    }

    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct appendBuffer *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80], renderStatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
      E.filename ? E.filename : "[No Name]", E.numRows,
      E.dirty ? "(modified)" : "");
  int renderLen = snprintf(renderStatus, sizeof(renderStatus), "%s | %d/%d",
      E.syntax ? E.syntax->filetype : "no filetype", E.cursorY + 1, E.numRows);
  if (len > E.screenCols) len = E.screenCols;
  abAppend(ab, status, len);
  while (len < E.screenCols) {
    if (E.screenCols - len == renderLen) {
      abAppend(ab, renderStatus, renderLen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct appendBuffer *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msgLen = strlen(E.statusMsg);
  if (msgLen > E.screenCols) msgLen = E.screenCols;
  if (msgLen && time(NULL) - E.statusMsgTime < 5)
    abAppend(ab, E.statusMsg, msgLen);
}

void editorRefreshScreen() {
  editorScroll();

  struct appendBuffer ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cursorY - E.rowOffset) + 1,
                                            (E.renderX - E.colOffset) + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusMsg, sizeof(E.statusMsg), fmt, ap);
  va_end(ap);
  E.statusMsgTime = time(NULL);
}

/*** input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
  size_t bufSize = 128;
  char *buf = malloc(bufSize);

  size_t bufLen = 0;
  buf[0] = '\0';

  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();

    int c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (bufLen != 0) buf[--bufLen] = '\0';
    } else if (c == '\x1b') {
      editorSetStatusMessage("");
      if (callback) callback(buf, c);
      free(buf);
      return NULL;
    } else if (c == '\r') {
      if (bufLen != 0) {
        editorSetStatusMessage("");
        if (callback) callback(buf, c);
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (bufLen == bufSize - 1) {
        bufSize *= 2;
        buf = realloc(buf, bufSize);
      }
      buf[bufLen++] = c;
      buf[bufLen] = '\0';
    }
    
    if (callback) callback(buf, c);
  }
}

void editorMoveCursor(int key) {
  editorRow *row = (E.cursorY >= E.numRows) ? NULL : &E.row[E.cursorY];

  switch (key) {
    case ARROW_LEFT:
      if (E.cursorX != 0) E.cursorX--;
      else if (E.cursorY > 0) {
        E.cursorY--;
        E.cursorX = E.row[E.cursorY].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cursorX < row->size) E.cursorX++;
      else if (row && E.cursorX == row->size) {
        E.cursorY++;
        E.cursorX = 0;
      }
      break;
    case ARROW_UP:
      if (E.cursorY != 0) E.cursorY--;
      break;
    case ARROW_DOWN:
      if (E.cursorY < E.numRows) E.cursorY++;
      break;
  }

  row = (E.cursorY >= E.numRows) ? NULL : &E.row[E.cursorY];
  int rowLen = row ? row->size : 0;
  if (E.cursorX > rowLen) E.cursorX = rowLen;
}

void editorProcessKeypress() {
  static int quit_times = KILO_QUIT_TIMES;

  int c = editorReadKey();

  switch (c) {
    case '\r':
      editorInsertNewLine();
      break;

    case CTRL_KEY('q'):
      if (E.dirty && quit_times > 0) {
        editorSetStatusMessage("WARNING!!! File has unsaved changes. "
            "Press Ctrl-Q %d more times to quit.", quit_times);
        quit_times--;
        return;
      }
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case CTRL_KEY('s'):
      editorSave();
      break;

    case HOME_KEY:
      E.cursorX = 0;
      break;

    case END_KEY:
      if (E.cursorY < E.numRows)
        E.cursorX = E.row[E.cursorY].size;
      break;

    case CTRL_KEY('f'):
      editorFind();
      break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
      editorDelChar();
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        if (c == PAGE_UP)
          E.cursorY = E.rowOffset;
        else if (c == PAGE_DOWN) {
          E.cursorY = E.rowOffset + E.screenRows - 1;
          if (E.cursorY > E.numRows) E.cursorY = E.numRows;
        }

        int times = E.screenRows;
        while (times--)
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;

    case CTRL_KEY('l'):
    case '\x1b':
      break;

    default:
      editorInsertChar(c);
      break;
  }

  quit_times = KILO_QUIT_TIMES;
}

/*** init ***/

void initEditor() {
  E.cursorX = 0, E.cursorY = 0;
  E.renderX = 0;
  E.rowOffset = 0, E.colOffset = 0;
  E.numRows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusMsg[0] = '\0';
  E.statusMsgTime = 0;
  E.syntax = NULL;

  if (getWindowSize(&E.screenRows, &E.screenCols) == -1) die("getWindowSize");
  E.screenRows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2)
    editorOpen(argv[1]);

  editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}
