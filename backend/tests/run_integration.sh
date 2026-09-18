#!/usr/bin/env bash
# 分面搜索集成测试运行器：
# 启动内存版 ES 测试替身 → 等待就绪 → 运行 C++ 集成测试 → 关闭替身。
#
# 用法: run_integration.sh <test_binary> <python> <mock_server.py> <sample_data.json>
set -euo pipefail

TEST_BIN="$1"
PYTHON="$2"
MOCK_SERVER="$3"
DATA_FILE="$4"
PORT="${MOCK_PORT:-19200}"

"$PYTHON" "$MOCK_SERVER" "$PORT" >/tmp/mock_es_server.log 2>&1 &
MOCK_PID=$!

cleanup() {
    kill "$MOCK_PID" 2>/dev/null || true
    wait "$MOCK_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# 等待测试替身就绪（不依赖 curl，用 Python 自身探测）
for _ in $(seq 1 50); do
    if "$PYTHON" - "$PORT" <<'PY' 2>/dev/null; then
import sys, urllib.request
urllib.request.urlopen("http://127.0.0.1:%s/" % sys.argv[1], timeout=1).read()
PY
        break
    fi
    sleep 0.2
done

"$TEST_BIN" 127.0.0.1 "$PORT" "$DATA_FILE"
