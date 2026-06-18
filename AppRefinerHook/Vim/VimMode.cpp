// Vim mode support for AppRefiner.
// Portions of the Vim behavior are derived from NppVim
// (https://github.com/h-jangra/NppVim), licensed under the MIT License.

#include "VimMode.h"
#include "VimCommand.h"
#include "VimKeymap.h"
#include "VimLineNumbers.h"
#include "VimMotion.h"
#include "VimOperator.h"

#include <unordered_map>
#include <vector>

namespace {
    std::unordered_map<HWND, VimEditorState> g_vimEditorStates;
    VimSessionState g_vimSessionState;

    int Sci(HWND hwnd, UINT message, WPARAM wParam = 0, LPARAM lParam = 0) {
        return static_cast<int>(SendMessage(hwnd, message, wParam, lParam));
    }

    void ApplyCaretForMode(HWND hwnd, VimMode mode) {
        if (!hwnd || !IsWindow(hwnd)) return;
        const int caretStyle = (mode == VimMode::Insert)
            ? CARETSTYLE_LINE
            : (mode == VimMode::Visual)
                ? CARETSTYLE_INVISIBLE
                : (CARETSTYLE_BLOCK | CARETSTYLE_BLOCK_AFTER);
        SendMessage(hwnd, SCI_SETCARETSTYLE, caretStyle, 0);
    }

    void CenterCaret(HWND hwnd) {
        int curLine  = Sci(hwnd, SCI_LINEFROMPOSITION, Sci(hwnd, SCI_GETCURRENTPOS));
        int visLines = Sci(hwnd, SCI_LINESONSCREEN);
        int firstVis = curLine - visLines / 2;
        if (firstVis < 0) firstVis = 0;
        Sci(hwnd, SCI_SETFIRSTVISIBLELINE, firstVis);
    }

    int ConsumeCount(VimEditorState& state) {
        int count = state.pendingCount > 0 ? state.pendingCount : 1;
        state.pendingCount = 0;
        return count;
    }

    void ClearVisualState(VimEditorState& state) {
        state.visualAnchor   = -1;
        state.visualCaret    = -1;
        state.visualLinewise = false;
    }

    void ClearSearchState(VimEditorState& state) {
        state.searchPending = false;
        state.searchForward = true;
        state.searchText.clear();
    }

    void NotifyVimSearch(HWND hwnd, UINT message, LPARAM lParam = 0) {
        if (g_callbackWindow && IsWindow(g_callbackWindow)) {
            SendMessage(g_callbackWindow, message, reinterpret_cast<WPARAM>(hwnd), lParam);
        }
    }

    void BeginSearchPrompt(HWND hwnd, VimEditorState& state, bool forward) {
        state.pendingCount = 0;
        state.opPending = 0;
        state.textObjectPending = 0;
        state.replacePending = false;
        state.findCharOp = 0;
        state.keymapCurrentNode = nullptr;
        state.keymapPendingKeys.clear();
        state.searchPending = true;
        state.searchForward = forward;
        state.searchText.clear();
        NotifyVimSearch(hwnd, WM_AR_VIM_SEARCH_BEGIN, forward ? '/' : '?');
    }

    // ─── Colon command prompt helpers ────────────────────────────────────────

    void ClearPendingState(VimEditorState& state);  // forward declaration

    void NotifyVimCmd(HWND hwnd, UINT message, LPARAM lParam = 0) {
        if (g_callbackWindow && IsWindow(g_callbackWindow))
            SendMessage(g_callbackWindow, message, reinterpret_cast<WPARAM>(hwnd), lParam);
    }

    void BeginColonPrompt(HWND hwnd, VimEditorState& state) {
        ClearPendingState(state);
        state.colonPending = true;
        state.colonBuffer  = ":";
        NotifyVimCmd(hwnd, WM_AR_VIM_CMD_BEGIN);
    }

    bool HandleColonPromptChar(HWND hwnd, WPARAM wParam, VimEditorState& state) {
        char key = static_cast<char>(wParam);

        if (key == '\x1b') {
            state.colonPending = false;
            state.colonBuffer.clear();
            NotifyVimCmd(hwnd, WM_AR_VIM_CMD_CANCEL);
            return true;
        }

        if (key == '\r') {
            // Execute the command (strip leading ':')
            std::string cmd = state.colonBuffer.size() > 1
                              ? state.colonBuffer.substr(1) : std::string{};
            state.colonPending = false;
            state.colonBuffer.clear();
            NotifyVimCmd(hwnd, WM_AR_VIM_CMD_COMMIT);
            if (!cmd.empty())
                ExecuteColonCommand(hwnd, state, g_vimSessionState, cmd);
            return true;
        }

        if (key == '\b') {
            if (state.colonBuffer.size() > 1) {
                state.colonBuffer.pop_back();
                NotifyVimCmd(hwnd, WM_AR_VIM_CMD_BACKSPACE);
            } else {
                // Backspace past ':' cancels the prompt
                state.colonPending = false;
                state.colonBuffer.clear();
                NotifyVimCmd(hwnd, WM_AR_VIM_CMD_CANCEL);
            }
            return true;
        }

        if (std::isprint(static_cast<unsigned char>(key))) {
            state.colonBuffer += key;
            NotifyVimCmd(hwnd, WM_AR_VIM_CMD_APPEND, key);
            return true;
        }

        return true;
    }

    bool HandleSearchPromptChar(HWND hwnd, WPARAM wParam, VimEditorState& state) {
        char key = static_cast<char>(wParam);

        if (key == '\x1b') {
            NotifyVimSearch(hwnd, WM_AR_VIM_SEARCH_CANCEL);
            ClearSearchState(state);
            return true;
        }

        if (key == '\r') {
            NotifyVimSearch(hwnd, WM_AR_VIM_SEARCH_COMMIT);
            ClearSearchState(state);
            return true;
        }

        if (key == '\b') {
            if (!state.searchText.empty()) {
                state.searchText.pop_back();
            }
            NotifyVimSearch(hwnd, WM_AR_VIM_SEARCH_BACKSPACE);
            return true;
        }

        if (std::isprint(static_cast<unsigned char>(key))) {
            state.searchText.push_back(key);
            NotifyVimSearch(hwnd, WM_AR_VIM_SEARCH_APPEND, key);
            return true;
        }

        return true;
    }

