"""Small, explicit validator for the Loot of Legends evidence contracts.

This intentionally validates only Loot of Legends evidence documents.  It is
not a general JSON Schema implementation.
"""

from __future__ import annotations

import hashlib
import json
import math
import re
from functools import lru_cache
from pathlib import Path
from typing import Any, Callable


EVALUATION_STATUSES = {"PASS", "FAIL", "INVALID", "ABORTED"}
PACKAGE_STATUSES = {"COMPLETE", "INCOMPLETE", "CORRUPT"}
GATE_CLASSES = {
    "SLICE_VERIFICATION",
    "PRODUCT_RELEASE",
    "OFFICIAL_CAPACITY",
    "FROZEN_REGRESSION_BASELINE",
}
RETENTION_CLASSES = {
    "PUBLIC_CLAIM_SUMMARY",
    "SANITIZED_EVIDENCE_BUNDLE",
    "RESTRICTED_RAW_DIAGNOSTICS",
}

_DIGEST = re.compile(r"^[0-9a-f]{64}$")
_SOURCE_SHA = re.compile(r"^[0-9a-f]{40}$")
_SAFE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
_CONTRACTS = Path(__file__).resolve().parents[4] / "contracts" / "evidence"


def canonical_json_bytes(value: object) -> bytes:
    """Return the one canonical encoding used by every evidence digest."""

    return json.dumps(
        value,
        ensure_ascii=False,
        allow_nan=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def document_digest(value: object) -> str:
    return hashlib.sha256(canonical_json_bytes(value)).hexdigest()


def validate_document(kind: str, value: object) -> list[str]:
    validators: dict[str, Callable[[object], list[str]]] = {
        "workload-profile": _validate_workload_profile,
        "run-input": _validate_run_input,
        "run-result": _validate_run_result,
        "evidence-package": _validate_evidence_package,
    }
    validator = validators.get(kind)
    if validator is None:
        return [f"unknown evidence document kind: {kind}"]
    return validator(value)

def _validate_workload_profile(value: object) -> list[str]:
    errors: list[str] = []
    if not _object(
        value,
        {
            "schemaVersion",
            "profileId",
            "version",
            "gateClass",
            "participantCount",
            "roomCount",
            "participantsPerRoom",
            "requiredCyclesPerRoom",
            "warmupSeconds",
            "measurementSeconds",
            "overallDeadlineSeconds",
            "seed",
            "capacityClaimEligible",
            "networkImpairment",
        },
        set(),
        "profile",
        errors,
    ):
        return errors
    assert isinstance(value, dict)
    _schema_version(value, errors)
    for field in ("profileId", "version"):
        _identifier(value.get(field), f"profile.{field}", errors)
    _enum(value.get("gateClass"), GATE_CLASSES, "profile.gateClass", errors)
    for field, minimum in (
        ("participantCount", 1),
        ("roomCount", 1),
        ("participantsPerRoom", 1),
        ("requiredCyclesPerRoom", 1),
        ("warmupSeconds", 0),
        ("measurementSeconds", 0),
        ("overallDeadlineSeconds", 1),
        ("seed", 0),
    ):
        _integer(value.get(field), minimum, f"profile.{field}", errors)
    if (
        isinstance(value.get("participantCount"), int)
        and isinstance(value.get("roomCount"), int)
        and isinstance(value.get("participantsPerRoom"), int)
        and value["participantCount"] != value["roomCount"] * value["participantsPerRoom"]
    ):
        errors.append("profile participantCount must equal roomCount * participantsPerRoom")
    if value.get("participantsPerRoom") != 10:
        errors.append("profile.participantsPerRoom must be 10")
    if not isinstance(value.get("capacityClaimEligible"), bool):
        errors.append("profile.capacityClaimEligible must be boolean")
    impairment = value.get("networkImpairment")
    if not _object(
        impairment,
        {"enabled", "lossPercent", "jitterMilliseconds", "reorderPercent"},
        set(),
        "profile.networkImpairment",
        errors,
    ):
        return errors
    assert isinstance(impairment, dict)
    if not isinstance(impairment.get("enabled"), bool):
        errors.append("profile.networkImpairment.enabled must be boolean")
    for field in ("lossPercent", "jitterMilliseconds", "reorderPercent"):
        number = impairment.get(field)
        if isinstance(number, bool) or not isinstance(number, (int, float)) or number < 0:
            errors.append(f"profile.networkImpairment.{field} must be a non-negative number")
    return errors


def _validate_run_input(value: object) -> list[str]:
    errors: list[str] = []
    if not _object(
        value,
        {
            "schemaVersion",
            "runId",
            "attemptId",
            "seriesId",
            "seriesIdentity",
            "seriesIdentityDigest",
            "provenance",
            "profile",
        },
        set(),
        "runInput",
        errors,
    ):
        return errors
    assert isinstance(value, dict)
    _schema_version(value, errors)
    for field in ("runId", "attemptId", "seriesId"):
        _identifier(value.get(field), f"runInput.{field}", errors)
    _series_and_provenance(value, errors)
    profile_errors = _validate_workload_profile(value.get("profile"))
    errors.extend(f"profile.{error}" for error in profile_errors)
    provenance = value.get("provenance")
    if isinstance(provenance, dict) and isinstance(value.get("profile"), dict):
        if document_digest(value["profile"]) != provenance.get("workloadProfileDigest"):
            errors.append("runInput.profile digest differs from provenance.workloadProfileDigest")
    return errors


def _validate_run_result(value: object) -> list[str]:
    errors: list[str] = []
    if not _object(
        value,
        {
            "schemaVersion",
            "runId",
            "attemptId",
            "seriesId",
            "seriesIdentity",
            "seriesIdentityDigest",
            "evaluationStatus",
            "packageStatus",
            "reasonCodes",
            "provenance",
        },
        {"statistics", "reclassificationOf"},
        "runResult",
        errors,
    ):
        return errors
    assert isinstance(value, dict)
    _schema_version(value, errors)
    for field in ("runId", "attemptId", "seriesId"):
        _identifier(value.get(field), f"runResult.{field}", errors)
    _enum(value.get("evaluationStatus"), EVALUATION_STATUSES, "runResult.evaluationStatus", errors)
    _enum(value.get("packageStatus"), PACKAGE_STATUSES, "runResult.packageStatus", errors)
    _reason_codes(value.get("reasonCodes"), "runResult.reasonCodes", errors)
    if "reclassificationOf" in value:
        _digest_value(value["reclassificationOf"], "runResult.reclassificationOf", errors)
    if "statistics" in value:
        _classifier_statistics(value["statistics"], errors)
    if (
        value.get("evaluationStatus") != "PASS" or value.get("packageStatus") != "COMPLETE"
    ) and not value.get("reasonCodes"):
        errors.append("runResult.reasonCodes must explain non-PASS or non-COMPLETE status")
    _series_and_provenance(value, errors)
    return errors


def _classifier_statistics(value: object, errors: list[str]) -> None:
    if not _object(
        value,
        {"sampleCounts", "percentiles", "maxima", "ratios"},
        set(),
        "runResult.statistics",
        errors,
    ):
        return
    assert isinstance(value, dict)
    sample_names = {
        "critical_terminal_latency_ms",
        "room_queue_delay_ms",
        "room_processing_duration_ms",
        "active_snapshot_interval_ms",
        "durable_append_latency_ms",
        "process_cpu_busy_ratio",
        "process_rss_bytes",
        "process_fd_count",
        "outbox_oldest_pending_ms",
        "outbox_drain_ms",
        "gameplay_progress_gap_ms",
        "worker_heartbeat_gap_ms",
        "scheduler_slip_ms",
    }
    counts = value.get("sampleCounts")
    if _object(counts, sample_names, set(), "runResult.statistics.sampleCounts", errors):
        assert isinstance(counts, dict)
        for name in sample_names:
            _integer(counts.get(name), 0, f"runResult.statistics.sampleCounts.{name}", errors)
    percentile_names = {
        "critical_terminal_latency_p99_ms",
        "room_queue_delay_p99_ms",
        "active_snapshot_interval_p99_ms",
        "durable_append_latency_p99_ms",
        "process_cpu_busy_p95_ratio",
        "scheduler_slip_p99_ms",
    }
    maximum_names = sample_names
    ratio_names = {
        "metrics_coverage",
        "command_emission_coverage",
        "memory_usage",
        "file_descriptor_usage",
    }
    for field, names in (
        ("percentiles", percentile_names),
        ("maxima", maximum_names),
        ("ratios", ratio_names),
    ):
        values = value.get(field)
        path = f"runResult.statistics.{field}"
        if not _object(values, names, set(), path, errors):
            continue
        assert isinstance(values, dict)
        for name in names:
            number = values.get(name)
            if number is not None and (
                isinstance(number, bool)
                or not isinstance(number, (int, float))
                or not math.isfinite(number)
                or number < 0
            ):
                errors.append(f"{path}.{name} must be null or a finite non-negative number")


def _validate_evidence_package(value: object) -> list[str]:
    errors: list[str] = []
    if not _object(
        value,
        {"schemaVersion", "runId", "packageStatus", "reasonCodes", "missingPaths", "files"},
        {"rawBundleDigest", "sanitizedBundleDigest", "inventoryDigest"},
        "evidencePackage",
        errors,
    ):
        return errors
    assert isinstance(value, dict)
    _schema_version(value, errors)
    _identifier(value.get("runId"), "evidencePackage.runId", errors)
    _enum(value.get("packageStatus"), PACKAGE_STATUSES, "evidencePackage.packageStatus", errors)
    _reason_codes(value.get("reasonCodes"), "evidencePackage.reasonCodes", errors)
    bundle_fields = ("rawBundleDigest", "sanitizedBundleDigest", "inventoryDigest")
    present_bundle_fields = [field for field in bundle_fields if field in value]
    if present_bundle_fields and len(present_bundle_fields) != len(bundle_fields):
        errors.append("evidencePackage bundle digest fields must be present together")
    for field in present_bundle_fields:
        _digest_value(value[field], f"evidencePackage.{field}", errors)
    missing = value.get("missingPaths")
    if not isinstance(missing, list) or any(not _safe_path(path) for path in missing):
        errors.append("evidencePackage.missingPaths must contain safe relative paths")
        missing = []
    files = value.get("files")
    mismatches = 0
    if not isinstance(files, list):
        errors.append("evidencePackage.files must be an array")
        files = []
    seen_paths: set[str] = set()
    for index, item in enumerate(files):
        path = f"evidencePackage.files[{index}]"
        if not _object(
            item,
            {"path", "sha256", "observedSha256", "bytes", "retentionClass"},
            set(),
            path,
            errors,
        ):
            continue
        assert isinstance(item, dict)
        if not _safe_path(item.get("path")):
            errors.append(f"{path}.path must be a safe relative path")
        elif item["path"] in seen_paths:
            errors.append(f"{path}.path is duplicated")
        else:
            seen_paths.add(item["path"])
        _digest_value(item.get("sha256"), f"{path}.sha256", errors)
        _digest_value(item.get("observedSha256"), f"{path}.observedSha256", errors)
        _integer(item.get("bytes"), 0, f"{path}.bytes", errors)
        _enum(item.get("retentionClass"), RETENTION_CLASSES, f"{path}.retentionClass", errors)
        if item.get("sha256") != item.get("observedSha256"):
            mismatches += 1
    status = value.get("packageStatus")
    if status == "COMPLETE" and (missing or mismatches):
        errors.append("evidencePackage.packageStatus COMPLETE cannot have missing/corrupt files")
    if status == "INCOMPLETE" and not missing:
        errors.append("evidencePackage.packageStatus INCOMPLETE requires missingPaths")
    if status == "CORRUPT" and mismatches == 0:
        errors.append("evidencePackage.packageStatus CORRUPT requires a checksum mismatch")
    if status != "COMPLETE" and not value.get("reasonCodes"):
        errors.append("evidencePackage.reasonCodes must explain non-COMPLETE status")
    return errors


def _series_and_provenance(value: dict[str, Any], errors: list[str]) -> None:
    identity = value.get("seriesIdentity")
    provenance = value.get("provenance")
    _series_identity(identity, "seriesIdentity", errors)
    _provenance(provenance, "provenance", errors)
    _digest_value(value.get("seriesIdentityDigest"), "seriesIdentityDigest", errors)
    if isinstance(identity, dict) and value.get("seriesIdentityDigest") != document_digest(identity):
        errors.append("seriesIdentityDigest does not match canonical seriesIdentity")
    if not isinstance(identity, dict) or not isinstance(provenance, dict):
        return
    pairs = {
        "gameSourceSha": ("game", "sourceSha"),
        "gameArtifactDigest": ("game", "artifactDigest"),
        "metaSourceSha": ("meta", "sourceSha"),
        "metaArtifactDigest": ("meta", "artifactDigest"),
        "harnessSourceSha": ("harness", "sourceSha"),
        "harnessArtifactDigest": ("harness", "artifactDigest"),
        "workloadProfileDigest": (None, "workloadProfileDigest"),
        "gateDigest": (None, "gateDigest"),
        "classifierDigest": (None, "classifierDigest"),
        "environmentDigest": (None, "environmentDigest"),
    }
    for identity_field, (owner, provenance_field) in pairs.items():
        source = provenance if owner is None else provenance.get(owner)
        if isinstance(source, dict) and identity.get(identity_field) != source.get(provenance_field):
            errors.append(f"seriesIdentity.{identity_field} differs from provenance.{provenance_field if owner is None else owner + '.' + provenance_field}")


def _series_identity(value: object, path: str, errors: list[str]) -> None:
    fields = {
        "gameSourceSha",
        "gameArtifactDigest",
        "metaSourceSha",
        "metaArtifactDigest",
        "harnessSourceSha",
        "harnessArtifactDigest",
        "workloadProfileId",
        "workloadProfileVersion",
        "workloadProfileDigest",
        "gateVersion",
        "gateDigest",
        "classifierVersion",
        "classifierDigest",
        "environmentDigest",
        "participantCount",
    }
    if not _object(value, fields, set(), path, errors):
        return
    assert isinstance(value, dict)
    for field in ("gameSourceSha", "metaSourceSha", "harnessSourceSha"):
        _source_sha(value.get(field), f"{path}.{field}", errors)
    for field in (
        "gameArtifactDigest",
        "metaArtifactDigest",
        "harnessArtifactDigest",
        "workloadProfileDigest",
        "gateDigest",
        "classifierDigest",
        "environmentDigest",
    ):
        _digest_value(value.get(field), f"{path}.{field}", errors)
    for field in ("workloadProfileId", "workloadProfileVersion", "gateVersion", "classifierVersion"):
        _identifier(value.get(field), f"{path}.{field}", errors)
    _integer(value.get("participantCount"), 1, f"{path}.participantCount", errors)


def _provenance(value: object, path: str, errors: list[str]) -> None:
    fields = {
        "game",
        "meta",
        "harness",
        "workloadProfileDigest",
        "gateDigest",
        "classifierDigest",
        "environmentDigest",
    }
    if not _object(value, fields, set(), path, errors):
        return
    assert isinstance(value, dict)
    for owner in ("game", "meta", "harness"):
        source = value.get(owner)
        source_path = f"{path}.{owner}"
        if not _object(source, {"sourceSha", "sourceDirty", "artifactDigest"}, set(), source_path, errors):
            continue
        assert isinstance(source, dict)
        _source_sha(source.get("sourceSha"), f"{source_path}.sourceSha", errors)
        if not isinstance(source.get("sourceDirty"), bool):
            errors.append(f"{source_path}.sourceDirty must be boolean")
        _digest_value(source.get("artifactDigest"), f"{source_path}.artifactDigest", errors)
    for field in ("workloadProfileDigest", "gateDigest", "classifierDigest", "environmentDigest"):
        _digest_value(value.get(field), f"{path}.{field}", errors)


@lru_cache(maxsize=1)
def _known_reason_codes() -> frozenset[str]:
    try:
        value = json.loads((_CONTRACTS / "reason-codes.v1.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return frozenset()
    codes = value.get("codes") if isinstance(value, dict) else None
    if not isinstance(codes, list):
        return frozenset()
    return frozenset(
        item["code"]
        for item in codes
        if isinstance(item, dict) and isinstance(item.get("code"), str)
    )


def _reason_codes(value: object, path: str, errors: list[str]) -> None:
    if not isinstance(value, list):
        errors.append(f"{path} must be an array")
        return
    known = _known_reason_codes()
    for index, code in enumerate(value):
        if not isinstance(code, str) or code not in known:
            errors.append(f"{path}[{index}] unknown reason code {code!r}")
    if len(value) != len(set(value)):
        errors.append(f"{path} contains duplicates")


def _schema_version(value: dict[str, Any], errors: list[str]) -> None:
    if value.get("schemaVersion") != 1:
        errors.append("schemaVersion must be 1")


def _object(
    value: object,
    required: set[str],
    optional: set[str],
    path: str,
    errors: list[str],
) -> bool:
    if not isinstance(value, dict):
        errors.append(f"{path} must be an object")
        return False
    missing = sorted(required - value.keys())
    if missing:
        errors.append(f"{path} missing required fields: {', '.join(missing)}")
    extra = sorted(value.keys() - required - optional)
    if extra:
        errors.append(f"{path} has unknown fields: {', '.join(extra)}")
    return not missing


def _identifier(value: object, path: str, errors: list[str]) -> None:
    if not isinstance(value, str) or _SAFE_ID.fullmatch(value) is None:
        errors.append(f"{path} must be a bounded identifier")


def _digest_value(value: object, path: str, errors: list[str]) -> None:
    if not isinstance(value, str) or _DIGEST.fullmatch(value) is None:
        errors.append(f"{path} must be a lowercase SHA-256 digest")


def _source_sha(value: object, path: str, errors: list[str]) -> None:
    if not isinstance(value, str) or _SOURCE_SHA.fullmatch(value) is None:
        errors.append(f"{path} must be a 40-character lowercase source SHA")


def _integer(value: object, minimum: int, path: str, errors: list[str]) -> None:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        errors.append(f"{path} must be an integer >= {minimum}")


def _enum(value: object, allowed: set[str], path: str, errors: list[str]) -> None:
    if not isinstance(value, str) or value not in allowed:
        errors.append(f"{path} must be one of {sorted(allowed)!r}")


def _safe_path(value: object) -> bool:
    if not isinstance(value, str) or not value or value.startswith(("/", "\\")):
        return False
    return ".." not in Path(value).parts and "\\" not in value
