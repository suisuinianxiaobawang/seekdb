# MiniVectorDB

第六届 OceanBase 数据库大赛加赛赛题一。使用 C++20 实现单进程、单客户端数据库。数据库通过标准输入输出交互，数据文件位于 `/data`，运行时无需网络。

[赛题说明](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lg8enxlmwg69unt6)

## 构建与运行

评测镜像的编译器为 GCC 14.3.0，构建镜像固定到清单摘要。最终镜像仅包含静态链接的数据库和数据目录。

```sh
docker build --platform linux/amd64 -t minivectordb .
mkdir -p data
docker run --rm -i --platform linux/amd64 --network none -v "$PWD/data:/data" minivectordb
```

把下面的 SQL 逐行输入。退出后用同一目录重启，再执行最后一条查询，可看到已提交的分组修改和回滚前的向量。

```sql
CREATE TABLE docs (id BIGINT PRIMARY KEY, group_id BIGINT, title VARCHAR(40), embedding VECTOR(2));
INSERT INTO docs VALUES (1, 10, '向量检索', [1, 0]), (2, 20, '持久化', '[0, 1]');
CREATE VECTOR INDEX docs_embedding ON docs(embedding);
SELECT id, title FROM docs WHERE group_id = 10 ORDER BY id;
BEGIN;
UPDATE docs SET group_id = 10 WHERE id = 2;
COMMIT;
BEGIN;
UPDATE docs SET embedding = [5, 5] WHERE id = 1;
ROLLBACK;
SELECT id, L2_DISTANCE(embedding, [0, 0]) AS distance
FROM docs WHERE group_id = 10 ORDER BY distance LIMIT 2;
```

本地构建需要支持 C++20 的编译器。测试脚本使用 Python 3 标准库。

```sh
make
make test
python3 tests/benchmark.py --check 1000
MINIVECTORDB_IMAGE=minivectordb MINIVECTORDB_MEMORY=64m python3 tests/large_storage.py
```

无须容器即可通过 `build/minivectordb --data-dir data` 指定本地目录。同一目录应由一个进程独占使用。

## SQL 和本地协议

- 支持一个 `BIGINT` 主键、`VARCHAR(n)` 和每表最多一个 `VECTOR(n)`。支持多行 `INSERT`、`SELECT` 投影和星号、`UPDATE`、`DELETE`、`AND`、比较运算、单项排序及 `LIMIT`。
- `VARCHAR` 按 Unicode 码点计长。向量转换为有限的 float32 后写入磁盘，计算距离时使用 float64。过滤在 TopK 之前执行，排序并列时按主键升序。
- `BEGIN`、`COMMIT`、`ROLLBACK` 支持事务内读到自己的修改。语句失败只撤销该语句，先前成功的语句仍可提交。
- 本地驱动等待 `READY`，用分号结束 SQL，接收 `OK<TAB>影响行数`、`ERROR<TAB>错误码<TAB>描述`，或 `RESULT<TAB>行数<TAB>列数` 加标题行和结果行。制表符、换行、回车、NUL 及反斜杠在字段中转义。EOF 和 `.quit` 可正常退出。调试输出使用标准错误。
- 截至本次验证，公开分支只有构建和材料占位文件。以上是本地自测协议。官方驱动的语句边界、错误码、就绪和模式命令仍须用正式题包核对。

## 存储和检索

`records.bin` 是追加文件。事务头存放数据长度及其按位取反值。程序先写完数据，再发布事务头并同步文件，随后才返回成功。能放入缓冲区的事务将事务头和数据合并写入。重启时只回放完整事务，截断未完成的文件尾部。语句失败回到该语句开始前的位置，未确认的事务在正常退出时回滚。

内存里的主键表只记录主键和磁盘位置。显式事务用单独的变更表覆盖这些位置。行数据按需访问，普通查询逐行输出。主键范围过滤直接定位起止位置，主键升序和降序都可按索引顺序读出。已有日志持续增长，当前版本没有后台压缩。无 `LIMIT` 的排序需要保留匹配记录的位置和排序值。

