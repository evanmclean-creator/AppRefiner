// Vim ':' ex-command layer for AppRefiner.
// Portions of the logic are derived from NppVim
// (https://github.com/h-jangra/NppVim), licensed under the MIT License.

#include "VimCommand.h"
#include "VimMode.h"
#include "../Scintilla.h"

#include <string>
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace {

    int Sci(HWND hwnd, UINT msg, WPARAM wParam = 0, LPARAM lParam = 0) {
        return static_cast<int>(::SendMessage(hwnd, msg, wParam, lParam));
    }

    std::string Trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return {};
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    // ─── Range parsing ───────────────────────────────────────────────────────

    int ResolveLineSpec(HWND hwnd, const std::string& spec) {
        if (spec.empty() || spec == ".")
            return Sci(hwnd, SCI_LINEFROMPOSITION, Sci(hwnd, SCI_GETCURRENTPOS));
        if (spec == "$")
            return Sci(hwnd, SCI_GETLINECOUNT) - 1;
        try   { return std::stoi(spec) - 1; }
        catch (...) { return -2; }
    }

    // Parse an optional range prefix from cmd into startLine/endLine (0-based,
    // inclusive).  Returns true and sets rest to the remainder of cmd if a
    // range was found; returns false and leaves everything unchanged otherwise.
    bool ParseRange(HWND hwnd, const std::string& cmd,
                    int& startLine, int& endLine, std::string& rest) {
        int lineCount = Sci(hwnd, SCI_GETLINECOUNT);

        if (!cmd.empty() && cmd[0] == '%') {
            startLine = 0;
            endLine   = lineCount - 1;
            rest      = cmd.substr(1);
            return true;
        }

        size_t i = 0;
        while (i < cmd.size() && (std::isdigit(static_cast<unsigned char>(cmd[i]))
               || cmd[i] == '.' || cmd[i] == '$')) i++;
        if (i == 0) return false;

        int line1 = ResolveLineSpec(hwnd, cmd.substr(0, i));
        if (line1 == -2) return false;
        line1 = std::max(0, std::min(line1, lineCount - 1));

        if (i < cmd.size() && cmd[i] == ',') {
            size_t j = i + 1;
            while (j < cmd.size() && (std::isdigit(static_cast<unsigned char>(cmd[j]))
                   || cmd[j] == '.' || cmd[j] == '$')) j++;
            int line2 = ResolveLineSpec(hwnd, cmd.substr(i + 1, j - i - 1));
            if (line2 == -2) line2 = line1;
            line2 = std::max(0, std::min(line2, lineCount - 1));
            if (line1 > line2) std::swap(line1, line2);
            startLine = line1;
            endLine   = line2;
            rest      = cmd.substr(j);
        } else {
            startLine = line1;
            endLine   = line1;
            rest      = cmd.substr(i);
        }
        return true;
    }

    // ─── Substitution ────────────────────────────────────────────────────────

    // Adapted from NppVim CommandMode::performSubstitution, MIT licensed.
    void PerformSubstitution(HWND hwnd,
                             const std::string& pattern,
                             const std::string& replacement,
                             bool useRegex, bool caseInsensitive,
                             bool replaceAll,
                             int startPos, int endPos) {
        if (pattern.empty()) return;

        Sci(hwnd, SCI_BEGINUNDOACTION);

        int flags = 0;
        if (useRegex)         flags |= SCFIND_REGEXP;
        if (!caseInsensitive) flags |= SCFIND_MATCHCASE;
        Sci(hwnd, SCI_SETSEARCHFLAGS, flags);

        int origPos = Sci(hwnd, SCI_GETCURRENTPOS);

        Sci(hwnd, SCI_SETTARGETSTART, startPos);
        Sci(hwnd, SCI_SETTARGETEND,   endPos);

        int replacements = 0;

        if (replaceAll) {
            int found = Sci(hwnd, SCI_SEARCHINTARGET,
                            static_cast<WPARAM>(pattern.size()),
                            reinterpret_cast<LPARAM>(pattern.c_str()));
            while (found != -1) {
                int oldEnd = Sci(hwnd, SCI_GETTARGETEND);
                useRegex
                    ? Sci(hwnd, SCI_REPLACETARGETRE,
                          static_cast<WPARAM>(replacement.size()),
                          reinterpret_cast<LPARAM>(replacement.c_str()))
                    : Sci(hwnd, SCI_REPLACETARGET,
                          static_cast<WPARAM>(replacement.size()),
                          reinterpret_cast<LPARAM>(replacement.c_str()));
                replacements++;
                int newEnd = Sci(hwnd, SCI_GETTARGETEND);
                endPos += (newEnd - oldEnd);
                Sci(hwnd, SCI_SETTARGETSTART, newEnd);
                Sci(hwnd, SCI_SETTARGETEND,   endPos);
                found = Sci(hwnd, SCI_SEARCHINTARGET,
                            static_cast<WPARAM>(pattern.size()),
                            reinterpret_cast<LPARAM>(pattern.c_str()));
            }
        } else {
            int found = Sci(hwnd, SCI_SEARCHINTARGET,
                            static_cast<WPARAM>(pattern.size()),
                            reinterpret_cast<LPARAM>(pattern.c_str()));
            if (found != -1) {
                useRegex
                    ? Sci(hwnd, SCI_REPLACETARGETRE,
                          static_cast<WPARAM>(replacement.size()),
                          reinterpret_cast<LPARAM>(replacement.c_str()))
                    : Sci(hwnd, SCI_REPLACETARGET,
                          static_cast<WPARAM>(replacement.size()),
                          reinterpret_cast<LPARAM>(replacement.c_str()));
                replacements++;
                Sci(hwnd, SCI_SETEMPTYSELECTION, Sci(hwnd, SCI_GETTARGETEND));
            }
        }

        if (replacements == 0)
            Sci(hwnd, SCI_SETEMPTYSELECTION, origPos);

        Sci(hwnd, SCI_ENDUNDOACTION);
    }

    // Parse and dispatch a :s command that has already had its range stripped.
    // trimCmd starts at 's', e.g. "s/foo/bar/g".
    void HandleSubstitution(HWND hwnd, VimSessionState& session,
                            const std::string& trimCmd,
                            int startPos, int endPos, bool rangeIsMultiLine) {
        if (trimCmd.size() < 2 || trimCmd[0] != 's') return;

        char delim      = trimCmd[1];
        size_t patStart = 2;
        size_t patEnd   = trimCmd.find(delim, patStart);
        if (patEnd == std::string::npos) return;

        std::string pattern = trimCmd.substr(patStart, patEnd - patStart);
        if (pattern.empty()) return;

        size_t repStart = patEnd + 1;
        size_t repEnd   = trimCmd.find(delim, repStart);
        if (repEnd == std::string::npos) repEnd = trimCmd.size();
        std::string replacement = trimCmd.substr(repStart, repEnd - repStart);

        bool caseInsensitive = session.ignoreCaseEnabled;
        bool replaceAll      = rangeIsMultiLine; // ranges replace all within them
        bool useRegex        = true;

        if (repEnd < trimCmd.size()) {
            for (char f : trimCmd.substr(repEnd + 1)) {
                switch (f) {
                case 'g': replaceAll = true;         break;
                case 'i': caseInsensitive = true;    break;
                case 'I': caseInsensitive = false;   break;
                case 'l': useRegex = false;           break;
                case 'c': break; // no blocking confirm in subclass proc
                default:  break;
                }
            }
        }

        PerformSubstitution(hwnd, pattern, replacement, useRegex,
                           caseInsensitive, replaceAll, startPos, endPos);
    }

    // ─── Register display ────────────────────────────────────────────────────

    std::string FormatRegisters(const VimSessionState& session) {
        auto preview = [](const std::string& s) -> std::string {
            std::string out;
            int n = 0;
            for (unsigned char c : s) {
                if (n >= 40) { out += "..."; break; }
                if      (c == '\n' || c == '\r') { out += "\\n"; n += 2; }
                else if (c == '\t')              { out += "\\t"; n += 2; }
                else if (c >= 32 && c < 127)     { out += static_cast<char>(c); n++; }
                else { char h[5]; sprintf_s(h, "\\x%02X", c); out += h; n += 4; }
            }
            return out;
        };

        std::string out = "--- Registers ---\r\n";
        if (!session.yankRegister.empty()) {
            out += "  \"  (default)   ";
            out += preview(session.yankRegister);
            out += "\r\n";
        }
        bool any = !session.yankRegister.empty();
        for (int i = 0; i < 26; i++) {
            const auto& r = session.namedRegisters[i];
            if (r.text.empty()) continue;
            char line[160];
            sprintf_s(line, "  %c             %s\r\n", 'a' + i, preview(r.text).c_str());
            out += line;
            any = true;
        }
        if (!any)
            out += "  (all registers empty)\r\n";
        return out;
    }

    // ─── Marks display ───────────────────────────────────────────────────────

    std::string FormatMarks(HWND hwnd, const VimEditorState& state) {
        std::string out = "--- Marks ---\r\n";
        bool any = false;
        for (int i = 0; i < 26; i++) {
            if (state.marks[i] < 0) continue;
            int pos  = state.marks[i];
            int line = Sci(hwnd, SCI_LINEFROMPOSITION, pos) + 1;
            int col  = pos - Sci(hwnd, SCI_POSITIONFROMLINE, line - 1) + 1;
            char buf[80];
            sprintf_s(buf, "  %c  line %-5d  col %d\r\n", 'a' + i, line, col);
            out += buf;
            any = true;
        }
        if (!any)
            out += "  (no marks set)\r\n";
        return out;
    }

    // ─── Cross-process dialog via WM_COPYDATA ────────────────────────────────

    // dwData identifier placed in COPYDATASTRUCT so C# can distinguish our messages.
    static const ULONG_PTR VIM_DIALOG_COPYDATA = 0x56494D44UL; // 'VIMD'

    // Send a formatted text string to C# for display in a MessageBoxDialog.
    // Payload layout: title NUL text NUL
    void SendDialogText(HWND editorHwnd,
                        const std::string& title,
                        const std::string& text) {
        if (!g_callbackWindow || !IsWindow(g_callbackWindow)) return;
        std::string payload = title + '\0' + text + '\0';
        COPYDATASTRUCT cds = {};
        cds.dwData = VIM_DIALOG_COPYDATA;
        cds.cbData = static_cast<DWORD>(payload.size());
        cds.lpData = const_cast<char*>(payload.data());
        ::SendMessage(g_callbackWindow, WM_COPYDATA,
                      reinterpret_cast<WPARAM>(editorHwnd),
                      reinterpret_cast<LPARAM>(&cds));
    }

} // anonymous namespace