    void ClearPendingState(VimEditorState& state) {
        state.pendingCount      = 0;
        state.opPending         = 0;
        state.textObjectPending = 0;
        state.visualTextObjectPending = 0;
        state.replacePending    = false;
        state.findCharOp        = 0;
        state.keymapCurrentNode = nullptr;
        state.keymapPendingKeys.clear();
        state.activeRegister    = 0;
        state.registerPending   = false;
        state.markSetPending    = false;
        state.jumpExactPending  = false;
        state.jumpLinePending   = false;
        state.macroRecordPending= false;
        state.macroPlayPending  = false;
        state.colonPending      = false;
        state.colonBuffer.clear();
    }

    void ExitVisualMode(HWND hwnd, VimEditorState& state) {
        int targetPos = state.visualCaret >= 0 ? state.visualCaret : Sci(hwnd, SCI_GETCURRENTPOS);
        ClearVisualState(state);
        ClearSearchState(state);
        ClearPendingState(state);
        state.mode = VimMode::Normal;
        ApplyCaretForMode(hwnd, state.mode);
        Sci(hwnd, SCI_SETEMPTYSELECTION, targetPos);
    }

    void EnterVisualMode(HWND hwnd, VimEditorState& state, bool linewise) {
        int currentPos = Sci(hwnd, SCI_GETCURRENTPOS);
        ClearPendingState(state);
        ClearSearchState(state);
        state.mode          = VimMode::Visual;
        state.visualAnchor  = currentPos;
        state.visualCaret   = currentPos;
        state.visualLinewise = linewise;
        ApplyCaretForMode(hwnd, state.mode);
    }

    void UpdateVisualSelection(HWND hwnd, VimEditorState& state) {
        if (state.visualAnchor < 0 || state.visualCaret < 0) return;

        auto setVisualSelection = [&](int start, int endExclusive, int anchor, int caret) {
            Sci(hwnd, SCI_SETSELECTIONNSTART, 0, start);
            Sci(hwnd, SCI_SETSELECTIONNEND, 0, endExclusive);
            Sci(hwnd, SCI_SETSELECTIONNANCHOR, 0, anchor);
            Sci(hwnd, SCI_SETSELECTIONNCARET, 0, caret);
            Sci(hwnd, SCI_SCROLLCARET);
        };

        if (state.visualLinewise) {
            int anchorLine = Sci(hwnd, SCI_LINEFROMPOSITION, state.visualAnchor);
            int caretLine  = Sci(hwnd, SCI_LINEFROMPOSITION, state.visualCaret);
            int loLine     = (std::min)(anchorLine, caretLine);
            int hiLine     = (std::max)(anchorLine, caretLine);
            int start      = Sci(hwnd, SCI_POSITIONFROMLINE, loLine);

            int lineCount = Sci(hwnd, SCI_GETLINECOUNT);
            int end = (hiLine + 1 < lineCount)
                ? Sci(hwnd, SCI_POSITIONFROMLINE, hiLine + 1)
                : Sci(hwnd, SCI_GETLENGTH);

            setVisualSelection(start, end, state.visualAnchor, state.visualCaret);
            return;
        }

        int docLen = Sci(hwnd, SCI_GETLENGTH);
        if (state.visualCaret >= state.visualAnchor) {
            int endExclusive = (state.visualCaret < docLen)
                ? Sci(hwnd, SCI_POSITIONAFTER, state.visualCaret)
                : docLen;
            setVisualSelection(state.visualAnchor, endExclusive, state.visualAnchor, endExclusive);
            return;
        }

        int anchorDisplay = (state.visualAnchor < docLen)
            ? Sci(hwnd, SCI_POSITIONAFTER, state.visualAnchor)
            : docLen;
        setVisualSelection(state.visualCaret, anchorDisplay, anchorDisplay, state.visualCaret);
    }

    void MoveVisualCaret(HWND hwnd, VimEditorState& state, char motion, int count) {
        if (state.visualCaret < 0) return;

        Sci(hwnd, SCI_SETEMPTYSELECTION, state.visualCaret);
        switch (motion) {
        case 'h':
            VimMotion::CharLeft(hwnd, count);
            break;
        case 'j':
            VimMotion::LineDown(hwnd, count);
            break;
        case 'k':
            VimMotion::LineUp(hwnd, count);
            break;
        case 'l':
            VimMotion::CharRight(hwnd, count);
            break;
        case 'w':
            VimMotion::WordRight(hwnd, count);
            break;
        case 'W':
            VimMotion::WordRightBig(hwnd, count);
            break;
        case 'b':
            VimMotion::WordLeft(hwnd, count);
            break;
        case 'B':
            VimMotion::WordLeftBig(hwnd, count);
            break;
        case 'e':
            VimMotion::WordEnd(hwnd, count);
            break;
        case 'E':
            VimMotion::WordEndBig(hwnd, count);
            break;
        case '0':
            VimMotion::LineStart(hwnd);
            break;
        case '$':
            VimMotion::LineEnd(hwnd, count);
            break;
        case '^':
            VimMotion::FirstNonWhitespace(hwnd);
            break;
        case '{':
            VimMotion::ParagraphUp(hwnd, count);
            break;
        case '}':
            VimMotion::ParagraphDown(hwnd, count);
            break;
        case 'G': {
            int lineCount = Sci(hwnd, SCI_GETLINECOUNT);
            int targetLine = (count > 1) ? count - 1 : lineCount - 1;
            Sci(hwnd, SCI_GOTOLINE, targetLine);
            break;
        }
        case '%':
            VimMotion::MatchPair(hwnd);
            break;
        default:
            break;
        }

        state.visualCaret = Sci(hwnd, SCI_GETCURRENTPOS);
        UpdateVisualSelection(hwnd, state);
    }

