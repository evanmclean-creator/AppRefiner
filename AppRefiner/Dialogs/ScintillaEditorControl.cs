using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace AppRefiner.Dialogs
{
    /// <summary>
    /// Hosts a native Scintilla edit control inside a WinForms control, in AppRefiner's own
    /// process. AppRefiner already ships Scintilla.dll (scintilla_mods/&lt;version&gt;) and loads it
    /// into Application Designer's process for the enhanced editor; here we LoadLibrary it into
    /// our own process so the "Scintilla" window class is registered, then CreateWindowEx a child.
    ///
    /// This is the foundation for the cross-environment diff surface (DIFF_TOOL.md step 4): two of
    /// these panes, aligned via annotations and decorated with markers/indicators, rather than the
    /// text-mutation alignment of the previous RichTextBox prototype.
    /// </summary>
    public sealed class ScintillaEditorControl : Control
    {
        // --- Scintilla message constants (subset needed here) ---
        private const int SCI_SETTEXT = 2181;
        private const int SCI_SETREADONLY = 2171;
        private const int SCI_SETCODEPAGE = 2037;
        private const int SC_CP_UTF8 = 65001;
        private const int SCI_STYLESETFONT = 2056;
        private const int SCI_STYLESETSIZE = 2055;
        private const int SCI_STYLECLEARALL = 2050;
        private const int STYLE_DEFAULT = 32;
        private const int SCI_SETMARGINWIDTHN = 2242;
        private const int SCI_SETWRAPMODE = 2268;
        private const int SC_WRAP_NONE = 0;

        private const int SCI_ANNOTATIONSETTEXT = 2540;
        private const int SCI_ANNOTATIONSETVISIBLE = 2548;
        private const int SCI_ANNOTATIONCLEARALL = 2547;
        private const int ANNOTATION_STANDARD = 1;

        private const int SCI_MARKERDEFINE = 2040;
        private const int SCI_MARKERSETBACK = 2042;
        private const int SCI_MARKERADD = 2043;
        private const int SCI_MARKERDELETEALL = 2045;
        private const int SC_MARK_BACKGROUND = 22;

        private const int SCI_INDICSETSTYLE = 2080;
        private const int SCI_INDICSETFORE = 2082;
        private const int SCI_INDICSETALPHA = 2523;
        private const int SCI_SETINDICATORCURRENT = 2500;
        private const int SCI_INDICATORFILLRANGE = 2504;
        private const int SCI_INDICATORCLEARRANGE = 2505;
        private const int INDIC_ROUNDBOX = 7;

        private const int SCI_POSITIONFROMLINE = 2167;
        private const int SCI_GETFIRSTVISIBLELINE = 2152;
        private const int SCI_SETFIRSTVISIBLELINE = 2613;
        private const int SCI_POINTYFROMPOSITION = 2165;
        private const int SCI_GETTEXTLENGTH = 2183;
        private const int SCI_GETTEXT = 2182;
        private const int SCI_SETMODEVENTMASK = 2359;
        private const int SC_MOD_INSERTTEXT = 0x1;
        private const int SC_MOD_DELETETEXT = 0x2;

        private const int WM_NOTIFY = 0x004E;
        private const int SCN_UPDATEUI = 2007;
        private const int SCN_MODIFIED = 2008;
        private const int WM_PARENTNOTIFY = 0x0210;
        private const int WM_LBUTTONDOWN = 0x0201;

        // Win32 window styles
        private const int WS_CHILD = 0x40000000;
        private const int WS_VISIBLE = 0x10000000;
        private const int WS_HSCROLL = 0x00100000;
        private const int WS_VSCROLL = 0x00200000;

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern IntPtr LoadLibrary(string lpFileName);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr GetProcAddress(IntPtr hModule, string procName);

        [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern IntPtr CreateWindowEx(int exStyle, string className, string windowName,
            int style, int x, int y, int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr param);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool DestroyWindow(IntPtr hwnd);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool MoveWindow(IntPtr hwnd, int x, int y, int width, int height, bool repaint);

        [DllImport("user32.dll", CharSet = CharSet.Auto)]
        private static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

        [DllImport("user32.dll")]
        private static extern IntPtr SetFocus(IntPtr hWnd);

        // Scintilla exports a class-registration entry point used when the DLL is loaded for hosting.
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int RegisterClassesDelegate(IntPtr hInstance);

        private static IntPtr s_scintillaModule = IntPtr.Zero;
        private static bool s_loadAttempted;
        private static string? s_loadError;

        private IntPtr _sciHwnd = IntPtr.Zero;

        /// <summary>Raised on Scintilla SCN_UPDATEUI (scroll/caret/selection change) — used for scroll sync.</summary>
        public event EventHandler? ViewChanged;

        /// <summary>Raised on Scintilla SCN_MODIFIED, restricted to text insert/delete (see the mod
        /// event mask set in CreateScintilla) — used to drive debounced local-edit recompute.</summary>
        public event EventHandler? TextModified;

        /// <summary>Last error encountered while loading/creating the Scintilla control, if any.</summary>
        public string? HostError { get; private set; }

        /// <summary>True if the native Scintilla child window was created successfully.</summary>
        public bool IsHosted => _sciHwnd != IntPtr.Zero;

        public ScintillaEditorControl()
        {
            // Be a real focus participant so the dialog routes keyboard through this control
            // (and not a previously-clicked button) when the Scintilla pane is active.
            SetStyle(ControlStyles.Selectable, true);
            TabStop = true;
        }

        protected override void OnGotFocus(EventArgs e)
        {
            base.OnGotFocus(e);
            // This control can't render text itself — hand Win32 focus to the hosted Scintilla child.
            if (_sciHwnd != IntPtr.Zero)
            {
                SetFocus(_sciHwnd);
            }
        }

        protected override bool IsInputKey(Keys keyData)
        {
            // These belong to the Scintilla pane (newline, indent, caret movement) — don't let the
            // dialog treat them as navigation keys and route them to a button.
            switch (keyData & Keys.KeyCode)
            {
                case Keys.Enter:
                case Keys.Tab:
                case Keys.Up:
                case Keys.Down:
                case Keys.Left:
                case Keys.Right:
                    return true;
                default:
                    return base.IsInputKey(keyData);
            }
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            CreateScintilla();
        }

        protected override void OnHandleDestroyed(EventArgs e)
        {
            if (_sciHwnd != IntPtr.Zero)
            {
                DestroyWindow(_sciHwnd);
                _sciHwnd = IntPtr.Zero;
            }
            base.OnHandleDestroyed(e);
        }

        protected override void OnSizeChanged(EventArgs e)
        {
            base.OnSizeChanged(e);
            if (_sciHwnd != IntPtr.Zero)
            {
                MoveWindow(_sciHwnd, 0, 0, Math.Max(0, ClientSize.Width), Math.Max(0, ClientSize.Height), true);
            }
        }

        protected override void WndProc(ref Message m)
        {
            base.WndProc(ref m);

            // Scintilla notifications arrive as WM_NOTIFY on the parent (this control). The
            // notification code sits at offset (2 * pointer size) past the start of the
            // SCNotification/NMHDR (hwndFrom + idFrom, then the UINT code).
            if (m.Msg == WM_NOTIFY && m.LParam != IntPtr.Zero)
            {
                int code = Marshal.ReadInt32(m.LParam, IntPtr.Size * 2);
                if (code == SCN_UPDATEUI)
                {
                    ViewChanged?.Invoke(this, EventArgs.Empty);
                }
                else if (code == SCN_MODIFIED)
                {
                    TextModified?.Invoke(this, EventArgs.Empty);
                }
            }
            else if (m.Msg == WM_PARENTNOTIFY && (m.WParam.ToInt32() & 0xFFFF) == WM_LBUTTONDOWN)
            {
                // The Scintilla child was clicked. Make this the active WinForms control so the
                // dialog stops routing keys (Enter) to a previously-focused button; OnGotFocus then
                // forwards Win32 focus back to the child.
                Focus();
            }
        }

        private void CreateScintilla()
        {
            IntPtr module = EnsureScintillaLoaded();
            if (module == IntPtr.Zero)
            {
                HostError = s_loadError ?? "Scintilla.dll could not be loaded.";
                return;
            }

            _sciHwnd = CreateWindowEx(0, "Scintilla", string.Empty,
                WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL,
                0, 0, Math.Max(0, ClientSize.Width), Math.Max(0, ClientSize.Height),
                Handle, IntPtr.Zero, module, IntPtr.Zero);

            if (_sciHwnd == IntPtr.Zero)
            {
                HostError = $"CreateWindowEx(\"Scintilla\") failed (error {Marshal.GetLastWin32Error()}).";
                return;
            }

            // Baseline configuration: UTF-8, monospace, no wrap, no margins (background markers
            // color the whole line so a margin isn't required for the spike).
            Send(SCI_SETCODEPAGE, SC_CP_UTF8, 0);
            SetMonospaceFont("Consolas", 10);
            Send(SCI_SETWRAPMODE, SC_WRAP_NONE, 0);
            for (int margin = 0; margin < 5; margin++)
            {
                Send(SCI_SETMARGINWIDTHN, margin, 0);
            }

            // Restrict SCN_MODIFIED to actual text insert/delete so the dialog's edit handler
            // isn't woken by marker/annotation/style changes.
            Send(SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT, 0);
        }

        private static IntPtr EnsureScintillaLoaded()
        {
            if (s_loadAttempted)
            {
                return s_scintillaModule;
            }

            s_loadAttempted = true;

            try
            {
                string? dllPath = ResolveScintillaDllPath();
                if (dllPath == null)
                {
                    s_loadError = "Could not locate scintilla_mods/<version>/Scintilla.dll next to AppRefiner.";
                    return IntPtr.Zero;
                }

                IntPtr module = LoadLibrary(dllPath);
                if (module == IntPtr.Zero)
                {
                    s_loadError = $"LoadLibrary failed for '{dllPath}' (error {Marshal.GetLastWin32Error()}).";
                    return IntPtr.Zero;
                }

                // Newer Scintilla registers the window class in DllMain, but call the explicit
                // registration entry point when present so we don't depend on that behavior.
                IntPtr regProc = GetProcAddress(module, "Scintilla_RegisterClasses");
                if (regProc != IntPtr.Zero)
                {
                    var register = Marshal.GetDelegateForFunctionPointer<RegisterClassesDelegate>(regProc);
                    register(module);
                }

                s_scintillaModule = module;
                return module;
            }
            catch (Exception ex)
            {
                s_loadError = $"Exception loading Scintilla.dll: {ex.Message}";
                return IntPtr.Zero;
            }
        }

        private static string? ResolveScintillaDllPath()
        {
            string? exeDir = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
            if (string.IsNullOrEmpty(exeDir))
            {
                return null;
            }

            string modsDir = Path.Combine(exeDir, "scintilla_mods");
            if (!Directory.Exists(modsDir))
            {
                return null;
            }

            // Prefer the highest version subfolder that actually contains Scintilla.dll.
            string? best = Directory.GetDirectories(modsDir)
                .Select(dir => Path.Combine(dir, "Scintilla.dll"))
                .Where(File.Exists)
                .OrderByDescending(path => path, StringComparer.OrdinalIgnoreCase)
                .FirstOrDefault();

            if (best != null)
            {
                return best;
            }

            // Fallback: a Scintilla.dll directly under scintilla_mods.
            string direct = Path.Combine(modsDir, "Scintilla.dll");
            return File.Exists(direct) ? direct : null;
        }

        private IntPtr Send(int msg, int wParam, int lParam)
        {
            if (_sciHwnd == IntPtr.Zero)
            {
                return IntPtr.Zero;
            }

            return SendMessage(_sciHwnd, msg, new IntPtr(wParam), new IntPtr(lParam));
        }

        private void SetMonospaceFont(string fontName, int sizePoints)
        {
            IntPtr fontPtr = Utf8ToHGlobal(fontName);
            try
            {
                SendMessage(_sciHwnd, SCI_STYLESETFONT, new IntPtr(STYLE_DEFAULT), fontPtr);
                Send(SCI_STYLESETSIZE, STYLE_DEFAULT, sizePoints);
                Send(SCI_STYLECLEARALL, 0, 0);
            }
            finally
            {
                Marshal.FreeHGlobal(fontPtr);
            }
        }

        public void SetText(string text)
        {
            if (_sciHwnd == IntPtr.Zero)
            {
                return;
            }

            IntPtr ptr = Utf8ToHGlobal(text ?? string.Empty);
            try
            {
                // SETTEXT is ignored while read-only; clear it first, caller re-applies read-only.
                SendMessage(_sciHwnd, SCI_SETREADONLY, IntPtr.Zero, IntPtr.Zero);
                SendMessage(_sciHwnd, SCI_SETTEXT, IntPtr.Zero, ptr);
            }
            finally
            {
                Marshal.FreeHGlobal(ptr);
            }
        }

        public void SetReadOnly(bool readOnly)
        {
            Send(SCI_SETREADONLY, readOnly ? 1 : 0, 0);
        }

        /// <summary>Gives keyboard focus to the hosted Scintilla pane. Needed after WinForms buttons
        /// are clicked — otherwise the button keeps focus and Enter re-activates it instead of
        /// inserting a newline. Sets this as the active WinForms control (so dialog key routing goes
        /// through it) and hands Win32 focus to the Scintilla child.</summary>
        public void FocusEditor()
        {
            Focus();
            if (_sciHwnd != IntPtr.Zero)
            {
                SetFocus(_sciHwnd);
            }
        }

        public string GetText()
        {
            if (_sciHwnd == IntPtr.Zero)
            {
                return string.Empty;
            }

            int byteLength = TextLength;
            if (byteLength <= 0)
            {
                return string.Empty;
            }

            IntPtr buffer = Marshal.AllocHGlobal(byteLength + 1);
            try
            {
                // SCI_GETTEXT copies up to (wParam - 1) bytes plus a null terminator.
                SendMessage(_sciHwnd, SCI_GETTEXT, new IntPtr(byteLength + 1), buffer);
                byte[] bytes = new byte[byteLength];
                Marshal.Copy(buffer, bytes, 0, byteLength);
                return Encoding.UTF8.GetString(bytes);
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }

        // --- Diff visuals (the ComparePlus / VS Code vocabulary, clean-room) ---

        public void DefineLineMarker(int markerId, Color background)
        {
            Send(SCI_MARKERDEFINE, markerId, SC_MARK_BACKGROUND);
            Send(SCI_MARKERSETBACK, markerId, ColorToBgr(background));
        }

        public void AddLineMarker(int markerId, int line)
        {
            Send(SCI_MARKERADD, line, markerId);
        }

        public void ClearLineMarkers(int markerId)
        {
            Send(SCI_MARKERDELETEALL, markerId, 0);
        }

        public void DefineChangeIndicator(int indicatorId, Color color, int alpha)
        {
            Send(SCI_INDICSETSTYLE, indicatorId, INDIC_ROUNDBOX);
            Send(SCI_INDICSETFORE, indicatorId, ColorToBgr(color));
            Send(SCI_INDICSETALPHA, indicatorId, alpha);
        }

        public void FillIndicatorRange(int indicatorId, int startPos, int length)
        {
            if (length <= 0)
            {
                return;
            }

            Send(SCI_SETINDICATORCURRENT, indicatorId, 0);
            Send(SCI_INDICATORFILLRANGE, startPos, length);
        }

        public void ClearIndicatorRange(int indicatorId, int startPos, int length)
        {
            Send(SCI_SETINDICATORCURRENT, indicatorId, 0);
            Send(SCI_INDICATORCLEARRANGE, startPos, Math.Max(0, length));
        }

        /// <summary>Adds <paramref name="blankLineCount"/> blank annotation lines below a document line,
        /// pushing subsequent lines down so the two panes align without mutating document text.</summary>
        public void SetAlignmentAnnotation(int line, int blankLineCount)
        {
            if (_sciHwnd == IntPtr.Zero || blankLineCount <= 0)
            {
                return;
            }

            // An annotation of N display lines is N-1 newlines; use spaces so the line renders.
            string annotation = string.Join("\n", Enumerable.Repeat(" ", blankLineCount));
            IntPtr ptr = Utf8ToHGlobal(annotation);
            try
            {
                SendMessage(_sciHwnd, SCI_ANNOTATIONSETTEXT, new IntPtr(line), ptr);
            }
            finally
            {
                Marshal.FreeHGlobal(ptr);
            }

            Send(SCI_ANNOTATIONSETVISIBLE, ANNOTATION_STANDARD, 0);
        }

        public void ClearAnnotations()
        {
            Send(SCI_ANNOTATIONCLEARALL, 0, 0);
        }

        public int PositionFromLine(int line) => (int)Send(SCI_POSITIONFROMLINE, line, 0);

        public int TextLength => (int)Send(SCI_GETTEXTLENGTH, 0, 0);

        public int GetFirstVisibleLine() => (int)Send(SCI_GETFIRSTVISIBLELINE, 0, 0);

        public void SetFirstVisibleLine(int line) => Send(SCI_SETFIRSTVISIBLELINE, Math.Max(0, line), 0);

        /// <summary>Y pixel (relative to the control) of the top of the given document line.</summary>
        public int PointYFromLine(int line)
        {
            int pos = PositionFromLine(line);
            return (int)Send(SCI_POINTYFROMPOSITION, 0, pos);
        }

        private static int ColorToBgr(Color color) => color.R | (color.G << 8) | (color.B << 16);

        private static IntPtr Utf8ToHGlobal(string text)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(text);
            IntPtr ptr = Marshal.AllocHGlobal(bytes.Length + 1);
            Marshal.Copy(bytes, 0, ptr, bytes.Length);
            Marshal.WriteByte(ptr, bytes.Length, 0);
            return ptr;
        }
    }
}
