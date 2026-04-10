---
title: 参与贡献
description: Rain 的代码、测试、文档与 benchmark 贡献说明
permalink: /contributing/
---

<section class="content-page" markdown="1">

# 参与贡献

## 提交前验证

建议至少执行：

```bash
cd rain
cmake -B build
cmake --build build
./build/rain_compile_check
```

如果改动了 benchmark：

```bash
cd rain
cmake -B build -DRAIN_BUILD_BENCHMARKS=ON
cmake --build build
bash benchmark/run_bench.sh
```

## 文档贡献

提交文档前请执行：

```bash
cd rain/jekyll
bundle exec jekyll build
```

## 提交建议

- 提交信息保持简短、动词开头。
- 说明改动涉及的模块层级，例如 `async`、`runtime` 或 `net`。
- 如果修改了公开 API，请同步更新 `README`、`README_EN` 和对应文档页面。
- 如果修改了教学章节，请确认侧边栏顺序和阅读地图仍然连贯。

