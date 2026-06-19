using System.Text;

namespace AppRefiner.Dialogs
{
    /// <summary>
    /// Dialog showing a Vim and AppRefiner reference sheet.
    /// </summary>
    public class CheatsheetDialog : Form
    {
        private readonly Panel headerPanel;
        private readonly Label headerLabel;
        private readonly TabControl tabControl;
        private readonly TabPage vimTabPage;
        private readonly TabPage appRefinerTabPage;
        private readonly RichTextBox vimTextBox;
        private readonly RichTextBox appRefinerTextBox;
        private readonly Button closeButton;
        private readonly IntPtr owner;
        private DialogHelper.ModalDialogMouseHandler? mouseHandler;

        public CheatsheetDialog(IEnumerable<Command> availableCommands, IntPtr owner = default)
        {
            this.owner = owner;
            headerPanel = new Panel();
            headerLabel = new Label();
            tabControl = new TabControl();
            vimTabPage = new TabPage();
            appRefinerTabPage = new TabPage();
            vimTextBox = new RichTextBox();
            appRefinerTextBox = new RichTextBox();
            closeButton = new Button();

            InitializeComponent();
            LoadContent(availableCommands);
        }

        private void InitializeComponent()
        {
            headerPanel.SuspendLayout();
            tabControl.SuspendLayout();
            vimTabPage.SuspendLayout();
            appRefinerTabPage.SuspendLayout();
            SuspendLayout();

            headerPanel.BackColor = Color.FromArgb(50, 50, 60);
            headerPanel.Dock = DockStyle.Top;
            headerPanel.Height = 30;
            headerPanel.Controls.Add(headerLabel);

            headerLabel.Text = "Vim + AppRefiner Cheatsheet";
            headerLabel.ForeColor = Color.White;
            headerLabel.Font = new Font("Segoe UI", 9F, FontStyle.Regular, GraphicsUnit.Point);
            headerLabel.Dock = DockStyle.Fill;
            headerLabel.TextAlign = ContentAlignment.MiddleCenter;

            tabControl.Location = new Point(20, 50);
            tabControl.Size = new Size(860, 520);
            tabControl.TabPages.Add(vimTabPage);
            tabControl.TabPages.Add(appRefinerTabPage);

            vimTabPage.Text = "Vim";
            vimTabPage.Controls.Add(vimTextBox);

            appRefinerTabPage.Text = "AppRefiner";
            appRefinerTabPage.Controls.Add(appRefinerTextBox);

            ConfigureTextBox(vimTextBox);
            ConfigureTextBox(appRefinerTextBox);

            closeButton.Text = "Close";
            closeButton.Size = new Size(100, 30);
            closeButton.Location = new Point(780, 585);
            closeButton.BackColor = Color.FromArgb(100, 100, 100);
            closeButton.ForeColor = Color.White;
            closeButton.FlatStyle = FlatStyle.Flat;
            closeButton.FlatAppearance.BorderSize = 0;
            closeButton.Click += (s, e) =>
            {
                DialogResult = DialogResult.OK;
                Close();
            };

            Text = "Cheatsheet";
            ClientSize = new Size(900, 630);
            Controls.Add(headerPanel);
            Controls.Add(tabControl);
            Controls.Add(closeButton);
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.Manual;
            MaximizeBox = false;
            MinimizeBox = false;
            ShowInTaskbar = false;
            BackColor = Color.FromArgb(240, 240, 245);
            Padding = new Padding(1);
            AcceptButton = closeButton;

            headerPanel.ResumeLayout(false);
            tabControl.ResumeLayout(false);
            vimTabPage.ResumeLayout(false);
            appRefinerTabPage.ResumeLayout(false);
            ResumeLayout(false);
        }

        private static void ConfigureTextBox(RichTextBox textBox)
        {
            textBox.Dock = DockStyle.Fill;
            textBox.BorderStyle = BorderStyle.None;
            textBox.ReadOnly = true;
            textBox.Font = new Font("Consolas", 10F, FontStyle.Regular, GraphicsUnit.Point);
            textBox.BackColor = Color.White;
            textBox.WordWrap = false;
            textBox.ScrollBars = RichTextBoxScrollBars.Both;
        }

        private void LoadContent(IEnumerable<Command> availableCommands)
        {
            vimTextBox.Text = BuildVimCheatsheet();
            appRefinerTextBox.Text = BuildAppRefinerCheatsheet(availableCommands);
        }

