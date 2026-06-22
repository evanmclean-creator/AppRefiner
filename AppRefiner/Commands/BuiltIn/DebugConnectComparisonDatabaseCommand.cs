using AppRefiner.Dialogs;

namespace AppRefiner.Commands.BuiltIn
{
    /// <summary>
    /// Command to open a temporary comparison database connection.
    /// </summary>
    public class DiffConnectComparisonDatabaseCommand : BaseCommand
    {
        public override string CommandName => "Diff: Connect Comparison Database";

        public override string CommandDescription => "Connect or reuse a comparison database and immediately diff against the current editor";

        public override bool RequiresActiveEditor => true;

        public override void Execute(CommandContext context)
        {
            var mainForm = context.MainForm as MainForm;
            if (mainForm == null)
            {
                return;
            }

            if (mainForm.ActiveComparisonConnection == null)
            {
                var connected = mainForm.ConnectComparisonDatabase();
                if (!connected || mainForm.ActiveComparisonConnection == null)
                {
                    return;
                }
            }

            mainForm.ShowComparisonDiffForActiveEditor();
        }
    }
}