// ─── Public entry point ──────────────────────────────────────────────────────

void ExecuteColonCommand(HWND hwnd, VimEditorState& state, VimSessionState& session,
                         const std::string& rawCmd) {
    const std::string cmd = Trim(rawCmd);
    if (cmd.empty()) return;

    // ── :noh / :nohl / :nohlsearch ───────────────────────────────────────
    if (cmd == "noh" || cmd == "nohl" || cmd == "nohlsearch") {
        if (g_callbackWindow && IsWindow(g_callbackWindow))
            ::SendMessage(g_callbackWindow, WM_AR_VIM_NOH,
                          reinterpret_cast<WPARAM>(hwnd), 0);
        return;
    }

    // ── bare line number ─────────────────────────────────────────────────
    {
        bool isNum = !cmd.empty();
        for (char c : cmd)
            if (!std::isdigit(static_cast<unsigned char>(c))) { isNum = false; break; }
        if (isNum) {
            int lineNum   = std::stoi(cmd);
            int lineCount = Sci(hwnd, SCI_GETLINECOUNT);
            if (lineNum >= 1 && lineNum <= lineCount) {
                Sci(hwnd, SCI_GOTOLINE, lineNum - 1);
                Sci(hwnd, SCI_SCROLLCARET);
            }
            return;
        }
    }

    // ── :set ignorecase / :set noignorecase ──────────────────────────────
    if (cmd == "set ignorecase" || cmd == "set ic") {
        session.ignoreCaseEnabled = true;
        return;
    }
    if (cmd == "set noignorecase" || cmd == "set noic") {
        session.ignoreCaseEnabled = false;
        return;
    }

    // ── :reg / :registers ────────────────────────────────────────────────
    if (cmd == "reg" || cmd == "registers") {
        SendDialogText(hwnd, "Vim Registers", FormatRegisters(session));
        return;
    }

    // ── :marks ───────────────────────────────────────────────────────────
    if (cmd == "marks") {
        SendDialogText(hwnd, "Vim Marks", FormatMarks(hwnd, state));
        return;
    }

    // ── :delm / :delmarks ────────────────────────────────────────────────
    if (cmd.rfind("delm", 0) == 0) {
        std::string arg = Trim(cmd.substr(4));
        if (arg == "!" || arg == "a-z") {
            for (int i = 0; i < 26; i++) state.marks[i] = -1;
        } else {
            for (char c : arg)
                if (c >= 'a' && c <= 'z') state.marks[c - 'a'] = -1;
        }
        return;
    }

    // ── :s and range variants (%, N, N,M, ., .$) ─────────────────────────
    {
        int  startLine = Sci(hwnd, SCI_LINEFROMPOSITION, Sci(hwnd, SCI_GETCURRENTPOS));
        int  endLine   = startLine;
        bool hasRange  = false;
        std::string rest;

        if (ParseRange(hwnd, cmd, startLine, endLine, rest))
            hasRange = true;
        else
            rest = cmd;

        const std::string trimRest = Trim(rest);

        if (!trimRest.empty() && trimRest[0] == 's' &&
            trimRest.size() > 1 &&
            !std::isalnum(static_cast<unsigned char>(trimRest[1]))) {

            int lineCount = Sci(hwnd, SCI_GETLINECOUNT);
            startLine = std::max(0, std::min(startLine, lineCount - 1));
            endLine   = std::max(0, std::min(endLine,   lineCount - 1));

            int startPos = Sci(hwnd, SCI_POSITIONFROMLINE, startLine);
            int endPos   = (endLine + 1 < lineCount)
                               ? Sci(hwnd, SCI_POSITIONFROMLINE, endLine + 1)
                               : Sci(hwnd, SCI_GETLENGTH);

            HandleSubstitution(hwnd, session, trimRest, startPos, endPos, hasRange);
            return;
        }
    }
}
