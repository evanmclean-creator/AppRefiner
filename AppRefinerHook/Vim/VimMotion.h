// Vim motion support for AppRefiner.
// Portions of the Vim behavior are derived from NppVim
// (https://github.com/h-jangra/NppVim), licensed under the MIT License.

#pragma once

#include "../Common.h"

namespace VimMotion {
    void CharLeft(HWND hwnd, int count);
    void CharRight(HWND hwnd, int count);
    void LineUp(HWND hwnd, int count);
    void LineDown(HWND hwnd, int count);
    void WordRight(HWND hwnd, int count);
    void WordRightBig(HWND hwnd, int count);
    void WordLeft(HWND hwnd, int count);
    void WordLeftBig(HWND hwnd, int count);
    void WordEnd(HWND hwnd, int count);
    void WordEndBig(HWND hwnd, int count);
    void LineStart(HWND hwnd);
    void FirstNonWhitespace(HWND hwnd);
    void LineEnd(HWND hwnd, int count);
    void ParagraphUp(HWND hwnd, int count);
    void ParagraphDown(HWND hwnd, int count);
    void PageUp(HWND hwnd, int count);
    void PageDown(HWND hwnd, int count);
    void MatchPair(HWND hwnd);

    // Find-char motions (f/F/t/T).
    // op = 'f' (forward-to), 'F' (backward-to), 't' (forward-until), 'T' (backward-until).
    // Returns the destination position, or -1 if the character is not found.
    // Does NOT move the cursor.
    int FindCharPos(HWND hwnd, char op, char ch, int count);

    // Executes the find-char motion (moves the cursor). Returns true if found.
    bool FindChar(HWND hwnd, char op, char ch, int count);
}
