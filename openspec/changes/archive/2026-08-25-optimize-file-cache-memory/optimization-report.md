# 文件下载内存优化报告

## 结论

当前服务没有发现响应缓冲生命周期泄漏。持续不回落的内存主要来自 Linux
page cache；请求进行时的峰值来自`并发数 × 单响应大小`的匿名缓冲，此外
glibc分配器可能保留已经释放的大块内存高水位。实现后的`drop_after_read`
把500 MiB目标文件的空闲驻留从500 MiB降至128 KiB（减少99.98%），同时
通过全部性能门槛。推荐保留`normal`默认策略，对一次性文件节点灰度启用
`drop_after_read`，并配合非零`max_inflight`和部署侧
`MemoryHigh/MemoryMax`。

内存池不是本问题的优先解：在10路、总吞吐10 MiB/s、8 MiB分块下仅约
1.25次大块分配/释放每秒，池化不会消除page cache，反而会让80 MiB以上
匿名内存长期驻留。

## 实测基线

测试日期为2026-08-25，环境为Linux 7.0、release版ferry-server、稳定
`pread`路径。创建磁盘上的冷500 MiB文件，使用32 MiB响应上限，第一批10路
互不重叠Range请求均限制为1 MiB/s，第二批覆盖剩余区间。客户端输出写入
`/dev/null`，同时采样服务`VmRSS/RssAnon/RssFile`、目标文件`fincore`和
cgroup-v2 `memory.stat`。

| 阶段 | 服务RSS | 服务RssAnon | 服务RssFile | 目标文件驻留缓存 |
|---|---:|---:|---:|---:|
| 下载前 | 5.3 MiB | 0.7 MiB | 4.6 MiB | 0 MiB |
| 第一批10路在途 | 325.9 MiB | 321.2 MiB | 4.7 MiB | 320.1 MiB |
| 第一批完成 | 5.9 MiB | 1.2 MiB | 4.7 MiB | 320.1 MiB |
| 完整读取500 MiB后 | 6.0 MiB | 1.3 MiB | 4.7 MiB | 500 MiB |

删除测试文件后，同一cgroup的`file`指标下降约494 MiB；该cgroup还包含其他
进程，因此与目标文件500 MiB存在少量采样噪声。另一个1.706 GiB ISO在没有
ferry进程运行时仍全部驻留，进一步证明文件缓存独立于请求和服务生命周期。

## 实现后验收

### 环境与命令

验收日期为2026-08-25。环境为Ubuntu 26.04 LTS、Linux
7.0.0-30-generic、Intel Core i5-8259U（4核8线程）、4 KiB页、ext4/NVMe；
工作树基于Git提交`6023451`，使用release构建。压测文件由脚本写入并
`fsync`，每次冷读前仅对该文件调用`POSIX_FADV_DONTNEED`，没有使用全局
`drop_caches`。吞吐计时包含客户端并发下载、落盘、合并和SHA-256完整性校验。

```bash
# 变更前基线：从 git archive HEAD 独立构建
xmake f -y -m release --asan=n
xmake -r ferry-server
FILE_SIZE=524288000 CONCURRENCY=10 POLICIES=normal \
  SERVER_BIN_OVERRIDE=/tmp/ferry-baseline-cache.dO1HrH/build/linux/x86_64/release/ferry-server \
  tests/stress/run_stress_cache.sh

# 工作树三策略与门槛校验
FILE_SIZE=524288000 CONCURRENCY=10 \
  BASELINE_RESULT=/tmp/ferry-cache-baseline-result.json \
  tests/stress/run_stress_cache.sh
```

每种策略均完成两次500 MiB、10并发、8 MiB分块的完整下载；所有SHA-256均为
`938244c786988f4b58f11be8f3a0199b2208d8447fed2c3c36f6401606c6705b`，
所有策略空闲采样时`inflight=0`。

### 性能、I/O与驻留结果