    bool HandleVisualChar(HWND hwnd, WPARAM wParam, VimEditorState& state) {
        char key = static_cast<char>(wParam);
        VimSessionState& session = g_vimSessionState;

        if (key >= '1' && key <= '9') {
            state.pendingCount = state.pendingCount * 10 + (key - '0');
            return true;
        }
        if (key == '0' && state.pendingCount > 0) {
            state.pendingCount = state.pendingCount * 10;
            return true;
        }

        if (state.visualTextObjectPending) {
            char modifier = state.visualTextObjectPending;
            state.visualTextObjectPending = 0;

            auto bounds = VimOperator::GetTextObjectBounds(hwnd, modifier, key);
            if (bounds.first >= 0 && bounds.second > bounds.first) {
                state.visualLinewise = false;
                state.visualAnchor = bounds.first;
                state.visualCaret = Sci(hwnd, SCI_POSITIONBEFORE, bounds.second);
                UpdateVisualSelection(hwnd, state);
            }
            state.pendingCount = 0;
            return true;
        }

        switch (key) {
        case '\x1b':
        case 'v':
            ExitVisualMode(hwnd, state);
            return true;
        case 'V':
            if (!state.visualLinewise) {
                state.visualLinewise = true;
                UpdateVisualSelection(hwnd, state);
            } else {
                ExitVisualMode(hwnd, state);
            }
            return true;
        case 'i':
        case 'a':
            state.visualTextObjectPending = key;
            state.pendingCount = 0;
            return true;
        case 'd':
        case 'y':
        case 'c':
            VimOperator::ApplyToSelection(hwnd, key, state.visualLinewise, state, session);
            ClearVisualState(state);
            ClearSearchState(state);
            ClearPendingState(state);
            state.mode = (key == 'c') ? VimMode::Insert : VimMode::Normal;
            ApplyCaretForMode(hwnd, state.mode);
            return true;
        case 'h': case 'j': case 'k': case 'l':
        case 'w': case 'W': case 'b': case 'B':
        case 'e': case 'E': case '0': case '$':
        case '^': case '{': case '}': case 'G': case '%':
            MoveVisualCaret(hwnd, state, key, ConsumeCount(state));
            return true;
        default:
            state.pendingCount = 0;
            return true;
        }
    }

    bool HandleNormalKeyDown(HWND hwnd, WPARAM wParam, VimEditorState& state) {
        switch (wParam) {
        case VK_PRIOR:
            VimMotion::PageUp(hwnd, ConsumeCount(state));
            return true;
        case VK_NEXT:
            VimMotion::PageDown(hwnd, ConsumeCount(state));
            return true;
        case VK_BACK:
        case VK_DELETE:
        case VK_RETURN:
        case VK_TAB:
            state.pendingCount = 0;
            return true;
        default:
            return false;
        }
    }

    bool HandleVisualKeyDown(HWND hwnd, WPARAM wParam, VimEditorState& state) {
        switch (wParam) {
        case VK_PRIOR:
            Sci(hwnd, SCI_SETEMPTYSELECTION, state.visualCaret);
            VimMotion::PageUp(hwnd, ConsumeCount(state));
            state.visualCaret = Sci(hwnd, SCI_GETCURRENTPOS);
            UpdateVisualSelection(hwnd, state);
            return true;
        case VK_NEXT:
            Sci(hwnd, SCI_SETEMPTYSELECTION, state.visualCaret);
            VimMotion::PageDown(hwnd, ConsumeCount(state));
            state.visualCaret = Sci(hwnd, SCI_GETCURRENTPOS);
            UpdateVisualSelection(hwnd, state);
            return true;
        case VK_BACK:
        case VK_DELETE:
        case VK_RETURN:
        case VK_TAB:
            state.pendingCount = 0;
            return true;
        default:
            return false;
        }
    }

    // Ctrl-only chords are handled on WM_KEYUP because Application Designer's
    // TranslateAccelerator eats the WM_KEYDOWN for several Ctrl+letter combos before
    // our subclass proc sees it. WM_KEYUP is never processed by TranslateAccelerator,
    // so it always reaches us reliably.
    bool HandleNormalCtrlKeyUp(HWND hwnd, WPARAM wParam, VimEditorState& state) {
        switch (wParam) {
        case 'R':
            state.pendingCount = 0;
            Sci(hwnd, SCI_REDO);
            return true;
        case 'E':
            state.pendingCount = 0;
            Sci(hwnd, SCI_LINESCROLL, 0, 1);
            return true;
        case 'Y':
            state.pendingCount = 0;
            Sci(hwnd, SCI_LINESCROLL, 0, -1);
            return true;
        case 'D': {
            int lines = Sci(hwnd, SCI_LINESONSCREEN) / 2;
            state.pendingCount = 0;
            VimMotion::LineDown(hwnd, lines > 0 ? lines : 1);
            CenterCaret(hwnd);
            return true;
        }
        case 'U': {
            int lines = Sci(hwnd, SCI_LINESONSCREEN) / 2;
            state.pendingCount = 0;
            VimMotion::LineUp(hwnd, lines > 0 ? lines : 1);
            CenterCaret(hwnd);
            return true;
        }
        case 'B':
            VimMotion::PageUp(hwnd, ConsumeCount(state));
            return true;
        default:
            return false;
        }
    }

