// Trie-based multi-key sequence matcher for Vim Normal mode.
// Ported from NppVim (https://github.com/h-jangra/NppVim), MIT License.
// Adapted to use per-editor state rather than a global VimState&.

#include "VimKeymap.h"
#include "VimMode.h"
#include "VimMotion.h"
#include "VimOperator.h"

namespace {
    int Sci(HWND hwnd, UINT msg, WPARAM wParam = 0, LPARAM lParam = 0) {
        return static_cast<int>(::SendMessage(hwnd, msg, wParam, lParam));
    }

    // Go to a 1-based line number, then move to first non-blank character.
    void GotoLine(HWND hwnd, int lineNumber) {
        int lineCount = Sci(hwnd, SCI_GETLINECOUNT);
        if (lineNumber < 1) lineNumber = 1;
        if (lineNumber > lineCount) lineNumber = lineCount;
        Sci(hwnd, SCI_GOTOLINE, lineNumber - 1); // Scintilla lines are 0-based
        Sci(hwnd, SCI_VCHOME);
    }
}

VimKeymap* g_normalKeymap = nullptr;

VimKeymap::VimKeymap() : root(std::make_shared<VimKeymapNode>()) {}

VimKeymap& VimKeymap::set(const std::string& keys, VimKeyHandler handler) {
    insertKeySequence(keys, handler);
    return *this;
}

void VimKeymap::insertKeySequence(const std::string& keys, VimKeyHandler handler, char motionChar) {
    auto node = root;
    for (char key : keys) {
        auto& child = node->children[key];
        if (!child) child = std::make_shared<VimKeymapNode>();
        node = child;
    }
    node->handler   = handler;
    node->motionChar = motionChar;
    node->isLeaf    = true;
}

bool VimKeymap::handleKey(HWND hwnd, char key, VimEditorState& state) {
    if (std::isdigit(static_cast<unsigned char>(key))) {
        // '0' with no pending count and no pending sequence fires as a command (line start),
        // not as a count digit. All other digits — and '0' when a count is already started —
        // accumulate into the count prefix.
        bool isZeroCommand = (key == '0' && state.pendingCount == 0 && state.keymapPendingKeys.empty());
        if (!isZeroCommand) {
            state.pendingCount = state.pendingCount * 10 + (key - '0');
            return true;
        }
    }

    // Pass the raw pendingCount (0 = no explicit count prefix was typed).
    // processKey resets it on success; leaves it intact if the key is not in the trie,
    // so the caller's switch statement can pick it up via ConsumeCount().
    int count = state.pendingCount;
    return processKey(hwnd, key, count, state);
}

bool VimKeymap::processKey(HWND hwnd, char key, int count, VimEditorState& state) {
    VimKeymapNode* searchNode = state.keymapCurrentNode ? state.keymapCurrentNode : root.get();

    auto it = searchNode->children.find(key);

    if (it == searchNode->children.end()) {
        if (searchNode != root.get()) {
            // Partial sequence failed — reset cursor and retry from root with the same key.
            state.keymapCurrentNode = nullptr;
            state.keymapPendingKeys.clear();
            return processKey(hwnd, key, count, state);
        }
        // Key is not in the trie at all — leave pendingCount for the caller's switch.
        return false;
    }

    state.keymapCurrentNode  = it->second.get();
    state.keymapPendingKeys += key;

    if (state.keymapCurrentNode->isLeaf && state.keymapCurrentNode->handler) {
        state.keymapCurrentNode->handler(hwnd, count);
        state.keymapCurrentNode = nullptr;
        state.keymapPendingKeys.clear();
        state.pendingCount = 0;
        return true;
    }

    // Partial match — waiting for the next key in the sequence.
    return true;
}

