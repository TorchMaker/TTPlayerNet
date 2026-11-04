# ZIP／RAR 剩余边界、只读确认与归档格式扩展

日期：2026-10-12。延续 [导入及信息读取对照](ARCHIVE_CUE_IMPORT_METADATA_PARITY_20261012.md) 和 [扩展方案](ARCHIVE_7Z_TAR_GZ_PLAN.md)。本轮按原版 5.7.9 的归档流、导入和 CUE Reader 继续核对，再实施明确属于新增能力的格式扩展。

**按用户最新要求，不实现压缩包写回。** 包内音频标签、封面、增益标签及 CUE 均保持只读；原版也没有可用的 ZIP／RAR 成员写回接口。

## 1. 原版是否能写回

### 1.1 伪代码与二进制

原版 EXE SHA256：`c7999ea5823469c1684148cac1bd196009824348ccac5cdd45978f8fb574e28c`。

`004C4984` 构造归档流，安装虚表 `00522680`；`004C4A6D` 从 `004DFDED` 取得共享成员。这个流交给 Reader，而不是磁盘文件的可写流。

| IStream 槽位 | 实际函数 | 二进制返回值 |
| --- | --- | --- |
| Write，槽 4 | `0040F7F0` | `E_NOTIMPL / 0x80004001` |
| SetSize，槽 6 | `0040BE91` | `E_NOTIMPL / 0x80004001` |
| Commit，槽 8 | `00407310` | `E_NOTIMPL / 0x80004001` |

这是直接核对虚表及函数机器码后的结论，不能因为某个输入插件具备标签编辑能力，就推断外层压缩包也可以保存。CUE 的 `004DD69E`／`004E323B` 还有内存来源和逻辑路径属性的只读判定；独立磁盘 CUE 的保存属于另一条路径。

### 1.2 原版实际运行

在独立启动的原版进程中，对 ZIP 和 RAR4 分别打开 `disc\image.wav`，再调用上述接口：

| 系统 | Open | Write | SetSize | Commit |
| --- | --- | --- | --- | --- |
| Windows 11 | S_OK | E_NOTIMPL | E_NOTIMPL | E_NOTIMPL |
| XP SP3 | S_OK | E_NOTIMPL | E_NOTIMPL | E_NOTIMPL |
| Win7 SP1 | S_OK | E_NOTIMPL | E_NOTIMPL | E_NOTIMPL |

本机两份样本调用前后 SHA256 不变。原版 Write 连输出计数都不填写。重建流继续明确返回 `STG_E_ACCESSDENIED` 并将写入计数清零；其只读 Commit 是无操作成功，**不代表写盘**。文件信息助手仍清除写标签能力和封面可写标志，内存／归档 CUE 保存也继续拒绝。

没有增加解包后重打包、直接改归档字节或退出时隐式保存的实现。

## 2. ZIP／RAR 的进一步原版对照

### 2.1 多 FILE CUE

增加同一 CUE 引用两份音频的样本：第一份 3000 ms，第二份 100 ms，TRACK 使用 01、07。原版 ZIP／RAR4 和重建版均得到：

- 两条 CUE 行的内部子曲目序号为 1、2；标题分别来自各 TRACK。
- 导入后时长为 `-2`，实际 Reader 查询后分别为 3000、100 ms。
- 第二个 FILE 的 INDEX 从该音频自身起点计算，不继续使用上一份音频的位置。
- 同目录整文件仍单独入表；有 CUE 不意味着隐藏整轨。

原版实际调用 `00474051` 和 `004ADA76`，Windows 11／XP／Win7 结果一致。重建版进一步比较了两段实际 PCM、末尾裁剪和 Seek，与独立音频相同；有插件和无插件两条路径均覆盖。

### 2.2 保留前轮恢复行为

仍使用原版的成员顺序、批次内去重、字面 CUE FILE 拼接、OEM 名称宽字符比较和当前成员共享任务。旧 ABI 1 的 163 项、ABI 2 的 271 项、宿主状态接口身份与 Clone 上下文生命周期回归均通过。皮肤 ZIP 的一次性读取规则没有随音乐 ZIP 的严格校验一起改变。

RAR 解码继续由静态 UnRAR 7.3.1 承担，不切换到新扩展库。原版归档工厂位于 EXE；**仅给原版播放器替换 ttpcomm.dll，不会让它自动识别 RAR5、7z、TAR 或 gzip**。

## 3. 此次新增能力