| 构建/策略 | 第一次冷读 MiB/s | 第一次 CPU s/GiB | 第一次磁盘读取 | 第二次 MiB/s | 第二次磁盘读取 | 空闲目标驻留 | advice 调用/接受字节/错误 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 变更前 normal | 214.22 | 1.2288 | 500 MiB | 233.53 | 0 | 500 MiB | 不适用 |
| 实现后 normal | 213.22 | 1.2698 | 500 MiB | 230.11 | 0 | 500 MiB | 0 / 0 / 0 |
| 实现后 noreuse | 203.30 | 1.2698 | 500 MiB | 240.20 | 0 | 500 MiB | 126 / 1000 MiB / 0 |
| 实现后 drop_after_read | 198.98 | 1.3517 | 500 MiB | 207.99 | 500.125 MiB | 128 KiB | 126 / 1000 MiB / 0 |

`noreuse`在本机内核上没有降低目标文件驻留，证明它只是替换策略提示，不应
当作缓存上限。`drop_after_read`两次读取后仅剩128 KiB，较normal的500 MiB
减少499.875 MiB（99.98%）；第二次读取重新产生约500 MiB磁盘I/O，明确体现
了牺牲热缓存命中的代价。advice字节为两次读取的累计接受范围，不代表内核
承诺回收的物理字节。

### 门槛结论

| 验收门槛 | 实测 | 结论 |
|---|---:|---|
| normal冷读吞吐回退不超过3% | 0.47% | 通过 |
| normal CPU/GiB回退不超过5% | 3.34% | 通过 |
| drop_after_read冷读吞吐回退不超过10% | 6.68% | 通过 |
| drop_after_read空闲驻留不超过`max(64 MiB, 2 × cap)` | 128 KiB ≤ 64 MiB | 通过 |

### 匿名内存偏差

报告原目标“空闲RssAnon回到基线+16 MiB”未达到：变更前normal空闲
RssAnon为281.7 MiB，实现后normal/noreuse/drop_after_read分别为
321.7/273.7/305.7 MiB，启动基线约0.7 MiB。20秒空闲复测仍为89.3 MiB，
但同时满足`inflight=0`；ASan+LeakSanitizer的202个单元测试和50个真实HTTP
集成测试均无泄漏或生命周期错误。该现象在变更前二进制同样存在，与glibc
大块分配的动态mmap阈值/arena高水位相符，不是本次page-cache策略引入的
回归，也不会被响应内存池消除。生产硬边界仍应使用`max_inflight × cap_bytes`
和cgroup限制；若必须主动压低空闲进程RSS，应另立变更评估固定
`MALLOC_MMAP_THRESHOLD_`、受控`malloc_trim(0)`或替代分配器，并单独验证
吞吐和尾延迟。

本机服务与其他进程共享cgroup，因此实现后空闲`mem_anon`采样为
879–923 MiB、`mem_file`为2.17–2.66 GiB，不能作为单进程或单文件归因；
目标文件`fincore`才是上述驻留门槛的精确口径。

## 回归验证

```bash
xmake run unit-test
xmake run integration-test
tests/system/run_l3.sh
tests/system/run_client_l3.sh

xmake f -y -m debug --asan=y
xmake -r
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 xmake run unit-test
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 xmake run integration-test
xmake f -y -m release --asan=n
xmake -r

STRESS_SECONDS=5 CONCURRENCY=16 tests/stress/run_stress_mmap.sh
```

结果为unit 202/202、integration 50/50、服务端L3 24/24、客户端L3
24/24，ASan/LeakSanitizer同样为202/202与50/50。既有热缓存压测无传输错误、
无mmap fallback；pread为668.74 MiB/s、0.878 CPU s/GiB，mmap为
736.19 MiB/s、0.430 CPU s/GiB。所有测试完成后已恢复release配置并重建。

## 根因与内存模型

`pread`把同一批文件页同时放入两个位置：内核page cache，以及服务拥有的
匿名响应缓冲。后者在HTTP任务完成时释放，前者由Linux按内存压力回收。

```text
峰值内存 ≈ 基础内存
         + inflight × 实际响应长度       （不可回收匿名缓冲）
         + 已读取的唯一文件页             （可回收page cache）
         + TCP socket及内核开销
```

同一文件相同区间被10路读取只产生一份page cache，但10路响应各有自己的匿名
缓冲。当前服务的进程RSS看不到普通`pread`文件缓存，容器/cgroup总内存会看到。

## 优化目标

1. 默认`normal`模式保持现有响应语义，吞吐回退不超过3%，CPU/GiB回退不
   超过5%。
