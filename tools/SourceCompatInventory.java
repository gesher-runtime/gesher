/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

import com.sun.source.tree.*;
import com.sun.source.util.JavacTask;
import com.sun.source.util.SourcePositions;
import com.sun.source.util.TreePath;
import com.sun.source.util.TreePathScanner;
import com.sun.source.util.TreeScanner;
import com.sun.source.util.Trees;
import java.io.IOException;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;
import javax.lang.model.element.Modifier;
import javax.tools.*;

/** Parse source metadata only. No attribution, annotation processors or code generation. */
public final class SourceCompatInventory {
    static final String PREFIX = "android.compat.annotation.";
    static final Set<String> COMPANIONS = Set.of("ChangeId", "Disabled", "EnabledAfter",
            "EnabledSince", "LoggingOnly", "NoLogging", "Overridable");

    static Map<String, Object> row(Object... pairs) {
        Map<String, Object> result = new LinkedHashMap<>();
        for (int i = 0; i < pairs.length; i += 2) result.put((String) pairs[i], pairs[i + 1]);
        return result;
    }

    static String json(Object value) {
        if (value == null) return "null";
        if (value instanceof Number || value instanceof Boolean) return value.toString();
        if (value instanceof Map<?, ?> map) {
            List<String> items = new ArrayList<>();
            for (var e : map.entrySet()) items.add(json(e.getKey().toString()) + ":" + json(e.getValue()));
            return "{" + String.join(",", items) + "}";
        }
        if (value instanceof Iterable<?> list) {
            List<String> items = new ArrayList<>();
            for (Object item : list) items.add(json(item));
            return "[" + String.join(",", items) + "]";
        }
        StringBuilder out = new StringBuilder("\"");
        for (char c : value.toString().toCharArray()) {
            switch (c) {
                case '\\' -> out.append("\\\\");
                case '"' -> out.append("\\\"");
                case '\n' -> out.append("\\n");
                case '\r' -> out.append("\\r");
                case '\t' -> out.append("\\t");
                default -> {
                    if (c < 32 || c > 126) out.append(String.format(Locale.ROOT, "\\u%04x", (int) c));
                    else out.append(c);
                }
            }
        }
        return out.append('"').toString();
    }

    static String leafName(String name) {
        return name.substring(name.lastIndexOf('.') + 1);
    }

    static final class Source extends SimpleJavaFileObject {
        final String text;
        Source(String path, String text) {
            super(URI.create("string:///" + path.replace(" ", "%20")), Kind.SOURCE);
            this.text = text;
        }
        @Override public CharSequence getCharContent(boolean ignore) { return text; }
    }

    static final class Scanner extends TreePathScanner<Void, Void> {
        final CompilationUnitTree unit;
        final SourcePositions positions;
        final String path;
        final String text;
        final String packageName;
        final List<Map<String, Object>> imports = new ArrayList<>();
        final Map<String, List<String>> explicitImports = new TreeMap<>();
        final List<String> wildcardImports = new ArrayList<>();
        final Set<String> declaredTypeNames = new TreeSet<>();
        final Set<String> declaredVariableNames = new TreeSet<>();
        final List<Map<String, Object>> scopes = new ArrayList<>();
        final List<Map<String, Object>> records = new ArrayList<>();
        int erroneousTrees;

        Scanner(CompilationUnitTree unit, SourcePositions positions, String path, String text) {
            this.unit = unit; this.positions = positions; this.path = path; this.text = text;
            packageName = unit.getPackageName() == null ? "" : unit.getPackageName().toString();
            for (ImportTree item : unit.getImports()) {
                String name = item.getQualifiedIdentifier().toString();
                imports.add(row("name", name, "static", item.isStatic()));
                if (name.endsWith(".*")) wildcardImports.add(name);
                else if (!item.isStatic()) explicitImports.computeIfAbsent(leafName(name),
                        k -> new ArrayList<>()).add(name);
            }
            new TreeScanner<Void, Void>() {
                @Override public Void visitClass(ClassTree tree, Void p) {
                    if (!tree.getSimpleName().isEmpty()) declaredTypeNames.add(tree.getSimpleName().toString());
                    return super.visitClass(tree, p);
                }
                @Override public Void visitTypeParameter(TypeParameterTree tree, Void p) {
                    declaredTypeNames.add(tree.getName().toString());
                    return super.visitTypeParameter(tree, p);
                }
                @Override public Void visitVariable(VariableTree tree, Void p) {
                    declaredVariableNames.add(tree.getName().toString());
                    return super.visitVariable(tree, p);
                }
            }.scan(unit, null);
        }