| 格式／能力 | 实现及边界 |
| --- | --- |
| TAR | ustar、PAX、GNU 长名称及 UTF-8 路径；支持音频和包内 CUE。跳过链接、设备和稀疏文件，拒绝这些成员的显式打开 |
| gzip | `song.wav.gz` 表示一个逻辑成员 `song.wav`；多个拼接 gzip 流拼成同一成员。读取时校验每段 CRC、ISIZE 和截断 |
| tar.gz／tgz | 作为一个多文件归档处理，CUE 可以引用同包成员；TAR 结束后继续验证 gzip 尾部 |
| 7z | 普通／固实、Copy、LZMA、LZMA2、PPMd；保留库的 BCJ 支持，并补齐 Copy／PPMd 等组合中的 Delta 字节历史 |
| ZIP UTF-8 | 遵守 general purpose bit 11；支持 CRC 与原文件名匹配的 Unicode Path extra `0x7075`。未标记的旧文件名继续使用 OEM |
| ZIP64 | 单卷 ZIP64 目录、计数、长度、成员偏移和本地头；继续使用原音乐 ZIP 的 Stored／Deflate 分块读取及 CRC 校验 |

这些是扩展，不是原版 5.7.9 已有功能。7z／TAR／gzip 不调用系统 Shell、tar.exe、7z.exe 或外部解压程序。测试使用 7-Zip **生成样本**，程序运行不依赖它。

### 3.1 统一接入

`ttpcomm` 增加 `archive_extended.cpp`，使用现有 `Sink::Begin/Write`。各格式共享后台成员任务、独立游标、Clone、取消、当前成员缓存和大成员临时文件。

播放器集中使用 `IsArchivePath`／`ParseArchiveMemberPath`；同步调整文件选择器、导入、播放列表有效性、媒体库有效性及“浏览文件”。信息助手、封面读取、转换和 CUE 音频源继续走已有的归档 IStream。没有只添加扩展名、却让后续读取仍落到普通文件路径。

不做任意嵌套压缩包递归；`.tar.gz` 是一个定义明确的复合格式。

### 3.2 完整性与资源限制

- libarchive 3.8.9 的 gzip 源码仍有尾部校验 TODO。本地适配补齐头 CRC、内容 CRC、ISIZE、拼接流和异常尾部处理；只产出预期字节数不等于成功。
- TAR 可能先遇到结束块，所以选中成员完成后还要验证 gzip 容器剩余部分。校验失败继续通过成员状态传给播放和信息读取。
- 原始 gzip 在完成前不知道总长度。它直接采用临时文件；Read 可以等待已产出的区间，Stat／Seek END 等待真实长度和最终验证。状态总长度在此期间为 `UINT64_MAX`。
- CUE 的 16 MiB **成员**上限不会误用为整个 tar.gz 专辑上限。gzip 容器校验另有预算：tar.gz 为 8 GiB 或调用方更大的成员预算；独立 gzip 仍遵守其成员预算。
- 7z 的目录／临时缓冲分配受 memory_limit 控制，LZMA／PPMd 字典单独受 dictionary_limit 控制；固实前序解压和 gzip 尾部校验均检查取消。
- 默认最多 100000 条记录；ZIP 目录元数据另设 64 MiB 上限。大成员继续使用可删除的磁盘后备，避免 32 位进程同时保存整包和多份音频。
- ZIP64 测试创建 NTFS 稀疏样本，在 **4 GiB 之后的实际文件偏移**读取成员；不是只给小文件改版本标记。

## 4. 依赖、ABI 与 Actions

