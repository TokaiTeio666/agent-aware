#!/usr/bin/env python3
import argparse
import csv
import html
import json
import math
from collections import defaultdict
from pathlib import Path
from xml.sax.saxutils import escape as xml_escape


PARAMETER_AUDIT = [
    (
        "search_width",
        "primary",
        "Directly bounds DiskGraphSearchConfig.search_width and the number of graph expansions.",
        "Recall, visited/page reads, QPS, P95/P99 latency.",
    ),
    (
        "beam_width",
        "primary",
        "Directly controls per-round candidate expansion and I/O batch size.",
        "avg_batch_size, io_submit_syscalls, QPS, P95/P99 latency.",
    ),
    (
        "memory_budget_ratio",
        "primary when cache_pages is auto",
        "Determines automatic cache_pages if cache_pages is not explicitly set.",
        "cache_pages/cache_bytes, cache_hit_rate, P95/P99 latency.",
    ),
    (
        "io_mode",
        "primary",
        "Flows into PackedDiskGraphIndex.configure_io and AsyncPageReader.",
        "io_effective_mode, odirect/io_uring status, I/O submit/completion stats, latency.",
    ),
    (
        "prefetch_depth",
        "secondary",
        "0 disables prefetch; positive values scale the query-level prefetch budget when effective io_uring is available.",
        "Demand/prefetch/submitted reads, useful/wasted prefetch pages, and page read wait.",
    ),
]


METRICS = [
    ("recall", "Recall@K", "Recall"),
    ("qps", "QPS", "Queries / sec"),
    ("p95_ms", "P95 latency", "ms"),
    ("p99_ms", "P99 latency", "ms"),
    ("reads_per_query", "Submitted reads/query", "reads/query"),
    ("demand_reads_per_query", "Demand reads/query", "reads/query"),
    ("prefetch_reads_per_query", "Prefetch reads/query", "reads/query"),
    ("cache_hit_rate", "Cache hit rate", "ratio"),
]


AXES = [
    ("search_width", "Search width"),
    ("beam_width", "Beam width"),
    ("memory_budget_ratio", "Memory budget ratio"),
    ("io_mode", "I/O mode"),
    ("prefetch_depth", "Prefetch depth"),
]


def to_float(value):
    if value is None:
        return None
    try:
        output = float(value)
    except (TypeError, ValueError):
        return None
    if math.isnan(output) or math.isinf(output):
        return None
    return output


def percentile(values, pct):
    values = sorted(v for v in values if to_float(v) is not None)
    if not values:
        return None
    if len(values) == 1:
        return float(values[0])
    rank = (pct / 100.0) * (len(values) - 1)
    lo = int(math.floor(rank))
    hi = int(math.ceil(rank))
    if lo == hi:
        return float(values[lo])
    frac = rank - lo
    return float(values[lo]) * (1.0 - frac) + float(values[hi]) * frac


def should_skip(path):
    if path.name.startswith("stdout") and (path.parent / "result.json").exists():
        return True
    return False


def iter_json_paths(inputs):
    for raw in inputs:
        path = Path(raw)
        if path.is_dir():
            for child in sorted(path.rglob("*.json")):
                if not should_skip(child):
                    yield child
        elif path.is_file() and path.suffix == ".json":
            if not should_skip(path):
                yield path


def get_stats_value(stats, *names):
    for name in names:
        value = stats.get(name)
        if value is not None:
            return value
    return None


def per_query(value, query_count):
    value = to_float(value)
    if value is None or query_count <= 0:
        return None
    return value / query_count


def infer_axis(record):
    axis = record.get("matrix_axis")
    if axis and axis != "grid":
        return axis
    if record.get("search_width") is not None:
        return "search_width"
    if record.get("beam_width") is not None:
        return "beam_width"
    if record.get("memory_budget_ratio") is not None:
        return "memory_budget_ratio"
    if record.get("io_mode") is not None:
        return "io_mode"
    return "unclassified"