        private static string BuildVimCheatsheet()
        {
            StringBuilder sb = new();
            sb.AppendLine("Most commands accept a count prefix, e.g. 3j, 5dd, 2dw, d3w.");
            sb.AppendLine();

            sb.AppendLine("MODES");
            sb.AppendLine("  Esc            return to Normal mode");
            sb.AppendLine("  i  a           insert before / after cursor");
            sb.AppendLine("  I  A           insert at first nonblank / end of line");
            sb.AppendLine("  o  O           open new line below / above");
            sb.AppendLine("  v  V           charwise / linewise Visual mode");
            sb.AppendLine();

            sb.AppendLine("CURSOR MOVEMENT");
            sb.AppendLine("  h j k l        left / down / up / right");
            sb.AppendLine("  w  W           next word / WORD (whitespace-delimited)");
            sb.AppendLine("  b  B           previous word / WORD");
            sb.AppendLine("  e  E           end of word / WORD");
            sb.AppendLine("  0  ^  $        line start / first nonblank / line end");
            sb.AppendLine("  { }            previous / next paragraph");
            sb.AppendLine("  %              jump to matching ( ) [ ] { }");
            sb.AppendLine("  gg  G          first line / last line");
            sb.AppendLine("  42G            go to line 42 (also :42)");
            sb.AppendLine("  f<c> F<c>      find char forward / backward on line");
            sb.AppendLine("  t<c> T<c>      till before char forward / backward");
            sb.AppendLine("  ;  ,           repeat / reverse last f F t T");
            sb.AppendLine();

            sb.AppendLine("SCROLLING");
            sb.AppendLine("  Ctrl+d Ctrl+u  half page down / up");
            sb.AppendLine("  PgDn PgUp      page down / up (Ctrl+b also pages up)");
            sb.AppendLine("  Ctrl+e Ctrl+y  scroll one line down / up");
            sb.AppendLine("  zz zt zb       cursor line to center / top / bottom");
            sb.AppendLine();

            sb.AppendLine("EDITING");
            sb.AppendLine("  x  X           delete char under / before cursor");
            sb.AppendLine("  r<c>           replace one character");
            sb.AppendLine("  s  S           substitute char / whole line");
            sb.AppendLine("  ~              toggle case of character");
            sb.AppendLine("  J              join line below into current");
            sb.AppendLine("  u  Ctrl+r      undo / redo");
            sb.AppendLine("  .              repeat last change");
            sb.AppendLine();

            sb.AppendLine("OPERATORS (combine with a motion or text object)");
            sb.AppendLine("  d  c  y        delete / change / yank");
            sb.AppendLine("  dd cc yy       whole line");
            sb.AppendLine("  D  C           delete / change to end of line");
            sb.AppendLine("  dw d$ d}       e.g. delete word / to line end / paragraph");
            sb.AppendLine("  p  P           paste after / before");
            sb.AppendLine();

            sb.AppendLine("TEXT OBJECTS (use after d / c / y, e.g. diw, ci\", da( )");
            sb.AppendLine("  iw  aw         inner / a word");
            sb.AppendLine("  i\"  a\"  i'  a'  inner / a quoted string");
            sb.AppendLine("  i(  a(  i)  a)  inner / a parentheses");
            sb.AppendLine("  i{  a{  i[  a[  inner / a braces / brackets");
            sb.AppendLine("  i<  a<         inner / a angle brackets");
            sb.AppendLine();

            sb.AppendLine("VISUAL MODE");
            sb.AppendLine("  v  V           start charwise / linewise selection");
            sb.AppendLine("  motions        extend the selection");
            sb.AppendLine("  d  y  c        delete / yank / change selection");
            sb.AppendLine("  Esc            leave Visual mode");
            sb.AppendLine();

            sb.AppendLine("SEARCH");
            sb.AppendLine("  /text  ?text   search forward / backward");
            sb.AppendLine("  n  N           next / previous match");
            sb.AppendLine("  :noh           clear search highlights");
            sb.AppendLine();

            sb.AppendLine("MARKS, REGISTERS, MACROS");
            sb.AppendLine("  m<a-z>         set mark");
            sb.AppendLine("  `<a-z>  '<a-z> jump to mark (exact / line)");
            sb.AppendLine("  \"<a-z>y \"<a-z>p  yank / paste via named register");
            sb.AppendLine("  q<a-z> ... q   record macro into register");
            sb.AppendLine("  @<a-z>  @@      replay macro / replay last");
            sb.AppendLine();

            sb.AppendLine("EX COMMANDS ( : )");
            sb.AppendLine("  :42            go to line 42");
            sb.AppendLine("  :s/old/new/    substitute on current line (flags g i I)");
            sb.AppendLine("  :%s/old/new/g  substitute in whole file");
            sb.AppendLine("  :5,12s/a/b/g   substitute in a line range");
            sb.AppendLine("  :noh           clear search highlights");
            sb.AppendLine("  :set ic        ignorecase on   (:set noic = off)");
            sb.AppendLine("  :reg  :marks   list registers / marks");
            sb.AppendLine("  :delm <a-z>    delete mark(s)");
            sb.AppendLine("  :q             close the current editor");
            sb.AppendLine("  :help  :h      this cheatsheet");
            sb.AppendLine();

            sb.AppendLine("APPREFINER-BACKED (Normal mode)");
            sb.AppendLine("  K              show tooltip / hover info at cursor");
            sb.AppendLine("  gd             go to definition (same as F12)");
            sb.AppendLine("  Ctrl+o Ctrl+i  jump back / forward (Go To Definition history)");
            sb.AppendLine("  Shift+h Shift+l  previous / next PeopleCode editor");
            return sb.ToString();
        }

