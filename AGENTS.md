# AGENTS.md

## 项目用途
- C 语言 DWG 解析/转换项目，当前主要关注将 DWG 文件转换为简化 GeoJSON 文本输出。
- 当前 GeoJSON 目标格式为按图层聚合文字：
  `[{ "Layer": "...", "Text": ["..."] }]`
- README 未提供；其他完整项目目标不确定。

## 技术栈
- C99。
- CMake 构建。
- Windows/MSVC 可用；CMake 中强制 Visual Studio 生成器使用 x64。
- 可选 iconv 用于旧 codepage 文本转 UTF-8。

## 目录结构
- `src/`：核心 DWG 解码、对象模型、输出和工具代码。
- `src/include/`：公开头文件，包含 `dwg.h`。
- `test/`：命令行入口和辅助测试/诊断脚本。
- `build/`：CMake 构建目录，不视为源码。

## 入口文件
- 命令行程序入口：`test/dwgread.c`。
- DWG 读取入口：`src/dwg.c` 中的 `dwg_read_file()`。
- R2007/AC1021 解码入口：`src/decode_r2007.c`。
- GeoJSON 文件输出入口：`src/out_geojson.c` 中的 `dwg_write_geojson()`。
- GeoJSON 图层文字聚合入口：`src/geojson_api.c` 中的 `dwg_geojson_layers_text()`。

## 构建、运行、测试命令
- 编译：
  `cmake --build . --config Debug`
  需在 `build/` 目录运行。
- DWG 转 GeoJSON 示例：
  `build\Debug\dwg2text.exe -O GeoJSON -o D:\dwg\1.geojson D:\dwg\1.dwg`
  需在项目根目录运行。
- 文本清单对照：
  `python test\compare_geojson_text.py D:\dwg\1all.txt D:\dwg\1.geojson`

## 代码风格
- 保持项目现有 C 风格和命名。
- 优先做最小、可验证、低风险修改。
- 不随意引入新依赖、新架构或大范围重构。
- 手工修改文件优先使用补丁方式。

## 核心流程
- `dwg2text` 调用 `dwg_read_file()` 读取 DWG。
- R2007/AC1021 文件主要经过 `decode_r2007.c` 的 section、object map、object 解码流程。
- 输出 GeoJSON 时，`dwg_write_geojson()` 调用 `dwg_geojson_layers_text()` 生成按图层聚合的文字 JSON。
- 文本编码处理需要区分 UTF-16LE/TU、旧 codepage/TV 和 raw/proxy 字节串。

## 常见 bug 定位入口
- AC1021/R2007 对象缺失、handle/object map 异常：`src/decode_r2007.c`。
- 图层识别、图层文字归属、块内文字遍历：`src/geojson_api.c`。
- GeoJSON 文件写出格式：`src/out_geojson.c`。
- 文字编码转换：`src/bits.c`、`src/codepages.c`、`src/geojson_api.c`。
- 对象释放和新增字段内存管理：`src/free.c`、`src/include/dwg.h`。
- AC1021/R2007 文字缺失需同时检查 object map、对象 string stream、EED/XDATA/proxy/unknown raw、STYLE/SHX bigfont，不能只在输出层补文本。
- raw 恢复必须保留 object handle、object address、layer handle、source/encoding 等来源信息；不要做无来源的全局去重。
- `D:\dwg\1all.txt` 可作为当前大样本正向验证基准，但不是生产过滤白名单；额外文字应诊断报告后再确认。
- CAD 查看器/浩辰 CAD 结果可作为 oracle；优先记录缺失文字的 handle、图层、对象类型、样式和坐标。
- `S=数字` 缺失可能来自 TEXT 缓存、代理/私有数据或闭合 LWPOLYLINE 面积；不要直接把所有闭合多段线面积输出成 `S=`，需先证明对象来源和过滤规则，否则会产生大量伪文本。
- 图层名即文字的 fallback 不应把 `HATCH` 填充面当作文字载体；全对象 raw 扫描只能作为有上限的诊断手段，不要默认启用。

## 禁止随意修改的内容
- 不要写死本地样本路径、图层名或具体文字内容。
- 不要删除用户或其他工具已有改动，除非用户明确要求。
- 不要把诊断样本清单当作永久白名单过滤真实 DWG 内容。
- 不要为了提高召回率无边界扫描大对象区，避免大文件转换超时。

## 修改后的验证方法
- 先编译确认无错误。
- 用小 DWG 或现有样例确认输出格式未破坏。
- 对大样本可运行 DWG 转 GeoJSON，并用 `test\compare_geojson_text.py` 对照可信文本清单。
- 重点检查 JSON 可解析、图层数量、`MJZJ`、`S=` 出现次数、中文文本、以及是否出现派生/虚假文本。
- 单个验证命令超过 8 分钟时先通知用户并建议是否停掉或继续，不要无提示长时间等待。