def extract_record(source, payload, run=None):
    data = run if run is not None else payload
    stats = data.get("stats", {}) if isinstance(data.get("stats", {}), dict) else {}
    query_count = (
        data.get("query_count")
        or payload.get("query_count")
        or data.get("recall_queries")
        or payload.get("recall_queries")
        or 0
    )
    query_count = int(query_count or 0)

    latency_values = data.get("query_latency_ms") or payload.get("query_latency_ms") or []
    p95_ms = (
        to_float(data.get("latency_p95_ms"))
        or to_float(data.get("p95_latency_ms"))
        or percentile(latency_values, 95.0)
    )
    p99_ms = (
        to_float(data.get("latency_p99_ms"))
        or to_float(data.get("p99_latency_ms"))
        or percentile(latency_values, 99.0)
    )

    submitted_reads = to_float(get_stats_value(stats, "submitted_reads", "page_reads"))
    completed_reads = to_float(stats.get("completed_reads"))
    demand_reads = to_float(stats.get("demand_reads"))
    prefetch_reads = to_float(stats.get("prefetch_reads"))
    duplicate_skipped = to_float(stats.get("duplicate_skipped"))
    cache_hits = to_float(get_stats_value(stats, "cache_hits", "page_cache_hits"))
    pinned_hits = to_float(stats.get("pinned_hits"))
    pending_hits = to_float(stats.get("pending_hits"))
    reads_per_query = to_float(data.get("reads_per_query"))
    if reads_per_query is None and submitted_reads is not None and query_count > 0:
        reads_per_query = submitted_reads / query_count

    demand_reads_per_query = to_float(data.get("demand_reads_per_query"))
    if demand_reads_per_query is None:
        demand_reads_per_query = per_query(demand_reads, query_count)
    prefetch_reads_per_query = to_float(data.get("prefetch_reads_per_query"))
    if prefetch_reads_per_query is None:
        prefetch_reads_per_query = per_query(prefetch_reads, query_count)
    duplicate_skipped_per_query = to_float(
        data.get("duplicate_skipped_per_query")
    )
    if duplicate_skipped_per_query is None:
        duplicate_skipped_per_query = per_query(duplicate_skipped, query_count)
    submitted_reads_per_query = per_query(submitted_reads, query_count)
    completed_reads_per_query = per_query(completed_reads, query_count)
    cache_hits_per_query = per_query(cache_hits, query_count)
    pinned_hits_per_query = per_query(pinned_hits, query_count)
    pending_hits_per_query = per_query(pending_hits, query_count)

    page_requests = to_float(get_stats_value(stats, "page_requests"))
    page_requests_per_query = None
    if page_requests is not None and query_count > 0:
        page_requests_per_query = page_requests / query_count

    cache_hit_rate = to_float(data.get("cache_hit_rate"))
    if cache_hit_rate is None:
        cache_hit_rate = to_float(stats.get("cache_hit_rate"))
    prefetch_submitted = to_float(stats.get("prefetch_submitted_pages"))
    prefetch_useful = to_float(stats.get("prefetch_useful_pages"))
    prefetch_useful_ratio = None
    if prefetch_submitted and prefetch_submitted > 0 and prefetch_useful is not None:
        prefetch_useful_ratio = prefetch_useful / prefetch_submitted

    requested = payload.get("matrix_requested", {})
    search_width = (
        data.get("effective_search_width")
        or data.get("requested_search_width")
        or data.get("search_width")
        or requested.get("search_width")
    )
    beam_width = (
        data.get("effective_beam_width")
        or data.get("requested_beam_width")
        or data.get("beam_width")
        or requested.get("beam_width")
    )
    memory_budget_ratio = data.get("memory_budget_ratio") or requested.get("memory_budget_ratio")
    io_mode = (
        data.get("io_effective_mode")
        or data.get("io_mode")
        or data.get("io_requested_mode")
        or requested.get("io_mode")
    )
    prefetch_depth = (
        data.get("effective_prefetch_depth")
        if data.get("effective_prefetch_depth") is not None
        else data.get("requested_prefetch_depth")
    )
    if prefetch_depth is None:
        prefetch_depth = requested.get("prefetch_depth")

    record = {
        "source": str(source),
        "status": data.get("status") or payload.get("status"),
        "matrix_phase": data.get("matrix_phase") or payload.get("matrix_phase") or "unclassified",
        "matrix_axis": data.get("matrix_axis") or payload.get("matrix_axis"),
        "dataset_mode": data.get("dataset_mode") or payload.get("dataset_mode"),
        "base_count": data.get("base_count") or payload.get("base_count"),
        "query_count": query_count or None,
        "search_width": to_float(search_width),
        "beam_width": to_float(beam_width),
        "memory_budget_ratio": to_float(memory_budget_ratio),
        "io_mode": io_mode,
        "prefetch_depth": to_float(prefetch_depth),
        "cache_policy": data.get("cache_policy") or payload.get("cache_policy"),
        "cache_pages": data.get("cache_pages") or payload.get("cache_pages"),
        "recall": to_float(data.get("recall_at_k")) or to_float(data.get("recall_at_10")),
        "qps": to_float(data.get("qps")),
        "p95_ms": p95_ms,
        "p99_ms": p99_ms,
        "reads_per_query": reads_per_query,
        "submitted_reads_per_query": submitted_reads_per_query,
        "completed_reads_per_query": completed_reads_per_query,
        "demand_reads_per_query": demand_reads_per_query,
        "prefetch_reads_per_query": prefetch_reads_per_query,
        "duplicate_skipped_per_query": duplicate_skipped_per_query,
        "cache_hits_per_query": cache_hits_per_query,
        "pinned_hits_per_query": pinned_hits_per_query,
        "pending_hits_per_query": pending_hits_per_query,
        "page_requests_per_query": page_requests_per_query,
        "cache_hit_rate": cache_hit_rate,
        "prefetch_useful_ratio": prefetch_useful_ratio,
        "git_commit": data.get("git_commit") or payload.get("git_commit"),
    }
    record["axis"] = infer_axis(record)
    return record


