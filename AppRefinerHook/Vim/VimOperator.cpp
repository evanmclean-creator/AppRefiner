// Vim operator implementations for AppRefiner.
// Portions of the Vim behavior are derived from NppVim
// (https://github.com/h-jangra/NppVim), licensed under the MIT License.

#include "VimOperator.h"
#include "VimMode.h"
#include "VimMotion.h"
#include "../Scintilla.h"

#include <string>
#include <algorithm>

namespace {
    int Sci(HWND hwnd, UINT msg, WPARAM wParam = 0, LPARAM lParam = 0) {
        return static_cast<int>(::SendMessage(hwnd, msg, wParam, lParam));
    }

    // Get the text between two byte positions.
    std::string GetTextRange(HWND hwnd, int start, int end) {
        if (start >= end) return {};
        int len = end - start;
        std::string buf(len + 1, '\0');
        Sci_TextRange tr;
        tr.chrg.cpMin = start;
        tr.chrg.cpMax = end;
        tr.lpstrText  = &buf[0];
        ::SendMessage(hwnd, SCI_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&tr));
        buf.resize(len);
        return buf;
    }

    // Store text in the OS clipboard.
    void SetClipboard(const std::string& text) {
        if (!::OpenClipboard(nullptr)) return;
        ::EmptyClipboard();
        HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
        if (hMem) {
            char* ptr = static_cast<char*>(::GlobalLock(hMem));
            if (ptr) {
                std::copy(text.begin(), text.end(), ptr);
                ptr[text.size()] = '\0';
                ::GlobalUnlock(hMem);
                ::SetClipboardData(CF_TEXT, hMem);
            }
        }
        ::CloseClipboard();
    }

    // Read text from the OS clipboard.
    std::string GetClipboard() {
        std::string result;
        if (!::OpenClipboard(nullptr)) return result;
        HANDLE hData = ::GetClipboardData(CF_TEXT);
        if (hData) {
            const char* ptr = static_cast<const char*>(::GlobalLock(hData));
            if (ptr) {
                result = ptr;
                ::GlobalUnlock(hData);
            }
        }
        ::CloseClipboard();
        return result;
    }

    // Select a byte range in Scintilla (anchor=start, caret=end).
    void SelectRange(HWND hwnd, int start, int end) {
        Sci(hwnd, SCI_SETSEL, start, end);
    }

    // Register helpers — route through state.activeRegister when set.
    // SetReg writes text/linewise and resets activeRegister to 0.
    void SetReg(VimEditorState& state, VimSessionState& session,
                const std::string& text, bool linewise) {
        char reg = state.activeRegister;
        state.activeRegister = 0;
        if (reg >= 'a' && reg <= 'z') {
            session.namedRegisters[reg - 'a'].text     = text;
            session.namedRegisters[reg - 'a'].linewise = linewise;
        } else {
            session.yankRegister = text;
            session.yankLinewise = linewise;
        }
    }

    // GetReg reads text/linewise from active register (or default), then resets activeRegister.
    void GetReg(VimEditorState& state, VimSessionState& session,
                std::string& outText, bool& outLinewise) {
        char reg = state.activeRegister;
        state.activeRegister = 0;
        if (reg >= 'a' && reg <= 'z') {
            outText     = session.namedRegisters[reg - 'a'].text;
            outLinewise = session.namedRegisters[reg - 'a'].linewise;
        } else {
            outText     = session.yankRegister.empty() ? GetClipboard() : session.yankRegister;
            outLinewise = session.yankLinewise;
        }
    }

    // Get full line range: returns {lineStart, lineEnd+1} including the newline char.
    // count lines starting at the current cursor line.
    std::pair<int,int> GetLineRangeForDelete(HWND hwnd, int count) {
        int pos       = Sci(hwnd, SCI_GETCURRENTPOS);
        int firstLine = Sci(hwnd, SCI_LINEFROMPOSITION, pos);
        int lastLine  = firstLine + count - 1;
        int lineCount = Sci(hwnd, SCI_GETLINECOUNT);
        if (lastLine >= lineCount) lastLine = lineCount - 1;

        int start = Sci(hwnd, SCI_POSITIONFROMLINE, firstLine);

        // Include the newline so the blank line disappears after deletion.
        // If this is the last line, take to document end instead.
        int end;
        if (lastLine + 1 < lineCount) {
            end = Sci(hwnd, SCI_POSITIONFROMLINE, lastLine + 1);
        } else {
            // Last line — delete from line start to document end,
            // but also eat the preceding newline so we don't leave a trailing blank.
            end = Sci(hwnd, SCI_GETLENGTH);
            if (firstLine > 0) {
                start = Sci(hwnd, SCI_GETLINEENDPOSITION, firstLine - 1);
            }
        }
        return {start, end};
    }

    // Execute a motion and return the new caret position without keeping the move.
    int GetMotionEndPos(HWND hwnd, char motion, int count) {
        int before = Sci(hwnd, SCI_GETCURRENTPOS);
        switch (motion) {
        case 'h': VimMotion::CharLeft(hwnd, count);        break;
        case 'l': VimMotion::CharRight(hwnd, count);       break;
        case 'j': VimMotion::LineDown(hwnd, count);        break;
        case 'k': VimMotion::LineUp(hwnd, count);          break;
        case 'w': VimMotion::WordRight(hwnd, count);       break;
        case 'W': VimMotion::WordRightBig(hwnd, count);    break;
        case 'b': VimMotion::WordLeft(hwnd, count);        break;
        case 'B': VimMotion::WordLeftBig(hwnd, count);     break;
        case 'e': VimMotion::WordEnd(hwnd, count);         break;
        case 'E': VimMotion::WordEndBig(hwnd, count);      break;
        case '$': VimMotion::LineEnd(hwnd, count);         break;
        case '^': VimMotion::FirstNonWhitespace(hwnd);     break;
        case '0': VimMotion::LineStart(hwnd);              break;
        case '{': VimMotion::ParagraphUp(hwnd, count);     break;
        case '}': VimMotion::ParagraphDown(hwnd, count);   break;
        case 'G': {
            int lineCount = Sci(hwnd, SCI_GETLINECOUNT);
            int targetLine = (count > 1) ? count - 1 : lineCount - 1;
            Sci(hwnd, SCI_GOTOLINE, targetLine);
            break;
        }
        case '%': VimMotion::MatchPair(hwnd); break;
        default:  break;
        }
        return Sci(hwnd, SCI_GETCURRENTPOS);
    }

    bool IsLineMotion(char motion) {
        return motion == 'j' || motion == 'k' || motion == 'G' ||
               motion == '{' || motion == '}';
    }

    // Find a matching bracket on the current line searching from pos.
    // direction: +1 = forward, -1 = backward.
    // open/close are the bracket chars (e.g. '(' / ')').
    // Returns -1 if not found.
    int FindBracketOnLine(HWND hwnd, int startPos, char open, char close, bool searchForward) {
        int docLen = Sci(hwnd, SCI_GETLENGTH);
        int line   = Sci(hwnd, SCI_LINEFROMPOSITION, startPos);
        int lineStart = Sci(hwnd, SCI_POSITIONFROMLINE, line);
        int lineEnd   = Sci(hwnd, SCI_GETLINEENDPOSITION, line);

        if (searchForward) {
            for (int p = startPos; p <= lineEnd && p < docLen; p = Sci(hwnd, SCI_POSITIONAFTER, p)) {
                char ch = static_cast<char>(Sci(hwnd, SCI_GETCHARAT, p));
                if (ch == open)  return p;
                if (ch == close) return p; // already inside — treat current as open
            }
        } else {
            for (int p = startPos; p >= lineStart; p = Sci(hwnd, SCI_POSITIONBEFORE, p)) {
                char ch = static_cast<char>(Sci(hwnd, SCI_GETCHARAT, p));
                if (ch == open || ch == close) return p;
                if (p == lineStart) break;
            }
        }
        return -1;
    }

    // Find the extent of an inner/around bracket text object.
    // Returns {start, end} byte positions, or {-1,-1} on failure.
    // For 'i': range is content between the brackets (exclusive of brackets).
    // For 'a': range includes the brackets.
    std::pair<int,int> FindBracketBounds(HWND hwnd, char open, char close, char modifier) {
        int pos = Sci(hwnd, SCI_GETCURRENTPOS);
        int docLen = Sci(hwnd, SCI_GETLENGTH);

        // Walk backward to find the open bracket, counting nesting.
        int depth = 0;
        int openPos = -1;
        for (int p = pos; p >= 0; ) {
            char ch = static_cast<char>(Sci(hwnd, SCI_GETCHARAT, p));
            if (ch == close) depth++;
            else if (ch == open) {
                if (depth == 0) { openPos = p; break; }
                depth--;
            }
            if (p == 0) break;
            p = Sci(hwnd, SCI_POSITIONBEFORE, p);
        }
        // If no enclosing bracket found backward, look ahead on the current line.
        if (openPos < 0) {
            int line    = Sci(hwnd, SCI_LINEFROMPOSITION, pos);
            int lineEnd = Sci(hwnd, SCI_GETLINEENDPOSITION, line);
            for (int p = pos; p <= lineEnd && p < docLen; p = Sci(hwnd, SCI_POSITIONAFTER, p)) {
                char ch = static_cast<char>(Sci(hwnd, SCI_GETCHARAT, p));
                if (ch == open) { openPos = p; break; }
            }
        }
        if (openPos < 0) return {-1, -1};

        // Walk forward from openPos to find the matching close.
        depth = 0;
        int closePos = -1;
        for (int p = openPos; p < docLen; p = Sci(hwnd, SCI_POSITIONAFTER, p)) {
            char ch = static_cast<char>(Sci(hwnd, SCI_GETCHARAT, p));
            if (ch == open)  depth++;
            else if (ch == close) {
                depth--;
                if (depth == 0) { closePos = p; break; }
            }
        }
        if (closePos < 0) return {-1, -1};

        if (modifier == 'i') {
            int innerStart = Sci(hwnd, SCI_POSITIONAFTER, openPos);
            return {innerStart, closePos};
        } else {
            return {openPos, Sci(hwnd, SCI_POSITIONAFTER, closePos)};
        }
    }

    // Find the extent of an inner/around quote text object.
    // Returns {start, end} or {-1,-1}.
    std::pair<int,int> FindQuoteBounds(HWND hwnd, char quote, char modifier) {
        int pos  = Sci(hwnd, SCI_GETCURRENTPOS);
        int line = Sci(hwnd, SCI_LINEFROMPOSITION, pos);
        int lineStart = Sci(hwnd, SCI_POSITIONFROMLINE, line);
        int lineEnd   = Sci(hwnd, SCI_GETLINEENDPOSITION, line);
        int docLen    = Sci(hwnd, SCI_GETLENGTH);

        // Scan the line to find two quotes.
        int first = -1, second = -1;
        for (int p = lineStart; p <= lineEnd && p < docLen; p = Sci(hwnd, SCI_POSITIONAFTER, p)) {
            char ch = static_cast<char>(Sci(hwnd, SCI_GETCHARAT, p));
            if (ch == quote) {
                if (first < 0) { first = p; }
                else           { second = p; break; }
            }
        }
        if (first < 0 || second < 0) return {-1, -1};

        // Cursor must be between or on the quotes.
        if (pos < first || pos > second) {
            // Try once more: maybe pos is before the first quote (search forward for a pair).
            if (pos < first) {
                // Already have our pair.
            } else {
                return {-1, -1};
            }
        }

        if (modifier == 'i') {
            int innerStart = Sci(hwnd, SCI_POSITIONAFTER, first);
            return {innerStart, second};
        } else {
            return {first, Sci(hwnd, SCI_POSITIONAFTER, second)};
        }
    }

    // Find the extent of an inner/around word (motion 'w' text object).
    bool IsWhitespaceChar(char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    }

    std::pair<int,int> FindWordBounds(HWND hwnd, char modifier, bool bigWord) {
        int pos = Sci(hwnd, SCI_GETCURRENTPOS);

        int wordStart;
        int wordEnd;

        if (bigWord) {
            int docLen = Sci(hwnd, SCI_GETLENGTH);
            if (docLen <= 0) return {-1, -1};
            if (pos >= docLen) pos = docLen - 1;

            char current = static_cast<char>(Sci(hwnd, SCI_GETCHARAT, pos));
            if (IsWhitespaceChar(current) && pos > 0) {
                int prev = Sci(hwnd, SCI_POSITIONBEFORE, pos);
                char prevCh = static_cast<char>(Sci(hwnd, SCI_GETCHARAT, prev));
                if (!IsWhitespaceChar(prevCh)) {
                    pos = prev;
                }
            }

            wordStart = pos;
            while (wordStart > 0) {
                int prev = Sci(hwnd, SCI_POSITIONBEFORE, wordStart);
                char prevCh = static_cast<char>(Sci(hwnd, SCI_GETCHARAT, prev));
                if (IsWhitespaceChar(prevCh)) break;
                wordStart = prev;
            }

            wordEnd = pos;
            while (wordEnd < docLen) {
                char ch = static_cast<char>(Sci(hwnd, SCI_GETCHARAT, wordEnd));
                if (IsWhitespaceChar(ch)) break;
                int next = Sci(hwnd, SCI_POSITIONAFTER, wordEnd);
                if (next <= wordEnd) break;
                wordEnd = next;
            }
        } else {
            // Move to start/end of a regular Scintilla word.
            wordStart = Sci(hwnd, SCI_WORDSTARTPOSITION, pos, true);
            wordEnd   = Sci(hwnd, SCI_WORDENDPOSITION,   pos, true);
        }

        if (wordStart < 0 || wordEnd < 0 || wordStart == wordEnd) {
            return {-1, -1};
        }

        if (modifier == 'i') {
            return {wordStart, wordEnd};
        } else {
            // 'aw': include trailing whitespace, or leading whitespace if at end of line.
            int lineEnd = Sci(hwnd, SCI_GETLINEENDPOSITION,
                              Sci(hwnd, SCI_LINEFROMPOSITION, wordEnd));
            int extEnd = wordEnd;
            while (extEnd < lineEnd) {
                char ch = static_cast<char>(Sci(hwnd, SCI_GETCHARAT, extEnd));
                if (ch != ' ' && ch != '\t') break;
                extEnd = Sci(hwnd, SCI_POSITIONAFTER, extEnd);
            }
            if (extEnd == wordEnd && wordStart > 0) {
                // No trailing space — include leading space.
                int extStart = wordStart;
                while (extStart > 0) {
                    int prev = Sci(hwnd, SCI_POSITIONBEFORE, extStart);
                    char ch = static_cast<char>(Sci(hwnd, SCI_GETCHARAT, prev));
                    if (ch != ' ' && ch != '\t') break;
                    extStart = prev;
                }
                return {extStart, wordEnd};
            }
            return {wordStart, extEnd};
        }
    }

}