        String spelling(Tree tree) {
            if (tree == null) return null;
            long begin = positions.getStartPosition(unit, tree), end = positions.getEndPosition(unit, tree);
            if (begin < 0 || end < begin || end > text.length()) return tree.toString();
            return text.substring((int) begin, (int) end);
        }

        Map<String, Object> location(Tree tree) {
            long start = positions.getStartPosition(unit, tree), end = positions.getEndPosition(unit, tree);
            return row("start", start, "end", end, "line", start < 0 ? null : unit.getLineMap().getLineNumber(start),
                    "column", start < 0 ? null : unit.getLineMap().getColumnNumber(start));
        }

        Map<String, Object> expression(ExpressionTree tree) {
            if (tree == null) return null;
            Map<String, Object> result = row("kind", tree.getKind().name(), "source", spelling(tree));
            if (tree instanceof LiteralTree literal) {
                Object value = literal.getValue();
                if (value instanceof Integer || value instanceof Long) {
                    result.put("value", value);
                    result.put("literal_type", value instanceof Long ? "long" : "int");
                } else if (value instanceof Character c) {
                    result.put("value", (int) c); result.put("literal_type", "char");
                }
            } else if (tree instanceof IdentifierTree || tree instanceof MemberSelectTree) {
                result.put("name", tree.toString());
            } else if (tree instanceof ParenthesizedTree parenthesized) {
                result.put("operand", expression(parenthesized.getExpression()));
            } else if (tree instanceof UnaryTree unary) {
                result.put("operand", expression(unary.getExpression()));
            }
            return result;
        }

        Map<String, Object> annotation(AnnotationTree tree) {
            String written = tree.getAnnotationType().toString();
            String first = written.split("\\.")[0];
            String resolved = null, reason, status;
            if (declaredTypeNames.contains(first)) {
                status = "unresolved"; reason = "source declares a matching type/type-parameter name; no attribution";
            } else if (written.startsWith(PREFIX)) {
                resolved = written; status = "qualified_spelling"; reason = "explicit platform annotation spelling";
            } else if (!written.contains(".") && explicitImports.containsKey(written)) {
                List<String> choices = explicitImports.get(written);
                if (choices.size() == 1) {
                    resolved = choices.get(0); status = "explicit_import"; reason = "unambiguous explicit type import";
                } else { status = "unresolved"; reason = "multiple explicit imports"; }
            } else {
                status = "unresolved";
                reason = "name not attributed; same-package, wildcard, static or relative qualification is not guessed";
            }
            List<Map<String, Object>> args = new ArrayList<>();
            for (ExpressionTree argument : tree.getArguments()) {
                if (argument instanceof AssignmentTree assignment)
                    args.add(row("name", assignment.getVariable().toString(), "expression", expression(assignment.getExpression())));
                else args.add(row("name", "value", "expression", expression(argument)));
            }
            return row("written_name", written, "resolved_name", resolved, "identity_status", status,
                    "identity_reason", reason, "source", spelling(tree), "location", location(tree), "arguments", args);
        }

        List<Map<String, Object>> annotations(ModifiersTree tree) {
            List<Map<String, Object>> result = new ArrayList<>();
            for (AnnotationTree annotation : tree.getAnnotations()) result.add(annotation(annotation));
            return result;
        }

        boolean candidate(List<? extends AnnotationTree> annotations) {
            return annotations.stream().anyMatch(a -> COMPANIONS.contains(leafName(a.getAnnotationType().toString()))
                    || a.getAnnotationType().toString().startsWith(PREFIX));
        }

