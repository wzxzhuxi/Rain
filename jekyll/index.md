---
title: Rain
description: 面向可理解性的 C++23 异步运行时
permalink: /
---

<section class="home-hero">
  <p class="home-kicker">C++23 异步运行时</p>
  <h1>Rain</h1>
  <p class="home-tagline">每核事件循环、协程任务、epoll reactor、层次时间轮与阻塞桥接组成的轻量运行时库。</p>
  <div class="home-actions">
    <a class="btn brand" href="{{ '/install-and-usage/' | relative_url }}">安装与使用</a>
    <a class="btn alt" href="{{ '/api/' | relative_url }}">API 文档</a>
    <a class="btn alt" href="{{ '/reading-map/' | relative_url }}">阅读地图</a>
  </div>
  <div class="home-hero-addon" aria-hidden="true">
    <img class="home-logo-mark" src="{{ site.logo | relative_url }}" alt="">
  </div>
</section>

<ul class="home-points">
  <li>文档覆盖安装与使用、API 文档、子库说明、阅读地图、基准测试与贡献说明。</li>
  <li>源码结构收敛为 <code>core/sync/async/runtime/net</code> 五层，依赖方向严格自上而下。</li>
  <li>设计目标是可被理解：每个组件都控制在可读范围内，适合逐文件拆解学习。</li>
</ul>