| 依赖 | 固定版本 | 源码包 SHA256 |
| --- | --- | --- |
| [libarchive](https://github.com/libarchive/libarchive/releases/tag/v3.8.9) | 3.8.9 | `f5a6539059cf5e597dbeda37bfa4874b1e8dea063c8d93bf85a2b44af90a5bd4` |
| [XZ／liblzma](https://github.com/tukaani-project/xz/releases/tag/v5.8.4) | 5.8.4 | `0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9` |

只静态链接所需读取器，关闭外部压缩程序、命令行工具、测试、加密提供者、无关压缩依赖和编码器。上游源码在构建时按固定版本下载，适配仅修改构建目录副本；许可证和版权通知保存在 ttpcomm 仓库，不嵌入 DLL。

实际 DLL 为 632832 字节（618 KiB），较扩展前的 480256 字节（469 KiB）增加 149 KiB；没有新增运行时压缩库 DLL 依赖。

原 67 个导出和现有 4 个扩展导出不变，ABI 1／ABI 2 的函数表布局不变。ABI 2 增加能力位，完整能力掩码为 `511`。新增只读 PE 能力资源，让 Actions 在不执行下载 DLL 的情况下核对这些功能；发行包检查也要求相同掩码。

**发布顺序：先发布此次 ttpcomm，再运行重建版 Actions。** Actions 仍默认取最新正式版；如果服务器上最新 DLL 还是旧归档能力，构建会明确拒绝，而不是产生缺失功能的发行包。本轮只生成本地包，没有执行发布。

## 5. 验证结果

| 检查 | Windows 11 | XP SP3 | Win7 SP1 |
| --- | --- | --- | --- |
| 原版 Write／SetSize／Commit 实测 | E_NOTIMPL | E_NOTIMPL | E_NOTIMPL |
| 原版 ZIP／RAR4 多 FILE CUE | 通过 | 通过 | 通过 |
| 扩展读取、Unicode、CRC、ZIP64、限制、取消、只读 | 2236 项通过 | 2236 项通过 | 2236 项通过 |
| 生产解码、CUE PCM 分段、Seek；有／无插件 | 163 组通过 | 163 组通过 | 163 组通过 |
| 生产导入、读取策略、独立信息助手、标签／封面 | 595 项通过 | 595 项通过 | 595 项通过 |
| 既有 ABI 1／ABI 2 回归 | 163／271 项通过 | 163／271 项通过 | 163／271 项通过 |

专门覆盖：Unicode CUE 引用 Unicode ZIP 成员、gzip 拼接、20 MiB gzip 磁盘后备、大专辑内小 CUE、tar.gz 最后 CRC 损坏、7z 头损坏／内容损坏／加密头、未启用方法、字典超限、成员超限、取消、独立克隆游标、缓存复用、链接／设备／稀疏成员过滤、原版路径分类和 DBCS 名称身份。

测试首次向虚拟机复制目录时形成多一层目录，探针报告缺样本；修正部署目录后通过，没有放宽断言。测试进程均正常结束，没有用“弹窗后手工关闭”代替通过。

证据在私有 `rebuild/tests/archive_support/extensions_20261012/`，以及 `cue_audit_20261012/original-multifile-host.log`、`import-host-run.log`。测试不上传、不加入 Actions、不进入发行包。Windows 11 代表用户指定的现代系统验证环境；没有记为 Windows 10 实机测试。

## 6. 仍有差异与后续顺序

**本节已按后续两轮修复推进。** 密码与后缀选项见 [密码、文件头与后缀提示实施记录](ARCHIVE_PASSWORD_AND_SIGNATURE_20261012.md)；后台导入、可取消进度、有界 CUE 缓存、7z BZip2／Zstandard、旧编码和封面继承见 [最新实施与三系统测试](ARCHIVE_IMPORT_PROGRESS_AND_CODECS_20261012.md)。下方保留最初建议作为历史记录；当前能力掩码为 `4095`，DLL 为 774144 字节，剩余顺序以最新文档第 7 节为准。

1. **继续异常交互对照。** 已覆盖后端 HRESULT、末尾失败、原版流和真实 Reader；没有宣称原版所有异常包的提示文字、出现时机和窗口像素完全一致。
2. **可选密码交互。** 原版宿主没有完成 RAR 密码回调；当前 RAR 服务具备密码参数，但播放器尚无密码对话框。ZIP 加密／AES、7z 加密和加密头不在本次实现范围，会返回明确失败。
3. **更多方法与边界。** 7z BZip2／Zstandard 等未链接的方法明确不支持；TAR 稀疏文件、非 UTF-8 的旧 TAR 名称、分卷 ZIP／7z 暂未支持。ZIP64 已验证跨 4 GiB 偏移，未实际解码单个超过 4 GiB 的音频成员。
4. **大包导入体验。** 导入枚举仍可能同步等待；尤其 tar.gz 为完成 CRC 校验需要读到容器末尾。可另做可取消的导入任务界面，不能把它当成原版已有而尚未恢复的对话框。
5. **辅助文件与特殊 Reader。** 包内 MIDI／CDA 仍保留原版能力边界；未增加自动落盘 MIDI，也没有压缩包内外置歌词／图片的完整关联机制。CUE 元数据不自动继承所引用音频的封面。
6. **缓存优化。** 仍是有边界的当前成员复用；换固实块中的不同成员可能重解压前置数据。分卷整体身份、多成员预取等可另行优化。

压缩包写回已按用户要求排除，不列为下一步工作。

## 7. 本地交付

- 通用播放器：`rebuild/build/Release/TTPlayerRebuild-2026.10.12.zip`。
- 独立公共库：`ttpcomm/build/Release/ttpcomm-2026.10.12.zip`。
- 包内保留 SHA256 清单，播放器包继续包含 EXE、更新器、HTTPS／歌词搜索插件和新 ttpcomm.dll；不加入测试、依赖源码或许可证资源。

本轮完成后的 DLL 大小和包哈希记录在私有 `extensions_20261012/release-verification.json`，并由发行包清单校验，避免文档中的中途构建哈希被误当成最终产物。