def load_records(inputs):
    records = []
    for path in iter_json_paths(inputs):
        try:
            payload = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        if isinstance(payload, dict) and payload.get("status") == "planned":
            continue
        if isinstance(payload, dict) and isinstance(payload.get("runs"), list):
            for run in payload["runs"]:
                records.append(extract_record(path, payload, run))
            continue
        if isinstance(payload, dict):
            status = payload.get("status")
            if status in ("completed", "archived", "planned", "skipped") or "recall_at_k" in payload:
                if status == "planned":
                    continue
                records.append(extract_record(path, payload))
    return records


def write_csv_summary(path, records):
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "source",
        "status",
        "matrix_phase",
        "axis",
        "dataset_mode",
        "base_count",
        "query_count",
        "search_width",
        "beam_width",
        "memory_budget_ratio",
        "io_mode",
        "prefetch_depth",
        "cache_policy",
        "cache_pages",
        "recall",
        "qps",
        "p95_ms",
        "p99_ms",
        "reads_per_query",
        "submitted_reads_per_query",
        "completed_reads_per_query",
        "demand_reads_per_query",
        "prefetch_reads_per_query",
        "duplicate_skipped_per_query",
        "cache_hits_per_query",
        "pinned_hits_per_query",
        "pending_hits_per_query",
        "page_requests_per_query",
        "cache_hit_rate",
        "prefetch_useful_ratio",
        "git_commit",
    ]
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for record in records:
            writer.writerow({field: record.get(field) for field in fields})


def safe_name(value):
    output = []
    for ch in str(value):
        if ch.isalnum() or ch in ("-", "_"):
            output.append(ch)
        else:
            output.append("_")
    return "".join(output).strip("_") or "chart"


def nice_number(value):
    if value is None:
        return ""
    value = float(value)
    if abs(value) >= 100:
        return f"{value:.0f}"
    if abs(value) >= 10:
        return f"{value:.1f}"
    if abs(value) >= 1:
        return f"{value:.2f}"
    return f"{value:.3f}"


def aggregate_points(records, xfield, yfield):
    grouped = defaultdict(list)
    x_is_numeric = True
    for record in records:
        y = to_float(record.get(yfield))
        x = record.get(xfield)
        if y is None or x is None:
            continue
        if to_float(x) is None:
            x_is_numeric = False
        grouped[x].append(y)
    points = []
    for x, values in grouped.items():
        avg = sum(values) / len(values)
        points.append((x, avg, len(values)))
    if x_is_numeric:
        points.sort(key=lambda item: float(item[0]))
    else:
        points.sort(key=lambda item: str(item[0]))
    return points, x_is_numeric


def svg_text(x, y, value, size=12, anchor="middle", extra=""):
    return (
        f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" '
        f'text-anchor="{anchor}" fill="#1f2937" {extra}>'
        f"{xml_escape(str(value))}</text>"
    )