void InitializeNormalModeKeymap() {
    delete g_normalKeymap;
    g_normalKeymap = new VimKeymap();
    auto& k = *g_normalKeymap;

    // '0' — go to column 0 / line start.
    // Only fires when no count is pending and no other sequence is in progress;
    // otherwise '0' is treated as a count-prefix digit by handleKey.
    k.set("0", [](HWND h, int) {
        VimMotion::LineStart(h);
    });

    // gg — go to the first line of the document.
    k.set("gg", [](HWND h, int) {
        GotoLine(h, 1);
    });

    // gd — go to definition at the caret (delegates to AppRefiner's F12 logic).
    k.set("gd", [](HWND h, int) {
        if (g_callbackWindow && IsWindow(g_callbackWindow))
            ::SendMessage(g_callbackWindow, WM_AR_VIM_GOTO_DEFINITION,
                          reinterpret_cast<WPARAM>(h), 0);
    });

    // G — go to the last line (no count) or to line N (count prefix N).
    // NppVim convention: count == 0 or count == 1 means "no explicit count" → last line.
    // This means 1G behaves like G (goes to end); use gg to reach line 1.
    k.set("G", [](HWND h, int c) {
        if (c <= 1) {
            int lineCount = static_cast<int>(::SendMessage(h, SCI_GETLINECOUNT, 0, 0));
            GotoLine(h, lineCount);
        } else {
            GotoLine(h, c);
        }
    });

    // --- Operator line handlers (dd / yy / cc) ---
    // These fire when the second d/y/c arrives with opPending already set.
    // The first d/y/c was intercepted by the operator state machine (step 5 in
    // HandleNormalChar) which set opPending and returned.  The second d/y/c reaches
    // the keymap because it does NOT match steps 1-5 (opPending is already set).

    k.set("d", [](HWND h, int c) {
        // dd: delete count lines.
        VimEditorState& st = GetVimEditorState(h);
        st.opPending = 0;
        int count = (c > 0) ? c : 1;
        VimOperator::DeleteLines(h, count, st, GetVimSessionState());
    });

    k.set("y", [](HWND h, int c) {
        // yy: yank count lines.
        VimEditorState& st = GetVimEditorState(h);
        st.opPending = 0;
        int count = (c > 0) ? c : 1;
        VimOperator::YankLines(h, count, st, GetVimSessionState());
    });

    k.set("c", [](HWND h, int c) {
        // cc: change count lines — delete them and enter insert mode.
        VimEditorState& st = GetVimEditorState(h);
        st.opPending = 0;
        int count = (c > 0) ? c : 1;
        // Implement cc as: go to first non-blank, delete to end, enter insert mode.
        // For multi-line cc use DeleteLines then reopen a blank line.
        if (count == 1) {
            int pos  = static_cast<int>(::SendMessage(h, SCI_GETCURRENTPOS, 0, 0));
            int line = static_cast<int>(::SendMessage(h, SCI_LINEFROMPOSITION, pos, 0));
            int lineStart = static_cast<int>(::SendMessage(h, SCI_POSITIONFROMLINE, line, 0));
            int lineEnd   = static_cast<int>(::SendMessage(h, SCI_GETLINEENDPOSITION, line, 0));
            ::SendMessage(h, SCI_BEGINUNDOACTION, 0, 0);
            ::SendMessage(h, SCI_DELETERANGE, lineStart, lineEnd - lineStart);
            ::SendMessage(h, SCI_SETEMPTYSELECTION, lineStart, 0);
            ::SendMessage(h, SCI_ENDUNDOACTION, 0, 0);
        } else {
            // Delete all but leave the line open for editing.
            ::SendMessage(h, SCI_BEGINUNDOACTION, 0, 0);
            VimOperator::DeleteLines(h, count, st, GetVimSessionState());
            // DeleteLines already placed cursor at start; delete content of that line.
            int pos  = static_cast<int>(::SendMessage(h, SCI_GETCURRENTPOS, 0, 0));
            int line = static_cast<int>(::SendMessage(h, SCI_LINEFROMPOSITION, pos, 0));
            int lineStart = static_cast<int>(::SendMessage(h, SCI_POSITIONFROMLINE, line, 0));
            int lineEnd   = static_cast<int>(::SendMessage(h, SCI_GETLINEENDPOSITION, line, 0));
            if (lineEnd > lineStart)
                ::SendMessage(h, SCI_DELETERANGE, lineStart, lineEnd - lineStart);
            ::SendMessage(h, SCI_SETEMPTYSELECTION, lineStart, 0);
            ::SendMessage(h, SCI_ENDUNDOACTION, 0, 0);
        }
        EnterVimInsertMode(h, st);
    });

    // z commands — scroll without moving cursor
    k.set("zz", [](HWND h, int) {
        int cur  = (int)::SendMessage(h, SCI_LINEFROMPOSITION, ::SendMessage(h, SCI_GETCURRENTPOS, 0, 0), 0);
        int vis  = (int)::SendMessage(h, SCI_LINESONSCREEN, 0, 0);
        int first = cur - vis / 2;
        if (first < 0) first = 0;
        ::SendMessage(h, SCI_SETFIRSTVISIBLELINE, first, 0);
    });
    k.set("zt", [](HWND h, int) {
        int cur = (int)::SendMessage(h, SCI_LINEFROMPOSITION, ::SendMessage(h, SCI_GETCURRENTPOS, 0, 0), 0);
        ::SendMessage(h, SCI_SETFIRSTVISIBLELINE, cur, 0);
    });
    k.set("zb", [](HWND h, int) {
        int cur  = (int)::SendMessage(h, SCI_LINEFROMPOSITION, ::SendMessage(h, SCI_GETCURRENTPOS, 0, 0), 0);
        int vis  = (int)::SendMessage(h, SCI_LINESONSCREEN, 0, 0);
        int first = cur - vis + 1;
        if (first < 0) first = 0;
        ::SendMessage(h, SCI_SETFIRSTVISIBLELINE, first, 0);
    });

    // r — replace pending: next character replaces the one under the cursor.
    // 'r' is handled here in the keymap so the operator state machine doesn't
    // intercept it (r is not d/c/y and doesn't conflict with operators).
    k.set("r", [](HWND h, int) {
        VimEditorState& st = GetVimEditorState(h);
        st.replacePending = true;
    });

    // p / P — paste
    k.set("p", [](HWND h, int) {
        VimEditorState& st = GetVimEditorState(h);
        VimOperator::PasteAfter(h, st, GetVimSessionState());
    });

    k.set("P", [](HWND h, int) {
        VimEditorState& st = GetVimEditorState(h);
        VimOperator::PasteBefore(h, st, GetVimSessionState());
    });

    // --- Text object sequences ---
    // These are only reachable when the operator state machine has set textObjectPending
    // (e.g. after "di", "da", "ci", "ca") and then HandleNormalChar dispatches to
    // ApplyToTextObject in step 2.  The keymap entries below are NOT how text objects
    // fire; they exist only so the trie is aware of the two-key prefixes and won't
    // swallow the first key as an unknown sequence.
    //
    // Actually: text objects are fully handled by steps 2-3 in HandleNormalChar and
    // never reach the keymap.  So we do NOT register any iw/aw/etc. entries here;
    // doing so would cause the keymap to consume the keys before the operator state
    // machine can intercept them.
    //
    // Nothing to register for text objects in the keymap.
}

void ShutdownNormalModeKeymap() {
    delete g_normalKeymap;
    g_normalKeymap = nullptr;
}