    bool HandleVisualCtrlKeyUp(HWND hwnd, WPARAM wParam, VimEditorState& state) {
        switch (wParam) {
        case 'D': {
            int lines = Sci(hwnd, SCI_LINESONSCREEN) / 2;
            Sci(hwnd, SCI_SETEMPTYSELECTION, state.visualCaret);
            VimMotion::LineDown(hwnd, lines > 0 ? lines : 1);
            CenterCaret(hwnd);
            state.visualCaret = Sci(hwnd, SCI_GETCURRENTPOS);
            UpdateVisualSelection(hwnd, state);
            state.pendingCount = 0;
            return true;
        }
        case 'U': {
            int lines = Sci(hwnd, SCI_LINESONSCREEN) / 2;
            Sci(hwnd, SCI_SETEMPTYSELECTION, state.visualCaret);
            VimMotion::LineUp(hwnd, lines > 0 ? lines : 1);
            CenterCaret(hwnd);
            state.visualCaret = Sci(hwnd, SCI_GETCURRENTPOS);
            UpdateVisualSelection(hwnd, state);
            state.pendingCount = 0;
            return true;
        }
        case 'E':
            Sci(hwnd, SCI_LINESCROLL, 0, 1);
            state.pendingCount = 0;
            return true;
        case 'Y':
            Sci(hwnd, SCI_LINESCROLL, 0, -1);
            state.pendingCount = 0;
            return true;
        case 'B':
            Sci(hwnd, SCI_SETEMPTYSELECTION, state.visualCaret);
            VimMotion::PageUp(hwnd, ConsumeCount(state));
            state.visualCaret = Sci(hwnd, SCI_GETCURRENTPOS);
            UpdateVisualSelection(hwnd, state);
            return true;
        default:
            return false;
        }
    }

    // Replay a recorded macro string against the current editor state.
    // Temporarily disables recording so replayed keystrokes don't self-record.
    void ReplayMacro(HWND hwnd, VimEditorState& state, const std::string& macroStr);

