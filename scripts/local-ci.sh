#!/usr/bin/env bash
# 本地一键复刻 CI:在与 GitHub 完全相同的镜像里跑某个 variant。
# 推代码前先在这里验证,挂了就在本地秒级看到,不用推上去等几分钟。
#
#   ./scripts/local-ci.sh fuzz      # 复刻 fuzz job
#   ./scripts/local-ci.sh asan      # 复刻 asan+ubsan job
#   ./scripts/local-ci.sh           # 默认 asan

set -euo pipefail
cd "$(dirname "$0")/.."

docker build -f Dockerfile.ci -t rain-ci .
docker run --rm -v "$PWD":/w -w /w rain-ci bash scripts/ci.sh "${1:-asan}"