def write_svg_chart(path, points, numeric_x, title, xlabel, ylabel):
    path.parent.mkdir(parents=True, exist_ok=True)
    width, height = 760, 430
    left, right, top, bottom = 72, 24, 46, 72
    plot_w = width - left - right
    plot_h = height - top - bottom

    if not points:
        content = [
            '<svg xmlns="http://www.w3.org/2000/svg" width="760" height="220">',
            '<rect width="100%" height="100%" fill="#ffffff"/>',
            svg_text(380, 104, title, 16),
            svg_text(380, 136, "No data", 13),
            "</svg>",
        ]
        path.write_text("\n".join(content) + "\n")
        return

    ys = [point[1] for point in points]
    y_min, y_max = min(ys), max(ys)
    if y_min == y_max:
        pad = abs(y_min) * 0.1 if y_min else 1.0
        y_min -= pad
        y_max += pad
    else:
        pad = (y_max - y_min) * 0.08
        y_min -= pad
        y_max += pad

    if numeric_x:
        xs = [float(point[0]) for point in points]
        x_min, x_max = min(xs), max(xs)
        if x_min == x_max:
            x_min -= 1.0
            x_max += 1.0

        def x_pos(raw):
            return left + (float(raw) - x_min) / (x_max - x_min) * plot_w

    else:
        labels = [str(point[0]) for point in points]

        def x_pos(raw):
            index = labels.index(str(raw))
            if len(labels) == 1:
                return left + plot_w / 2
            return left + index / (len(labels) - 1) * plot_w

    def y_pos(value):
        return top + (y_max - value) / (y_max - y_min) * plot_h

    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        svg_text(width / 2, 24, title, 16),
        f'<line x1="{left}" y1="{top + plot_h}" x2="{left + plot_w}" y2="{top + plot_h}" stroke="#374151" stroke-width="1"/>',
        f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top + plot_h}" stroke="#374151" stroke-width="1"/>',
    ]

    for tick in range(5):
        ratio = tick / 4
        y = top + plot_h * ratio
        value = y_max - (y_max - y_min) * ratio
        lines.append(
            f'<line x1="{left}" y1="{y:.1f}" x2="{left + plot_w}" y2="{y:.1f}" stroke="#e5e7eb" stroke-width="1"/>'
        )
        lines.append(svg_text(left - 10, y + 4, nice_number(value), 11, "end"))

    point_pairs = [(x_pos(point[0]), y_pos(point[1])) for point in points]
    polyline = " ".join(f"{x:.1f},{y:.1f}" for x, y in point_pairs)
    lines.append(
        f'<polyline points="{polyline}" fill="none" stroke="#2563eb" stroke-width="2.5" stroke-linejoin="round"/>'
    )
    for (x, y), (raw_x, raw_y, count) in zip(point_pairs, points):
        lines.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="4" fill="#2563eb"/>')
        label = nice_number(raw_y)
        if count > 1:
            label += f" n={count}"
        lines.append(svg_text(x, y - 9, label, 10))

    for raw_x, _, _ in points:
        x = x_pos(raw_x)
        label = nice_number(raw_x) if numeric_x else str(raw_x)
        lines.append(svg_text(x, top + plot_h + 22, label, 11))

    lines.append(svg_text(left + plot_w / 2, height - 18, xlabel, 12))
    lines.append(
        svg_text(
            18,
            top + plot_h / 2,
            ylabel,
            12,
            "middle",
            'transform="rotate(-90 18 202)"',
        )
    )
    lines.append("</svg>")
    path.write_text("\n".join(lines) + "\n")


def write_charts(output_dir, records):
    chart_dir = output_dir / "charts"
    chart_paths = []
    for axis, axis_label in AXES:
        axis_records = [
            record
            for record in records
            if record.get(axis) is not None
            and (record.get("axis") == axis or record.get("axis") == "grid")
        ]
        if not axis_records:
            continue
        for metric, metric_title, ylabel in METRICS:
            points, numeric_x = aggregate_points(axis_records, axis, metric)
            if not points:
                continue
            path = chart_dir / f"{safe_name(axis)}_{safe_name(metric)}.svg"
            write_svg_chart(
                path,
                points,
                numeric_x,
                f"{axis_label} -> {metric_title}",
                axis_label,
                ylabel,
            )
            chart_paths.append(path)
    return chart_paths


