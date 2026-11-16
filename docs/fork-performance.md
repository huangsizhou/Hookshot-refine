# This fork: frame rate on Windows

A fork of [NotRexed/Hookshot](https://github.com/NotRexed/Hookshot) at `51da046`, with one set of changes: the frame rate
of the built game on Windows. This page says what limits a frame, what was changed, what was measured, and what was not
tested. A Chinese summary is at the end (文末有中文摘要).

**Result:** about +9 fps at 3840x2160 in the heaviest scene measured (52.7 to 61.7 fps, ten interleaved pairs of runs),
+5 to +9 in three other places, +3 to +7 in a light scene. The goal was +10 and it was not reached. No more flashes than
before in the frame log. Only one PC was measured.

## What limits a frame

Found with `HOS_CHAIN` + `scripts/chain.py`, `HOS_SAMPLE_ALL` and `RX_VK_PASS_TIMING` in Kokiri Forest (about 2,450
draws, Ryzen 7 5800X, RTX 5070 Ti, 3840x2160, frame generation off):

1. **The replay is one thread.** The Presentation Thread turns the frame's commands into Vulkan calls for about 16 ms.
2. **The game's own loop is as long.** The main thread and its job threads take about 18 ms a frame, 4 ms of that the
   renderer's work on the main thread's submits (the capture of written pages, a wait for the GPU). The two run side by
   side, so a faster replay alone gains little: the game's loop is then what the frame waits for.
3. **The game's next frame reads what the GPU wrote in this one.** The exposure meter (15 KB) and several larger lists
   are read back every frame, and the host makes the reader wait until the GPU has run the work that writes them. The
   faster the replay, the sooner that work is recorded and the longer the reader waits for the GPU. This is why
   `RX_VK_PARALLEL` on its own was no faster with three threads than with two, and slower than serial in light scenes.
4. **The GPU copies the captures over the bus.** 20 MB of captured pages a frame went from host memory to the mirrors:
   3.4 ms of the GPU's ~13 ms.

## What was changed

Each has a switch that turns it off (`docs/development.md`), except the last three, which change no behaviour.

| Change | Files | Off with |
|---|---|---|
| The parallel replay is on by default: three recording threads where the PC has 12 logical processors or more | `gpu/vk/vk_replay.cpp` | `RX_VK_PARALLEL=0` |
| A command buffer is recorded in up to 12 segments of ~80 draws (were 6 of ~160): the GPU has the first one sooner | `gpu/vk/vk_replay.cpp` | `RX_VK_PARALLEL_SEGS=6`, `RX_VK_PARALLEL_SEG=160` |
| Each segment, and the recording before the segments, copies back what it wrote at its own end and is on the recordings' timeline as a part | `gpu/vk/vk_replay.cpp`, `vk_memory.cpp`, `vk_internal.h` | `RX_VK_PARALLEL_SEG_WB=0` |
| The captures' buffers are in the card's own CPU-writable memory (device-local and host-visible), in host memory when they do not fit | `gpu/vk/vk_memory.cpp`, `vk_vma.cpp`, `vk_internal.h` | `RX_VK_CAP_DEVICE=0` |
| A readback of a range larger than 64 KB is not waited for | `gpu/vk/vk_replay.cpp` | `RX_VK_READBACK_WAIT_MAX_KB=0` |
| The presenting thread is first in line for a virtual core | `host/hos_thread.c`, `host/nvn_native.c` | `HOS_VCORE_PRESENTER=0` |
| A thread looks for its virtual core for 25 us before it sleeps | `host/hos_thread.c` | `HOS_VCORE_SPIN_US=0` |
| The pools' capture ranges are not built again while the list of pools is the same | `gpu/vk/vk_memory.cpp` | |
| The copy helpers are used only for captures of 4 MB or more | `gpu/vk/vk_memory.cpp` | |
| The write marks are scanned 256 pages at a time while none is marked | `host/rx_backend.c` | |

The one that matters most is the write-back of the recording before the segments. It was left for the recording after
the last segment, so the exposure meter's readback waited for the GPU to run every segment: 4.5 ms a frame at about 750
draws, which made the parallel replay 13 fps slower there than the serial one.

## What was measured

One PC: Ryzen 7 5800X (8 cores, 16 threads), 64 GB, RTX 5070 Ti 16 GB, Windows 11, 3840x2160 output, a 1280x720 window,
frame generation off, V-Sync off (`RX_VK_IMMEDIATE=1`). The original code and this branch were built with the same
toolchain and run by turns; fps is the renderer's "N presents, X fps" line, standing still.

| Where | Draws | Original | This branch |
|---|---|---|---|
| Kokiri Forest, the village, ten pairs (mean) | ~2,450 | 52.7 | 61.7 |
| Kokiri Forest, the village, the last four pairs | ~2,450 | 52.6, 54.1, 52.7, 54.2 | 61.5, 63.9, 61.7, 61.8 |
| Kokiri Forest, by the chest, two pairs | ~750 | 74.5, 77.7 | 81.2, 81.2 |
| Kakariko Village (debug teleport) | ~2,540 | 57.9, 60.7 | 66.2, 69.1 |
| Castle Town market (debug teleport), one pair | ~2,835 | 50.2 | 55.7 |
| Hyrule Field (debug teleport) | ~1,260 | 74.9 | 74.9, 80.3 |

Flashes (`RX_VK_FRAMELOG` + `scripts/framelog.py`): none standing in Kokiri Forest, 30 s each, in either build. Walking a
fixed stick sequence for about 25 s, events counted (those over ten blocks in brackets): Kakariko Village 164 (4) and
44 (0) original against 130 (1) and 73 (0); the market 132 (4) against 56 (2); Hyrule Field 65 (0) against 58 (0) and
56 (0). The counts include a map load on the way and vary a lot from run to run: they show nothing got worse, not that
anything got better.

`python -I -B tests/run_all.py`: the same tests fail before and after (the ones that need a compiler this PC lacks).

## What was not tested, and the risks

- **The parallel replay was off upstream** because a walk through Kakariko showed one-frame black scenes; one cause was
  fixed since and that walk was not run again. Here a walk in Kakariko, the market and Hyrule Field showed no more flashes
  than the original, but it is not the same route, each place was run once or twice, and no dungeon and no adult map was
  run. `RX_VK_PARALLEL=0` where a place shows black frames.
- **The 64 KB rule is a rule of thumb.** The game may read a large list one frame older than before. Nothing showed in
  the places above.
- A card without Resizable BAR has 256 MB of CPU-writable memory: the fallback to host memory is there and was not run.
- A PC with fewer than 12 logical processors keeps the serial replay; not run.
- AMD and Intel cards, and Linux, were not run.

## Tried and taken out

Late pages left for the next capture (slower: the game then polled for the GPU itself); pages left out of a capture when
their hash is unchanged (53 % of them are, but hashing costs what the copy saves); the capture's late wait by part; the
pools' syncs in a recording of their own; copy helpers that stay awake (the copy is bound by the write to the card's
memory, ~14 GB/s); the replaying thread recording a segment too; a per-thread rule for leaving out repeated full barriers
in a parallel replay (139 fewer a frame, the same frame rate); two reserved cores; four threads; 14 segments;
`HOS_CORES=8`; `RX_VK_ASYNC=1` (37-40 fps). `RX_VK_NO_READBACK_WAIT=1` reaches 62 fps and brings flashes back;
`HOS_VCORES=0` is 2.6 fps faster and breaks the game's per-core data.