    bool HandleNormalChar(HWND hwnd, WPARAM wParam, VimEditorState& state) {
        char key = static_cast<char>(wParam);
        VimSessionState& session = g_vimSessionState;

        // --- Macro recording: append every normal-mode keystroke. ---
        // 'q' with nothing else pending stops recording (not appended).
        // When any sub-command is waiting for a second char, 'q' is that char,
        // so anyPending gates the stop-check.
        if (session.macroRecording) {
            bool anyPending = state.replacePending        ||
                              state.findCharOp     != 0   ||
                              state.textObjectPending != 0 ||
                              state.registerPending       ||
                              state.markSetPending        ||
                              state.jumpExactPending      ||
                              state.jumpLinePending       ||
                              state.macroRecordPending    ||
                              state.macroPlayPending;
            if (key == 'q' && !anyPending) {
                // Stop recording — save to named register, do not append 'q'.
                session.macroRecording = false;
                session.namedRegisters[session.macroRecordReg - 'a'].text     = session.macroRecordBuf;
                session.namedRegisters[session.macroRecordReg - 'a'].linewise = false;
                session.macroRecordBuf.clear();
                state.opPending    = 0;   // cancel any dangling operator
                state.pendingCount = 0;
                return true;
            }
            session.macroRecordBuf += key;
        }

        // --- Step 0: 2-char pending state completions ---
        if (state.registerPending) {
            state.registerPending = false;
            if (key >= 'a' && key <= 'z')
                state.activeRegister = key;
            state.pendingCount = 0;
            return true;
        }
        if (state.markSetPending) {
            state.markSetPending = false;
            if (key >= 'a' && key <= 'z')
                state.marks[key - 'a'] = Sci(hwnd, SCI_GETCURRENTPOS);
            state.pendingCount = 0;
            return true;
        }
        if (state.jumpExactPending) {
            state.jumpExactPending = false;
            state.opPending = 0;  // v1: cancel pending op (op+mark motions not yet supported)
            int targetPos = -1;
            if (key == '\x60')          targetPos = state.jumpMarkPos;  // `` = return
            else if (key >= 'a' && key <= 'z') targetPos = state.marks[key - 'a'];
            if (targetPos >= 0) {
                state.jumpMarkPos = Sci(hwnd, SCI_GETCURRENTPOS);
                Sci(hwnd, SCI_SETEMPTYSELECTION, targetPos);
                Sci(hwnd, SCI_SCROLLCARET);
            }
            state.pendingCount = 0;
            return true;
        }
        if (state.jumpLinePending) {
            state.jumpLinePending = false;
            state.opPending = 0;  // v1: cancel pending op
            int targetPos = -1;
            if (key == '\'')            targetPos = state.jumpMarkPos;  // '' = return
            else if (key >= 'a' && key <= 'z') targetPos = state.marks[key - 'a'];
            if (targetPos >= 0) {
                state.jumpMarkPos = Sci(hwnd, SCI_GETCURRENTPOS);
                int line = Sci(hwnd, SCI_LINEFROMPOSITION, targetPos);
                Sci(hwnd, SCI_GOTOLINE, line);
                Sci(hwnd, SCI_VCHOME);
            }
            state.pendingCount = 0;
            return true;
        }
        if (state.macroRecordPending) {
            state.macroRecordPending = false;
            if (key >= 'a' && key <= 'z') {
                session.macroRecording = true;
                session.macroRecordReg = key;
                session.macroRecordBuf.clear();
            }
            state.pendingCount = 0;
            return true;
        }
        if (state.macroPlayPending) {
            state.macroPlayPending = false;
            char reg = (key == '@') ? session.lastMacroReg : key;
            if (reg >= 'a' && reg <= 'z' && !session.namedRegisters[reg - 'a'].text.empty()) {
                session.lastMacroReg = reg;
                ReplayMacro(hwnd, state, session.namedRegisters[reg - 'a'].text);
            }
            state.pendingCount = 0;
            return true;
        }

        // --- Step 1: replace-char pending ---
        if (state.replacePending) {
            state.replacePending = false;
            if (key != '\x1b') {
                VimOperator::ReplaceChar(hwnd, key, state, session);
            }
            state.pendingCount = 0;
            return true;
        }

        // --- Step 1.5: find-char pending (f/F/t/T waiting for target character) ---
        if (state.findCharOp) {
            char op  = state.findCharOp;
            state.findCharOp = 0;
            int count = ConsumeCount(state);
            if (key == '\x1b') return true;  // Escape cancels, handled above too

            if (state.opPending) {
                // Operator + find-char: compute destination then apply operator to range.
                char pendingOp = state.opPending;
                state.opPending = 0;
                int startPos = Sci(hwnd, SCI_GETCURRENTPOS);
                int endPos   = VimMotion::FindCharPos(hwnd, op, key, count);
                if (endPos >= 0) {
                    // f/F are inclusive (include the found char); t/T are exclusive.
                    bool inclusive = (op == 'f' || op == 'F');
                    if (endPos > startPos) {
                        int rangeEnd = inclusive ? Sci(hwnd, SCI_POSITIONAFTER, endPos) : endPos;
                        VimOperator::ApplyOpToRange(hwnd, pendingOp, startPos, rangeEnd, false, state, session);
                    } else {
                        int rangeEnd = inclusive ? startPos : Sci(hwnd, SCI_POSITIONAFTER, startPos);
                        VimOperator::ApplyOpToRange(hwnd, pendingOp, endPos, rangeEnd, false, state, session);
                    }
                }
            } else {
                VimMotion::FindChar(hwnd, op, key, count);
            }
            state.lastFindOp   = op;
            state.lastFindChar = key;
            return true;
        }

        // --- Step 2: text-object pending ---
        // After op + 'i'/'a', the next key is the object character.
        if (state.textObjectPending) {
            char modifier = state.textObjectPending;
            char op       = state.opPending;
            state.textObjectPending = 0;
            state.opPending         = 0;
            int  count = ConsumeCount(state);
            (void)count; // text objects don't use a count for now
            VimOperator::ApplyToTextObject(hwnd, op, modifier, key, state, session);
            return true;
        }

        // --- Step 3: op + 'i'/'a' → set textObjectPending ---
        if (state.opPending && (key == 'i' || key == 'a')) {
            state.textObjectPending = key;
            return true;
        }

        // --- Step 4: op + motion → apply operator ---
        if (state.opPending && state.keymapPendingKeys.empty()) {
            // f/F/t/T need a second char — park into findCharOp; step 1.5 completes it.
            if (key == 'f' || key == 'F' || key == 't' || key == 'T') {
                state.findCharOp = key;
                return true;  // opPending intentionally kept for step 1.5
            }

            static const char motionChars[] = "hjklwWbBeE$^0G{}%";
            for (const char* m = motionChars; *m; ++m) {
                if (key == *m) {
                    int count = ConsumeCount(state);
                    char op   = state.opPending;
                    state.opPending = 0;
                    VimOperator::ApplyToMotion(hwnd, op, key, count, state, session);
                    return true;
                }
            }
        }

        // --- Step 5: first press of d/c/y sets operator pending ---
        // Condition: no operator already set, and no keymap multi-key sequence in progress.
        if (!state.opPending && state.keymapPendingKeys.empty() &&
            (key == 'd' || key == 'c' || key == 'y'))
        {
            state.opPending = key;
            // pendingCount is intentionally NOT reset here so the already-typed
            // count (e.g. the '3' in '3dd') is still available for the keymap handler.
            return true;
        }

        // --- Step 6: keymap (handles digits, gg/G/0, and fires dd/yy/cc handlers) ---
        if (!g_normalKeymap) {
            InitializeNormalModeKeymap();
        }
        if (g_normalKeymap->handleKey(hwnd, key, state)) {
            return true;
        }

        // --- Step 7: single-key commands ---
        switch (wParam) {
        // --- Motions ---
        case 'h':
            VimMotion::CharLeft(hwnd, ConsumeCount(state));
            return true;
        case 'j':
            VimMotion::LineDown(hwnd, ConsumeCount(state));
            return true;
        case 'k':
            VimMotion::LineUp(hwnd, ConsumeCount(state));
            return true;
        case 'l':
            VimMotion::CharRight(hwnd, ConsumeCount(state));
            return true;
        case 'w':
            VimMotion::WordRight(hwnd, ConsumeCount(state));
            return true;
        case 'W':
            VimMotion::WordRightBig(hwnd, ConsumeCount(state));
            return true;
        case 'b':
            VimMotion::WordLeft(hwnd, ConsumeCount(state));
            return true;
        case 'B':
            VimMotion::WordLeftBig(hwnd, ConsumeCount(state));
            return true;
        case 'e':
            VimMotion::WordEnd(hwnd, ConsumeCount(state));
            return true;
        case 'E':
            VimMotion::WordEndBig(hwnd, ConsumeCount(state));
            return true;
        case '$':
            VimMotion::LineEnd(hwnd, ConsumeCount(state));
            return true;
        case '^':
            VimMotion::FirstNonWhitespace(hwnd);
            state.pendingCount = 0;
            return true;
        case '{':
            VimMotion::ParagraphUp(hwnd, ConsumeCount(state));
            return true;
        case '}':
            VimMotion::ParagraphDown(hwnd, ConsumeCount(state));
            return true;
        case '%':
            VimMotion::MatchPair(hwnd);
            state.pendingCount = 0;
            return true;
        case '/':
            BeginSearchPrompt(hwnd, state, true);
            return true;
        case '?':
            BeginSearchPrompt(hwnd, state, false);
            return true;
        case ':':
            BeginColonPrompt(hwnd, state);
            return true;
        case 'n':
            NotifyVimSearch(hwnd, WM_AR_VIM_SEARCH_NEXT, 1);
            state.pendingCount = 0;
            return true;
        case 'N':
            NotifyVimSearch(hwnd, WM_AR_VIM_SEARCH_NEXT, 0);
            state.pendingCount = 0;
            return true;
        case 'K':
            NotifyVimSearch(hwnd, WM_AR_VIM_SHOW_TOOLTIP, Sci(hwnd, SCI_GETCURRENTPOS));
            state.pendingCount = 0;
            return true;
        case 'H':
            NotifyVimSearch(hwnd, WM_AR_VIM_CYCLE_EDITOR, -1);
            state.pendingCount = 0;
            return true;
        case 'L':
            NotifyVimSearch(hwnd, WM_AR_VIM_CYCLE_EDITOR, 1);
            state.pendingCount = 0;
            return true;
        case 'v':
            EnterVisualMode(hwnd, state, false);
            UpdateVisualSelection(hwnd, state);
            return true;
        case 'V':
            EnterVisualMode(hwnd, state, true);
            UpdateVisualSelection(hwnd, state);
            return true;

        // --- Find-char motions ---
        case 'f': case 'F': case 't': case 'T':
            state.findCharOp = key;
            // pendingCount intentionally kept — consumed when char arrives
            return true;
        case ';':
            if (state.lastFindOp) {
                VimMotion::FindChar(hwnd, state.lastFindOp, state.lastFindChar,
                                   ConsumeCount(state));
            } else {
                state.pendingCount = 0;
            }
            return true;
        case ',': {
            if (state.lastFindOp) {
                // Reverse direction: f↔F, t↔T
                char rev = state.lastFindOp == 'f' ? 'F' :
                           state.lastFindOp == 'F' ? 'f' :
                           state.lastFindOp == 't' ? 'T' : 't';
                VimMotion::FindChar(hwnd, rev, state.lastFindChar, ConsumeCount(state));
            } else {
                state.pendingCount = 0;
            }
            return true;
        }

        // --- Insert-mode entries ---
        case 'i':
            EnterVimInsertMode(hwnd, state);
            return true;
        case 'a':
            VimMotion::CharRight(hwnd, 1);
            EnterVimInsertMode(hwnd, state);
            return true;
        case 'A':
            Sci(hwnd, SCI_LINEEND);
            EnterVimInsertMode(hwnd, state);
            return true;
        case 'I':
            Sci(hwnd, SCI_VCHOME);
            EnterVimInsertMode(hwnd, state);
            return true;
        case 'o':
            Sci(hwnd, SCI_LINEEND);
            Sci(hwnd, SCI_NEWLINE);
            EnterVimInsertMode(hwnd, state);
            return true;
        case 'O':
            Sci(hwnd, SCI_HOME);
            Sci(hwnd, SCI_NEWLINE);
            VimMotion::LineUp(hwnd, 1);
            EnterVimInsertMode(hwnd, state);
            return true;

        // --- Single-char deletions (x/X write to register) ---
        case 'x': {
            int count = ConsumeCount(state);
            Sci(hwnd, SCI_BEGINUNDOACTION);
            for (int i = 0; i < count; i++) {
                int pos     = Sci(hwnd, SCI_GETCURRENTPOS);
                int line    = Sci(hwnd, SCI_LINEFROMPOSITION, pos);
                int lineEnd = Sci(hwnd, SCI_GETLINEENDPOSITION, line);
                if (pos >= lineEnd) break;
                int next = Sci(hwnd, SCI_POSITIONAFTER, pos);
                session.yankRegister = std::string(1, static_cast<char>(Sci(hwnd, SCI_GETCHARAT, pos)));
                session.yankLinewise = false;
                Sci(hwnd, SCI_DELETERANGE, pos, next - pos);
            }
            Sci(hwnd, SCI_ENDUNDOACTION);
            return true;
        }
        case 'X': {
            int count = ConsumeCount(state);
            Sci(hwnd, SCI_BEGINUNDOACTION);
            for (int i = 0; i < count; i++) {
                int pos = Sci(hwnd, SCI_GETCURRENTPOS);
                if (pos <= 0) break;
                int prev = Sci(hwnd, SCI_POSITIONBEFORE, pos);
                session.yankRegister = std::string(1, static_cast<char>(Sci(hwnd, SCI_GETCHARAT, prev)));
                session.yankLinewise = false;
                Sci(hwnd, SCI_DELETERANGE, prev, pos - prev);
            }
            Sci(hwnd, SCI_ENDUNDOACTION);
            return true;
        }
        case 's': {
            int count = ConsumeCount(state);
            for (int i = 0; i < count; i++) {
                int pos     = Sci(hwnd, SCI_GETCURRENTPOS);
                int line    = Sci(hwnd, SCI_LINEFROMPOSITION, pos);
                int lineEnd = Sci(hwnd, SCI_GETLINEENDPOSITION, line);
                if (pos >= lineEnd) break;
                int next = Sci(hwnd, SCI_POSITIONAFTER, pos);
                Sci(hwnd, SCI_DELETERANGE, pos, next - pos);
            }
            EnterVimInsertMode(hwnd, state);
            return true;
        }

        // --- Delete / change to end of line ---
        case 'D': {
            int count = ConsumeCount(state);
            Sci(hwnd, SCI_BEGINUNDOACTION);
            for (int i = 0; i < count; i++) {
                int pos  = Sci(hwnd, SCI_GETCURRENTPOS);
                int line = Sci(hwnd, SCI_LINEFROMPOSITION, pos);
                int end  = Sci(hwnd, SCI_GETLINEENDPOSITION, line);
                if (pos < end) Sci(hwnd, SCI_DELETERANGE, pos, end - pos);
                if (i < count - 1) VimMotion::LineDown(hwnd, 1);
            }
            Sci(hwnd, SCI_ENDUNDOACTION);
            return true;
        }
        case 'C': {
            int pos  = Sci(hwnd, SCI_GETCURRENTPOS);
            int line = Sci(hwnd, SCI_LINEFROMPOSITION, pos);
            int end  = Sci(hwnd, SCI_GETLINEENDPOSITION, line);
            if (pos < end) Sci(hwnd, SCI_DELETERANGE, pos, end - pos);
            EnterVimInsertMode(hwnd, state);
            return true;
        }
        case 'S': {
            Sci(hwnd, SCI_VCHOME);
            int pos  = Sci(hwnd, SCI_GETCURRENTPOS);
            int line = Sci(hwnd, SCI_LINEFROMPOSITION, pos);
            int end  = Sci(hwnd, SCI_GETLINEENDPOSITION, line);
            if (pos < end) Sci(hwnd, SCI_DELETERANGE, pos, end - pos);
            EnterVimInsertMode(hwnd, state);
            return true;
        }

        // --- Join lines ---
        case 'J': {
            int joins = ConsumeCount(state);  // J = 1 join, 3J = 3 joins
            Sci(hwnd, SCI_BEGINUNDOACTION);
            for (int i = 0; i < joins; i++) {
                int line      = Sci(hwnd, SCI_LINEFROMPOSITION, Sci(hwnd, SCI_GETCURRENTPOS));
                if (line >= Sci(hwnd, SCI_GETLINECOUNT) - 1) break;

                int lineEnd   = Sci(hwnd, SCI_GETLINEENDPOSITION, line);
                int nextStart = Sci(hwnd, SCI_POSITIONFROMLINE, line + 1);
                int nextEnd   = Sci(hwnd, SCI_GETLINEENDPOSITION, line + 1);

                // Scan past leading whitespace on the next line.
                int content = nextStart;
                while (content < nextEnd) {
                    char ch = static_cast<char>(Sci(hwnd, SCI_GETCHARAT, content));
                    if (ch != ' ' && ch != '\t') break;
                    content = Sci(hwnd, SCI_POSITIONAFTER, content);
                }
                bool nextHasContent = (content < nextEnd);

                // Remove newline (and any leading whitespace on next line).
                if (content > lineEnd)
                    Sci(hwnd, SCI_DELETERANGE, lineEnd, content - lineEnd);

                // Insert a space if both lines have content and current doesn't end with one.
                if (nextHasContent) {
                    int lineStart = Sci(hwnd, SCI_POSITIONFROMLINE, line);
                    if (lineEnd > lineStart) {
                        char last = static_cast<char>(
                            Sci(hwnd, SCI_GETCHARAT, Sci(hwnd, SCI_POSITIONBEFORE, lineEnd)));
                        if (last != ' ')
                            Sci(hwnd, SCI_INSERTTEXT, lineEnd, (LPARAM)" ");
                    }
                }
                Sci(hwnd, SCI_SETEMPTYSELECTION, lineEnd);
            }
            Sci(hwnd, SCI_ENDUNDOACTION);
            return true;
        }

        // --- Undo ---
        case 'u':
            state.pendingCount = 0;
            Sci(hwnd, SCI_UNDO);
            return true;

        // --- Repeat last change ---
        case '.':
            VimOperator::RepeatLastOp(hwnd, state, session);
            return true;

        // --- Marks ---
        case 'm':
            state.markSetPending = true;
            state.pendingCount   = 0;
            return true;
        case '\x60':  // backtick: jump to exact mark position
            state.jumpExactPending = true;
            state.pendingCount     = 0;
            return true;
        case '\'':  // single quote: jump to first non-blank of mark's line
            state.jumpLinePending = true;
            state.pendingCount    = 0;
            return true;

        // --- Register prefix ---
        case '"':
            state.registerPending = true;
            // pendingCount intentionally kept so "a3p works
            return true;

        // --- Macro record/play ---
        case 'q':
            // If already recording, stopping is handled at the top of this function.
            // This case only fires when NOT recording.
            if (!session.macroRecording)
                state.macroRecordPending = true;
            state.pendingCount = 0;
            return true;
        case '@':
            state.macroPlayPending = true;
            state.pendingCount     = 0;
            return true;

        // --- Case toggle ---
        case '~': {
            int count = ConsumeCount(state);
            for (int i = 0; i < count; i++) {
                int pos  = Sci(hwnd, SCI_GETCURRENTPOS);
                if (pos >= Sci(hwnd, SCI_GETLENGTH)) break;
                int next = Sci(hwnd, SCI_POSITIONAFTER, pos);
                char ch  = static_cast<char>(Sci(hwnd, SCI_GETCHARAT, pos));
                Sci(hwnd, SCI_SETSEL, pos, next);
                if (islower(static_cast<unsigned char>(ch)))
                    Sci(hwnd, SCI_UPPERCASE);
                else if (isupper(static_cast<unsigned char>(ch)))
                    Sci(hwnd, SCI_LOWERCASE);
                Sci(hwnd, SCI_SETEMPTYSELECTION, next);
            }
            return true;
        }

        default:
            state.pendingCount = 0;
            return true;
        }
    }