2. `drop_after_read`完成500 MiB冷文件测试并空闲5秒后，目标文件驻留缓存
   不超过`max(64 MiB, 2 × cap_bytes)`；以32 MiB cap计，较500 MiB基线至少
   减少436 MiB（87.2%）。
3. 任意策略下响应状态、Content-Length/Content-Range和内容哈希完全一致；
   10路重叠/非重叠读取均不得产生5xx、截断或错字节。
4. 所有请求完成后`RssAnon`回到基线+16 MiB以内，且
   `inflight = 0`。
5. `mem_anon/mem_file/mem_sock`与同一时刻cgroup-v2 `memory.stat`对应字段
   一致；不可用时输出`-1`，不得伪装为0。
6. `drop_after_read`在受支持Linux环境中`cache_advice_errors = 0`；建议字节
   仅代表成功提交给内核的范围，不宣称等量物理页已经回收。

## 方案比较

| 方案 | 降低持续page cache | 降低匿名峰值 | 主要代价 | 结论 |
|---|---|---|---|---|
| 仅调整监控口径 | 否 | 否 | 无 | 必做，避免误报 |
| `max_inflight × cap_bytes` | 否 | 是 | 超限返回503 | 必做，保护不可回收内存 |
| cgroup `MemoryHigh/Max` | 压力下是 | 限制总量 | 节流/OOM风险 | 必做，作为硬边界 |
| `POSIX_FADV_NOREUSE` | 可能 | 否 | 内核/版本相关 | 低风险canary |
| `POSIX_FADV_DONTNEED` | 是，best effort | 否 | 重复读取增加磁盘IO | 一次性大文件推荐 |
| 内存池 | 否 | 否 | RSS长期高水位 | 不针对当前根因 |
| mmap/sendfile | 否 | 可减少匿名复制 | mmap安全/架构复杂度 | 另立变更评估 |
| `O_DIRECT` | 是 | 否 | 对齐和Range复杂度高 | 暂不采用 |
| 全局`drop_caches` | 是 | 否 | 影响整机所有服务 | 禁止生产使用 |

## 推荐配置与容量规划

当前工作树使用8 MiB响应上限。面向10路下载，可从以下部署样例开始：

```ini
cap_bytes = 8MiB
max_inflight = 16
max_inflight_per_ip = 10
page_cache_policy = normal
stats_interval_sec = 5
```

匿名响应预算为`16 × 8 MiB = 128 MiB`。systemd样例：

```ini
[Service]
MemoryHigh=512M
MemoryMax=768M
```

`MemoryHigh`触发回收/节流，`MemoryMax`只是最终安全线；生产值必须额外容纳
基础RSS、socket、线程栈及其他同cgroup进程。确认工作负载以一次性ISO/安装包
为主后，再把策略从`normal`灰度到`noreuse`，最后按验收结果选择是否使用
`drop_after_read`。

## 验收矩阵

| 测试 | normal | noreuse | drop_after_read |
|---|---|---|---|
| 单Range/短尾Range/空文件完整性 | 必须通过 | 必须通过 | 必须通过 |
| 10路非重叠Range sha256 | 必须通过 | 必须通过 | 必须通过 |
| 10路重叠Range sha256 | 必须通过 | 必须通过 | 必须通过 |
| 500 MiB冷文件结束后缓存阈值 | 记录基线 | 记录、非硬门槛 | 必须达标 |
| 单次冷读吞吐与CPU/GiB | 基准 | 相对基准记录 | 回退不超过10% |
| 同文件第二次热读 | 基准收益 | 记录差异 | 允许变慢并明确报告 |
| advice失败注入 | 不调用 | 响应仍成功且计错 | 响应仍成功且计错 |
| cgroup v2缺失/字段缺失 | 输出-1 | 输出-1 | 输出-1 |

## 上线与回滚

先仅上线观测字段和`normal`默认，确认没有性能回归；随后配置
`max_inflight`与cgroup限制；最后只对一次性下载节点逐级灰度`noreuse`和
`drop_after_read`。出现磁盘延迟、吞吐回退、advice错误或缓存抖动时，将
`page_cache_policy`恢复为`normal`并重启即可，无协议或数据迁移。
