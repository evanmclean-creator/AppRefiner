using AppRefiner.Database.Models;

namespace AppRefiner.Commands.BuiltIn
{
    /// <summary>
    /// Diagnostic command that shows how the active editor resolves to an OpenTarget.
    /// </summary>
    public class DebugShowCurrentOpenTargetCommand : BaseCommand
    {
        public override string CommandName => "Debug: Show Current OpenTarget";

        public override string CommandDescription => "Show the OpenTarget AppRefiner resolves for the active PeopleCode or SQL editor";

        public override bool RequiresActiveEditor => true;

        public override void Execute(CommandContext context)
        {
            OpenTarget? openTarget;
            string failureReason;
            var resolved = EditorOpenTargetResolver.TryResolve(context.ActiveEditor, out openTarget, out failureReason);
            var diagnosticText = EditorOpenTargetResolver.BuildDiagnosticText(context.ActiveEditor, openTarget, failureReason);

            Debug.Log($"DebugShowCurrentOpenTargetCommand: {diagnosticText.Replace(Environment.NewLine, " | ")}");

            var icon = resolved ? MessageBoxIcon.Information : MessageBoxIcon.Warning;
            var title = resolved ? "Current OpenTarget" : "OpenTarget Resolution Failed";
            MessageBox.Show(diagnosticText, title, MessageBoxButtons.OK, icon);
        }
    }
}