def write_audit(path):
    lines = [
        "# SIFT Parameter Matrix Audit",
        "",
        "| Parameter | Role | Why it is expected to affect results | Expected moving metrics |",
        "| --- | --- | --- | --- |",
    ]
    for name, role, reason, metrics in PARAMETER_AUDIT:
        lines.append(f"| `{name}` | {role} | {reason} | {metrics} |")
    lines.extend(
        [
            "",
            "Primary matrix axes are `search_width`, `beam_width`, `memory_budget_ratio`, and `io_mode`.",
            "`prefetch_depth` is secondary; staged `io_mode` comparisons default it to 0 for all modes so the sweep isolates the reader path.",
        ]
    )
    path.write_text("\n".join(lines) + "\n")


def format_cell(value):
    if value is None:
        return ""
    if isinstance(value, float):
        return nice_number(value)
    return str(value)


def write_dashboard(path, records, chart_paths):
    rel_charts = [chart.relative_to(path.parent) for chart in chart_paths]
    rows = sorted(
        records,
        key=lambda r: (
            str(r.get("matrix_phase") or ""),
            str(r.get("axis") or ""),
            str(r.get("search_width") or ""),
            str(r.get("beam_width") or ""),
            str(r.get("io_mode") or ""),
        ),
    )
    table_fields = [
        "matrix_phase",
        "axis",
        "search_width",
        "beam_width",
        "memory_budget_ratio",
        "io_mode",
        "recall",
        "qps",
        "p95_ms",
        "p99_ms",
        "reads_per_query",
        "demand_reads_per_query",
        "prefetch_reads_per_query",
        "duplicate_skipped_per_query",
        "cache_hit_rate",
    ]
    html_lines = [
        "<!doctype html>",
        "<html><head><meta charset=\"utf-8\">",
        "<title>SIFT parameter matrix dashboard</title>",
        "<style>",
        "body{font-family:Arial,sans-serif;margin:24px;color:#111827}",
        "h1{font-size:24px} h2{font-size:18px;margin-top:28px}",
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(360px,1fr));gap:18px}",
        ".card{border:1px solid #d1d5db;border-radius:8px;padding:10px;background:#fff}",
        ".card img{width:100%;height:auto}",
        "table{border-collapse:collapse;width:100%;font-size:12px}",
        "th,td{border:1px solid #e5e7eb;padding:6px;text-align:right}",
        "th{background:#f3f4f6} td:first-child,th:first-child{text-align:left}",
        "code{background:#f3f4f6;padding:2px 4px;border-radius:4px}",
        "</style></head><body>",
        "<h1>SIFT parameter matrix dashboard</h1>",
        f"<p>Records: <strong>{len(records)}</strong>. Generated from benchmark JSON files.</p>",
        "<p>See <code>matrix_summary.csv</code> for raw extracted values and <code>parameter_matrix_audit.md</code> for parameter rationale.</p>",
        "<h2>Curves</h2>",
        "<div class=\"grid\">",
    ]
    for chart in rel_charts:
        html_lines.append(
            f"<div class=\"card\"><img src=\"{html.escape(str(chart))}\" alt=\"{html.escape(chart.stem)}\"></div>"
        )
    html_lines.extend(["</div>", "<h2>Extracted Results</h2>", "<table><thead><tr>"])
    for field in table_fields:
        html_lines.append(f"<th>{html.escape(field)}</th>")
    html_lines.append("</tr></thead><tbody>")
    for record in rows:
        html_lines.append("<tr>")
        for field in table_fields:
            html_lines.append(f"<td>{html.escape(format_cell(record.get(field)))}</td>")
        html_lines.append("</tr>")
    html_lines.extend(["</tbody></table>", "</body></html>"])
    path.write_text("\n".join(html_lines) + "\n")


def main():
    parser = argparse.ArgumentParser(
        description="Summarize agent_aware_flow matrix JSON files and render SVG curves."
    )
    parser.add_argument("inputs", nargs="+", help="Matrix output directories or JSON files.")
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    records = load_records(args.inputs)
    if not records:
        raise SystemExit("no benchmark records found")

    write_csv_summary(output_dir / "matrix_summary.csv", records)
    write_audit(output_dir / "parameter_matrix_audit.md")
    chart_paths = write_charts(output_dir, records)
    write_dashboard(output_dir / "matrix_dashboard.html", records, chart_paths)

    print(f"records={len(records)}")
    print(f"summary={output_dir / 'matrix_summary.csv'}")
    print(f"dashboard={output_dir / 'matrix_dashboard.html'}")
    print(f"charts={output_dir / 'charts'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