        private static string BuildAppRefinerCheatsheet(IEnumerable<Command> availableCommands)
        {
            StringBuilder sb = new();

            sb.AppendLine("GLOBAL SHORTCUTS (active in any PeopleCode editor)");
            sb.AppendLine("  Ctrl+Shift+P        Command Palette");
            sb.AppendLine("  Ctrl+O              Smart Open (object search)");
            sb.AppendLine("  Ctrl+F              Better Find");
            sb.AppendLine("  Ctrl+H              Better Find / Replace");
            sb.AppendLine("  F3 / Shift+F3       Find Next / Previous");
            sb.AppendLine("  F12                 Go To Definition");
            sb.AppendLine("  Ctrl+Space          Invoke Autocomplete");
            sb.AppendLine("  Shift+Up / Down     Extend selection by line");
            sb.AppendLine("  Alt+Left / Right    Navigate Back / Forward");
            sb.AppendLine();
            sb.AppendLine("  Several of these (Open, Find/Replace, line selection) can be");
            sb.AppendLine("  turned off under Settings > Feature Overrides.");
            sb.AppendLine();

            // Commands that registered their own shortcut: GetDisplayName() formats the
            // title as "Name (Shortcut)", so a trailing parenthesized group is the shortcut.
            var withShortcuts = new List<(string shortcut, string name)>();
            foreach (var command in availableCommands)
            {
                var (name, shortcut) = SplitTitleShortcut(command.Title ?? string.Empty);
                if (shortcut != null)
                {
                    withShortcuts.Add((shortcut, name));
                }
            }

            if (withShortcuts.Count > 0)
            {
                sb.AppendLine("COMMAND SHORTCUTS");
                foreach (var (shortcut, name) in withShortcuts.OrderBy(s => s.name, StringComparer.OrdinalIgnoreCase))
                {
                    sb.AppendLine($"  {shortcut,-20}{name}");
                }
                sb.AppendLine();
            }

            sb.AppendLine("ALL COMMAND PALETTE COMMANDS (Ctrl+Shift+P)");
            foreach (var command in availableCommands.OrderBy(c => c.Title, StringComparer.OrdinalIgnoreCase))
            {
                string name = command.Title ?? string.Empty;
                string description = command.Description ?? string.Empty;
                sb.Append("  ");
                sb.Append(name);
                if (!string.IsNullOrWhiteSpace(description))
                {
                    sb.Append(" - ");
                    sb.Append(description);
                }
                sb.AppendLine();
            }

            return sb.ToString();
        }

        /// <summary>
        /// Splits a command display title of the form "Name (Shortcut)" into its name and
        /// shortcut parts. Returns a null shortcut when the title has no registered shortcut.
        /// </summary>
        private static (string name, string? shortcut) SplitTitleShortcut(string title)
        {
            int open = title.LastIndexOf(" (", StringComparison.Ordinal);
            if (open > 0 && title.EndsWith(")", StringComparison.Ordinal))
            {
                string shortcut = title.Substring(open + 2, title.Length - open - 3);
                return (title.Substring(0, open), shortcut);
            }
            return (title, null);
        }

        protected override void OnShown(EventArgs e)
        {
            base.OnShown(e);

            if (owner != IntPtr.Zero)
            {
                WindowHelper.CenterFormOnWindow(this, owner);
            }
            else
            {
                CenterToScreen();
            }

            if (Modal && owner != IntPtr.Zero)
            {
                mouseHandler = new DialogHelper.ModalDialogMouseHandler(this, headerPanel, owner);
            }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            using var pen = new Pen(Color.FromArgb(100, 100, 120));
            e.Graphics.DrawRectangle(pen, 0, 0, Width - 1, Height - 1);
        }

        protected override bool ProcessDialogKey(Keys keyData)
        {
            if (keyData == Keys.Escape)
            {
                DialogResult = DialogResult.OK;
                Close();
                return true;
            }

            // Tab / Shift+Tab cycles between the Vim and AppRefiner tabs
            // (Ctrl+Tab also works for muscle memory).
            if (keyData == Keys.Tab || keyData == (Keys.Control | Keys.Tab))
            {
                SelectTab(tabControl.SelectedIndex + 1);
                return true;
            }
            if (keyData == (Keys.Shift | Keys.Tab) || keyData == (Keys.Control | Keys.Shift | Keys.Tab))
            {
                SelectTab(tabControl.SelectedIndex - 1);
                return true;
            }

            return base.ProcessDialogKey(keyData);
        }

        private void SelectTab(int index)
        {
            int count = tabControl.TabCount;
            if (count == 0) return;
            tabControl.SelectedIndex = ((index % count) + count) % count;
            // Move focus to the newly shown page so repeated Tab keeps cycling
            // and the page's scrollable content responds to the keyboard.
            if (tabControl.SelectedTab?.Controls.Count > 0)
            {
                tabControl.SelectedTab.Controls[0].Focus();
            }
        }

        protected override void OnFormClosed(FormClosedEventArgs e)
        {
            base.OnFormClosed(e);
            mouseHandler?.Dispose();
            mouseHandler = null;
        }
    }
}