        @Override public Void visitClass(ClassTree tree, Void p) {
            Tree parent = getCurrentPath().getParentPath() == null ? null : getCurrentPath().getParentPath().getLeaf();
            String simple = tree.getSimpleName().toString();
            boolean anonymous = simple.isEmpty();
            boolean local = !(parent instanceof CompilationUnitTree || parent instanceof ClassTree);
            String canonical = null;
            if (!anonymous && !local) {
                if (scopes.isEmpty()) canonical = packageName.isEmpty() ? simple : packageName + "." + simple;
                else if (scopes.get(scopes.size() - 1).get("canonical_name") instanceof String outer)
                    canonical = outer + "." + simple;
            }
            scopes.add(row("name", simple, "kind", tree.getKind().name(), "canonical_name", canonical,
                    "local", local, "anonymous", anonymous,
                    "parent_kind", parent == null ? null : parent.getKind().name(), "location", location(tree)));
            super.visitClass(tree, p);
            scopes.remove(scopes.size() - 1);
            return null;
        }

        @Override public Void visitVariable(VariableTree tree, Void p) {
            Tree parent = getCurrentPath().getParentPath().getLeaf();
            boolean field = parent instanceof ClassTree;
            String owner = scopes.isEmpty() ? null : (String) scopes.get(scopes.size() - 1).get("canonical_name");
            boolean versionConstant = field && ("android.os.Build.VERSION_CODES".equals(owner)
                    || "android.os.Build.VERSION_CODES_FULL".equals(owner));
            boolean annotatedCandidate = candidate(tree.getModifiers().getAnnotations());
            if (annotatedCandidate || versionConstant) {
                List<String> syntax = tree.getModifiers().getFlags().stream().map(Modifier::toString).sorted().toList();
                Set<String> effective = new TreeSet<>(syntax);
                List<String> implicit = new ArrayList<>();
                if (field && (parent.getKind() == Tree.Kind.INTERFACE || parent.getKind() == Tree.Kind.ANNOTATION_TYPE)) {
                    for (String modifier : List.of("public", "static", "final"))
                        if (effective.add(modifier)) implicit.add(modifier);
                }
                Map<String, Object> record = row("record_type", annotatedCandidate ? "annotated_variable" : "version_constant",
                        "path", path, "package", packageName, "field", field, "parent_kind", parent.getKind().name(),
                        "declaring_type", owner, "declaring_field_identity", field && owner != null ? owner + "#" + tree.getName() : null,
                        "scope_path", new ArrayList<>(scopes), "name", tree.getName().toString(),
                        "type", tree.getType() == null ? null : tree.getType().toString(),
                        "syntax_modifiers", syntax, "effective_modifiers", new ArrayList<>(effective),
                        "implicit_interface_modifiers", implicit, "modifiers_source", spelling(tree.getModifiers()),
                        "initializer", expression(tree.getInitializer()), "annotations", annotations(tree.getModifiers()),
                        "location", location(tree));
                Tree baseType = tree.getType();
                while (baseType instanceof AnnotatedTypeTree annotated) baseType = annotated.getUnderlyingType();
                record.put("primitive_type", baseType instanceof PrimitiveTypeTree primitive
                        ? primitive.getPrimitiveTypeKind().name().toLowerCase(Locale.ROOT) : null);
                records.add(record);
                if (annotatedCandidate && versionConstant) {
                    Map<String, Object> constant = new LinkedHashMap<>(record);
                    constant.put("record_type", "version_constant");
                    records.add(constant);
                }
            }
            return super.visitVariable(tree, p);
        }

        @Override public Void visitAnnotation(AnnotationTree tree, Void p) {
            if (COMPANIONS.contains(leafName(tree.getAnnotationType().toString()))) {
                TreePath parent = getCurrentPath().getParentPath();
                if (parent != null && parent.getLeaf() instanceof ModifiersTree) parent = parent.getParentPath();
                if (parent == null || !(parent.getLeaf() instanceof VariableTree))
                    records.add(row("record_type", "non_variable_annotation", "path", path,
                            "parent_kind", parent == null ? null : parent.getLeaf().getKind().name(),
                            "scope_path", new ArrayList<>(scopes), "annotation", annotation(tree), "location", location(tree)));
            }
            return super.visitAnnotation(tree, p);
        }

