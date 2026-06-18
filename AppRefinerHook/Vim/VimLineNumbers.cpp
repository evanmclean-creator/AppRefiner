// Hybrid line numbers for Vim mode.
// Portions of the Vim behavior are derived from NppVim
// (https://github.com/h-jangra/NppVim), licensed under the MIT License.

#include "VimLineNumbers.h"
#include "../Scintilla.h"

// We use margin 0 (the same area App Designer uses for its own line numbers).
// When Vim mode is on, we take ownership of margin 0 as SC_MARGIN_RTEXT.
// App Designer's built-in line numbers are disabled by default, so there is no
// practical conflict; if the user enables App Designer numbers while Vim is active,
// our next SCN_UPDATEUI will restore the RTEXT type.
#define VLN_MARGIN          0
#define VLN_STYLE_OFFSET    40   // Base for our two custom styles (40 and 41)
#define VLN_STYLE_CURRENT   0    // offset+0 = style 40 -> current line (dark/bold)
#define VLN_STYLE_RELATIVE  1    // offset+1 = style 41 -> relative distance (gray)

namespace VimLineNumbers {

    static int Sci(HWND hwnd, UINT msg, WPARAM wp = 0, LPARAM lp = 0) {
        return static_cast<int>(SendMessage(hwnd, msg, wp, lp));
    }

    void Init(HWND hwnd) {
        if (!hwnd || !IsWindow(hwnd)) return;

        // Read the existing line-number margin background so our styles blend in.
        int bgColor = Sci(hwnd, SCI_STYLEGETBACK, STYLE_LINENUMBER);

        // Style offset: per-line style bytes are added to this base to get the real style.
        Sci(hwnd, SCI_MARGINSETSTYLEOFFSET, VLN_STYLE_OFFSET, 0);

        // Style 40 — current line: dark text, bold (prominent on App Designer's light theme)
        Sci(hwnd, SCI_STYLESETFORE, VLN_STYLE_OFFSET + VLN_STYLE_CURRENT, RGB(40, 40, 40));
        Sci(hwnd, SCI_STYLESETBACK, VLN_STYLE_OFFSET + VLN_STYLE_CURRENT, bgColor);
        Sci(hwnd, SCI_STYLESETBOLD, VLN_STYLE_OFFSET + VLN_STYLE_CURRENT, 1);

        // Style 41 — relative lines: dim gray, normal weight
        Sci(hwnd, SCI_STYLESETFORE, VLN_STYLE_OFFSET + VLN_STYLE_RELATIVE, RGB(160, 160, 160));
        Sci(hwnd, SCI_STYLESETBACK, VLN_STYLE_OFFSET + VLN_STYLE_RELATIVE, bgColor);
        Sci(hwnd, SCI_STYLESETBOLD, VLN_STYLE_OFFSET + VLN_STYLE_RELATIVE, 0);

        // Configure margin 0 as right-aligned text margin.
        Sci(hwnd, SCI_SETMARGINTYPEN, VLN_MARGIN, SC_MARGIN_RTEXT);

        Update(hwnd);
    }

    void Shutdown(HWND hwnd) {
        if (!hwnd || !IsWindow(hwnd)) return;

        Sci(hwnd, SCI_MARGINTEXTCLEARALL);
        Sci(hwnd, SCI_SETMARGINWIDTHN, VLN_MARGIN, 0);
        // Restore margin 0 to a standard number margin so App Designer can use it normally.
        Sci(hwnd, SCI_SETMARGINTYPEN, VLN_MARGIN, SC_MARGIN_NUMBER);
    }

    void Update(HWND hwnd) {
        if (!hwnd || !IsWindow(hwnd)) return;

        // Re-assert margin type in case App Designer reset it.
        Sci(hwnd, SCI_SETMARGINTYPEN, VLN_MARGIN, SC_MARGIN_RTEXT);

        int totalLines = Sci(hwnd, SCI_GETLINECOUNT);
        int curPos     = Sci(hwnd, SCI_GETCURRENTPOS);
        int curLine    = Sci(hwnd, SCI_LINEFROMPOSITION, curPos);  // 0-based
        int firstVis   = Sci(hwnd, SCI_GETFIRSTVISIBLELINE);
        int visLines   = Sci(hwnd, SCI_LINESONSCREEN);
        int lastVis    = firstVis + visLines;
        if (lastVis >= totalLines) lastVis = totalLines - 1;

        // Determine required margin width by measuring the widest string that will appear:
        // the absolute line number (could be up to 5+ digits) vs the relative distance.
        char sample[16];
        sprintf_s(sample, "%d", curLine + 1);
        int wCurrent = Sci(hwnd, SCI_TEXTWIDTH, VLN_STYLE_OFFSET + VLN_STYLE_CURRENT, (LPARAM)sample);

        int maxDist = (curLine - firstVis) > (lastVis - curLine)
                      ? (curLine - firstVis) : (lastVis - curLine);
        if (maxDist < 1) maxDist = 1;
        sprintf_s(sample, "%d", maxDist);
        int wRelative = Sci(hwnd, SCI_TEXTWIDTH, VLN_STYLE_OFFSET + VLN_STYLE_RELATIVE, (LPARAM)sample);

        int textWidth = (wCurrent > wRelative) ? wCurrent : wRelative;
        Sci(hwnd, SCI_SETMARGINWIDTHN, VLN_MARGIN, textWidth + 6);  // 6px horizontal padding

        // Clear stale text from previous position and repopulate visible lines.
        Sci(hwnd, SCI_MARGINTEXTCLEARALL);

        char buf[16];
        for (int line = firstVis; line <= lastVis; ++line) {
            if (line == curLine) {
                sprintf_s(buf, "%d", line + 1);  // absolute, 1-based
                Sci(hwnd, SCI_MARGINSETTEXT,  line, (LPARAM)buf);
                Sci(hwnd, SCI_MARGINSETSTYLE, line, VLN_STYLE_CURRENT);
            } else {
                int dist = (line > curLine) ? (line - curLine) : (curLine - line);
                sprintf_s(buf, "%d", dist);
                Sci(hwnd, SCI_MARGINSETTEXT,  line, (LPARAM)buf);
                Sci(hwnd, SCI_MARGINSETSTYLE, line, VLN_STYLE_RELATIVE);
            }
        }
    }

}  // namespace VimLineNumbers