    void ReplayMacro(HWND hwnd, VimEditorState& state, const std::string& macroStr) {
        VimSessionState& session = g_vimSessionState;
        if (session.macroPlayDepth >= 10) return;  // recursion guard

        // Suspend recording so replayed keystrokes don't feed back into the buffer.
        bool savedRecording = session.macroRecording;
        session.macroRecording = false;
        session.macroPlayDepth++;

        for (size_t i = 0; i < macroStr.size(); i++) {
            unsigned char ch = static_cast<unsigned char>(macroStr[i]);
            if (state.mode == VimMode::Insert) {
                if (ch == '\x1b') {
                    ExitVimInsertMode(hwnd, state);
                } else if (ch == '\r' || ch == '\n') {
                    Sci(hwnd, SCI_NEWLINE);
                } else if (ch == '\b') {
                    Sci(hwnd, SCI_DELETEBACK);
                } else {
                    char buf[2] = { static_cast<char>(ch), '\0' };
                    Sci(hwnd, SCI_ADDTEXT, 1, reinterpret_cast<LPARAM>(buf));
                }
            } else {
                HandleNormalChar(hwnd, static_cast<WPARAM>(ch), state);
            }
        }

        session.macroPlayDepth--;
        session.macroRecording = savedRecording;
    }

} // anonymous namespace

