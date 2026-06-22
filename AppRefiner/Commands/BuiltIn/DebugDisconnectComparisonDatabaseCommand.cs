using AppRefiner.Dialogs;

namespace AppRefiner.Commands.BuiltIn
{
    /// <summary>
    /// Command to close the active temporary comparison database connection.
    /// </summary>
    public class DiffDisconnectComparisonDatabaseCommand : BaseCommand
    {
        public override string CommandName => "Diff: Disconnect Comparison Database";

        public override string CommandDescription => "Disconnect the active temporary comparison database session";

        public override bool RequiresActiveEditor => false;

        public override void Execute(CommandContext context)
        {
            var mainForm = context.MainForm as MainForm;
            if (mainForm == null)
            {
                return;
            }

            var disconnected = mainForm.DisconnectComparisonDatabase();
            if (!disconnected)
            {
                return;
            }

            var owner = context.MainWindowHandle != IntPtr.Zero ? context.MainWindowHandle : mainForm.Handle;
            var wrapper = new WindowWrapper(owner);
            new MessageBoxDialog(
                "Comparison database disconnected.",
                "Comparison Database Disconnected",
                MessageBoxButtons.OK,
                owner).ShowDialog(wrapper);
        }
    }
}