        @Override public Void visitErroneous(ErroneousTree tree, Void p) {
            erroneousTrees++;
            return super.visitErroneous(tree, p);
        }
    }

    static void inventory(Path root) throws IOException {
        JavaCompiler compiler = ToolProvider.getSystemJavaCompiler();
        if (compiler == null) throw new IllegalStateException("A JDK with jdk.compiler is required");
        if (Runtime.version().feature() != 21) throw new IllegalStateException("This inventory requires JDK21");
        System.out.println(json(row("record_type", "parser", "backend", "JavacTask.parse",
                "source_level", "21", "preview_enabled", false, "java_version", System.getProperty("java.version"),
                "attribution", false, "annotation_processing", false, "code_generation", false,
                "source_offset_unit", "UTF-16 code units")));
        List<Path> files;
        try (var walk = Files.walk(root)) {
            files = walk.filter(Files::isRegularFile).filter(p -> p.toString().endsWith(".java")).sorted().toList();
        }
        try (StandardJavaFileManager manager = compiler.getStandardFileManager(null, Locale.ROOT, StandardCharsets.UTF_8)) {
            for (Path file : files) {
                String path = root.relativize(file).toString().replace('\\', '/');
                String source = Files.readString(file, StandardCharsets.UTF_8);
                DiagnosticCollector<JavaFileObject> diagnostics = new DiagnosticCollector<>();
                JavacTask task = (JavacTask) compiler.getTask(null, manager, diagnostics,
                        List.of("-proc:none", "-source", "21", "-encoding", "UTF-8", "-Xlint:none"),
                        null, List.of(new Source(path, source)));
                List<Map<String, Object>> records = new ArrayList<>();
                List<Map<String, Object>> imports = new ArrayList<>();
                Set<String> typeNames = new TreeSet<>();
                Set<String> variableNames = new TreeSet<>();
                String packageName = null;
                String exception = null;
                int erroneous = 0;
                try {
                    for (CompilationUnitTree unit : task.parse()) {
                        Scanner scanner = new Scanner(unit, Trees.instance(task).getSourcePositions(), path, source);
                        scanner.scan(unit, null);
                        records.addAll(scanner.records); imports.addAll(scanner.imports);
                        typeNames.addAll(scanner.declaredTypeNames); erroneous += scanner.erroneousTrees;
                        variableNames.addAll(scanner.declaredVariableNames);
                        packageName = scanner.packageName;
                    }
                } catch (RuntimeException | IOException e) { exception = e.getClass().getName() + ": " + e.getMessage(); }
                List<Map<String, Object>> messages = new ArrayList<>();
                boolean complete = exception == null && erroneous == 0;
                for (Diagnostic<? extends JavaFileObject> d : diagnostics.getDiagnostics()) {
                    if (d.getKind() == Diagnostic.Kind.ERROR) complete = false;
                    messages.add(row("kind", d.getKind().name(), "code", d.getCode(), "line", d.getLineNumber(),
                            "column", d.getColumnNumber(), "start", d.getStartPosition(), "end", d.getEndPosition(),
                            "message", d.getMessage(Locale.ROOT)));
                }
                System.out.println(json(row("record_type", "source_file", "path", path, "complete", complete,
                        "package", packageName, "imports", imports, "declared_type_names", typeNames,
                        "declared_variable_names", variableNames,
                        "erroneous_trees", erroneous, "diagnostics", messages, "exception", exception)));
                for (Map<String, Object> record : records) {
                    record.put("source_parse_complete", complete);
                    System.out.println(json(record));
                }
            }
        }
    }

    public static void main(String[] args) throws IOException {
        if (args.length != 1) throw new IllegalArgumentException("usage: SourceCompatInventory SOURCE_DIRECTORY");
        inventory(Path.of(args[0]));
    }
}
