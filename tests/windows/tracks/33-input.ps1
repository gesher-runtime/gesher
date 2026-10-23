# Input (docs/input.md): libinput's transport over gesher-base's socketpairs,
# a Gesher window's Win32 messages as Android MotionEvents and KeyEvents, and
# the framework's input natives.
# Dot-sourced by run-tests.ps1 after 30-graphics.ps1 (GraphicsDex is its):
# Check, RunDex, NoDrive, $Dir, $root, $bcp and $failed are run-tests.ps1's.

# Without Java: the translation, Generic.kcm, and a window's posted messages
# read back with libinput's InputConsumer on the client end of the channel.
# Its INFO line (touch through a synthetic pointer device, which needs an
# interactive desktop) is shown either way.
$inputProbeOut = & "$Dir/input_probe.exe" 2>&1 | Out-String
$inputProbeCode = $LASTEXITCODE
Check 'input: Win32 messages to MotionEvent/KeyEvent over an InputChannel (InputPublisher/InputConsumer)' {
  $inputProbeOut
  $global:LASTEXITCODE = $inputProbeCode
} @('all passed')
($inputProbeOut -split "`n" | Select-String -Pattern '^INFO  ') | Out-Host

# The framework's input natives on framework.jar (KeyEvent, MotionEvent,
# VelocityTracker, KeyCharacterMap with Generic.kcm, InputChannel,
# InputEventSender/Receiver on a Looper), then a Gesher window attached to an
# InputChannel (com.gesher.app.input.WindowInput): posted Win32 messages and
# injected window events reach a Java InputEventReceiver, which finishes them.
GraphicsDex 'input: Win32 window messages to a Java InputEventReceiver (MotionEvent, KeyEvent, Generic.kcm)' InputTest
