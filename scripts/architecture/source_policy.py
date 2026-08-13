import re
from pathlib import Path


CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
IGNORED_DIRECTORIES = {
    ".git",
    "__pycache__",
    "build",
    "dist",
    "external",
    "out",
    "third-party",
    "third_party",
    "vendor",
}
GLOBAL_CMAKE_COMMAND = re.compile(
    r"(?im)^[ \t]*(include_directories|link_libraries|add_definitions)\s*\("
)
GLOBAL_CXX_FLAGS = re.compile(r"(?im)^[ \t]*set\s*\(\s*CMAKE_CXX_FLAGS\b")
RECURSIVE_GLOB = re.compile(r"(?is)\bfile\s*\(\s*GLOB_RECURSE\b")
INCLUDE_DIRECTIVE = re.compile(r"^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]")
PRIVATE_NAMESPACE = re.compile(
    r"\blol::[A-Za-z_]\w*::(?:application|domain|internal|private_)::"
)


def _is_scannable(path: Path) -> bool:
    return path.name == "CMakeLists.txt" or path.suffix in CPP_SUFFIXES | {".cmake"}


def _is_ignored(relative_path: Path) -> bool:
    parts = relative_path.parts
    if any(part in IGNORED_DIRECTORIES for part in parts):
        return True
    return parts[:3] == ("tests", "architecture", "fixtures")


def _line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _finding(code: str, path: Path, line: int, **details: str) -> dict:
    return {"code": code, "line": line, "path": path.as_posix(), **details}


def _scan_cmake(path: Path, text: str) -> list[dict]:
    findings = []
    for match in GLOBAL_CMAKE_COMMAND.finditer(text):
        findings.append(
            _finding(
                "FORBIDDEN_GLOBAL_CMAKE_COMMAND",
                path,
                _line_number(text, match.start()),
                command=match.group(1),
            )
        )
    for match in GLOBAL_CXX_FLAGS.finditer(text):
        findings.append(
            _finding(
                "FORBIDDEN_GLOBAL_CMAKE_COMMAND",
                path,
                _line_number(text, match.start()),
                command="CMAKE_CXX_FLAGS",
            )
        )
    for match in RECURSIVE_GLOB.finditer(text):
        findings.append(
            _finding(
                "FIRST_PARTY_RECURSIVE_GLOB",
                path,
                _line_number(text, match.start()),
            )
        )
    return findings


def _scan_cpp(path: Path, text: str) -> list[dict]:
    findings = []
    is_public_header = "include" in path.parts and "lol" in path.parts
    for line_number, line in enumerate(text.splitlines(), start=1):
        include = INCLUDE_DIRECTIVE.match(line)
        if include:
            include_path = include.group(1).replace("\\", "/")
            include_parts = include_path.split("/")
            if ".." in include_parts:
                findings.append(
                    _finding("PARENT_PATH_BOUNDARY_ESCAPE", path, line_number)
                )
            if any(
                segment in include_parts
                for segment in ("application", "domain", "private", "src")
            ):
                findings.append(
                    _finding("CROSS_MODULE_PRIVATE_INCLUDE", path, line_number)
                )
                if is_public_header:
                    findings.append(
                        _finding("PUBLIC_HEADER_PRIVATE_TYPE_LEAK", path, line_number)
                    )
        if is_public_header and PRIVATE_NAMESPACE.search(line):
            findings.append(
                _finding("PUBLIC_HEADER_PRIVATE_TYPE_LEAK", path, line_number)
            )
    return findings


def scan_source_policy(source_root: Path) -> tuple[int, list[dict]]:
    files_scanned = 0
    findings: list[dict] = []
    for file_path in sorted(source_root.rglob("*")):
        if not file_path.is_file():
            continue
        relative_path = file_path.relative_to(source_root)
        if _is_ignored(relative_path) or not _is_scannable(file_path):
            continue

        files_scanned += 1
        text = file_path.read_text(errors="replace")
        if file_path.name == "CMakeLists.txt" or file_path.suffix == ".cmake":
            findings.extend(_scan_cmake(relative_path, text))
        elif file_path.suffix in CPP_SUFFIXES:
            findings.extend(_scan_cpp(relative_path, text))

    findings.sort(
        key=lambda finding: (
            finding["path"],
            finding["line"],
            finding["code"],
            finding.get("command", ""),
        )
    )
    return files_scanned, findings
