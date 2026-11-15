# Echo Of Void｜空页回响

一个把入口伪装成报错页的短篇浏览器游戏。打开时你看到的是标准的 Apache 404，
中间那个 `0` 可以点。点三下，会有 0.42 秒彻底没有声音的间隔，然后 404 化开，
你掉进「残页域」——被丢弃的页面、失效的链接、没保存的草稿堆成的阈限空间。

全流程不需要后端，不加载任何外部图片或音频，双击 `web/index.html` 就能玩。

## 怎么跑

直接双击 `web/index.html`。`file://` 下也能完整通关。

想用本地服务器：

```
cd web
python -m http.server 8080
```

然后打开 `http://localhost:8080`。

## 操作

| 按键 | 作用 |
|---|---|
| `A` / `D`、`←` / `→` | 左右移动 |
| `空格` / `W` / `↑` | 跳跃 |
| `Shift` 或 `F` | 切换残相 |
| `E` | 交互（读日志、取页芯、开门） |
| `ESC` | 暂停 |

移动端会自动出现四个触摸键（只在 `pointer:coarse` 时显示）。

## 三种残相

玩家没有固定样貌，是一团细碎光斑。切换残相会换掉移动手感、脚步音色和能做的事：

| 残相 | 速度 | 跳跃 | 特性 |
|---|---|---|---|
| 静影 | 150 | 402 | 重，能推动废墟里的货箱压住开关 |
| 流萤 | 205 | 430 | 轻，二段跳 + 短暂悬停，能跨过裂隙 |
| 碎码 | 180 | 386 | 能短暂穿过半透明的损坏屏障 |

同一个机关在不同残相下解法不同，这是三章里主要的谜题来源。

## 三章与两个结局

- 第一章 废弃走廊：教学，第一块页芯
- 第二章 草稿库房：推箱开闸、悬停跨裂隙、穿屏障取第二块页芯
- 第三章 索引废墟：索引之门缺三块页芯，第三块有两条来路

结局 A 远走回归：拿到高台上那块自由漂浮的页芯，把三块嵌进门，穿过去。
结局 B 留守域中：不拿那块，回到库房深处找正在消散的守码者，接纳它的内核。

存档写在 `localStorage`：`eov_endA`、`eov_endB`、`eov_logs`。
通关过任意结局后再进，会直接落在第三章并预置两块页芯，方便补看另一个结局。

## 调试参数

```
web/index.html?fresh=1            清空存档
web/index.html?dbg=1              跳过三次叩击，左上角显示状态读数
web/index.html?form=N             初始残相 0/1/2
web/index.html?frag=N             初始页芯数
web/index.html?spawn=x,y          指定出生坐标
```

状态读数格式：`x= y= f= fr= e= cp= door= end= gl=`。
`gl=1` 表示 WebGL 氛围层生效，`gl=0` 表示退化到 Canvas2D。

`file://` 会缓存脚本，改完文件要加 `&cb=<随便一个新数字>` 才会生效。

## 工程结构

```
web/index.html     单文件游戏：HTML + 内联 CSS + 内联 JS，46 KB
tools/funread.c    7z 归档的只读内存挂载器（纯 C）
echo-of-void.fun   一份 7z 归档，作为 C 侧读取器的验证对象
```

## C 侧：不解压，直接从 .fun 里读

`tools/funread.c` 把 `.fun` 当成只读资源包挂进内存：解析 7z 索引，需要哪个条目才解哪个，
解压结果只存在于堆上，磁盘不会多出任何文件。

支持 `kEncodedHeader`（索引本身被压缩过的情况）、LZMA1 / LZMA2 / Copy / BCJ-x86、
UTF-16LE 文件名、单包流隐式索引。

对外的接口是一套资源加载 API：

```c
int      fun_open(FunArc *arc, const char *path);
uint8_t *fun_read(FunArc *arc, int idx, uint64_t *outLen);
int      fun_find(FunArc *arc, const char *name);
void     fun_close(FunArc *arc);
```

命令行：

```
funread list  <a.fun>                 列出条目
funread check <a.fun>                 逐条解码，打印声明长度/实读长度/CRC32
funread head  <a.fun> <entry> <n>     只解一个条目，输出前 n 字节
funread cat   <a.fun> <entry>         输出整个条目
funread info  <a.fun>                 打印包偏移、folder、coder
```

编译（Windows + Strawberry Perl 自带的 gcc，需要 liblzma）：

```
gcc -O2 -std=c99 -Wall -Wextra -o tools\funread.exe tools\funread.c ^
    -IC:\Strawberry\c\include -LC:\Strawberry\c\lib -llzma
```

Linux / macOS 只需要：

```
gcc -O2 -std=c99 -o funread tools/funread.c -llzma
```

## 零素材

- 图形：地面、雾、残页、光斑、屏幕故障全部由 Canvas2D 现画；氛围层用 WebGL 着色器实时算，
  拿不到 WebGL 时同一层改用 Canvas2D 画，`file://` 下也不会崩
- 音频：脚步、闷响、真空静默、环境底噪全部由 Web Audio 现场合成，没有一个音频文件
- 没有第三方库，没有构建步骤，没有后端

## 参考与差异

这个项目只吸收了两个设计思想：

1. 「用一张 404 报错页当游戏入口，点某个字符三次解锁」的互动概念
2. 程序化生成脚步音效、颗粒噪点纹理、WebGL/Canvas 双渲染降级的工程做法

世界观（残页域 / 页灵 / 守码者 / 消蚀体）、角色体系（三种残相而非四只动物）、
标题、全部文本、美术与音频参数都是这一版自己的。
标题与任何已有作品都不同，也没有沿用别人的角色名或故事线。

## 许可

代码 MIT，见 `LICENSE`。