向量索引保留按文件位置排列的记录引用，不复制向量。小表可为一个等值过滤列构造候选分组。修改后索引失效，下次查询按需重建。显式事务、大表和其他过滤条件使用精确扫描。两条执行路径都用固定大小的堆保留前 K 个结果。当前 `.mode ann` 返回同样的精确结果，近似搜索算法尚未实现。性能配置采用 exact。

## 八轮优化与验证

同一台 macOS 机器，10,000 行、128 维、1,000 次查询，其中一半带等值过滤。每轮独立运行三次，表内为耗时中位数。记录文件为 5,371,708 字节。耗时仅用于本地比较，文件大小不是平台统计的物理 I/O。

| 轮次 | 改动 | 写入毫秒 | 查询毫秒 |
| --- | --- | ---: | ---: |
| 初版 | 逐行追加，直接扫描 | 1800.51 | 514.99 |
| 1 | 缓冲追加记录，减少写调用 | 1717.35 | 512.33 |
| 2 | 去掉 SQL 整行复制和常见数字临时堆分配 | 1708.89 | 516.90 |
| 3 | 用 NEON 和 AVX 计算距离，保留标量实现 | 1700.53 | 504.25 |
| 4 | 用平方距离排序，输出时复用距离结果 | 1731.14 | 502.50 |
| 5 | 为有索引的小表缓存扫描顺序和等值候选 | 1762.07 | 424.13 |
| 6 | 逐行输出结果，主键升序查询无需排序 | 1748.81 | 422.49 |
| 7 | 索引直接定位向量字节，跳过逐行解码 | 1752.47 | 388.30 |
| 8 | 将可放入缓冲区的事务头和内容合并写入 | 1741.63 | 388.12 |

Linux x86_64 镜像禁用网络，并在 2 CPU、64 MiB 内存、禁用交换的环境中通过 10 项回归。大数据测试写入 12,000 条 2,048 维向量，文件约 94 MiB，强制终止后恢复全部已提交记录并得到相同的过滤 TopK。该环境通过 Rosetta 运行，性能耗时不能用于估算赛场排名。

## 第 9 至 13 轮

| 轮次 | 实验与处理 |
| --- | --- |
| 9 | 记录向量 TopK 堆当前最差候选的距离和主键，先排除无法进入结果的候选。 |
| 10 | 尝试按距离下界提前结束，128 维基准上查询变慢，撤回。改为直接解析完整、无引号和注释的单行 SQL，保留原有多行处理。 |
| 11 | 批量写入复用向量解析缓冲区，常见的单个数字直接转换。 |
| 12 | 用主键范围缩小扫描集合，按主键降序直接遍历，合并事务内变更。 |
| 13 | 更新时直接使用已有向量和字符串的字节，复用行编码缓冲区。 |

以下是本轮在同一台 macOS 机器上重测的三次中位数。标准基准仍使用 10,000 行、128 维和 1,000 次查询。专项基准使用 10,000 行、512 维整数向量和 256 字符的文本列，并检查范围查询和更新结果。

| 耗时 | 第 8 轮重测 | 第 13 轮 |
| --- | ---: | ---: |
| 标准批量写入 | 1675.59 ms | 1610.76 ms |
| 标准 TopK 查询 | 375.07 ms | 343.18 ms |
| 整数向量导入 | 954.88 ms | 901.31 ms |
| 1,000 次窄主键范围查询 | 213.94 ms | 18.65 ms |
| 一次全表标量更新 | 12.14 ms | 8.48 ms |

```sh
python3 tests/benchmark.py --rounds 3
python3 tests/benchmark.py --maintenance --dimensions 512 --rounds 3
```

第 13 轮的 Linux x86_64 镜像通过 12 项回归，限内存恢复测试复现了第 8 轮的结果。
