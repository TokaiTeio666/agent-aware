#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DATE_TAG="${DATE_TAG:-$(date +%F)}"
OUT="$ROOT/archive/v6-autodl-evidence-$DATE_TAG.tar.gz"

tar -czf "$OUT" \
  -C "$ROOT" \
  docs/PROJECT_PLAN.md \
  docs/VALIDATION_METHOD.md \
  docs/AUTODL_DEPLOYMENT.md \
  docs/iterations \
  archive/results \
  archive/configs \
  archive/logs \
  archive/build_info \
  archive/validation

echo "Packaged $OUT"
