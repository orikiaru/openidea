# 待办导航与复现入口

> 近期五组行动与当前非全绿边界见根目录 [活动待办](<../todo.md>)；其他独立未完成事项、延期理由与提级条件统一在根目录 [条件待办](<../todonext.md>)。本页只保留导航和原锚点，避免维护第二套排期。推迟不等于不存在或已修，未确认疑点也没有因迁移关闭。
>
> 已完成、已核对与已接受策略见 [归档](<fixed.md#2026-11-25-状态归档完成不等于最终四路全绿>)；[11-24 review](<review-20261124.md>) / [11-25 review](<review-20261125.md>)是完整历史证据，状态为审查当时快照。

## 唤醒路径延迟再评估（条件待办）

已迁到 [条件待办 E：Blocking 唤醒开销](<../todonext.md#e-性能与未来优化>)；仅真实业务证明瓶颈才重评，不将接受开销写成已优化。历史测量及既定策略仍见 [归档](<fixed.md#已接受策略blocking-暂不优化>)。

## 尚未证实的疑点

UDP ICMP/poll、旧 Windows ST admission、helper 120、停服 EBUSY、注册失败 fd 复用等均迁到 [条件待办 B：复现证据](<../todonext.md#b-需要抓到复现证据>)；旧 admission 的完整证据仍在 [故障记录](<windows-st-admission-20261121.md>)。无匹配 PDB/dump/二进制身份不能归因或用后来通过关闭。

## Redis 迟到回包偶发问题：未确认，等待复现

完整历史时间线、`max_pending` 永不回包风险与复现/关闭要求统一迁到 [Redis 观察时间线](<../todonext.md#hold-的-redis-late-ping-偶发超时>)。继续按 [测试规程](<testing.md>)运行 `--case hold-redis`，指定 `--artifacts` / `--redis-timeline`；不能放宽断言或以一次通过关闭，MONITOR 执行记录不等于回包到达证据。

## 2026-11-21 分诊结论与排期

已修复与已定策略见 [已完成记录](<fixed.md>)；未完成的三轮重复项已合并到 [近期行动](<../todo.md>) / [条件待办 A–F](<../todonext.md>)，按当前证据排期，不沿用旧 review 的建议顺序。
