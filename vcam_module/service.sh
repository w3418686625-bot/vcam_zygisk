#!/system/bin/sh
# VCAM 模块 - service.sh
# 开机完成后执行，配置 LSPosed 作用域
#
# 自动将 VCAM 模块添加到所有已安装 App 的 LSPosed 作用域中。
# LSPosed 模块包名: com.example.vcam

MODDIR=${0%/*}
MODULE_PKG="com.example.vcam"

log() {
    echo "VCAM: $1" > /dev/kmsg
}

log "service.sh starting..."

# ---- 查找 LSPosed 数据库 ----
LSP_DB=""
for db in \
    /data/adb/lspd/config/modules.db \
    /data/adb/lspd/modules.db \
    /data/adb/lspd/config/modules_config.db \
    /data/user_de/0/org.lsposed.manager/databases/lsposed.db \
    /data/data/org.lsposed.manager/databases/lsposed.db; do
    if [ -f "$db" ]; then
        LSP_DB="$db"
        break
    fi
done

# 如果已知路径都没有，用 find 搜索
if [ -z "$LSP_DB" ]; then
    LSP_DB=$(find /data/adb -maxdepth 4 -name "modules.db" -type f 2>/dev/null | head -1)
fi

# 搜索 user_de 中的数据库
if [ -z "$LSP_DB" ]; then
    LSP_DB=$(find /data/user_de -maxdepth 5 -path "*/org.lsposed*" -name "*.db" -type f 2>/dev/null | head -1)
fi

if [ -z "$LSP_DB" ]; then
    log "LSPosed database not found - skipping scope setup"
    log "Make sure LSPosed is installed and has been opened at least once"
    exit 0
fi

log "Found LSPosed DB: $LSP_DB"

# ---- 等待 LSPosed 数据库就绪 ----
for i in $(seq 1 60); do
    if [ -f "$LSP_DB" ] && [ -s "$LSP_DB" ]; then
        break
    fi
    sleep 1
done

# ---- 获取模块在 LSPosed 中的 ID ----
MODULE_ID=$(sqlite3 "$LSP_DB" "SELECT mid FROM modules WHERE pkg='$MODULE_PKG' OR package_name='$MODULE_PKG' LIMIT 1;" 2>/dev/null)
if [ -z "$MODULE_ID" ]; then
    MODULE_ID=$(sqlite3 "$LSP_DB" "SELECT mid FROM modules WHERE apk_path LIKE '%vcam%' LIMIT 1;" 2>/dev/null)
fi

if [ -z "$MODULE_ID" ]; then
    log "VCAM module not found in LSPosed - make sure VCAM APK is enabled as LSPosed module"
    log "Open LSPosed → Modules → Enable VCAM → Set scope to 'System Framework' or any app"
    exit 0
fi

log "Module ID: $MODULE_ID"

# ---- 获取所有已安装 App ----
PKG_LIST=""
if pm list packages >/dev/null 2>&1; then
    PKG_LIST=$(pm list packages 2>/dev/null | sed 's/package://g')
fi
if [ -z "$PKG_LIST" ]; then
    PKG_LIST=$(cat /data/system/packages.list 2>/dev/null | awk '{print $1}')
fi

if [ -z "$PKG_LIST" ]; then
    log "Cannot get installed package list"
    exit 0
fi

# 如果当前 scope 是 "android" (系统框架)，改为具体 app 列表
CURRENT_SCOPE=$(sqlite3 "$LSP_DB" "SELECT scope FROM modules WHERE mid=$MODULE_ID;" 2>/dev/null)
log "Current scope: $CURRENT_SCOPE"

ADDED=0
SKIPPED=0
SCOPE_CHANGED=0

# 如果 scope 是空的或只有 "android"，重建作用域
if [ -z "$CURRENT_SCOPE" ] || [ "$CURRENT_SCOPE" = "android" ]; then
    log "Rebuilding scope from installed apps..."
    NEW_SCOPE=""
    for pkg in $PKG_LIST; do
        # 跳过系统框架（会导致所有系统服务也被 hook）
        case "$pkg" in
            android|com.android.systemui|com.android.phone|com.android.settings|\
            com.android.server.telecom|com.android.providers.settings|\
            com.google.android.gms|com.google.android.gsf)
                continue
                ;;
        esac
        if [ -z "$NEW_SCOPE" ]; then
            NEW_SCOPE="$pkg"
        else
            NEW_SCOPE="$NEW_SCOPE|$pkg"
        fi
    done

    sqlite3 "$LSP_DB" "UPDATE modules SET scope='$NEW_SCOPE' WHERE mid=$MODULE_ID;" 2>/dev/null
    SCOPE_CHANGED=1
    ADDED=$(echo "$NEW_SCOPE" | tr '|' '\n' | wc -l)
    log "Set scope to $ADDED apps"
else
    # 已有作用域，只添加缺失的
    for pkg in $PKG_LIST; do
        case "$pkg" in
            android|com.android.systemui|com.android.phone|com.android.settings|\
            com.android.server.telecom|com.android.providers.settings|\
            com.google.android.gms|com.google.android.gsf)
                continue
                ;;
        esac

        if echo "$CURRENT_SCOPE" | grep -q "$pkg"; then
            SKIPPED=$((SKIPPED + 1))
            continue
        fi

        CURRENT_SCOPE="$CURRENT_SCOPE|$pkg"
        ADDED=$((ADDED + 1))
        SCOPE_CHANGED=1
    done

    if [ $SCOPE_CHANGED -eq 1 ]; then
        sqlite3 "$LSP_DB" "UPDATE modules SET scope='$CURRENT_SCOPE' WHERE mid=$MODULE_ID;" 2>/dev/null
    fi
fi

log "Scope updated: +$ADDED apps (already had $SKIPPED)"
log "Total apps in scope: $(echo "$CURRENT_SCOPE" | tr '|' '\n' | wc -l)"

# 通知 LSPosed 重新加载
am broadcast -a org.lsposed.manager.action.RELOAD_MODULES 2>/dev/null

# 标记已完成
touch "$MODDIR/.scope_setup_done"
log "service.sh done"
