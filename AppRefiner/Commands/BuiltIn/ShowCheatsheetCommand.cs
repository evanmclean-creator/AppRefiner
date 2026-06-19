namespace AppRefiner.Commands.BuiltIn
{
    /// <summary>
    /// Command to show a quick reference for Vim and AppRefiner commands.
    /// </summary>
    public class ShowCheatsheetCommand : BaseCommand
    {
        public override string CommandName => "Help: Show Cheatsheet";

        public override string CommandDescription => "Show a Vim and AppRefiner quick reference";

        public override bool RequiresActiveEditor => false;

        public override void Execute(CommandContext context)
        {
            (context.MainForm as MainForm)?.ShowCheatsheetDialog();
        }
    }
}
