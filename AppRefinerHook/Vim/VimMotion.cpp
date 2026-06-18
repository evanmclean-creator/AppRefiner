// Vim motion support for AppRefiner.
// Portions of the Vim behavior are derived from NppVim
// (https://github.com/h-jangra/NppVim), licensed under the MIT License.

#include "VimMotion.h"

namespace {
    int Sci(HWND hwnd, UINT message, WPARAM wParam = 0, LPARAM lParam = 0) {
        return static_cast<int>(SendMessage(hwnd, message, wParam, lParam));
    }

    bool IsBlank(char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    }

    void RepeatCommand(HWND hwnd, UINT message, int count) {
        if (count < 1) {
            count = 1;
        }

        for (int i = 0; i < count; i++) {
            Sci(hwnd, message);
        }
    }

    void MoveCaret(HWND hwnd, int position) {
        int docLength = Sci(hwnd, SCI_GETLENGTH);
        if (position < 0) {
            position = 0;
        } else if (position > docLength) {
            position = docLength;
        }

        Sci(hwnd, SCI_SETEMPTYSELECTION, position);
        Sci(hwnd, SCI_SCROLLCARET);
    }
}

namespace VimMotion {
    void CharLeft(HWND hwnd, int count) {
        // Vim's 'h' never crosses to the previous line; clamp at line start.
        if (count < 1) count = 1;
        int pos       = Sci(hwnd, SCI_GETCURRENTPOS);
        int line      = Sci(hwnd, SCI_LINEFROMPOSITION, pos);
        int lineStart = Sci(hwnd, SCI_POSITIONFROMLINE, line);
        for (int i = 0; i < count && pos > lineStart; i++) {
            pos = Sci(hwnd, SCI_POSITIONBEFORE, pos);
        }
        if (pos < lineStart) pos = lineStart;
        MoveCaret(hwnd, pos);
    }

    void CharRight(HWND hwnd, int count) {
        // Vim's 'l' never crosses to the next line; clamp at line end
        // (the position after the last char, matching how '$' rests).
        if (count < 1) count = 1;
        int pos     = Sci(hwnd, SCI_GETCURRENTPOS);
        int line    = Sci(hwnd, SCI_LINEFROMPOSITION, pos);
        int lineEnd = Sci(hwnd, SCI_GETLINEENDPOSITION, line);
        for (int i = 0; i < count && pos < lineEnd; i++) {
            pos = Sci(hwnd, SCI_POSITIONAFTER, pos);
        }
        if (pos > lineEnd) pos = lineEnd;
        MoveCaret(hwnd, pos);
    }

    void LineUp(HWND hwnd, int count) {
        RepeatCommand(hwnd, SCI_LINEUP, count);
    }

    void LineDown(HWND hwnd, int count) {
        RepeatCommand(hwnd, SCI_LINEDOWN, count);
    }

    void WordRight(HWND hwnd, int count) {
        RepeatCommand(hwnd, SCI_WORDRIGHT, count);
    }

    void WordLeft(HWND hwnd, int count) {
        RepeatCommand(hwnd, SCI_WORDLEFT, count);
    }

    void WordEnd(HWND hwnd, int count) {
        RepeatCommand(hwnd, SCI_WORDRIGHTEND, count);
    }

    void WordRightBig(HWND hwnd, int count) {
        int docLength = Sci(hwnd, SCI_GETLENGTH);
        int position = Sci(hwnd, SCI_GETCURRENTPOS);
        if (count < 1) {
            count = 1;
        }

        for (int i = 0; i < count; i++) {
            while (position < docLength && !IsBlank(static_cast<char>(Sci(hwnd, SCI_GETCHARAT, position)))) {
                position++;
            }
            while (position < docLength && IsBlank(static_cast<char>(Sci(hwnd, SCI_GETCHARAT, position)))) {
                position++;
            }
        }

        MoveCaret(hwnd, position);
    }

    void WordLeftBig(HWND hwnd, int count) {
        int position = Sci(hwnd, SCI_GETCURRENTPOS);
        if (count < 1) {
            count = 1;
        }

        for (int i = 0; i < count; i++) {
            if (position <= 0) {
                break;
            }

            position--;
            while (position > 0 && IsBlank(static_cast<char>(Sci(hwnd, SCI_GETCHARAT, position)))) {
                position--;
            }
            while (position > 0 && !IsBlank(static_cast<char>(Sci(hwnd, SCI_GETCHARAT, position - 1)))) {
                position--;
            }
        }

        MoveCaret(hwnd, position);
    }

