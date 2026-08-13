import json
from pathlib import Path


PRODUCTION_TARGET_TYPES = {
    "EXECUTABLE",
    "MODULE_LIBRARY",
    "OBJECT_LIBRARY",
    "SHARED_LIBRARY",
    "STATIC_LIBRARY",
}


def ensure_codemodel_query(build_directory: Path) -> None:
    query = build_directory / ".cmake" / "api" / "v1" / "query" / "codemodel-v2"
    query.parent.mkdir(parents=True, exist_ok=True)
    query.touch(exist_ok=True)


def _load_latest_index(reply_directory: Path) -> tuple[dict, Path]:
    indices = sorted(
        reply_directory.glob("index-*.json"),
        key=lambda path: (path.stat().st_mtime_ns, path.name),
        reverse=True,
    )
    for index_path in indices:
        try:
            index = json.loads(index_path.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        codemodel = index.get("reply", {}).get("codemodel-v2")
        if isinstance(codemodel, dict) and isinstance(codemodel.get("jsonFile"), str):
            return index, index_path
    raise ValueError("No valid CMake File API codemodel-v2 index found")


def read_target_graph(build_directory: Path) -> dict[str, list[str]]:
    reply_directory = build_directory / ".cmake" / "api" / "v1" / "reply"
    index, _ = _load_latest_index(reply_directory)
    codemodel_file = index["reply"]["codemodel-v2"]["jsonFile"]
    codemodel = json.loads((reply_directory / codemodel_file).read_text())
    configurations = codemodel.get("configurations", [])
    if not configurations:
        raise ValueError("CMake codemodel has no configurations")

    target_entries = configurations[0].get("targets", [])
    names_by_id = {
        target["id"]: target["name"]
        for target in target_entries
        if isinstance(target.get("id"), str) and isinstance(target.get("name"), str)
    }

    graph: dict[str, list[str]] = {}
    for target in target_entries:
        name = target.get("name")
        target_file = target.get("jsonFile")
        if not isinstance(name, str) or not name.startswith("lol_"):
            continue
        if not isinstance(target_file, str):
            raise ValueError(f"CMake target {name} has no jsonFile")

        target_data = json.loads((reply_directory / target_file).read_text())
        if target_data.get("type") not in PRODUCTION_TARGET_TYPES:
            continue

        dependencies = {
            names_by_id[dependency["id"]]
            for dependency in target_data.get("dependencies", [])
            if dependency.get("id") in names_by_id
            and names_by_id[dependency["id"]].startswith("lol_")
        }
        graph[name] = sorted(dependencies)

    return {name: graph[name] for name in sorted(graph)}
