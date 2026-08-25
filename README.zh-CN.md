# ferry — 支持 Range 的 HTTP 文件下载服务端 + 客户端

[English](README.md) | 简体中文

基于 [Sogou Workflow](https://github.com/sogou/workflow)（全异步 C++ 框架）
构建、使用 [xmake](https://xmake.io) 构建的文件传输组合：
`ferry-server` 以带上限的 Range 响应提供文件服务；`ferry-client` 是与它配套的
多线程、可断点续传下载器（也可以对接任何符合 RFC 的 Range 服务器）。

特性：

- **HTTP Range 下载**（`206 Partial Content`）：支持断点续传且适合并行。
  每个响应都会暴露完整文件大小 —— 通过任意 206 的
  `Content-Range: bytes s-e/TOTAL`、`HEAD` 的 `Content-Length`，或
  200 的 `Content-Length` —— 便于下载管理器规划和续传。
- **内存有界**：每个响应被截断到一个可配置的上限
  （默认 8 MiB；RFC 9110 允许更短的 range）。大文件以一串带上限的
  range 响应的形式提供；对超过阈值的文件发起非 Range 请求会收到
  带自描述响应体的 `413`。
- **IP 访问控制**：黑名单/白名单，支持 IPv4/IPv6 CIDR 条目，
  黑名单优先，热加载（轮询文件 mtime，无需重启）。
- **按 IP 限速**：令牌桶软整形 —— 超预算的请求在 Workflow series 内部
  延迟（不阻塞任何线程），最多等待配置的等待上限，超过则返回
  `429 + Retry-After`。
- **准入门禁**：全局 QPS、在途请求数和带宽上限保护整个服务端；按 IP 的
  QPS、在途数和带宽配额避免客户端互相挤占。速率门禁先软整形、超限后返回
  `429`；并发门禁立即返回 `503 + Retry-After`。
- **运行统计**：请求/状态/已服务字节总数、各门禁拒绝数、当前/峰值在途数和
  活跃的按 IP bucket，可周期性输出为单行日志，也可用 `SIGUSR1` 按需触发。
- **代理感知的客户端 IP**：真实客户端取自 `X-Forwarded-For` 最右侧
  的条目（由最近的受信代理追加的那个），客户端无法伪造；
  无该头时回退到 socket 对端地址。

服务端源码位于 `server/`，客户端源码位于 `client/`（没有共享库 ——
两者互为对方 Range 逻辑的镜像）。

源码按领域组织：服务端分为 `access/`、`admission/`、`config/`、`file/`、
`http/` 和 `observability/`，客户端分为 `download/`、`http/`、`integrity/`、
`resume/` 和 `ui/`；两端的可执行程序入口均位于各自的 `app/`。`tests/unit/`
和 `tests/integration/` 先按 server/client、再按对应源码领域镜像组织。

## 客户端（ferry-client）

多线程分块下载器：将文件切成固定大小的块（默认 8 MiB），
在工作线程间动态认领，校验并重组，最终输出文件须通过流式 sha256 校验才会落盘。

```bash
xmake run ferry-client -- http://host:8080/path/file.bin
ferry-client -j 8 -o out.bin --checksum sha-256=<hex> <url>
ferry-client --chunk-size 16 --single-stream-limit 512 <url>
```

| 选项 | 默认值 | 含义 |
|---|---|---|
| `-o, --output PATH` | URL 的 basename | 输出文件（数据先写入 `<out>.part`） |
| `-j, --jobs N` | `4` | 工作线程数（实际生效 = min(jobs, 分块数)） |
| `--chunk-size MiB` | `8 MiB` | 单次请求的块大小，以整数 MiB 数量输入 —— 限定每个响应的内存占用 |
| `--checksum sha-256=<hex>` | 关闭 | 期望的摘要；不匹配则保留文件并报错失败 |
| `--no-verify` | 关闭 | 跳过最后的 sha256 校验（否则摘要总是会被计算并打印） |
| `--receive-timeout SEC` | `60` | 单请求超时 —— 必须大于服务端的 `max_wait_sec` |
| `--single-stream-limit MiB` | `256 MiB` | 对不支持 Range 的服务器可接受的最大文件大小，以整数 MiB 数量输入 |
| `-q, --quiet` | 关闭 | 不打印每秒进度行 |

两个 `MiB` 参数都填写不带后缀的整数数量：应写成 `--chunk-size 8`，
而不是 `--chunk-size 8MiB`。客户端会检查溢出并在内部换算为字节。

**断点续传。** 进度持久化在 `<out>.part`（数据）+ `<out>.ferry.json`
（url、大小、Last-Modified、chunk_size、完成位图）中，
每完成一个块后原子重写。重新执行同一条命令即可续传；
会先做 HEAD 检查比较大小和 Last-Modified —— 若文件已变化，
会打印警告、丢弃状态并重新下载。SIGINT 可干净退出并保留可续传状态；
kill -9 也能幸存（最多重下一个已完成的块）。

**服务器兼容性。** 对接支持 Range 的服务器（包括 ferry-server）时，
客户端会自适应服务端施加的任何上限 —— 响应被截断只是意味着多几轮迭代。
对接不支持 Range 的服务器时，回退为受 `--single-stream-limit` 限制的
单流下载（Workflow 会整体缓冲响应，不加限制的回退有 OOM 风险）。
429 响应按 `max(Retry-After, 退避)` 处理；临时错误按指数退避重试
（500 ms × 2^n，上限 30 s，共 8 次）；403/404 会停止所有工作线程。

**两处需要注意的耦合。**（1）`--receive-timeout` 必须大于服务端的
`max_wait_sec`，否则被软整形延迟的响应会被读成超时。
（2）同一个客户端的所有工作线程共享其 IP 的 QPS、并发数和带宽预算。
较大的 `-j` 因而可能触发 `429`/`503` 和退避；并发可以隐藏延迟，
但不能成倍放大按 IP 配额。

## 构建与运行

要求：Linux、C++17 编译器、xmake。Sogou Workflow（v1.0.1）和 gtest
会在首次构建时由 xmake 的包管理器（xrepo）自动拉取并构建。

```bash
xmake                 # 构建 ferry-server、ferry-server-core、unit-test、integration-test
xmake run ferry-server config/server.conf
```

服务端启动时会打印生效的配置，收到 SIGINT/SIGTERM 时优雅退出。

### 配置参考（`config/server.conf`）

扁平的 `key = value` 格式文件；`#` 为注释。非法值会导致启动时直接报错。

`cap_bytes`、`size_threshold_bytes` 和 `rate_bytes_per_sec` 既可以填写十进制
字节数，也可以在整数后添加区分大小写的二进制单位 `B`、`KiB`、`MiB`、
`GiB` 或 `TiB`；整数与单位之间可以有空格。例如 `cap_bytes = 8MiB` 和
`size_threshold_bytes = 16MiB` 分别设置响应上限与非 Range 阈值，
`rate_bytes_per_sec = 10 MiB` 表示每秒 10 MiB。不支持 `MB` 等十进制单位
或小数值。

| 键 | 默认值 | 含义 |
|---|---|---|
| `port` | `8080` | 监听端口 |
| `root` | `.` | 提供文件服务的目录 |
| `file_body_mode` | `pread` | 文件响应路径：稳定的 `pread`，或用于 A/B 测量的实验性 `mmap` |
| `page_cache_policy` | `normal` | 缓冲I/O缓存策略：`normal`、`noreuse`或`drop_after_read`（非normal仅支持`pread`） |
| `cap_bytes` | `8388608` | 每个响应的最大字节数（range 截断上限） |
| `size_threshold_bytes` | = `cap_bytes` | 非 Range 请求超过该值 → `413` |
| `rate_bytes_per_sec` | `0`（关闭） | 按 IP 的带宽限制 |
| `rate_total_bps` | `0`（关闭） | 全服务端带宽限制 |
| `qps_total` | `0`（关闭） | 全服务端请求速率限制 |
| `qps_per_ip` | `0`（关闭） | 按 IP 的请求速率配额 |
| `max_inflight` | `0`（关闭） | 全服务端在途请求数上限 |
| `max_inflight_per_ip` | `0`（关闭） | 按 IP 的在途请求数配额 |
| `max_wait_sec` | `30` | 返回 `429` 前的最大整形延迟 |
| `stats_interval_sec` | `0`（关闭） | 周期性统计日志间隔 |
| `trust_hops` | `1` | 从右往左数第几个 XFF 条目是客户端 |
| `acl_file` | 空（关闭） | ACL 规则文件（热加载） |
| `acl_poll_interval_sec` | `5` | ACL mtime 轮询周期 |
| `max_connections` | `2000` | Workflow 服务端连接数上限 |

### ACL 文件格式

每行一条：`blacklist <ip-or-cidr>` 或 `whitelist <ip-or-cidr>`。
支持 IPv4 和 IPv6；裸 IP 视为 `/32`/`/128`。语义：命中黑名单
一律拒绝（即使同时在白名单中）；白名单非空时，不在其中的
任何 IP 都被拒绝。文件 mtime 变化时重新读取；文件损坏时
继续使用上一份规则。参见 `config/acl.conf`。

## 部署注意事项

### 反向代理要求

服务端信任其前置代理追加的 `X-Forwarded-For` 条目。
请将代理配置为**追加**连接的真实地址，例如 nginx：

```nginx
proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
```

`trust_hops = 1`（默认）时使用**最右侧**的 XFF 条目 ——
即最近的受信代理写入的那个。客户端伪造该头左侧的内容没有任何作用。
若有 N 层级联的受信代理，请设置 `trust_hops = N`。
没有 XFF 头 → 使用 socket 对端地址（直连调试可正常工作）。

### 内存规划

响应在发送前会整体缓冲（Workflow 的服务端模型）。默认 `pread` 模式下，
配置 `max_inflight` 后，匿名响应缓冲的内存峰值受
`max_inflight × cap_bytes` 限制；例如 128 × 8 MiB 约为 1 GiB。
该门禁关闭时，可用 `max_connections × cap_bytes` 作为保守估算。

缓冲式`pread`还会填充Linux page cache。这部分是可回收的cgroup `file`
内存，不计入进程普通`RssAnon`，并可能在请求清空后继续保留。
`page_cache_policy = normal`保持当前有利于重复下载的行为；`noreuse`在读取前
提交`POSIX_FADV_NOREUSE`提示；`drop_after_read`在成功读取并复制到响应缓冲
后，对完整覆盖的页面提交`POSIX_FADV_DONTNEED`。两者都是best effort内核
建议，不是硬缓存配额。一次性大文件可使用`drop_after_read`，热门重复下载
通常应保持`normal`。非normal策略与mmap模式组合会被拒绝；回滚只需恢复
`page_cache_policy = normal`并重启。

总内存硬边界应由cgroup提供。例如8 MiB cap、全局16在途请求对应128 MiB
匿名响应预算，可从以下systemd灰度配置开始：

```ini
[Service]
MemoryHigh=512M
MemoryMax=768M
```

实际数值还要容纳基础RSS、socket、线程栈和同cgroup其他进程。
`MemoryHigh`触发回收/节流；`MemoryMax`过低可能因不可回收内存触发OOM。
禁止把全局`/proc/sys/vm/drop_caches`作为单服务的生产缓存控制。

`file_body_mode = mmap` 是用于测量的实验路径：它只映射本次响应区间，
消除匿名 `malloc + pread` 缓冲，但 Workflow 仍从内存写入 socket，因此不
是 `sendfile` 零拷贝。冷映射页可能在通信路径触发缺页；发送期间截断文件
还可能引发 `SIGBUS`，验证充分前只应用于不可变文件。统计行中的
`mmap_resps`、`mmap_bytes`、`mmap_active`、`mmap_peak` 和
`mmap_fallbacks` 可用于识别是否真正走了 mmap，以及是否发生静默回退。

### 保护与公平性

全局门禁（`qps_total`、`max_inflight`、`rate_total_bps`）保护单个
服务端实例，不受流量来自多少个身份的影响。按 IP 门禁
（`qps_per_ip`、`max_inflight_per_ip`、`rate_bytes_per_sec`）用于公平
分配容量，但 IP 数量增长时不能限制总需求。应先配置全局保护，再按需增加
按 IP 公平配额。全局门禁先于按 IP 门禁执行，因此被拒绝的洪泛流量不会撑大
按 IP 限制器 map。等待整形的请求仍占用在途 slot，设置 `max_inflight` 时
需要为这部分 backlog 留出空间。

### 限速语义

限速是**按服务端实例**的（单实例设计）。多实例横向扩展时，
请使用负载均衡的 IP 亲和（一致性哈希）让每个客户端 IP 固定落在
同一个实例上，或者把令牌桶放到共享存储。空闲 IP 的限速器
状态条目会被自动回收。

### 统计与按需输出

设置 `stats_interval_sec` 后，服务端会按间隔输出一行可解析的统计：

```text
[stats] reqs=120(+20) 2xx=100 404=2 4xx=15 5xx=3 rej(qps_total)=10 ... inflight=4 peak=18 buckets(qps)=7 buckets(bw)=6 served=10485760
```

其中包含累计及区间请求数、各状态分类、六个门禁的拒绝计数、当前/峰值
在途请求、活跃的按 IP bucket 数量、已服务字节、缓存建议调用/接受字节/错误
计数，以及cgroup-v2的`mem_anon`、`mem_file`、`mem_sock`。内存单位为字节，
`-1`表示不可用；`mem_file`是整个cgroup的可回收文件内存，不是单文件归因或
进程RSS，advice字节也不代表等量物理页已被回收。即使周期输出关闭，
也可发送 `SIGUSR1`，要求服务端在一秒内输出一行：

```bash
kill -USR1 <ferry-server-pid>
```

## 测试

共三层（参见设计文档中的测试章节）：

```bash
xmake run unit-test          # L1：纯逻辑，无 sleep（服务端：range/ACL/XFF/限速器/配置/路径；
                             #     客户端：规划器/位图/退避/命令行/进度/探测/sha256）
xmake run integration-test   # L2：服务端套件 + 客户端闭环（ferry handler 提供服务，
                             #     客户端引擎下载：上限、整形、429、续传、致命错误）
tests/system/run_l3.sh       # L3 服务端：curl 驱动（内容校验的 range、热加载、XFF）
tests/system/run_client_l3.sh # L3 客户端：真实二进制（SIGKILL 后续传、校验和门禁、
                             #     python http.server 互操作、无 Range 回退）
tests/stress/run_stress_mmap.sh # 可选：热缓存 pread/mmap A/B 压测
tests/stress/run_stress_cache.sh # 可选：冷缓存策略与重复读取压测
```

AddressSanitizer 运行方式（用于捕捉 nocopy-buffer 生命周期类 bug）：

```bash
xmake f -m debug --asan=y    # 以 ASan + LeakSanitizer 配置
xmake -r                     # 全量重建
xmake run unit-test && xmake run integration-test
xmake f -m release --asan=n  # 恢复正常
```

## 设计文档

需求、决策和任务历史位于
[`openspec/`](openspec/changes/add-range-file-server/)（proposal、design、
specs、tasks）。
