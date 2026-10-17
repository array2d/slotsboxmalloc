# 性能优化报告 (v3)

对象: `include/slotsboxmalloc/slotsboxobj.h`（唯一的实现文件）。
环境: Intel Xeon w7-3455 (48 核), gcc 13.2, `-O3 -DNDEBUG`（与 CMake Release 相同）,
经共享库链接调用。数据区 128MB (`8*64^4`), meta 16MB。
所有数字为 3 次运行中位数; 吞吐按**成功**操作计数, 失败率单独列出。

## 基线 (v2) 的四个问题

1. **多线程假失败**: 128MB 数据区在 v2 只有一个 root slot（一把锁）, 且
   `sbo_alloc` 用 trylock, 抢不到锁直接返回 -1。8 线程时 94.5% 的分配
   在空间充足时失败（仓库自带 bench 只数成功数, 掩盖了这一点）。
2. **填充二次退化**: alloc-only 保持存活的场景下, 每次分配从下标 0
   线性扫描子 box 并逐个解引用, 50k 次分配平均 47µs/op 且随填充恶化。
3. **逐位扫描**: free bitmap 是 `uint8_t[8]`, 连续最大空闲、找连续 run、
   置/清位全是逐 bit 循环; `update_parent` 每次容量变化都重扫全部子
   box 并解引用各自指针; `packed` 结构体使每次成员访问都是不对齐访问。
4. **潜在越界**: `sbo_alloc_obj_slots` 在 `start==0` 时
   `obj_boundary = start-1` 下溢为 255, 随后 `sbo_alloc_box_slot` 会
   扫描并写入 slot 64 以外的内存（基线在压测中复现过段错误, 8 线程
   模型检查出现过 `allocated_size=0`）。

## v3 的改动

- `free_mask` 改为单个 `uint64_t`: 连续最大（按 run 跳跃的 ctz 循环）、
  找 ≥m 连续位（二进制分解的词运算）、置/清位都是一两条指令。
- slot 状态改为一字节, `OBJ_START` 字节内嵌对象长度: free 与
  `allocated_size` O(1) 取长, 不再逐 slot 走 `OBJ_CONT`。
- 父 box 缓存每个子 box 的 combined 容量（uint16 编码 `(level<<6)|mult`）,
  下降时先比缓存、命中才解引用子 box；最大值用“持有者计数 + child_mask
  定向重扫”精确维护, 沿父链每层 O(1) 传播。
- 每 box 加 `hint_child` roving 提示 + `child_mask` 只遍历现有子 box,
  填充场景恢复 O(depth)。
- 热路径去开销: magic 用两个 64 位字比较完整 16 字节,
  `sbo_align_to` 用 clz 一次算出（原循环试除）, shard 编号用移位
  （原 64 位除法）, `parent`/self id 直接存字段（原每次建 box 做除法反算）。
- 锁改为 TTAS + acquire/release（原 seq_cst CAS 忙等 + seq_cst store）。
- **分片**: 数据区 ≥ 64×2MB 时切为 64 个独立 shard（各自锁 + blockmalloc
  池 + 树根）, 线程用 TLS 亲和 home shard；`sbo_alloc` 先 trylock 轮转、
  再阻塞轮转, 不再因竞争误报 OOM。shard 边界与 6-bit 树解码对齐,
  free 仍可由 offset 直接定位。
- box 经填充保证恒 8 字节对齐（块头 2/4 字节时加 pad, 步长取 8 的倍数）,
  去掉 `packed` 的不对齐访问；ASan/UBSan 干净。

代价/兼容性:

- meta 格式不兼容 v2（magic 升级为 `slotsboxobj64v3`）, 需重新 `sbo_init`。
- 单个 box 元数据 343B → 480B（多了 child_cap 缓存等）, 相同 meta 区可放
  的 box 数约减少 29%。
- 单次分配上限约为 shard 大小的 63/64（128MB 数据区 → 约 1.97MB）,
  这是 v2“根 box 63 个 slot”同一规则在分片后的形式；可用
  `SBO_MIN_SHARD_BYTES` 调大 shard 下限换取更大单次上限（分片数随之减少）。
- 极小数据区（<512B）现在 `sbo_init` 直接拒绝（v2 接受但根 level 为 0
  实际不可用）；meta 区要求 8 字节对齐。

## 结果（自研严格基准, 成功吞吐, 中位数）

alloc+free 混合尺寸 {8,64,512,4096}B, 每线程 200k 次:

| 线程 | v2 Mops/s | v2 失败率 | v3 Mops/s | 提升 |
|---:|---:|---:|---:|---:|
| 1 | 3.28 | 0% | 11.94 | 3.6× |
| 4 | 1.13 | 90.8% | 45.20 | 40.1× |
| 8 | 0.68 | 94.4% | 99.61 | 146× |
| 16 | 0.32 | 97.4% | 199.80 | 626× |

其它场景（1 线程 / 16 线程）:

- 小尺寸 {8..64}B alloc+free: 3.55 → 12.49 Mops/s（3.5×）/ 0.43 → 192.96（452×）
- 随机 churn（2048 存活集, 8B–64KB）: 1.94 → 10.09（5.2×）/ 0.93 → 67.95（73×）
- alloc-only 填充 50k 不释放: 0.022 → 7.71 Mops/s（350×, 消除二次退化）
- v3 在所有多线程场景失败率 0（空间充足时）。

仓库自带基准抽查（单次）:

- `08_bench_pure`: 单线程 v3 8.08M ops/s; 8 线程全部完成 400000 次,
  62.1M ops/s（v2 此前测得仅完成 45300/400000 次, 1.17M ops/s）。
- `16_bench_malloc`: v3 单线程 14.5M、8 线程 104.5M ops/s
  （同机 glibc 为 46.2M / 240.1M ops/s）。

## 正确性验证

- 新增 `tutorial/18_model_check.c`（ctest 以 `4 10000` 运行）: 随机 churn
  下用逐 8B 单位的 claim 表验证分配永不重叠、首尾图案验证存活对象不被
  disturb、`sbo_allocated_size` 恒等于模型容量, 且多线程空间充足时零假失败;
  另含溢出尺寸、shard 上限、未对齐/双重 free、错误 magic、小数据区与
  单 shard 多线程检查。该测试在 v2 上稳定 FAIL（40 万次中约 37 万次假
  失败）, 在 v3 上 PASS。
- 现有 tutorial 01/02/04/05/06/17 全部通过（04/05/06 连跑 5 轮）;
  ASan+UBSan（`-fno-sanitize-recover=all`）干净。

## 复现

```bash
make            # 需 cmake；或手动:
gcc -O3 -DNDEBUG -fPIC -shared -std=c11 -Iinclude -I<blockmalloc>/include \
    src/slotsboxmalloc.c -L<blockmalloc>/build -lblockmalloc -o libslotsboxmalloc.so
./build/tutorial/08_bench_pure 8
./build/tutorial/16_bench_malloc
./build/tutorial/18_model_check 8 100000
```
