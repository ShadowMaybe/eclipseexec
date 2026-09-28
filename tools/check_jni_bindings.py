#!/usr/bin/env python3
"""Check that the Java bindings and the native registration table agree.

EclipseExec.java and the g_methods[] table in src/eclipse_jni.c are two halves
of one contract. Neither the compiler nor RegisterNatives can see both: if a
method is renamed on one side only, the library still loads and the failure
appears later as an UnsatisfiedLinkError on a user's device.

This compares names, JNI descriptors and the class name, and exits non-zero on
any mismatch. Run by tests/run_tests.sh and by CI.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
JAVA = ROOT / "jni_bindings/src/main/java/me/shadow/eclipselauncher/exec/EclipseExec.java"
C_SOURCE = ROOT / "src/eclipse_jni.c"
CMAKE = ROOT / "CMakeLists.txt"

PRIMITIVES = {
    "void": "V",
    "boolean": "Z",
    "byte": "B",
    "char": "C",
    "short": "S",
    "int": "I",
    "long": "J",
    "float": "F",
    "double": "D",
}

# public static native boolean prepareEgl(String path, boolean useBypass, ...);
NATIVE_DECLARATION = re.compile(
    r"public\s+static\s+native\s+(?P<returns>[\w.\[\]]+)\s+(?P<name>\w+)\s*"
    r"\((?P<params>[^)]*)\)\s*;"
)

# {"prepareEgl", "(Ljava/lang/String;ZZI)Z", (void *)native_prepare_egl},
TABLE_ENTRY = re.compile(
    r"\{\s*\"(?P<name>\w+)\",\s*\"(?P<descriptor>[^\"]+)\",\s*"
    r"\(void\s*\*\)\s*(?P<function>\w+)\s*\}"
)

PACKAGE = re.compile(r"^\s*package\s+([\w.]+)\s*;", re.MULTILINE)
IMPORT = re.compile(r"^\s*import\s+(?:static\s+)?([\w.]+)\s*;", re.MULTILINE)
CLASS_NAME = re.compile(r"\bclass\s+(EclipseExec)\b")
JNI_CLASS = re.compile(r'#define\s+ECLIPSE_JNI_CLASS\s+"([^"]+)"')
LOAD_LIBRARY = re.compile(r'loadLibrary\("([^"]+)"\)')
CMAKE_TARGET = re.compile(r"add_library\(\s*(\w+)\s+SHARED")
ANNOTATION = re.compile(r"@\w+(?:\([^)]*\))?\s*")


def parameter_type(parameter: str) -> str:
    """Strip a Java parameter declaration down to just its type.

    Parameters are declared as `int[] args` or `@NonNull String path`; a JNI
    descriptor wants neither the name nor the annotations. Everything from the
    last whitespace on is dropped, which keeps `int[]` in `int[] args`.
    """
    cleaned = ANNOTATION.sub("", parameter).strip()
    if " " in cleaned:
        cleaned = cleaned[: cleaned.rfind(" ")].strip()
    return cleaned


def qualified_name(java_type: str, imports: dict[str, str]) -> str:
    """Resolve a source-level type to the fully qualified name descriptors use.

    `String` is only unqualified in source: the descriptor always spells out
    `java/lang/String`. A simple name is looked up in the file's imports
    first, then assumed to be java.lang — the rule javac itself applies when
    there is no import.
    """
    if "." in java_type:
        return java_type
    if java_type in imports:
        return imports[java_type]
    return "java.lang." + java_type


def descriptor_for(java_type: str, imports: dict[str, str]) -> str:
    """Turn a Java type into its JNI type descriptor."""
    java_type = java_type.strip()
    if java_type.endswith("[]"):
        return "[" + descriptor_for(java_type[:-2], imports)
    if java_type in PRIMITIVES:
        return PRIMITIVES[java_type]
    return "L" + qualified_name(java_type, imports).replace(".", "/") + ";"


def method_descriptor(returns: str, params: str, imports: dict[str, str]) -> str:
    """Build `(IIF)V` from a declaration's return type and parameter list."""
    param_list = [] if not params.strip() else [parameter_type(p) for p in params.split(",")]
    return (
        "("
        + "".join(descriptor_for(p, imports) for p in param_list)
        + ")"
        + descriptor_for(returns, imports)
    )


def main() -> int:
    problems: list[str] = []

    for path in (JAVA, C_SOURCE, CMAKE):
        if not path.is_file():
            print(f"missing file: {path.relative_to(ROOT)}", file=sys.stderr)
            return 1

    java_source = JAVA.read_text(encoding="utf-8")
    c_source = C_SOURCE.read_text(encoding="utf-8")
    cmake_source = CMAKE.read_text(encoding="utf-8")

    # Simple names in declarations resolve through these, as they do for javac.
    imports = {}
    for match in IMPORT.finditer(java_source):
        dotted = match.group(1)
        imports[dotted.rsplit(".", 1)[-1]] = dotted

    java_methods = {
        match.group("name"): method_descriptor(
            match.group("returns"), match.group("params"), imports
        )
        for match in NATIVE_DECLARATION.finditer(java_source)
    }
    native_methods = {
        match.group("name"): match.group("descriptor") for match in TABLE_ENTRY.finditer(c_source)
    }

    if not java_methods:
        problems.append(f"no native methods found in {JAVA.name}")
    if not native_methods:
        problems.append(f"no g_methods[] entries found in {C_SOURCE.name}")

    for name in sorted(java_methods.keys() - native_methods.keys()):
        problems.append(f"{JAVA.name} declares {name}(), but g_methods[] has no entry for it")
    for name in sorted(native_methods.keys() - java_methods.keys()):
        problems.append(f"g_methods[] registers {name}(), but {JAVA.name} does not declare it")
    for name in sorted(java_methods.keys() & native_methods.keys()):
        if java_methods[name] != native_methods[name]:
            problems.append(
                f"{name}: Java descriptor {java_methods[name]} "
                f"!= native descriptor {native_methods[name]}"
            )

    # The class name FindClass() uses at load time must match the real class.
    package = PACKAGE.search(java_source)
    class_name = CLASS_NAME.search(java_source)
    jni_class = JNI_CLASS.search(c_source)
    if package is None or class_name is None or jni_class is None:
        problems.append("could not read the package, class name or ECLIPSE_JNI_CLASS")
    else:
        expected = (package.group(1) + "." + class_name.group(1)).replace(".", "/")
        actual = jni_class.group(1)
        if expected != actual:
            problems.append(
                f"FindClass() targets {actual}, but the class is "
                f"{package.group(1)}.{class_name.group(1)}"
            )

    # System.loadLibrary("x") must name a library the build actually produces.
    loaded = LOAD_LIBRARY.search(java_source)
    targets = CMAKE_TARGET.findall(cmake_source)
    if loaded is None:
        problems.append(f"{JAVA.name} does not call System.loadLibrary()")
    elif loaded.group(1) not in targets:
        problems.append(
            f'{JAVA.name} loads "{loaded.group(1)}" but CMakeLists.txt builds {targets}'
        )

    if problems:
        print("JNI binding check FAILED:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1

    print(
        f"JNI binding check: {len(java_methods)} methods, class name and "
        f"library name agree"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
