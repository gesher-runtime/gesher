"""CI-only copies with one named correction disabled; never edits production files."""
import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VARIANTS = [
    ("lease", "compat/input/ime.cc",
     "      auto service = tsf;\n      service->Update(std::move(snapshot));",
     "      tsf->Update(std::move(snapshot));",
     ["document Update retains its TSF owner throughout a real OnTextChange COM detach callback, then releases it"]),
    ("caret", "compat/input/ime.cc",
     "    if (!impl_->state.active || epoch != impl_->state.editor_epoch) return;",
     "    if (!impl_->state.active) return;",
     ["a delayed old-editor caret cannot overwrite the newer editor's actual Windows caret"]),
    ("geometry", "compat/input/text_store.cc",
     "  geometry_ = std::move(geometry);", "  geometry_ = TextGeometry{};",
     ["TSF composing-range extents use actual measured UTF-16 character bounds in screen coordinates",
      "measured character hit testing preserves ExtractedText offset and ACP indices",
      "measured TSF range extents report actual client clipping without invented off-window geometry",
      "a new editor epoch recovers only from its own fresh text and geometry"]),
    ("composition", "compat/input/text_store.cc",
     "  edit.kind = composition_text ? DocumentEdit::kCompositionText : DocumentEdit::kReplace;",
     "  edit.kind = DocumentEdit::kReplace;",
     ["TSF composition transactions preserve full composing text, UTF-16 replacement ranges, offsets and the native final commit",
      "a partial edit inside a TSF composition restores the native insertion caret after publishing the full composing text",
      "Windows TSF delivers reconversion replacement and composition completion to the ACP document",
      "the window-owned TSF adapter executes the real test-service DLL against its own document"]),
    ("initial-size", "compat/hwui/window/gesher_window.cc",
     "  if (hwnd != nullptr) SetClientSize(width, height);",
     "  // Counterfactual: leave CreateWindowEx's initial desktop clamp in place.",
     ["a client larger than the desktop retains its exact requested size"]),
]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    cases = []
    for name, relative, before, after, failures in VARIANTS:
        source = (ROOT / relative).read_text(encoding="utf-8")
        if source.count(before) != 1:
            raise RuntimeError(f"{name}: correction text must occur exactly once; review the counterfactual")
        (args.output / f"{name}.cc").write_text(source.replace(before, after), encoding="utf-8")
        cases.append({"name": name, "source": relative, "expected_failures": failures})
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    sha = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    args.manifest.write_text(json.dumps({"source_sha": sha, "variants": cases}, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared {len(cases)} explicit negative source copies at {sha}; production sources unchanged")

if __name__ == "__main__":
    main()