// Public — used by VimOperator.cpp and keymap handlers that need to enter insert mode.
void EnterVimInsertMode(HWND hwnd, VimEditorState& state) {
    ClearVisualState(state);
    ClearSearchState(state);
    ClearPendingState(state);
    state.mode = VimMode::Insert;
    ApplyCaretForMode(hwnd, state.mode);
}

void ExitVimInsertMode(HWND hwnd, VimEditorState& state) {
    // Record the insert→normal transition whenever a macro is being recorded.
    // Done here (not in the WM_KEYDOWN handler) so it fires even when Escape
    // is first consumed by AppRefiner's autocomplete/calltip dismiss logic.
    if (g_vimSessionState.macroRecording && state.mode == VimMode::Insert) {
        g_vimSessionState.macroRecordBuf += '\x1b';
    }

    if (state.mode == VimMode::Insert) {
        int currentPos = Sci(hwnd, SCI_GETCURRENTPOS);
        if (currentPos > 0) {
            int previousPos = Sci(hwnd, SCI_POSITIONBEFORE, currentPos);
            Sci(hwnd, SCI_SETEMPTYSELECTION, previousPos);
        }
    }

    ClearVisualState(state);
    ClearSearchState(state);
    ClearPendingState(state);
    state.mode = VimMode::Normal;
    ApplyCaretForMode(hwnd, state.mode);
    SendMessage(hwnd, SCI_CANCEL, 0, 0);
}

