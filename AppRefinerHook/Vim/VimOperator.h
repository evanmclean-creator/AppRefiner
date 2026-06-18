// Vim operator implementations for AppRefiner.
// Portions of the Vim behavior are derived from NppVim
// (https://github.com/h-jangra/NppVim), licensed under the MIT License.

#pragma once

#include "../Common.h"
#include <utility>

struct VimEditorState;
struct VimSessionState;

namespace VimOperator {
    // Resolve the bounds for a text object as an exclusive [start, end) range.
    std::pair<int, int> GetTextObjectBounds(HWND hwnd, char modifier, char obj);

    // Apply operator (d/c/y) to a motion character.
    // count is the repeat count for the motion itself.
    void ApplyToMotion(HWND hwnd, char op, char motion, int count,
                       VimEditorState& state, VimSessionState& session);

    // Apply operator (d/c/y) to a text object.
    // modifier is 'i' (inner) or 'a' (around).
    // obj is the object char: w W s p " ' ` ( [ { <
    void ApplyToTextObject(HWND hwnd, char op, char modifier, char obj,
                           VimEditorState& state, VimSessionState& session);

    // dd: delete count full lines.
    void DeleteLines(HWND hwnd, int count, VimEditorState& state, VimSessionState& session);

    // yy: yank count full lines (cursor does not move).
    void YankLines(HWND hwnd, int count, VimEditorState& state, VimSessionState& session);

    // p: paste after cursor (linewise: below; charwise: after cursor).
    void PasteAfter(HWND hwnd, VimEditorState& state, VimSessionState& session);

    // P: paste before cursor (linewise: above; charwise: before cursor).
    void PasteBefore(HWND hwnd, VimEditorState& state, VimSessionState& session);

    // r<ch>: replace the character under the cursor with ch.
    void ReplaceChar(HWND hwnd, char ch, VimEditorState& state, VimSessionState& session);

    // .: repeat the last recorded operation.
    void RepeatLastOp(HWND hwnd, VimEditorState& state, VimSessionState& session);

    // Apply operator (d/c/y) to an explicit character range [start, end).
    // Used by find-char operators (df<c>, cf<c>, yf<c>) and visual-mode operators.
    void ApplyOpToRange(HWND hwnd, char op, int start, int end,
                        bool linewise, VimEditorState& state, VimSessionState& session);

    // Apply operator (d/c/y) to the current visual selection.
    void ApplyToSelection(HWND hwnd, char op, bool linewise,
                          VimEditorState& state, VimSessionState& session);
}
