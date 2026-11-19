"""Normal Java source fixtures, with a separate javac Element/constant oracle."""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("source_release_diff", TOOLS / "source_release_diff.py")
diff = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diff)


def annotation_sources():
    result = {}
    for name in diff.ANNOTATIONS:
        argument = "int targetSdkVersion();" if name in ("EnabledAfter", "EnabledSince") else ""
        result["android/compat/annotation/" + name + ".java"] = (
            "package android.compat.annotation; import java.lang.annotation.*; "
            "@Retention(RetentionPolicy.SOURCE) @Target({ElementType.FIELD, ElementType.PARAMETER}) "
            "public @interface " + name + " { " + argument + " }")
    result["android/os/Build.java"] = """package android.os;
public class Build { public static class VERSION_CODES {
public static final int BASE=1; public static final int BAKLAVA=36;
public static final int CINNAMON_BUN=37;
} }"""
    return result


ORACLE = """
import java.util.*;
import javax.annotation.processing.*;
import javax.lang.model.SourceVersion;
import javax.lang.model.element.*;
@SupportedAnnotationTypes("*") @SupportedSourceVersion(SourceVersion.RELEASE_21)
public final class ElementOracle extends AbstractProcessor {
  void visit(Element element) {
    if (element.getKind() == ElementKind.FIELD && !element.getAnnotationMirrors().isEmpty()) {
      VariableElement field = (VariableElement) element;
      List<String> modifiers = field.getModifiers().stream().map(Object::toString).sorted().toList();
      List<String> annotations = new ArrayList<>();
      for (AnnotationMirror a : field.getAnnotationMirrors()) {
        String name = ((TypeElement) a.getAnnotationType().asElement()).getQualifiedName().toString();
        List<String> values = new ArrayList<>();
        for (var value : a.getElementValues().entrySet())
          values.add(value.getKey().getSimpleName() + "=" + value.getValue().getValue());
        Collections.sort(values); annotations.add(name + "(" + String.join(",", values) + ")");
      }
      System.out.println("ORACLE\\t" + field.getEnclosingElement() + "#" + field.getSimpleName()
          + "\\t" + field.getConstantValue() + "\\t" + String.join(",", modifiers)
          + "\\t" + String.join(";", annotations));
    }
    for (Element child : element.getEnclosedElements()) visit(child);
  }
  public boolean process(Set<? extends TypeElement> types, RoundEnvironment round) {
    for (Element root : round.getRootElements()) visit(root);
    return false;
  }
}
"""


class SourceCompatInventoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.java = os.environ.get("GESHER_SOURCE_DIFF_JAVA") or shutil.which("java")
        if not cls.java:
            raise unittest.SkipTest("JDK21 java and jdk.compiler are required")
        check = subprocess.run([cls.java, "-m", "jdk.compiler/com.sun.tools.javac.Main", "-version"], capture_output=True, text=True)
        if check.returncode or not check.stdout.startswith("javac 21."):
            raise unittest.SkipTest("JDK21 compiler is required for genuine AST/Element validation")
        cls.temp = tempfile.TemporaryDirectory(prefix="source-compat-tests-")
        cls.root = Path(cls.temp.name)
        cls.classes = cls.root / "classes"
        cls.classes.mkdir()
        oracle = cls.root / "ElementOracle.java"
        oracle.write_text(ORACLE)
        subprocess.run([cls.java, "-m", "jdk.compiler/com.sun.tools.javac.Main", "-d", str(cls.classes),
                        str(TOOLS / "SourceCompatInventory.java"), str(oracle)], check=True, capture_output=True, text=True)
        cls.counter = 0

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "temp"):
            cls.temp.cleanup()

    def observe(self, additions, *, annotations=True):
        type(self).counter += 1
        root = self.root / ("fixture-" + str(self.counter))
        sources = annotation_sources() if annotations else {}
        sources.update(additions)
        pins = []
        for name, source in sources.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source)
            data = path.read_bytes()
            pins.append({"path": name, "mode": "100644", "bytes": len(data), "blob": diff.git_object("blob", data), "sha256": diff.sha256(data)})
        result = subprocess.run([self.java, "-Xmx256m", "-cp", str(self.classes), "SourceCompatInventory", str(root)],
                                check=True, capture_output=True, text=True)
        self.assertEqual(result.stderr, "")
        records = [json.loads(line) for line in result.stdout.splitlines()]
        summary, observations, occurrences = diff.interpret_java(records, pins, "fixture")
        return root, summary, observations, occurrences, records

    def oracle(self, root):
        output = root / "processor-output"
        output.mkdir()
        result = subprocess.run([self.java, "-m", "jdk.compiler/com.sun.tools.javac.Main", "-proc:only",
                                 "-processor", "ElementOracle", "-processorpath", str(self.classes), "-d", str(output),
                                 *map(str, sorted(root.rglob("*.java")))], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = {}
        for line in result.stdout.splitlines():
            if line.startswith("ORACLE\t"):
                _, identity, value, modifiers, annotations = line.split("\t")
                rows[identity] = {"value": int(value) if value != "null" else None,
                                  "modifiers": modifiers.split(","), "annotations": annotations.split(";")}
        return rows

    def test_interface_modifiers_constants_imports_and_order_match_javac_elements(self):
        root, summary, _, rows, _ = self.observe({"fixtures/Changes.java": """
package fixtures;
import android.compat.annotation.ChangeId;
import android.compat.annotation.EnabledAfter;
import android.compat.annotation.EnabledSince;
import android.compat.annotation.NoLogging;
import android.compat.annotation.Overridable;
import android.os.Build;
import static android.os.Build.VERSION_CODES.CINNAMON_BUN;
interface Changes {
    @ChangeId long IMPLICIT = 0x1234L;
    @NoLogging @EnabledSince(targetSdkVersion=CINNAMON_BUN) @ChangeId
    public static final long ORDER_A = 12L;
    @ChangeId @EnabledSince(targetSdkVersion=Build.VERSION_CODES.CINNAMON_BUN) @NoLogging
    long ORDER_B = 12L;
    @android.compat.annotation.ChangeId @Overridable
    @EnabledAfter(targetSdkVersion=android.os.Build.VERSION_CODES.BAKLAVA)
    long QUALIFIED = -(-123L);
    class Nested { @ChangeId static final long LIMIT = -0x8000_0000_0000_0000L; }
}
"""})
        oracle = self.oracle(root)
        self.assertTrue(summary["complete_resolved_metadata"])
        self.assertEqual(len(rows), 5)
        for row in rows:
            actual = oracle[row["declaring_field_identity"]]
            self.assertEqual(row["change_id"]["value"], actual["value"])
            self.assertEqual(row["effective_modifiers"], actual["modifiers"])
            self.assertEqual([a["resolved_name"] for a in row["annotations"]], [a.split("(")[0] for a in actual["annotations"]])
        by_name = {r["name"]: r for r in rows}
        self.assertEqual(by_name["IMPLICIT"]["syntax_modifiers"], [])
        self.assertEqual(set(by_name["IMPLICIT"]["implicit_interface_modifiers"]), {"public", "static", "final"})
        self.assertEqual(by_name["ORDER_A"]["syntax_modifiers"], ["final", "public", "static"])
        self.assertEqual(by_name["ORDER_A"]["processor_metadata"]["EnabledSince"]["value"], 37)
        self.assertEqual(diff.measured(by_name["ORDER_A"]), diff.measured(by_name["ORDER_B"]))
        self.assertEqual(by_name["QUALIFIED"]["processor_metadata"]["EnabledAfter"]["value"], 36)
        self.assertEqual(len(summary["duplicate_change_ids"]["12"]), 2)

    def test_comments_strings_parameters_local_anonymous_and_shadowed_types(self):
        root, summary, observations, rows, _ = self.observe({"fixtures/Contexts.java": """
package fixtures;
import android.compat.annotation.ChangeId;
class Contexts {
  String TEXT = "@ChangeId static final long FAKE = 999L;";
  // @ChangeId static final long COMMENT = 998L;
  @ChangeId static final long REAL = 10L;
  void consume(@ChangeId long argument) {
    class Local { @ChangeId static final long LOCAL = 11L; }
    Object object = new Object() { @ChangeId static final long ANONYMOUS = 12L; };
  }
}
class Shadow {
  @interface ChangeId {}
  @ChangeId static final long OTHER_ANNOTATION = 13L;
  @android.compat.annotation.ChangeId static final long QUALIFIED = 14L;
}
"""})
        oracle = self.oracle(root)
        self.assertIn("fixtures.Shadow.ChangeId()", oracle["fixtures.Shadow#OTHER_ANNOTATION"]["annotations"])
        # Conservative file-level shadow detection retains every candidate instead of guessing.
        self.assertEqual({r.get("name") for r in observations}, {"REAL", "argument", "LOCAL", "ANONYMOUS", "OTHER_ANNOTATION", "QUALIFIED"})
        self.assertFalse(summary["complete_resolved_metadata"])
        self.assertIsNone(summary["complete_source_field_denominator"])
        self.assertEqual([r["name"] for r in rows], ["QUALIFIED"])
        self.assertGreater(len(summary["unresolved_annotation_identities"]), 0)
        local = next(r for r in observations if r["name"] == "LOCAL")
        anonymous = next(r for r in observations if r["name"] == "ANONYMOUS")
        self.assertIsNone(local["declaring_field_identity"])
        self.assertTrue(local["scope_path"][-1]["local"])
        self.assertIsNone(anonymous["declaring_field_identity"])
        self.assertTrue(anonymous["scope_path"][-1]["anonymous"])
        # In a separate unshadowed unit, parameter annotations remain non-field observations.
        _, s, obs, fields, _ = self.observe({"Plain.java": """import android.compat.annotation.ChangeId;
class Plain { void f(@ChangeId long id) {
class Local { @ChangeId static final long LOCAL=1L; }
Object value=new Object() { @ChangeId static final long ANONYMOUS=2L; };
} }"""})
        self.assertEqual([r["name"] for r in fields], ["LOCAL"])
        self.assertTrue(all(r["declaring_field_identity"] is None for r in fields))
        anonymous = next(r for r in obs if r["name"] == "ANONYMOUS")
        self.assertIsNone(anonymous["platform_change_id"])
        self.assertTrue(anonymous["scope_path"][-1]["unattributed_inheritance"])
        self.assertFalse(s["complete_resolved_metadata"])
        self.assertEqual(len(s["non_field_changeid_occurrences"]), 1)
        self.assertFalse(next(r for r in obs if r["name"] == "id")["field"])

    def test_shadowed_static_import_threshold_is_unresolved_instead_of_guessed(self):
        root, summary, _, rows, _ = self.observe({"Threshold.java": """
import android.compat.annotation.ChangeId;
import android.compat.annotation.EnabledSince;
import static android.os.Build.VERSION_CODES.BAKLAVA;
class Threshold {
 static final int BAKLAVA=99;
 @ChangeId @EnabledSince(targetSdkVersion=BAKLAVA) static final long CHANGE=5L;
}
"""})
        self.assertIn("android.compat.annotation.EnabledSince(targetSdkVersion=99)", self.oracle(root)["Threshold#CHANGE"]["annotations"])
        self.assertIsNone(rows[0]["processor_metadata"]["EnabledSince"]["value"])
        self.assertIn("shadowing", rows[0]["processor_metadata"]["EnabledSince"]["reason"])
        self.assertFalse(summary["complete_resolved_metadata"])

    def test_inherited_constant_hides_import_and_remains_unresolved(self):
        root, summary, observations, rows, records = self.observe({
            "fixtures/Parent.java": "package fixtures; class Parent { public static final int BAKLAVA=99; }",
            "fixtures/Child.java": """package fixtures;
import android.compat.annotation.ChangeId;
import android.compat.annotation.EnabledSince;
import static android.os.Build.VERSION_CODES.BAKLAVA;
class Child extends Parent {
 @ChangeId @EnabledSince(targetSdkVersion=BAKLAVA) static final long CHANGE=5L;
}"""})
        self.assertIn("android.compat.annotation.EnabledSince(targetSdkVersion=99)",
                      self.oracle(root)["fixtures.Child#CHANGE"]["annotations"])
        record = next(r for r in observations if r.get("name") == "CHANGE")
        self.assertEqual(record["initializer"]["value"], 5)
        argument = next(a for a in record["annotations"] if a["written_name"] == "EnabledSince")["arguments"][0]["expression"]
        file = next(r for r in records if r["record_type"] == "source_file" and r["path"] == "fixtures/Child.java")
        target = diff.resolve_constant(argument, file, {})
        self.assertEqual(argument["source"], "BAKLAVA")
        self.assertIsNone(target["value"])
        self.assertIn("inherited", target["reason"])
        self.assertIsNone(record["platform_change_id"])
        self.assertEqual(rows, [])
        self.assertFalse(summary["complete_resolved_metadata"])
        self.assertIsNone(summary["complete_source_field_denominator"])
        profile = {"aidl_files": [], "source_references": {"references": []}, "java": summary}
        _, comparison = diff.compare_sources(profile, profile, rows, rows)
        self.assertFalse(comparison["compat_source_presence_comparable"])
        self.assertFalse(comparison["complete_resolved_metadata_comparable"])
        self.assertIsNone(comparison["resolved_id_additions"])
        self.assertIsNone(comparison["resolved_id_removals"])

    def test_inherited_annotation_hides_import_without_false_platform_count(self):
        root, summary, observations, rows, _ = self.observe({
            "fixtures/Parent.java": "package fixtures; class Parent { public @interface ChangeId {} }",
            "fixtures/Child.java": """package fixtures;
import android.compat.annotation.ChangeId;
class Child extends Parent { @ChangeId static final long CHANGE=5L; }"""})
        self.assertEqual(self.oracle(root)["fixtures.Child#CHANGE"]["annotations"], ["fixtures.Parent.ChangeId()"])
        record = next(r for r in observations if r.get("name") == "CHANGE")
        self.assertIsNone(record["platform_change_id"])
        self.assertIsNone(record["annotations"][0]["resolved_name"])
        self.assertEqual(record["annotations"][0]["written_name"], "ChangeId")
        self.assertEqual(rows, [])
        self.assertEqual(summary["observed_changeid_field_occurrences"], 0)
        self.assertEqual(summary["unresolved_changeid_field_candidates"], 1)
        self.assertFalse(summary["complete_resolved_metadata"])
        self.assertIsNone(summary["complete_source_field_denominator"])

    def test_outer_interface_member_can_hide_fully_qualified_annotation(self):
        root, summary, observations, rows, _ = self.observe({
            "fixtures/Parent.java": """package fixtures;
interface Parent {
 class android { public static class compat { public static class annotation {
  public @interface ChangeId {}
 } } }
}""",
            "fixtures/Child.java": """package fixtures;
class Child implements Parent { static class Nested {
 @android.compat.annotation.ChangeId static final long CHANGE=5L;
} }"""})
        actual = self.oracle(root)["fixtures.Child.Nested#CHANGE"]["annotations"]
        self.assertEqual(actual, ["fixtures.Parent.android.compat.annotation.ChangeId()"])
        record = next(r for r in observations if r.get("name") == "CHANGE")
        self.assertIsNone(record["platform_change_id"])
        self.assertEqual(record["scope_path"][0]["supertypes"], ["Parent"])
        self.assertTrue(record["scope_path"][0]["unattributed_inheritance"])
        self.assertFalse(record["scope_path"][1]["unattributed_inheritance"])
        self.assertEqual(rows, [])
        self.assertFalse(summary["complete_resolved_metadata"])
        self.assertIsNone(summary["complete_source_field_denominator"])

    def test_unsupported_constant_expression_is_visible_despite_valid_compiler_constant(self):
        root, summary, _, rows, _ = self.observe({"Formula.java": """
import android.compat.annotation.ChangeId;
class Formula { static final long BASE=20L; @ChangeId static final long FORMULA=BASE+1L; }
"""})
        self.assertEqual(self.oracle(root)["Formula#FORMULA"]["value"], 21)
        self.assertIsNone(rows[0]["change_id"]["value"])
        self.assertEqual(rows[0]["initializer"]["source"], "BASE+1L")
        self.assertEqual(summary["validity_counts"], {"unresolved": 1})
        self.assertFalse(summary["complete_resolved_metadata"])

    def test_preview_source_diagnostic_prevents_complete_corpus_claim(self):
        _, summary, _, _, records = self.observe({"Preview.java": r'''class Preview {
String message(String name) { return STR."hello \{name}"; }
}'''}, annotations=False)
        self.assertFalse(summary["parse_complete"])
        self.assertIsNone(summary["complete_source_field_denominator"])
        self.assertTrue(summary["parse_failures"][0]["diagnostics"])
        self.assertTrue(any("preview" in d["code"] for r in records if r["record_type"] == "source_file" for d in r["diagnostics"]))

    def test_duplicate_declaring_fields_in_source_alternatives_survive(self):
        _, summary, _, rows, _ = self.observe({
            "alternative-a/Queue.java": "package variants; import android.compat.annotation.ChangeId; class Queue { @ChangeId static final long CHANGE=100L; }",
            "alternative-b/Queue.java": "package variants; import android.compat.annotation.ChangeId; class Queue { @ChangeId static final long CHANGE=100L; }",
            "android/os/Build.java": "package android.os; public class Build { public static class VERSION_CODES { @android.compat.annotation.ChangeId public static final long SOURCE_FIELD=101L; } }"})
        self.assertEqual(len(rows), 3)
        self.assertEqual(summary["canonical_field_identities"], 2)
        self.assertEqual(summary["resolved_unique_change_ids"], 2)
        self.assertTrue(any(r["declaring_field_identity"] == "android.os.Build.VERSION_CODES#SOURCE_FIELD" for r in rows))
        self.assertEqual(len(summary["duplicate_declaring_fields"]["variants.Queue#CHANGE"]), 2)
        self.assertEqual(len(summary["duplicate_change_ids"]["100"]), 2)

    def test_upstream_conflicts_and_nonconstant_fields_are_diagnostics_not_precedence(self):
        _, summary, _, rows, _ = self.observe({"Policy.java": """
import android.compat.annotation.ChangeId;
import android.compat.annotation.Disabled;
import android.compat.annotation.EnabledSince;
import android.compat.annotation.LoggingOnly;
class Policy {
 @Disabled @ChangeId @EnabledSince(targetSdkVersion=37) static final long CONFLICT=1L;
 @EnabledSince(targetSdkVersion=37) @ChangeId @Disabled static final long REVERSED=1L;
 @LoggingOnly @Disabled @ChangeId static final long LOG_CONFLICT=2L;
 @ChangeId static long NONCONSTANT=3L;
}
"""})
        by_name = {r["name"]: r for r in rows}
        self.assertEqual(summary["validity_counts"], {"invalid": 4})
        self.assertEqual(diff.measured(by_name["CONFLICT"]), diff.measured(by_name["REVERSED"]))
        self.assertTrue(by_name["CONFLICT"]["processor_metadata"]["Disabled"])
        self.assertEqual(by_name["CONFLICT"]["processor_metadata"]["EnabledSince"]["value"], 37)
        self.assertIn("upstream processor requires a static constant/final field", by_name["NONCONSTANT"]["validity"]["invalid_reasons"])


if __name__ == "__main__":
    unittest.main()
