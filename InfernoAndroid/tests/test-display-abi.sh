#!/usr/bin/env bash
# Compile both actual ABI definitions, not a second test-only struct model.
set -euo pipefail
BASE="$(cd "$(dirname "$0")/.." && pwd)"
: "${ENGINE_SRC:=$BASE/build/inferno-src}"
: "${CC:=cc}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/check.c" <<'EOF'
#include <stddef.h>
#define InfernoDisplayMetricsV1 AppMetrics
#include "display_metrics.h"
#undef InfernoDisplayMetricsV1
#include "ui/inferno-embed.h"
_Static_assert(sizeof(InfernoFrameInfo)==32, "legacy FrameInfo ABI");
_Static_assert(offsetof(InfernoFrameInfo,generation)==28, "legacy generation offset");
_Static_assert(sizeof(InfernoDisplayStats)==16, "legacy stats ABI");
_Static_assert(sizeof(AppMetrics)==sizeof(InfernoDisplayMetricsV1), "metrics ABI size");
_Static_assert(_Alignof(AppMetrics)==_Alignof(InfernoDisplayMetricsV1), "metrics ABI alignment");
#define FIELD(f) _Static_assert(offsetof(AppMetrics,f)==offsetof(InfernoDisplayMetricsV1,f), #f)
FIELD(version); FIELD(size); FIELD(presents); FIELD(refreshes); FIELD(pixman_cpu_ns);
FIELD(copy_cpu_ns); FIELD(copy_wait_ns); FIELD(copy_bytes); FIELD(main_cpu_ns);
FIELD(gap_count); FIELD(gap_sum_ns); FIELD(gap_max_ns); FIELD(gap_hist);
int main(void) { return INFERNO_FRAME_NONE != 0 || INFERNO_FRAME_OK != 1 || INFERNO_FRAME_RESIZE != 2; }
EOF
"$CC" -std=c11 -Wall -Wextra -Werror -I"$BASE/app/src/main/cpp" -I"$ENGINE_SRC/include" "$TMP/check.c" -o "$TMP/check"
"$TMP/check"
echo 'PASS: legacy display ABI and optional metrics layout agree'
