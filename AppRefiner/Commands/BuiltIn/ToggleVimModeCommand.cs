namespace AppRefiner.Commands.BuiltIn
{
    /// <summary>
    /// Command to toggle Vim-style modal editing.
    /// </summary>
    public class ToggleVimModeCommand : BaseCommand
    {
        public override string CommandName => "Editor: Toggle Vim Mode";

        public override string CommandDescription => "Enable or disable Vim-style modal editing";

        public override bool RequiresActiveEditor => false;

        public override void Execute(CommandContext context)
        {
            context.MainForm?.ToggleVimModeEnabled();
        }
    }
}
