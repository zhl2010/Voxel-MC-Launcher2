use crate::error::VoxelError;
use crate::model::*;

/// 合并 inheritsFrom 并求值 rules。
///
/// 合并语义（下一条消息会给出逐字段覆盖表，这里是签名占位）：
/// - id          : 取 child
/// - main_class  : child 有则用 child，否则用 parent
/// - libraries   : parent 在前、child 在后按坐标去重（child 覆盖同坐标项）
/// - arguments   : 按 group 归并；conditional 项保留 rules，求值推迟到渲染阶段
///
/// # Errors
/// 继承链断裂、父版本缺失字段、坐标非法时返回 `VoxelError::Meta`。
pub fn resolve_inheritance(
    child: &VersionJson,
    parent: Option<&VersionJson>,
    env: &EnvContext,
) -> Result<ResolvedVersion, VoxelError> {
    let _ = (child, parent, env);
    todo!("Vol.2 实现：逐字段合并 + rules 求值")
}

/// 判断一组 rules 在当前环境下是否放行（后写的规则优先，与 Mojang 语义一致）。
pub fn evaluate_rules(rules: &[Rule], env: &EnvContext) -> bool {
    let _ = (rules, env);
    todo!("Vol.2 实现：空 rules 视为放行；allow/disallow 后覆盖前")
}

/// 把 1.12 及以前的单串 minecraftArguments 切成 Argument 序列。
/// 需要处理引号与转义，不能直接 split_whitespace——路径里带空格会把它切坏。
pub fn split_legacy_arguments(raw: &str) -> Vec<Argument> {
    let _ = raw;
    todo!("Vol.2 实现：带引号状态机的分词")
}