VimEditorState& GetVimEditorState(HWND hwnd) {
    return g_vimEditorStates[hwnd];
}

bool IsVimNormalModeEditor(HWND hwnd) {
    auto it = g_vimEditorStates.find(hwnd);
    return it != g_vimEditorStates.end() &&
           it->second.caretInitialized &&
           it->second.mode == VimMode::Normal;
}

VimSessionState& GetVimSessionState() {
    return g_vimSessionState;
}

void RemoveVimEditorState(HWND hwnd) {
    g_vimEditorStates.erase(hwnd);
}

void SetVimEnabledForKnownEditors(bool enabled) {
    std::vector<HWND> editorHandles;
    editorHandles.reserve(g_vimEditorStates.size());
    for (const auto& entry : g_vimEditorStates) {
        editorHandles.push_back(entry.first);
    }
    for (HWND hwnd : editorHandles) {
        if (!hwnd || !IsWindow(hwnd)) continue;
        auto it = g_vimEditorStates.find(hwnd);
        if (it == g_vimEditorStates.end()) continue;
        VimEditorState& state = it->second;
        ClearVisualState(state);
        ClearSearchState(state);
        ClearPendingState(state);
        if (enabled) {
            state.mode = VimMode::Normal;
            state.caretInitialized = true;
            ApplyCaretForMode(hwnd, state.mode);
            VimLineNumbers::Init(hwnd);
        } else {
            state.caretInitialized = false;
            ApplyCaretForMode(hwnd, VimMode::Insert);
            VimLineNumbers::Shutdown(hwnd);
        }
    }
}

void EnsureVimCaretInitialized(HWND hwnd, VimEditorState& state) {
    if (state.caretInitialized) return;
    state.caretInitialized = true;
    ApplyCaretForMode(hwnd, state.mode);
    VimLineNumbers::Init(hwnd);
}

bool HandleVimEditorMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, VimEditorState& state) {
    if (!hwnd || !IsWindow(hwnd)) return false;

    if ((message == WM_KEYDOWN || message == WM_KEYUP) && wParam == VK_ESCAPE) {
        if (state.colonPending) {
            state.colonPending = false;
            state.colonBuffer.clear();
            NotifyVimCmd(hwnd, WM_AR_VIM_CMD_CANCEL);
        } else if (state.mode == VimMode::Visual) {
            ExitVisualMode(hwnd, state);
        } else {
            ExitVimInsertMode(hwnd, state);
        }
        return true;
    }

    // Record insert-mode keystrokes (WM_CHAR) into the macro buffer.
    if (g_vimSessionState.macroRecording && state.mode == VimMode::Insert &&
        message == WM_CHAR) {
        g_vimSessionState.macroRecordBuf += static_cast<char>(wParam);
    }
    if (state.mode == VimMode::Insert) return false;

    if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
        if (state.searchPending || state.colonPending) {
            switch (wParam) {
            case VK_BACK:
            case VK_RETURN:
            case VK_TAB:
                return true;
            default:
                break;
            }
        }
        // Suppress Ctrl+D/U/E/Y/B on keydown so Scintilla's built-in commands
        // (e.g. Ctrl+D = duplicate line) don't fire before our keyup handler.
        // Ctrl+R is not suppressed here — App Designer's TranslateAccelerator
        // eats its keydown before we ever see it.
        const bool ctrlDown  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shiftDown = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
        const bool altDown   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
        if (ctrlDown && !shiftDown && !altDown) {
            switch (wParam) {
            case 'D': case 'U': case 'E': case 'Y': case 'B':
                return true; // consumed — we act on the matching keyup instead
            }
        }
        return state.mode == VimMode::Visual
            ? HandleVisualKeyDown(hwnd, wParam, state)
            : HandleNormalKeyDown(hwnd, wParam, state);
    }

    if (message == WM_KEYUP) {
        const bool ctrlDown  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shiftDown = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
        const bool altDown   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
        if (ctrlDown && !shiftDown && !altDown) {
            return state.mode == VimMode::Visual
                ? HandleVisualCtrlKeyUp(hwnd, wParam, state)
                : HandleNormalCtrlKeyUp(hwnd, wParam, state);
        }
        return false;
    }

    if (message != WM_CHAR) return false;

    if (wParam == VK_ESCAPE) {
        if (state.colonPending) {
            state.colonPending = false;
            state.colonBuffer.clear();
            NotifyVimCmd(hwnd, WM_AR_VIM_CMD_CANCEL);
        } else if (state.searchPending) {
            NotifyVimSearch(hwnd, WM_AR_VIM_SEARCH_CANCEL);
            ClearSearchState(state);
        } else if (state.mode == VimMode::Visual) {
            ExitVisualMode(hwnd, state);
        } else {
            ExitVimInsertMode(hwnd, state);
        }
        return true;
    }

    if (state.colonPending) {
        return HandleColonPromptChar(hwnd, wParam, state);
    }

    if (state.searchPending) {
        return HandleSearchPromptChar(hwnd, wParam, state);
    }

    if (state.mode == VimMode::Visual) {
        return HandleVisualChar(hwnd, wParam, state);
    }

    return HandleNormalChar(hwnd, wParam, state);
}