    void WordEndBig(HWND hwnd, int count) {
        int docLength = Sci(hwnd, SCI_GETLENGTH);
        int position = Sci(hwnd, SCI_GETCURRENTPOS);
        if (count < 1) {
            count = 1;
        }

        for (int i = 0; i < count; i++) {
            if (position < docLength) {
                position++;
            }

            while (position < docLength && IsBlank(static_cast<char>(Sci(hwnd, SCI_GETCHARAT, position)))) {
                position++;
            }
            while (position < docLength && !IsBlank(static_cast<char>(Sci(hwnd, SCI_GETCHARAT, position)))) {
                position++;
            }

            if (position > 0) {
                position--;
            }
        }

        MoveCaret(hwnd, position);
    }

    void LineStart(HWND hwnd) {
        int position = Sci(hwnd, SCI_GETCURRENTPOS);
        int line = Sci(hwnd, SCI_LINEFROMPOSITION, position);
        MoveCaret(hwnd, Sci(hwnd, SCI_POSITIONFROMLINE, line));
    }

    void FirstNonWhitespace(HWND hwnd) {
        int position = Sci(hwnd, SCI_GETCURRENTPOS);
        int line = Sci(hwnd, SCI_LINEFROMPOSITION, position);
        int lineStart = Sci(hwnd, SCI_POSITIONFROMLINE, line);
        int lineEnd = Sci(hwnd, SCI_GETLINEENDPOSITION, line);

        int target = lineStart;
        while (target < lineEnd) {
            char ch = static_cast<char>(Sci(hwnd, SCI_GETCHARAT, target));
            if (ch != ' ' && ch != '\t') {
                break;
            }
            target++;
        }

        MoveCaret(hwnd, target);
    }

    void LineEnd(HWND hwnd, int count) {
        RepeatCommand(hwnd, SCI_LINEEND, count);
    }

    void ParagraphUp(HWND hwnd, int count) {
        RepeatCommand(hwnd, SCI_PARAUP, count);
    }

    void ParagraphDown(HWND hwnd, int count) {
        RepeatCommand(hwnd, SCI_PARADOWN, count);
    }

    void PageUp(HWND hwnd, int count) {
        RepeatCommand(hwnd, SCI_PAGEUP, count);
    }

    void PageDown(HWND hwnd, int count) {
        RepeatCommand(hwnd, SCI_PAGEDOWN, count);
    }

    void MatchPair(HWND hwnd) {
        int position = Sci(hwnd, SCI_GETCURRENTPOS);
        int match = Sci(hwnd, SCI_BRACEMATCH, position);
        if (match == -1 && position > 0) {
            match = Sci(hwnd, SCI_BRACEMATCH, position - 1);
        }

        if (match != -1) {
            MoveCaret(hwnd, match);
        }
    }

    int FindCharPos(HWND hwnd, char op, char ch, int count) {
        bool forward  = (op == 'f' || op == 't');
        int  pos      = Sci(hwnd, SCI_GETCURRENTPOS);
        int  line     = Sci(hwnd, SCI_LINEFROMPOSITION, pos);
        int  lineEnd  = Sci(hwnd, SCI_GETLINEENDPOSITION, line);
        int  docLen   = Sci(hwnd, SCI_GETLENGTH);
        int  hits     = 0;

        if (forward) {
            for (int p = Sci(hwnd, SCI_POSITIONAFTER, pos); p <= lineEnd && p < docLen;
                 p = Sci(hwnd, SCI_POSITIONAFTER, p)) {
                if (static_cast<char>(Sci(hwnd, SCI_GETCHARAT, p)) == ch) {
                    if (++hits == count) {
                        // 't' lands one cell before the found char
                        return (op == 't') ? Sci(hwnd, SCI_POSITIONBEFORE, p) : p;
                    }
                }
            }
        } else {
            int lineStart = Sci(hwnd, SCI_POSITIONFROMLINE, line);
            int p = Sci(hwnd, SCI_POSITIONBEFORE, pos);
            while (p >= lineStart) {
                if (static_cast<char>(Sci(hwnd, SCI_GETCHARAT, p)) == ch) {
                    if (++hits == count) {
                        // 'T' lands one cell after the found char
                        return (op == 'T') ? Sci(hwnd, SCI_POSITIONAFTER, p) : p;
                    }
                }
                if (p == lineStart) break;
                p = Sci(hwnd, SCI_POSITIONBEFORE, p);
            }
        }
        return -1;
    }

    bool FindChar(HWND hwnd, char op, char ch, int count) {
        int dest = FindCharPos(hwnd, op, ch, count);
        if (dest < 0) return false;
        MoveCaret(hwnd, dest);
        return true;
    }
}