namespace VimOperator {
    std::pair<int, int> GetTextObjectBounds(HWND hwnd, char modifier, char obj)
    {
        switch (obj) {
        case 'w':
            return FindWordBounds(hwnd, modifier, false);
        case 'W':
            return FindWordBounds(hwnd, modifier, true);
        case '"': case '\'': case '`':
            return FindQuoteBounds(hwnd, obj, modifier);
        case '(': case ')':
            return FindBracketBounds(hwnd, '(', ')', modifier);
        case '[': case ']':
            return FindBracketBounds(hwnd, '[', ']', modifier);
        case '{': case '}':
            return FindBracketBounds(hwnd, '{', '}', modifier);
        case '<': case '>':
            return FindBracketBounds(hwnd, '<', '>', modifier);
        default:
            return {-1, -1};
        }
    }

    void ApplyOpToRange(HWND hwnd, char op, int start, int end,
                        bool linewise, VimEditorState& state, VimSessionState& session) {
        if (start >= end) return;

        // Read and store text before modifying.
        std::string text = GetTextRange(hwnd, start, end);
        SetReg(state, session, text, linewise);

        SelectRange(hwnd, start, end);

        if (op == 'y') {
            Sci(hwnd, SCI_COPY);
            // Restore cursor to start.
            Sci(hwnd, SCI_SETEMPTYSELECTION, start);
            return;
        }

        // op == 'd' or op == 'c'
        SetClipboard(text);
        Sci(hwnd, SCI_DELETERANGE, start, end - start);
        Sci(hwnd, SCI_SETEMPTYSELECTION, start);

        if (op == 'c') {
            if (linewise) {
                // For linewise change: position at indent of the now-current line.
                Sci(hwnd, SCI_VCHOME);
            }
            EnterVimInsertMode(hwnd, state);
        }
    }

void ApplyToSelection(HWND hwnd, char op, bool linewise,
                      VimEditorState& state, VimSessionState& session)
{
    int start = Sci(hwnd, SCI_GETSELECTIONSTART);
    int end   = Sci(hwnd, SCI_GETSELECTIONEND);
    if (start >= end) return;

    Sci(hwnd, SCI_BEGINUNDOACTION);
    ApplyOpToRange(hwnd, op, start, end, linewise, state, session);
    Sci(hwnd, SCI_ENDUNDOACTION);
}

void ApplyToMotion(HWND hwnd, char op, char motion, int count,
                   VimEditorState& state, VimSessionState& session)
{
    Sci(hwnd, SCI_BEGINUNDOACTION);

    int startPos = Sci(hwnd, SCI_GETCURRENTPOS);

    bool linewise = IsLineMotion(motion);

    if (linewise) {
        int startLine = Sci(hwnd, SCI_LINEFROMPOSITION, startPos);
        int endLine   = startLine;

        if (motion == 'j') {
            endLine = startLine + count;
        } else if (motion == 'k') {
            endLine = startLine - count;
            if (endLine < 0) endLine = 0;
        } else if (motion == 'G') {
            int lineCount = Sci(hwnd, SCI_GETLINECOUNT);
            endLine = (count > 1) ? count - 1 : lineCount - 1;
        } else if (motion == '{') {
            VimMotion::ParagraphUp(hwnd, count);
            endLine = Sci(hwnd, SCI_LINEFROMPOSITION, Sci(hwnd, SCI_GETCURRENTPOS));
            Sci(hwnd, SCI_SETEMPTYSELECTION, startPos);
        } else if (motion == '}') {
            VimMotion::ParagraphDown(hwnd, count);
            endLine = Sci(hwnd, SCI_LINEFROMPOSITION, Sci(hwnd, SCI_GETCURRENTPOS));
            Sci(hwnd, SCI_SETEMPTYSELECTION, startPos);
        }

        int lineCount = Sci(hwnd, SCI_GETLINECOUNT);
        if (endLine < 0) endLine = 0;
        if (endLine >= lineCount) endLine = lineCount - 1;

        int lo = (std::min)(startLine, endLine);
        int hi = (std::max)(startLine, endLine);

        int rangeStart = Sci(hwnd, SCI_POSITIONFROMLINE, lo);
        int rangeEnd;
        if (hi + 1 < lineCount) {
            rangeEnd = Sci(hwnd, SCI_POSITIONFROMLINE, hi + 1);
        } else {
            rangeEnd = Sci(hwnd, SCI_GETLENGTH);
            if (lo > 0) rangeStart = Sci(hwnd, SCI_GETLINEENDPOSITION, lo - 1);
        }

        std::string yankText = GetTextRange(hwnd, rangeStart, rangeEnd);
        SetReg(state, session, yankText, true);
        SetClipboard(yankText);
        SelectRange(hwnd, rangeStart, rangeEnd);

        if (op == 'y') {
            Sci(hwnd, SCI_COPY);
            Sci(hwnd, SCI_SETEMPTYSELECTION, startPos);
        } else {
            Sci(hwnd, SCI_DELETERANGE, rangeStart, rangeEnd - rangeStart);
            int newPos = Sci(hwnd, SCI_POSITIONFROMLINE,
                             (std::min)(lo, Sci(hwnd, SCI_GETLINECOUNT) - 1));
            Sci(hwnd, SCI_SETEMPTYSELECTION, newPos);
            Sci(hwnd, SCI_VCHOME);
            if (op == 'c') EnterVimInsertMode(hwnd, state);
        }

        state.lastOp = { VimOpType::MotionOp, count, op, motion };
        Sci(hwnd, SCI_ENDUNDOACTION);
        return;
    }

    // Characterwise motion.
    int endPos = GetMotionEndPos(hwnd, motion, count);

    // For 'e'/'E': the motion leaves the cursor ON the last char of the word.
    // Vim operators are exclusive by default, so we include the char under the
    // new cursor (make the end inclusive by advancing one position).
    if (motion == 'e' || motion == 'E') {
        endPos = Sci(hwnd, SCI_POSITIONAFTER, endPos);
    }

    int lo = (std::min)(startPos, endPos);
    int hi = (std::max)(startPos, endPos);

    ApplyOpToRange(hwnd, op, lo, hi, false, state, session);

    state.lastOp = { VimOpType::MotionOp, count, op, motion };
    Sci(hwnd, SCI_ENDUNDOACTION);
}

void ApplyToTextObject(HWND hwnd, char op, char modifier, char obj,
                       VimEditorState& state, VimSessionState& session)
{
    std::pair<int,int> bounds = GetTextObjectBounds(hwnd, modifier, obj);

    if (bounds.first < 0) return;

    Sci(hwnd, SCI_BEGINUNDOACTION);
    ApplyOpToRange(hwnd, op, bounds.first, bounds.second, false, state, session);
    state.lastOp = { VimOpType::TextObjectOp, 1, op, 0, modifier, obj };
    Sci(hwnd, SCI_ENDUNDOACTION);
}

void DeleteLines(HWND hwnd, int count, VimEditorState& state, VimSessionState& session)
{
    Sci(hwnd, SCI_BEGINUNDOACTION);

    std::pair<int,int> lr = GetLineRangeForDelete(hwnd, count);
    int start = lr.first, end = lr.second;
    std::string text = GetTextRange(hwnd, start, end);

    SetReg(state, session, text, true);
    SetClipboard(text);

    Sci(hwnd, SCI_DELETERANGE, start, end - start);

    // Land at first non-blank of the current line.
    int lineCount = Sci(hwnd, SCI_GETLINECOUNT);
    int curLine   = Sci(hwnd, SCI_LINEFROMPOSITION, Sci(hwnd, SCI_GETCURRENTPOS));
    if (curLine >= lineCount) curLine = lineCount - 1;
    Sci(hwnd, SCI_SETEMPTYSELECTION, Sci(hwnd, SCI_POSITIONFROMLINE, curLine));
    Sci(hwnd, SCI_VCHOME);

    state.lastOp = { VimOpType::DeleteLine, count };
    Sci(hwnd, SCI_ENDUNDOACTION);
}

void YankLines(HWND hwnd, int count, VimEditorState& state, VimSessionState& session)
{
    int pos       = Sci(hwnd, SCI_GETCURRENTPOS);
    int firstLine = Sci(hwnd, SCI_LINEFROMPOSITION, pos);
    int lineCount = Sci(hwnd, SCI_GETLINECOUNT);
    int lastLine  = firstLine + count - 1;
    if (lastLine >= lineCount) lastLine = lineCount - 1;

    int start = Sci(hwnd, SCI_POSITIONFROMLINE, firstLine);
    int end;
    if (lastLine + 1 < lineCount) {
        end = Sci(hwnd, SCI_POSITIONFROMLINE, lastLine + 1);
    } else {
        end = Sci(hwnd, SCI_GETLENGTH);
    }

    std::string text = GetTextRange(hwnd, start, end);
    SetReg(state, session, text, true);
    SetClipboard(text);

    SelectRange(hwnd, start, end);
    Sci(hwnd, SCI_COPY);
    Sci(hwnd, SCI_SETEMPTYSELECTION, pos); // cursor does not move

    state.lastOp = { VimOpType::YankLine, count };
}

void PasteAfter(HWND hwnd, VimEditorState& state, VimSessionState& session)
{
    std::string text;
    bool linewise;
    GetReg(state, session, text, linewise);
    if (text.empty()) return;

    Sci(hwnd, SCI_BEGINUNDOACTION);

    if (linewise) {
        // Paste below the current line.
        int pos  = Sci(hwnd, SCI_GETCURRENTPOS);
        int line = Sci(hwnd, SCI_LINEFROMPOSITION, pos);
        int insertPos = Sci(hwnd, SCI_GETLINEENDPOSITION, line);

        // Ensure text ends with a newline.
        if (!text.empty() && text.back() != '\n') text += '\n';
        Sci(hwnd, SCI_INSERTTEXT, insertPos, reinterpret_cast<LPARAM>("\n"));
        int newLineStart = Sci(hwnd, SCI_POSITIONAFTER, insertPos);
        // Remove the trailing newline from text before inserting so we don't double up.
        std::string content = text;
        if (!content.empty() && content.back() == '\n') content.pop_back();
        if (!content.empty() && content.back() == '\r') content.pop_back();
        // Insert the content on the new line.
        Sci(hwnd, SCI_INSERTTEXT, newLineStart, reinterpret_cast<LPARAM>(content.c_str()));
        Sci(hwnd, SCI_SETEMPTYSELECTION, newLineStart);
        Sci(hwnd, SCI_VCHOME);
    } else {
        // Paste after cursor (one character to the right).
        int pos    = Sci(hwnd, SCI_GETCURRENTPOS);
        int line   = Sci(hwnd, SCI_LINEFROMPOSITION, pos);
        int lineEnd = Sci(hwnd, SCI_GETLINEENDPOSITION, line);
        int insertPos = (pos < lineEnd) ? Sci(hwnd, SCI_POSITIONAFTER, pos) : pos;
        Sci(hwnd, SCI_INSERTTEXT, insertPos, reinterpret_cast<LPARAM>(text.c_str()));
        // Move cursor to start of pasted text.
        Sci(hwnd, SCI_SETEMPTYSELECTION, insertPos);
    }

    Sci(hwnd, SCI_ENDUNDOACTION);
    state.lastOp = { VimOpType::Paste };
}

void PasteBefore(HWND hwnd, VimEditorState& state, VimSessionState& session)
{
    std::string text;
    bool linewise;
    GetReg(state, session, text, linewise);
    if (text.empty()) return;

    Sci(hwnd, SCI_BEGINUNDOACTION);

    if (linewise) {
        // Paste above the current line.
        int pos  = Sci(hwnd, SCI_GETCURRENTPOS);
        int line = Sci(hwnd, SCI_LINEFROMPOSITION, pos);
        int lineStart = Sci(hwnd, SCI_POSITIONFROMLINE, line);

        std::string content = text;
        if (!content.empty() && content.back() == '\n') content.pop_back();
        if (!content.empty() && content.back() == '\r') content.pop_back();
        content += '\n';
        Sci(hwnd, SCI_INSERTTEXT, lineStart, reinterpret_cast<LPARAM>(content.c_str()));
        Sci(hwnd, SCI_SETEMPTYSELECTION, lineStart);
        Sci(hwnd, SCI_VCHOME);
    } else {
        int pos = Sci(hwnd, SCI_GETCURRENTPOS);
        Sci(hwnd, SCI_INSERTTEXT, pos, reinterpret_cast<LPARAM>(text.c_str()));
        Sci(hwnd, SCI_SETEMPTYSELECTION, pos);
    }

    Sci(hwnd, SCI_ENDUNDOACTION);
    state.lastOp = { VimOpType::Paste };
}

void ReplaceChar(HWND hwnd, char ch, VimEditorState& state, VimSessionState& session)
{
    int pos     = Sci(hwnd, SCI_GETCURRENTPOS);
    int line    = Sci(hwnd, SCI_LINEFROMPOSITION, pos);
    int lineEnd = Sci(hwnd, SCI_GETLINEENDPOSITION, line);
    if (pos >= lineEnd) return; // nothing under cursor

    int next = Sci(hwnd, SCI_POSITIONAFTER, pos);

    Sci(hwnd, SCI_BEGINUNDOACTION);
    Sci(hwnd, SCI_DELETERANGE, pos, next - pos);
    char buf[2] = { ch, '\0' };
    Sci(hwnd, SCI_INSERTTEXT, pos, reinterpret_cast<LPARAM>(buf));
    Sci(hwnd, SCI_SETEMPTYSELECTION, pos);
    Sci(hwnd, SCI_ENDUNDOACTION);

    state.lastOp = { VimOpType::Replace, 1, 0, 0, 0, 0, ch };
}

void RepeatLastOp(HWND hwnd, VimEditorState& state, VimSessionState& session)
{
    VimLastOp op = state.lastOp;
    int savedCount = op.count > 0 ? op.count : 1;

    switch (op.type) {
    case VimOpType::DeleteLine:
        DeleteLines(hwnd, savedCount, state, session);
        break;
    case VimOpType::YankLine:
        YankLines(hwnd, savedCount, state, session);
        break;
    case VimOpType::MotionOp:
        ApplyToMotion(hwnd, op.op, op.motion, savedCount, state, session);
        break;
    case VimOpType::TextObjectOp:
        ApplyToTextObject(hwnd, op.op, op.textMod, op.textObj, state, session);
        break;
    case VimOpType::Replace:
        if (op.replChar) ReplaceChar(hwnd, op.replChar, state, session);
        break;
    case VimOpType::Paste:
        PasteAfter(hwnd, state, session);
        break;
    default:
        break;
    }

    // Restore the original lastOp (RepeatLastOp itself should not overwrite it).
    state.lastOp = op;
}

} // namespace VimOperator
