# 项目约定

## 构建与测试节奏

小改动**不要**跑全量回归，按改动范围选择最小测试集：

| 时机 | 范围 | 命令 |
|---|---|---|
| 改完一个点（单文件/单函数） | 直接相关用例 | `python tests/run_tests.py --filter <关键字>`；或 `build\muc.exe check <file.mu>` |
| 改完一个特性的一组改动 | 该特性相关用例集 | 例如可见性：`python tests/run_tests.py --filter vis` |
| 阶段收尾 / 版本号变更 / 提交前 | 全量 | `python tests/run_tests.py` |
| 冷编译零 warning | 仅阶段收尾 | `python -m cmake --build build --clean-first` |

每次改完代码都要构建，但构建通过后只跑相关用例：

```powershell
python -m cmake --build build
```

全量基线（0.0.57）：289/289（70 成功 + 206 报错 + 13 check）。

## 报错用例的辅助 unit

`tests/errors/` 和 `tests/check/` 中以 `lib_` 开头的 `.mu` 是多 unit 用例的依赖库，
运行器不会把它们当作独立用例。带点号 unit 名的辅助文件放在子目录（如 `tests/errors/vislib/secret.mu`），
不会被 `*.mu` glob 收集。

## 文档

- 语言规范：`SPEC.md`（逐条标注已定 / 建议 / 待定）
- 设计待定项：`docs/design-backlog.md`
- 可见性：`docs/visibility-design.md`
- 版本记录：`docs/versions.md`（`VERSION` 是唯一版本来源）