## 中文摘要

这是 [NotRexed/Hookshot](https://github.com/NotRexed/Hookshot) 在 `51da046` 上的一个分叉，只做了一件事：提升 Windows
上编译后游戏的帧率。

**结果：** 4K 输出下最重的测试场景约 +9 帧（52.7 到 61.7，10 组交替对比），另外三个地图 +5 到 +9，轻场景 +3 到 +7。
目标是 +10，没有达到。帧日志里的闪烁没有变多。只在一台电脑上测过。

**瓶颈：**

1. 渲染重放是单线程，每帧约 16 ms。
2. 游戏自己的逻辑循环一样长，约 18 ms，其中约 4 ms 是渲染器压在主线程上的活。两者并行，所以只加快重放收益很小。
3. 游戏下一帧要读 GPU 这一帧写出的结果（15 KB 的曝光测光缓冲和几块更大的列表），宿主会让读的线程等 GPU 做完。重放越快，
   等得越久。这就是原来的多线程重放开三个线程不比两个快、轻场景还比单线程慢的原因。
4. 每帧 20 MB 的捕获数据要由 GPU 从系统内存搬进显存，占 GPU 约 13 ms 里的 3.4 ms。

**改动：** 多线程重放默认开启（逻辑核 ≥ 12 时 3 线程）并切成更细的分段；每个分段以及分段之前那段录制各自回写结果
（最关键的一处）；捕获缓冲直接放进显卡可写的显存；大于 64 KB 的读回不再等 GPU；呈现线程优先拿虚拟核，等核时先自旋
25 微秒；另有三处不改变行为的重复劳动被去掉。除最后三处外都有环境变量可以关，见上表。

**风险：** 上游默认关闭多线程重放是因为卡卡利科村出现过单帧黑屏。这里用调试传送在卡卡利科村、城镇、海拉尔平原各走了
一段，没有比原始代码更多的闪烁，但路线不一定相同，迷宫和成年林克的地图没测，AMD、Intel 显卡和 Linux 也没测。遇到黑屏
用 `RX_VK_PARALLEL=0` 关掉。
