/*
 * 音乐窗页面侧 JSON 桥封装（webui/app.js）
 *
 * 协议（与 src/musicfree/musicfree_bridge.h 一致，语义已定不许改）：
 *   页面 → C++ ：{"cmd":"<name>","id":<int>,"payload":{...}}
 *   C++ → 页面 ：
 *     事件      {"evt":"<name>","payload":{...}}
 *     成功响应  {"id":N,"ok":true,"result":{...}}
 *     失败响应  {"id":N,"ok":false,"error":"<原因>"}
 *
 * 用法：
 *   Bridge.send("player.play", { mid: "xx" }).then(result => ...).catch(err => ...);
 *   Bridge.send("video.open", {...}, { timeout: 60000 });  // 按命令放宽超时
 *   Bridge.on("player.state", payload => ...);     // 返回取消函数
 *   await Bridge.ready;                            // 收到 bridge.ready 后 resolve
 *
 * 约定与降级：
 *   · send() 自动递增 id、按 id 匹配响应，默认 5 秒超时 reject；
 *     第三参 opts.timeout（毫秒）可按命令放宽 —— video.open 取详情走网络
 *     可能超过 5 秒，页面对它单独用 30000ms；其余命令仍 5000ms；
 *   · chrome.webview 不存在（普通浏览器里调试页面）时降级为本地 mock：
 *     console 警告一次，send() 直接返回 window.__MOCK__[cmd]（缺省 {}），
 *     并直接放行 Bridge.ready，避免页面初始化在浏览器里挂死；
 *   · 事件可能先于监听到达：无监听者时进 window.__pendingMessages__ 兜底
 *     队列，on() 注册时补投存量。
 *   · postMessage 以「对象」投递，C++ 宿主需用 get_WebMessageAsJson() 取串
 *     后交给 MusicFreeBridge::OnWebMessage。
 */
(function () {
    "use strict";

    // 封面加载兜底 + 诊断（影视源封面来自各站图床，失败原因差异很大）：
    //   · 豆瓣图床只认 douban.com 来源页，缺 Referer 直接 418（宿主已注入）；
    //   · 个别站图床已失效，返回 JSON 占位/404，本源封面永远拿不到。
    // 失败先上报宿主落盘（COVER_FAIL 行），再做兜底：
    //   ① http → https 重试一次；
    //   ② 业务层注册的 window.__coverFallback（换同一部片在其它源里的封面）。
    // 注意：本文件有两个 IIFE —— 这里只放「桥层」能用的东西，渲染相关
    // （规范化标题/封面索引）在下面的业务 IIFE 里，通过全局钩子回调。
    function reportCover(url, title, site, stage, wasHttp) {
        try {
            if (!(window.chrome && window.chrome.webview &&
                  window.chrome.webview.postMessage)) return;
            window.chrome.webview.postMessage(JSON.stringify({
                cmd: "video.coverFail", id: 0,
                payload: {
                    url: url || "", title: title || "", site: site || "",
                    stage: stage, wasHttp: !!wasHttp
                }
            }));
        } catch (e) { /* ignore */ }
    }

    function ttCoverFail(el) {
        var s = "", title = "", site = "";
        try {
            s = el.getAttribute("src") || "";
            title = el.getAttribute("data-title") || "";
            site = el.getAttribute("data-site") || "";
            var wasHttp = s.indexOf("http://") === 0;
            reportCover(s, title, site, "error", wasHttp);
            if (wasHttp) {
                el.setAttribute("src", "https://" + s.slice(7));  // http→https 重试一次
                return;
            }
            // 业务层（第二个 IIFE）注册的跨源补图：命中就换下一张图。
            if (typeof window.__coverFallback === "function" &&
                window.__coverFallback(el, s, title, site)) {
                return;
            }
        } catch (e) { /* ignore */ }
        el.style.visibility = "hidden";  // 彻底失败，隐藏破图
    }
    window.ttCoverFail = ttCoverFail;
    window.__ttCoverReport = reportCover;

    // ── 协议契约清单：必须与 src/musicfree/musicfree_bridge.cpp 中的
    //    kSupportedCmds / kSupportedEvts 完全一致
    //    （tools/bridge_selftest.py 会正则抽取两边比对，改协议必须同步两处）──
    var SUPPORTED_CMDS = [
        "app.init", "ping", "tab.switch",
        "player.play", "player.pause", "player.next",
        "player.prev", "player.seek", "player.volume",
        "video.home", "video.open", "video.sources", "video.search",
        "music.import", "music.discover", "music.sheet",
        "search.query", "fav.set",
        "cfg.get", "cfg.set"
    ];
    var SUPPORTED_EVTS = [
        "bridge.ready", "player.state", "video.home", "video.category",
        "video.status", "video.search", "search.result", "source.result",
        "music.discover", "music.sheet"
    ];

    // 请求-响应超时（毫秒）：超时未收到 {"id":N,...} 则 reject。
    var SEND_TIMEOUT_MS = 5000;

    var hasNative =
        typeof chrome !== "undefined" &&
        !!(chrome.webview && chrome.webview.postMessage);

    var nextId = 1;
    var pending = Object.create(null);   // id -> {resolve, reject, timer, cmd}
    var listeners = Object.create(null); // evt -> [handler]
    var warned = false;

    // Bridge.ready：收到第一个 bridge.ready 事件时 resolve（幂等）。
    var readyResolve;
    var readyPromise = new Promise(function (resolve) {
        readyResolve = resolve;
    });

    function warnOnce(msg) {
        if (warned) return;
        warned = true;
        if (typeof console !== "undefined" && console.warn) {
            console.warn("[Bridge] " + msg);
        }
    }

    function safeCall(handler, payload, evt) {
        try {
            handler(payload);
        } catch (e) {
            if (typeof console !== "undefined" && console.error) {
                console.error("[Bridge] 事件处理器异常 evt=" + evt, e);
            }
        }
    }

    // 事件分发：已有监听者则直投；否则进 __pendingMessages__ 兜底队列，
    // 兼容「消息先于监听到达」（on() 注册时补投存量）。
    function dispatchEvent(evt, payload) {
        if (evt === "bridge.ready") {
            readyResolve(); // 幂等：后续 bridge.ready 只再通知监听者
        }
        var hs = listeners[evt];
        if (hs && hs.length > 0) {
            for (var i = 0; i < hs.length; i++) {
                safeCall(hs[i], payload, evt);
            }
            return;
        }
        if (!Array.isArray(window.__pendingMessages__)) {
            window.__pendingMessages__ = [];
        }
        window.__pendingMessages__.push({ evt: evt, payload: payload });
    }

    // 收 C++ 消息的唯一入口（同时供 mock/测试手动注入）。
    function onMessage(raw) {
        var msg = raw;
        // WebView2 的 message 事件对象形如 { data: <消息体> }，需先剥壳。
        // 实测（tools/webview_smoke.cpp 诊断实例，真实 WebView2 Runtime）：
        //   host 用 PostWebMessageAsJson 发送时，e.data 是 **已解析的对象**
        //   （typeof e.data === "object"，如 {evt:"bridge.ready",payload:{}}），
        //   不是 JSON 字符串 —— 旧条件只认 typeof data === "string"，导致真实
        //   模式下所有 C++→页面 消息被当成"无法识别的消息信封"丢弃
        //   （console: [Bridge] 无法识别的消息信封: {"isTrusted":false}），
        //   bridge.ready 到不了握手逻辑，页面永远不会自发发 app.init。
        // 故剥壳条件放宽为「有 data 字段且外层不是协议信封」，剥出后：
        // 字符串走下面的 JSON.parse，对象直接当消息体用。
        // 测试直接注入消息体（串/带 id/evt/cmd 的对象）不带 data 字段，不受影响。
        if (msg && typeof msg === "object" &&
            msg.data !== undefined &&
            msg.id === undefined && msg.evt === undefined && msg.cmd === undefined) {
            msg = msg.data;
        }
        if (typeof msg === "string") {
            try {
                msg = JSON.parse(msg);
            } catch (e) {
                warnOnce("收到无法解析的消息: " + String(raw).slice(0, 200));
                return;
            }
        }
        if (!msg || typeof msg !== "object") return;

        // 成功/失败响应信封：按 id 关联到在途请求
        if (typeof msg.id === "number" && typeof msg.ok === "boolean") {
            var p = pending[msg.id];
            if (!p) return; // 已超时或未知 id：静默忽略
            delete pending[msg.id];
            clearTimeout(p.timer);
            if (msg.ok) {
                p.resolve(msg.result === undefined ? {} : msg.result);
            } else {
                p.reject(new Error(msg.error || "bridge error"));
            }
            return;
        }

        // 事件信封
        if (typeof msg.evt === "string") {
            dispatchEvent(msg.evt, msg.payload);
            return;
        }

        warnOnce("无法识别的消息信封: " + JSON.stringify(msg).slice(0, 200));
    }

    // 发命令：自动 id、按 id 匹配响应，默认 5s 超时 reject；
    // 第三参 opts = { timeout: 毫秒 } 可按命令放宽（video.open 用 30000）。
    // chrome.webview 缺失时降级为本地 mock（window.__MOCK__[cmd]，缺省 {}）。
    function send(cmd, payload, opts) {
        if (!hasNative) {
            warnOnce(
                "chrome.webview 不存在，send() 降级为本地 mock（window.__MOCK__）"
            );
            var table =
                typeof window.__MOCK__ === "object" && window.__MOCK__
                    ? window.__MOCK__
                    : {};
            var mock = table[cmd];
            return Promise.resolve(mock === undefined || mock === null ? {} : mock);
        }

        var timeoutMs =
            opts && typeof opts.timeout === "number" && opts.timeout > 0
                ? opts.timeout
                : SEND_TIMEOUT_MS;
        var id = nextId++;
        return new Promise(function (resolve, reject) {
            var timer = setTimeout(function () {
                delete pending[id];
                reject(
                    new Error("bridge timeout after " + timeoutMs + "ms: " + cmd)
                );
            }, timeoutMs);
            pending[id] = { resolve: resolve, reject: reject, timer: timer, cmd: cmd };
            try {
                // 以对象投递；C++ 宿主用 get_WebMessageAsJson() 取串
                chrome.webview.postMessage({
                    cmd: cmd,
                    id: id,
                    payload: payload || {}
                });
            } catch (e) {
                delete pending[id];
                clearTimeout(timer);
                reject(e);
            }
        });
    }

    // 注册事件监听；返回取消函数。注册时补投 __pendingMessages__ 中
    // 先于监听到达的同名事件（投给本 handler 后从队列移除）。
    function on(evt, handler) {
        if (!listeners[evt]) listeners[evt] = [];
        listeners[evt].push(handler);

        var queue = window.__pendingMessages__;
        if (Array.isArray(queue) && queue.length > 0) {
            var rest = [];
            for (var i = 0; i < queue.length; i++) {
                var item = queue[i];
                if (item && item.evt === evt) {
                    safeCall(handler, item.payload, evt);
                } else {
                    rest.push(item);
                }
            }
            window.__pendingMessages__ = rest;
        }

        return function off() {
            var hs = listeners[evt];
            if (!hs) return;
            var idx = hs.indexOf(handler);
            if (idx >= 0) hs.splice(idx, 1);
        };
    }

    if (hasNative) {
        chrome.webview.addEventListener("message", onMessage);
    } else {
        // 浏览器调试降级：宿主不存在，不会有 bridge.ready，直接放行 ready
        //（warnOnce 在此发唯一一条 console 警告）。
        warnOnce(
            "chrome.webview 不存在，send() 降级为本地 mock（window.__MOCK__）"
        );
        setTimeout(function () {
            readyResolve();
        }, 0);
    }

    // 对外唯一入口。commands/events 为契约清单，便于页面侧与自测工具检查。
    window.Bridge = {
        send: send,
        on: on,
        ready: readyPromise,
        commands: SUPPORTED_CMDS,
        events: SUPPORTED_EVTS,
        isMock: !hasNative
    };
})();

/* ============================================================
 * 模块 2：真实模式业务（WebView2 宿主已连接时启用）
 *
 * 判别与降级：
 *   · window.Bridge.isMock === true（chrome.webview 不存在，浏览器调试）
 *     → 本模块整体 return，不注册任何桥监听；两个 tab 由 index.html 的
 *     __MOCK__ 内联分支渲染（window.__MOCK_RENDERED__ === true）；
 *   · isMock === false → 接管全部渲染与交互：收到 bridge.ready 后握手
 *     app.init，成功即进入真实数据模式，依次拉初始数据并订阅事件。
 *
 * 握手与初始拉取顺序：
 *   bridge.ready → app.init {ua,version}
 *     → tab.switch（同步初始页签）→ video.home（首页聚合，结果走事件）
 *
 * 播放职责边界：
 *   音乐窗只做内容浏览与点播发起；音乐播放使用主窗口原有控制条，
 *   视频播放由 video.open 打开独立播放窗口（页面内不嵌播放器）。
 *
 * 事件 → 渲染目标（与 README §5 清单一致）：
 *   video.home   → #video-sections（section.vsec > h3.vsec-title + div.cards）
 *   video.status → #video-status（info/error）
 *   search.result→ #search-results（.result-group 分组）
 *   source.result→ #toast
 *
 * 超时口径（协议约定：仅 video.open 放宽 30s，其余 cmd 5s）：
 *   video.open { timeout: 30000 }（详情取选集走网络，见 send 第三参）；
 *   事件级看门狗：video.home 90s / search.result 30s —— 到时显示
 *   #video-status 错误行或 .empty，而不是白屏（事件真到了仍照常渲染）。
 * ============================================================ */
(function () {
    "use strict";

    var B = window.Bridge;
    if (!B || B.isMock) return; // 降级：浏览器调试走 index.html 的 mock 分支

    var $ = function (id) { return document.getElementById(id); };

    function esc(s) {
        return String(s === undefined || s === null ? "" : s).replace(/[&<>"]/g, function (c) {
            return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c];
        });
    }

    function errmsg(e) {
        return e && e.message ? e.message : String(e);
    }

    // ── 封面诊断/跨源兜底（与桥层的 ttCoverFail 配套）────────────────────
    // 卡片封面来自各站图床：某个站图床挂了（返回 JSON 占位/404）时，用同一部片
    // 在其它源里的封面顶上。规范化片名后匹配，忽略标点/空格/结尾年份。
    var coverAltIndex = {};   // 规范化标题 -> [封面 url]

    function normTitle(t) {
        return String(t || "").toLowerCase()
            .replace(/[\s\u3000]/g, "")
            .replace(/[()（）【】\[\]《》〈〉!！?？,，.。:：;；·、'"“”‘’\-—_~～]/g, "")
            .replace(/20\d{2}$/, "");
    }

    function rebuildCoverAltIndex(cards) {
        coverAltIndex = {};
        (cards || []).forEach(function (c) {
            if (!c || !c.cover) return;
            var k = normTitle(c.title);
            if (!k) return;
            var list = coverAltIndex[k] || (coverAltIndex[k] = []);
            if (list.indexOf(c.cover) < 0) list.push(c.cover);
        });
    }

    // 统一走桥层上报（保证日志格式只有一份实现）。
    function reportCover(url, title, site, stage, wasHttp) {
        if (typeof window.__ttCoverReport === "function")
            window.__ttCoverReport(url, title, site, stage, wasHttp);
    }

    // 桥层 ttCoverFail 的回调：换下一张未试过的同名片封面；无可用封面返回 false。
    window.__coverFallback = function (el, src, title, site) {
        try {
            var alts = coverAltIndex[normTitle(title)] || [];
            var tried = el.__coverTried || (el.__coverTried = {});
            tried[src] = 1;
            for (var i = 0; i < alts.length; i++) {
                var u = alts[i];
                if (u && !tried[u]) {
                    tried[u] = 1;
                    el.setAttribute("src", u);
                    reportCover(u, title, site, "fallback", false);
                    return true;
                }
            }
        } catch (e) { /* ignore */ }
        return false;
    };


    // 容错取字段：按别名列表取第一个有值的（宿主字段命名未最终定稿时的缓冲）
    function pick(obj, keys) {
        if (!obj || typeof obj !== "object") return undefined;
        for (var i = 0; i < keys.length; i++) {
            if (obj[keys[i]] !== undefined && obj[keys[i]] !== null) return obj[keys[i]];
        }
        return undefined;
    }

    // ---------- 轻提示（与 index.html mock 分支同款视觉） ----------
    var toastTimer = null;
    var lastToast = { text: "", at: 0 };
    function toast(msg) {
        if (!msg) return;
        var now = Date.now();
        if (msg === lastToast.text && now - lastToast.at < 3000) return; // 去重（回执+事件双报）
        lastToast = { text: msg, at: now };
        var el = $("toast");
        el.textContent = msg;
        el.hidden = false;
        el.classList.add("show");
        clearTimeout(toastTimer);
        toastTimer = setTimeout(function () {
            el.classList.remove("show");
            setTimeout(function () { el.hidden = true; }, 250);
        }, 2400);
    }

    // ---------- 状态行 ----------
    function setStatus(level, text) {
        var el = $("video-status");
        if (!el) return;
        if (!text) { el.hidden = true; el.textContent = ""; return; }
        el.hidden = false;
        el.className = "video-status " + (level === "error" ? "error" : "info");
        el.textContent = text;
    }

    // 封面 URL 加载失败 → 隐藏 broken 图，露出 CSS 占位底（不请求网络占位图）
    document.addEventListener("error", function (e) {
        var t = e.target;
        if (t && t.tagName === "IMG") t.style.visibility = "hidden";
    }, true);

    // ================= 页面运行态 =================
    var inited = false;            // app.init 是否已成功（真实数据模式开关）
    var curTab = "music";
    var videoCards = [];           // 扁平卡片表（data-idx → 卡片对象）
    var videoEpisodes = [];        // 扁平选集表（data-ep → {card,line,ep}）
    var videoHomeBusy = false;
    var videoHomeGot = false;
    var homeEvtTimer = null;
    var pendingQ = "";             // 在途搜索关键词（过滤迟到的旧结果）
    var searchEvtTimer = null;

    // ---------- tab ----------
    function switchTab(name) {
        if (!name) return;
        curTab = name;
        Array.prototype.forEach.call(document.querySelectorAll("#tabbar .tab"), function (b) {
            var on = b.getAttribute("data-tab") === name;
            b.classList.toggle("active", on);
            b.setAttribute("aria-selected", on ? "true" : "false");
        });
        ["music", "video"].forEach(function (n) {
            $("panel-" + n).hidden = (n !== name);
        });
        // ★ 用户要求「切换 tab 不要重刷数据」：两个 panel 本来就只是 hidden
        //   切换，DOM / 滚动位置 / 搜索结果都原样保留。旧代码每次切回音乐都
        //   setSearchMode(false) 把搜索结果藏掉、切回视频也强制回首页，看起来
        //   就是「重刷」。现在切 tab 不改任何视图状态，只在确实还没数据时
        //   补一次加载（*Got 标志 + 内容签名双重保护，已有数据不再发请求）。
        B.send("tab.switch", { tab: name }).catch(function (e) {
            console.warn("[TTPlayer] tab.switch 失败:", errmsg(e));
        });
        if (name === "video" && inited && !videoHomeBusy && !videoHomeGot) loadVideoHome();
        if (name === "music" && inited && !musicDiscoverBusy && !musicDiscoverGot) loadMusicDiscover();
    }

    // ---------- 视频首页 ----------
    // 影视仓式分类页签：顶部一排「全部 / 电影 / 剧集 / 综艺 …」。
    // 「全部」展示首页推荐流（homeSectionsCache）；其余分类点进去按该分类从各源拉片。
    var VIDEO_CAT_ALL = "全部";
    var videoCat = VIDEO_CAT_ALL;
    var videoCatTitles = [];        // 分类页签名（不含「全部」）
    var videoCats = {};             // { 分类名: [{key,id}] } —— 点分类时回传后端
    var homeSectionsCache = [];     // 首页推荐流 sections（「全部」页签用）
    var videoCategoryBusy = false;

    function clearVideoCats() {
        videoCatTitles = [];
        videoCats = {};
        if ($("video-cats")) $("video-cats").innerHTML = "";
    }

    // 渲染分类页签：从首页 evt 的 cats 取出分类名（不含「全部」）。
    function renderVideoCats(catTitles) {
        // 防御：去重 + 剔除与「全部」同名的项（个别源的分类列表自带「全部」，
        // 曾导致页签出现两个「全部」+ 两排重复）。
        var seen = {};
        videoCatTitles = [];
        (catTitles || []).forEach(function (t) {
            t = String(t || "");
            if (!t || t === VIDEO_CAT_ALL || seen[t]) return;
            seen[t] = 1;
            videoCatTitles.push(t);
        });
        // 当前选中的分类若已不在新首页里，回到「全部」
        if (videoCat !== VIDEO_CAT_ALL && videoCatTitles.indexOf(videoCat) < 0) {
            videoCat = VIDEO_CAT_ALL;
        }
        var titles = [VIDEO_CAT_ALL].concat(videoCatTitles);
        $("video-cats").innerHTML = titles.map(function (t) {
            return '<button class="vcat' + (t === videoCat ? " active" : "") +
                '" type="button" data-cat="' + esc(t) + '">' + esc(t) + "</button>";
        }).join("");
    }

    // 只更新按钮高亮（分类内容由 evt video.category 渲染，不重渲页签）
    function applyVideoCatHighlight(cat) {
        videoCat = cat;
        Array.prototype.forEach.call($("video-cats").querySelectorAll(".vcat"), function (b) {
            b.classList.toggle("active", b.getAttribute("data-cat") === cat);
        });
    }

    // 切到「全部」页签：直接显示已缓存的首页推荐流（不重发网络）
    function applyVideoCat(cat) {
        applyVideoCatHighlight(cat);
        if (cat === VIDEO_CAT_ALL && homeSectionsCache.length) {
            renderVideoSections(homeSectionsCache);
        }
    }

    // 渲染一组 section（首页推荐流 / 分类拉取结果共用）
    function renderVideoSections(sections) {
        var s = Array.isArray(sections) ? sections : [];
        videoCards = [];
        var all = [];
        s.forEach(function (sec) {
            (Array.isArray(sec.cards) ? sec.cards : []).forEach(function (c) {
                all.push(c);
            });
        });
        // 多源推荐流的同一部片可互相补封面（某站图床挂了也能显示）。
        rebuildCoverAltIndex(all);
        var missing = [];
        var html = s.map(function (sec) {
            var cards = (Array.isArray(sec.cards) ? sec.cards : []).map(function (c) {
                var idx = videoCards.push(c) - 1;
                var cover = c.cover
                    ? '<img loading="lazy" referrerpolicy="no-referrer" src="' +
                      esc(c.cover) + '" alt="" data-title="' +
                      esc(c.title || "") + '" data-site="' +
                      esc(c.from || c.site || "") + '" onerror="ttCoverFail(this)">'
                    : "";
                if (!c.cover) missing.push(c);
                var rating = c.rate ? '<span class="rating">' + esc(c.rate) + "</span>" : "";
                var tags = [c.from, c.remarks, c.type, c.year]
                    .filter(function (t) { return t; })
                    .map(function (t) { return '<span class="tag">' + esc(t) + "</span>"; })
                    .join("");
                return '<article class="card" data-idx="' + idx + '">' +
                    '<div class="card-cover">' + cover + rating + "</div>" +
                    '<div class="card-title">' + esc(c.title || "") + "</div>" +
                    '<div class="card-meta">' + tags + "</div></article>";
            }).join("");
            if (!cards) cards = '<div class="empty" style="grid-column:1/-1">该分区暂无内容</div>';
            return '<section class="vsec"><h3 class="vsec-title">' + esc(sec.title || "") +
                '</h3><div class="cards">' + cards + "</div></section>";
        }).join("");
        $("video-sections").innerHTML = html;
        reportMissingCovers(missing);
    }

    // 完全没有封面地址的卡片也上报（此前这类卡片无任何日志，无法定位是
    // 「源里就没图」还是「图加载失败」）。每次渲染最多报 20 条，避免刷屏。
    function reportMissingCovers(list) {
        var n = Math.min(list.length, 20);
        for (var i = 0; i < n; i++) {
            var c = list[i] || {};
            reportCover("", c.title || "", c.from || c.site || "", "empty", false);
        }
    }

    // 点分类页签：发 video.category（后端逐站拉该分类第一页）
    function loadVideoCategory(title) {
        // 分类浏览与搜索互斥：切分类时退出搜索态，左栏恢复推荐流。
        setVideoSearchMode(false);
        if (videoCategoryBusy) return;
        var sources = videoCats[title];
        if (!sources) return;
        videoCategoryBusy = true;
        applyVideoCatHighlight(title);
        showSectionsLoading("正在加载「" + title + "」分类（多源并发）…");
        setStatus("info", "正在加载分类：" + title);
        B.send("video.category", { cat: title, sources: sources }, { timeout: 60000 }).catch(function (e) {
            videoCategoryBusy = false;
            setStatus("error", "分类加载失败：" + errmsg(e));
            showSectionsEmpty("分类加载失败");
        });
    }

    function showSectionsLoading(text) {
        $("video-sections").innerHTML = '<div class="loading">' + esc(text || "正在加载视频首页…") + "</div>";
        videoCards = [];
        // 注意：不清空 video-cats —— 分类页签在加载中应保留（分类拉取期间
        // 仍要可见可点），由 renderVideoHome / evt 决定页签内容。
    }

    function showSectionsEmpty(text) {
        $("video-sections").innerHTML = '<div class="empty">' + esc(text || "暂无视频内容") + "</div>";
        videoCards = [];
    }

    function loadVideoHome() {
        if (videoHomeBusy) return;
        videoHomeBusy = true;
        videoHomeGot = false;
        showSectionsLoading("正在加载视频首页（聚合可能需要数十秒）…");
        setStatus("info", "正在请求视频首页…");
        clearTimeout(homeEvtTimer);
        // 事件级看门狗：宿主聚合动辄几十秒，90s 还没来就报状态行（不白屏）
        homeEvtTimer = setTimeout(function () {
            videoHomeBusy = false;
            if (!videoHomeGot) {
                setStatus("error", "视频首页加载超时，切到其它页签再回来可重试");
                showSectionsEmpty("首页数据未到达，请稍后重试");
            }
        }, 90000);
        B.send("video.home", {}).catch(function (e) {
            videoHomeBusy = false;
            clearTimeout(homeEvtTimer);
            setStatus("error", "视频首页请求失败：" + errmsg(e));
            showSectionsEmpty("视频首页加载失败");
        });
    }

    function renderVideoHome(p) {
        p = p || {};
        videoHomeBusy = false;
        videoHomeGot = true;
        videoCategoryBusy = false;
        clearTimeout(homeEvtTimer);

        var sections = Array.isArray(p.sections) ? p.sections : [];
        homeSectionsCache = sections;

        // 从首页 evt 的 cats 构建分类映射（分类名 → [{key,id}]）
        videoCats = {};
        var catTitles = [];
        if (Array.isArray(p.cats)) {
            p.cats.forEach(function (c) {
                if (c && c.title && Array.isArray(c.sources) && c.sources.length) {
                    videoCats[c.title] = c.sources;
                    catTitles.push(c.title);
                }
            });
        }

        if (!sections.length) {
            setStatus(p.error ? "error" : "info", p.error || "暂无视频内容，可点「导入视频源」添加影视仓 / TVBox 配置");
            showSectionsEmpty(p.error || "暂无视频内容");
            renderVideoCats(catTitles);
            return;
        }
        // 有分区内容即正常展示：个别站点失效是常态，不再弹「部分站点加载失败」
        // 红色横幅打扰（宿主侧也只写诊断日志）。真正的错误（无内容）走上面的分支。
        setStatus("info", "已加载 " + sections.length + " 个分区" +
            (p.sites ? " · " + p.sites + " 个视频源" : "") +
            (catTitles.length ? " · " + catTitles.length + " 个分类" : ""));

        renderVideoSections(sections);
        renderVideoCats(catTitles);
        applyVideoCat(videoCat);   // 保留上次选中分类；分类已不存在则自动回到「全部」
    }

    // ---------- 视频搜索（cmd video.search → evt video.search） ----------
    // 左栏出搜索结果、右栏常驻剧集：搜索态左栏切到结果（覆盖推荐流），
    // 右栏「剧集」保持可见——点左栏结果后剧集在此展开。
    var videoSearchResults = [];
    var videoPendingQ = "";
    var videoSearchEvtTimer = null;

    function setVideoSearchMode(on) {
        var res = $("video-search-results");
        if (!res) return;
        // 左栏：搜索态显示结果、隐藏推荐流与分类 chips 及其标题。
        res.hidden = !on;
        var secs = $("video-sections"), cats = $("video-cats"),
            rec = $("video-rec-title");
        if (secs) secs.hidden = !!on;
        if (cats) cats.hidden = !!on;
        if (rec) rec.hidden = !!on;
        // 右栏剧集（#episode-list / #video-list-title）不再随搜索态隐藏：
        // 用户希望搜索时左看结果、右看剧集。
    }

    // 搜索态顶部的「返回首页」按钮：搜索框内容保留，便于再次搜索。
    function videoSearchHead() {
        return '<div class="result-head">' +
            '<button id="video-search-back" class="search-back-btn" type="button"' +
            ' title="返回视频首页推荐">← 返回首页</button></div>';
    }

    function videoSearchEmpty(text) {
        $("video-search-results").innerHTML =
            videoSearchHead() + '<div class="empty">' + esc(text) + "</div>";
    }

    function doVideoSearch() {
        var q = ($("video-search-input").value || "").trim();
        if (!q) {
            videoPendingQ = "";
            videoSearchEmpty("输入关键词开始搜索");
            setVideoSearchMode(false);
            return;
        }
        videoPendingQ = q;
        setVideoSearchMode(true);
        $("video-search-results").innerHTML = videoSearchHead() +
            '<div class="loading">正在搜索“' + esc(q) + "”…</div>";
        clearTimeout(videoSearchEvtTimer);
        // 事件看门狗：宿主会并发扫描全部视频源，40s 未到给空态提示
        videoSearchEvtTimer = setTimeout(function () {
            if (videoPendingQ === q) videoSearchEmpty("搜索超时，请稍后重试");
        }, 40000);
        // C++ 立即回 {accepted:true}，真实结果走 evt video.search
        B.send("video.search", { q: q }).then(function (r) {
            if (r && r.accepted === false) {
                clearTimeout(videoSearchEvtTimer);
                videoSearchEmpty("搜索未被宿主接受");
            }
        }).catch(function (e) {
            clearTimeout(videoSearchEvtTimer);
            if (videoPendingQ === q) videoSearchEmpty("搜索失败：" + errmsg(e));
        });
    }

    function renderVideoSearch(p) {
        p = p || {};
        clearTimeout(videoSearchEvtTimer);
        setVideoSearchMode(true);
        var cards = Array.isArray(p.cards) ? p.cards : [];
        videoSearchResults = cards;
        if (!cards.length) {
            videoSearchEmpty(p.error ||
                ("未找到" + (p.q ? "「" + p.q + "」" : "相关") + "结果"));
            return;
        }
        // 搜索结果用大图卡片网格（与首页一致的影视仓观感），不再是细行列表。
        rebuildCoverAltIndex(cards);
        var missing = [];
        var html = cards.map(function (c, i) {
            var meta = [c.from, c.remarks || c.type, c.year]
                .filter(function (x) { return x; }).join(" · ");
            var cover = c.cover
                ? '<img loading="lazy" referrerpolicy="no-referrer" src="' +
                  esc(c.cover) + '" alt="" data-title="' +
                  esc(c.title || "") + '" data-site="' +
                  esc(c.from || c.site || "") + '" onerror="ttCoverFail(this)">'
                : "";
            if (!c.cover) missing.push(c);
            var rating = c.rate
                ? '<span class="rating">' + esc(c.rate) + "</span>" : "";
            return '<article class="card search-card" data-idx="' + i + '">' +
                '<div class="card-cover">' + cover + rating + "</div>" +
                '<div class="card-title">' + esc(c.title || "") + "</div>" +
                (meta ? '<div class="card-meta">' + esc(meta) + "</div>" : "") +
                "</article>";
        }).join("");
        $("video-search-results").innerHTML =
            '<div class="result-group">' + videoSearchHead() +
            '<h3 class="vsec-title">“' + esc(p.q || "") +
            '” 的搜索结果（' + cards.length +
            (p.partial ? "，搜索中…" : "") + "）</h3>" +
            '<div class="search-cards">' + html + "</div></div>";
        reportMissingCovers(missing);
    }

    // 点卡片 → 取详情（30s 超时）或直开播放窗
    function openVideo(c) {
        var payload = {};
        ["url", "id", "site", "title", "sources", "line"].forEach(function (k) {
            if (c[k] !== undefined && c[k] !== null) payload[k] = c[k];
        });
        setStatus("info", "正在加载《" + (c.title || "") + "》的选集…");
        $("episode-list").innerHTML = '<div class="loading" style="flex:1 1 100%">正在加载选集…</div>';
        videoEpisodes = [];
        // 详情取选集走网络、可能远超 5s：原站详情（6s）失败后还有跨源同名
        // 回退（18s）+ 直链前置扫描（最长 25s），串行最坏情况超过 30s ——
        // 曾表现为「bridge timeout after 30000ms」。放宽到 60s（协议注释同步）。
        B.send("video.open", payload, { timeout: 60000 }).then(function (r) {
            r = r || {};
            if (r.mode === "detail" || Array.isArray(r.lines)) {
                renderEpisodes(c, r);
                // 宿主在取详情后会自动播第 1 集（影视仓式「点开即播」）
                if (r.autoPlayed) {
                    setStatus("info", "已在播放窗打开：" +
                        (r.autoEpisode ? r.autoEpisode + " · " : "") +
                        (r.title || c.title || ""));
                    toast("开始播放：" + (r.title || c.title || ""));
                }
            } else if (r.mode === "play") {
                setStatus("info", "已在播放窗打开《" + (r.title || c.title || "") + "》");
                toast("已在播放窗打开：" + (c.title || ""));
            }
        }).catch(function (e) {
            setStatus("error", "加载选集失败：" + errmsg(e));
            $("episode-list").innerHTML = '<div class="empty" style="flex:1 1 100%">选集加载失败，请重试</div>';
            videoEpisodes = [];
        });
    }

    function renderEpisodes(card, r) {
        var lines = Array.isArray(r.lines) ? r.lines : [];
        var multi = lines.length > 1;
        videoEpisodes = [];
        var html = "";
        lines.forEach(function (ln) {
            (Array.isArray(ln.episodes) ? ln.episodes : []).forEach(function (ep) {
                var i = videoEpisodes.push({ card: card, line: ln.name || "", ep: ep }) - 1;
                var label = multi && ln.name
                    ? ln.name + " · " + (ep.name || "")
                    : (ep.name || "第 " + (i + 1) + " 集");
                html += '<button class="episode" type="button" data-ep="' + i + '">' + esc(label) + "</button>";
            });
        });
        if (html) {
            $("episode-list").innerHTML = html;
            setStatus("info", r.message || ("《" + (r.title || card.title || "") + "》已加载 " + videoEpisodes.length + " 个选集"));
        } else {
            $("episode-list").innerHTML = '<div class="empty" style="flex:1 1 100%">' + esc(r.message || "该视频暂无可播放选集") + "</div>";
            setStatus("info", r.message || "该视频暂无可播放选集");
        }
    }

    // 点选集 → 全字段回发 video.open 直开播放窗（30s 超时）
    function playEpisode(btn) {
        var it = videoEpisodes[Number(btn.getAttribute("data-ep"))];
        if (!it || !it.ep) return;
        var ep = it.ep;
        if (!ep.url) { toast("该选集缺少播放地址"); return; }
        var payload = {
            url: ep.url,
            id: it.card.id,
            site: it.card.site,
            title: it.card.title,
            sources: Array.isArray(ep.sources) ? ep.sources : [],
            ep: ep.name || ""
        };
        // 整部剧的选集一起带上：播放窗的「选集」按钮据此在同一窗口里切集
        // （以前只有电影能切源，剧集在播放窗里没有任何换集入口）。
        // 只取与当前点击集同一线路的集数：videoEpisodes 是跨线路铺平的，
        // 全带上会让播放窗菜单里每集重复出现（线路1·第1集、线路2·第1集…）。
        var allEps = [];
        var pickLine = it.line || "";
        for (var ei = 0; ei < videoEpisodes.length; ei++) {
            var v = videoEpisodes[ei];
            if (!v || !v.ep || !v.ep.url) continue;
            if (pickLine && v.line && v.line !== pickLine) continue;
            allEps.push({
                title: v.ep.name || "",
                url: v.ep.url,
                line: v.line || "",
                sources: Array.isArray(v.ep.sources) ? v.ep.sources : []
            });
        }
        if (allEps.length > 1) payload.episodes = allEps;
        if (it.line) payload.line = it.line;
        B.send("video.open", payload, { timeout: 30000 }).then(function (r) {
            r = r || {};
            if (r.mode === "detail" || Array.isArray(r.lines)) {
                renderEpisodes(it.card, r); // 宿主回详情则刷新选集
                return;
            }
            Array.prototype.forEach.call($("episode-list").querySelectorAll(".episode"), function (b) {
                b.classList.remove("active");
            });
            btn.classList.add("active");
            setStatus("info", "已在播放窗打开：" + (ep.name || "") + (it.card.title ? " · " + it.card.title : ""));
            toast("正在播放：" + (ep.name || it.card.title || ""));
        }).catch(function (e) {
            toast("打开失败：" + errmsg(e));
        });
    }

    // ---------- 搜索 ----------
    function searchEmpty(text) {
        $("search-results").innerHTML = '<div class="empty">' + esc(text) + "</div>";
    }

    // 搜索结果显示在右栏：有结果时隐藏曲目列表（及标题），清空搜索后恢复
    function setSearchMode(on) {
        var res = $("search-results"), list = $("track-list"), title = $("track-list-title");
        if (!res || !list || !title) return;
        res.hidden = !on;
        list.hidden = !!on;
        title.hidden = !!on;
    }

    var searchItems = [];
    function renderSearchResult(p) {
        p = p || {};
        clearTimeout(searchEvtTimer);
        setSearchMode(true);
        var groups = Array.isArray(p.groups) ? p.groups : [];
        searchItems = [];
        if (!groups.length) {
            searchEmpty("未找到" + (p.q ? "「" + p.q + "」" : "相关") + "结果");
            return;
        }
        var html = groups.map(function (g) {
            var rows = (Array.isArray(g.items) ? g.items : []).map(function (it) {
                // ★ 搜索结果分组名就是音源标识（截图里是 "qq"），但 item 本身
                //   往往不带 platform。缺了它 C++ BuildTrackSpec 会直接
                //   reply(false)「曲目缺少 url，也缺少 platform+id」——用户实测
                //   「不如之前，直接报错」。这里补上，C++ 侧再从 id 前缀兜底。
                if (!it.platform && !it.source && g.name)
                    it.platform = String(g.name);
                var i = searchItems.push(it) - 1;
                var sub = [it.artist, it.album].filter(function (x) { return x; }).join(" · ");
                return '<div class="result-row" data-idx="' + i + '">' +
                    '<span class="track-no">♪</span>' +
                    '<div class="track-main"><div class="track-title">' + esc(it.title || "") + "</div>" +
                    (sub ? '<div class="track-artist">' + esc(sub) + "</div>" : "") + "</div>" +
                    '<span class="track-dur">' + esc(it.duration || "") + "</span></div>";
            }).join("");
            return '<div class="result-group"><h3 class="vsec-title">' + esc(g.name || "结果") +
                "</h3>" + rows + "</div>";
        }).join("");
        $("search-results").innerHTML = html;
    }

    function doSearch() {
        var q = ($("search-input").value || "").trim();
        if (!q) { pendingQ = ""; searchEmpty("输入关键词开始搜索"); setSearchMode(false); return; }
        pendingQ = q;
        setSearchMode(true);
        $("search-results").innerHTML = '<div class="loading">正在搜索“' + esc(q) + '”…</div>';
        clearTimeout(searchEvtTimer);
        // 事件看门狗：结果经 evt search.result 回推，30s 未到给空态提示
        searchEvtTimer = setTimeout(function () {
            if (pendingQ === q) searchEmpty("搜索超时，请稍后重试");
        }, 30000);
        // C++ 立即回 {accepted:true}，真实结果走 evt search.result
        B.send("search.query", { q: q }).then(function (r) {
            if (r && r.accepted === false) {
                clearTimeout(searchEvtTimer);
                searchEmpty("搜索未被宿主接受");
            }
        }).catch(function (e) {
            clearTimeout(searchEvtTimer);
            if (pendingQ === q) searchEmpty("搜索失败：" + errmsg(e));
        });
    }

    // ===== 双击短时拦截（榜单双击 + 搜索结果点击共用）=====
    // ★ 用户要求「双击不要添加两条，做短时拦截」：真机 play.log 21:29:57
    //   同一秒出现两条完全相同的 PLAY entry added=1。榜单曲目走 dblclick
    //   （原本已有 800ms 去重），但搜索结果是 click 监听 —— 双击会触发两次
    //   click，各发一次 player.play，于是列表加两首。这里按曲目身份做 1.5s
    //   窗口去重，两条路径共用；宿主侧 bridge_cmds_music.cpp 还有一层兜底。
    var enqueueTicks = Object.create(null);
    var ENQUEUE_GUARD_MS = 1500;
    function trackKey(item) {
        return String(item.platform || "") + "\u001f" +
               String(item.id || item.mid || item.url || item.title || "");
    }
    function claimEnqueue(item) {
        if (!item) return false;
        var k = trackKey(item), now = Date.now();
        if (enqueueTicks[k] && now - enqueueTicks[k] < ENQUEUE_GUARD_MS)
            return false;
        enqueueTicks[k] = now;
        return true;
    }

    function playSearchItem(item) {
        if (!claimEnqueue(item)) return;
        // ★ 旧白名单漏掉 platform / raw / url，且用「平铺 payload」这条与榜单
        //   双击不同的路径，导致搜索结果点播被宿主整单拒收。现在统一成
        //   {track: 单曲}，字段与榜单双击一致，宿主「立即入列 + 立即回执」。
        var payload = {};
        ["id", "mid", "title", "artist", "album", "duration", "duration_ms",
         "cover", "art", "url", "platform", "source", "raw", "extra"].forEach(function (k) {
            if (item[k] !== undefined && item[k] !== null) payload[k] = item[k];
        });
        // 分组名可能是中文，交给宿主再按 id 前缀兜底；这里只在缺值时补。
        if (!payload.platform && payload.id) {
            var m = /^([a-z][a-z0-9]{1,12})[_:-]/i.exec(String(payload.id));
            if (m) payload.platform = m[1].toLowerCase();
        }
        toast("正在加入播放列表…");
        B.send("player.play", { track: payload }, { timeout: 15000 })
         .then(function (r) {
            if (r && r.dedup) return;   // 被宿主去重：不重复提示
            if (r && r.warning) toast(r.warning);
            else if (r && r.pending)
                toast("已加入播放列表《" + (item.title || "") +
                      "》，正在解析音源…");
            else toast("已加入播放列表：" + (item.title || ""));
        }).catch(function (e) {
            toast("添加失败：" + errmsg(e));
        });
    }

    // 音乐窗不再承载播放控制（底部控制条已移除）：音乐播放走主窗口既有控件，
    // 视频播放走 video.open 弹出的独立播放窗口。页面只负责内容浏览与点播发起。

    // ---------- 音乐：榜单 / 推荐（music.discover → music.sheet） ----------
    // 数据来源是各音源插件的 getTopLists / getRecommendSheets（与 Win32 发现页
    // 同一份实现）；点卡片拉曲目，点曲目整单入列并从该曲起播。
    var musicSheets = [];
    var musicItems = [];
    var musicDiscoverBusy = false;
    var musicDiscoverGot = false;

    function loadMusicDiscover() {
        if (musicDiscoverBusy) return;
        musicDiscoverBusy = true;
        if (!musicDiscoverGot) {
            $("music-charts").innerHTML =
                '<div class="empty" style="grid-column:1/-1">正在加载各音源的榜单与推荐…</div>';
        }
        B.send("music.discover", {}).catch(function (e) {
            musicDiscoverBusy = false;
            $("music-charts").innerHTML =
                '<div class="empty" style="grid-column:1/-1">加载榜单失败：' +
                esc(errmsg(e)) + "</div>";
        });
    }

    function renderMusicDiscover(p) {
        p = p || {};
        musicDiscoverBusy = false;
        musicDiscoverGot = true;
        var groups = Array.isArray(p.groups) ? p.groups : [];
        musicSheets = [];
        var html = "";
        groups.forEach(function (g) {
            (Array.isArray(g.sheets) ? g.sheets : []).forEach(function (s) {
                var i = musicSheets.push({
                    platform: s.platform || g.platform || "",
                    title: s.title || "",
                    cover: s.cover || "",
                    group: s.group || "",
                    kind: s.kind || "top",
                    raw: s.raw || {}
                }) - 1;
                html += '<article class="card" data-idx="' + i + '">' +
                    '<div class="card-cover">' +
                    (s.cover ? '<img loading="lazy" src="' + esc(s.cover) +
                               '" alt="" onerror="ttCoverFail(this)">' : "") +
                    "</div>" +
                    '<div class="card-title">' + esc(s.title || "") + "</div>" +
                    '<div class="card-meta">' +
                    esc(s.group || s.description || g.platform || "") + "</div>" +
                    "</article>";
            });
        });
        if (!html) {
            $("music-charts").innerHTML =
                '<div class="empty" style="grid-column:1/-1">' +
                esc(p.error ? "暂无榜单数据（" + p.error + "）"
                            : "暂无榜单数据，请先点「导入音源」添加音源插件") +
                "</div>";
            return;
        }
        $("music-charts").innerHTML = html;
    }

    function openMusicSheet(s) {
        musicItems = [];
        $("track-list").innerHTML =
            '<li class="empty">正在加载《' + esc(s.title) + "》…</li>";
        B.send("music.sheet", { platform: s.platform, raw: s.raw, kind: s.kind })
            .catch(function (e) {
                $("track-list").innerHTML = '<li class="empty">加载失败：' +
                    esc(errmsg(e)) + "</li>";
            });
    }

    function renderMusicSheet(p) {
        p = p || {};
        var items = Array.isArray(p.items) ? p.items : [];
        musicItems = items;
        if (!items.length) {
            $("track-list").innerHTML = '<li class="empty">' +
                esc(p.error ? "暂无曲目（" + p.error + "）" : "该榜单暂无曲目") + "</li>";
            return;
        }
        $("track-list").innerHTML = items.map(function (it, i) {
            var sub = [it.artist, it.album].filter(function (x) { return x; }).join(" · ");
            return '<li class="track" data-idx="' + i + '">' +
                '<span class="track-no">' + (i + 1) + "</span>" +
                '<div class="track-main"><div class="track-title">' +
                esc(it.title || "") + "</div>" +
                (sub ? '<div class="track-artist">' + esc(sub) + "</div>" : "") +
                "</div>" +
                '<span class="track-dur">' + esc(it.duration || "") + "</span></li>";
        }).join("");
    }

    // ---------- evt：music.discover / video.home ----------
    // ★ 切换 tab 不重刷数据：宿主可能在 tab.switch 之后重推一份内容相同的
    //   首页事件，render 会无条件重建整个 DOM（卡片闪一下、滚动位置归零），
    //   观感就是「切换 tab 重刷了数据」。按内容签名去重：内容没变就不重绘。
    var homeSig = { music: "", video: "" };
    function sameHome(kind, p) {
        var sig;
        try { sig = JSON.stringify(p || {}); } catch (e) { return false; }
        if (!sig) return false;
        if (sig === homeSig[kind]) return true;
        homeSig[kind] = sig;
        return false;
    }
    B.on("music.discover", function (p) {
        if (sameHome("music", p)) return;
        renderMusicDiscover(p);
    });

    // ---------- evt：music.sheet ----------
    B.on("music.sheet", function (p) { renderMusicSheet(p); });

    // ---------- evt：video.home ----------
    B.on("video.home", function (p) {
        if (sameHome("video", p)) return;
        renderVideoHome(p);
    });

    // ---------- evt：video.category（分类页签拉取的影片） ----------
    B.on("video.category", function (p) {
        videoCategoryBusy = false;
        p = p || {};
        var sections = Array.isArray(p.sections) ? p.sections : [];
        if (!sections.length) {
            setStatus(p.error ? "error" : "info",
                      (p.error || ("「" + videoCat + "」分类暂无内容")));
            showSectionsEmpty(p.error || ("「" + videoCat + "」分类暂无内容"));
        } else {
            setStatus("info", "已加载「" + videoCat + "」分类 · " + sections.length + " 个分区");
            renderVideoSections(sections);
        }
        applyVideoCatHighlight(videoCat);  // 保持分类页签高亮（不重渲）
    });

    // ---------- evt：video.search（迟到的旧结果按 q 过滤） ----------
    B.on("video.search", function (p) {
        if (videoPendingQ && p && p.q && p.q !== videoPendingQ) return;
        videoPendingQ = "";
        renderVideoSearch(p);
    });

    // ---------- evt：video.status ----------
    B.on("video.status", function (p) {
        p = p || {};
        var text = pick(p, ["text", "message", "msg"]);
        var isErr = p.error === true || p.level === "error" || p.type === "error";
        if (text) setStatus(isErr ? "error" : "info", String(text));
        else setStatus("info", "");
    });

    // ---------- evt：search.result（迟到的旧结果按 q 过滤） ----------
    B.on("search.result", function (p) {
        if (pendingQ && p && p.q && p.q !== pendingQ) return;
        pendingQ = "";
        renderSearchResult(p);
    });

    // ---------- evt：source.result（视频源导入完成） ----------
    B.on("source.result", function (p) {
        p = p || {};
        toast(pick(p, ["text", "message"]) || "视频源导入完成");
        // 首页还空着 → 顺手刷一次首页（有内容则不动，避免打断浏览）
        if (!videoHomeGot && !videoHomeBusy && inited) loadVideoHome();
    });

    // ================= 控件接线（全部走桥） =================

    // tab 切换
    $("tabbar").addEventListener("click", function (e) {
        var b = e.target.closest(".tab");
        if (b) switchTab(b.getAttribute("data-tab"));
    });

    // 搜索执行
    $("search-btn").addEventListener("click", doSearch);
    $("search-input").addEventListener("keydown", function (e) {
        if (e.key === "Enter") doSearch();
    });

    // 视频搜索执行
    $("video-search-btn").addEventListener("click", doVideoSearch);
    $("video-search-input").addEventListener("keydown", function (e) {
        if (e.key === "Enter") doVideoSearch();
    });

    // 视频搜索结果行 → 取详情 / 开播。左栏搜索结果保留（方便在结果间切换），
    // 剧集在右栏展开；清空搜索框即恢复推荐流。
    $("video-search-results").addEventListener("click", function (e) {
        // 返回首页：收起搜索结果，恢复推荐流（搜索框内容保留）。
        if (e.target.closest("#video-search-back")) {
            videoPendingQ = "";
            clearTimeout(videoSearchEvtTimer);
            setVideoSearchMode(false);
            return;
        }
        var row = e.target.closest(".result-row, .search-card");
        if (!row) return;
        var c = videoSearchResults[Number(row.getAttribute("data-idx"))];
        if (!c) return;
        openVideo(c);
    });

    // 搜索结果行 → 播放
    $("search-results").addEventListener("click", function (e) {
        var row = e.target.closest(".result-row");
        if (!row) return;
        var item = searchItems[Number(row.getAttribute("data-idx"))];
        if (item) playSearchItem(item);
    });

    // 音乐榜单卡片 → 拉曲目
    $("music-charts").addEventListener("click", function (e) {
        var card = e.target.closest(".card");
        if (!card) return;
        var s = musicSheets[Number(card.getAttribute("data-idx"))];
        if (s) openMusicSheet(s);
    });

    // 音乐曲目：单击只选中（高亮，不发请求），**双击哪一首都只把那一首**
    // 加入左侧播放列表并起播。用户 2026-11-10 明确要求：「音乐添加是双击
    // 哪个添加哪个，不是整个列表都加进来」。
    // 踩坑史（完整记录见 docs/三大反复问题_根因与修复记录.md）：
    //  v1 单击就发 player.play → 双击连发两次解析，都卡在插件串行锁；
    //  v2 单击只选中、双击才播，但把整张榜单 100 首一起发 → 串行解析 +
    //     一首失败整批 reply(false) → 播放列表一首都不加；
    //  v3 单击即播 + 整单懒解析入列 → 虽然修好了入列，但一次加 100 首，
    //     与用户「只加双击那一首」的期望相反。
    // 现在：payload 只带 {track: 单曲}，C++ 只解析这一首、只入列这一首。
    function playTrackAt(idx) {
        var item = musicItems[idx];
        if (!claimEnqueue(item)) return;   // 与搜索结果共用双击短时拦截
        // 宿主现在「立即入列 + 立即回执」，取链放到后台；回执里 pending=1
        // 表示音源仍在解析，声音随后以流式方式接上（不会完整下载）。
        toast("正在加入播放列表…");
        B.send("player.play", { track: item }, { timeout: 15000 })
         .then(function (r) {
            if (r && r.dedup) return;   // 被宿主去重：不重复提示
            if (r && r.warning) toast(r.warning);
            else if (r && r.pending)
                toast("已加入播放列表《" + (item.title || "") +
                      "》，正在解析音源…");
            else toast("已加入播放列表：" + (item.title || ""));
        }).catch(function (er) {
            toast("添加失败：" + errmsg(er));
        });
    }

    $("track-list").addEventListener("click", function (e) {
        var li = e.target.closest(".track");
        if (!li) return;
        Array.prototype.forEach.call(
            $("track-list").querySelectorAll(".track.active"),
            function (x) { x.classList.remove("active"); });
        li.classList.add("active");
    });

    $("track-list").addEventListener("dblclick", function (e) {
        var li = e.target.closest(".track");
        if (!li) return;
        playTrackAt(Number(li.getAttribute("data-idx")));
    });

    // 视频分类 chips → 「全部」切回首页推荐流；分类名逐站拉该分类影片
    $("video-cats").addEventListener("click", function (e) {
        var b = e.target.closest(".vcat");
        if (!b) return;
        var cat = b.getAttribute("data-cat");
        // 切分区后回到顶部（分区容器现在是独立滚动区，滚动位置会保留）。
        var secs = $("video-sections");
        if (secs) secs.scrollTop = 0;
        if (cat === VIDEO_CAT_ALL) {
            applyVideoCat(VIDEO_CAT_ALL);
            if (!homeSectionsCache.length) loadVideoHome();
        } else if (videoCats[cat]) {
            loadVideoCategory(cat);
        }
    });

    // 视频卡片 → 取详情 / 开播
    $("video-sections").addEventListener("click", function (e) {
        var card = e.target.closest(".card");
        if (!card) return;
        var c = videoCards[Number(card.getAttribute("data-idx"))];
        if (c) openVideo(c);
    });

    // 选集 → 开播
    $("episode-list").addEventListener("click", function (e) {
        var btn = e.target.closest(".episode");
        if (btn) playEpisode(btn);
    });

    // 导入视频源（顶栏按钮，仅视频页签显示；对话框模态：结果主要靠
    // evt source.result 提示，回执若超 5s 超时属预期，静默等事件即可）
    var btnImportVideo = $("btn-import-video");
    if (btnImportVideo) {
        btnImportVideo.addEventListener("click", function () {
            B.send("video.sources", {}).then(function (r) {
                if (r && (r.text || r.message)) toast(r.text || r.message);
            }).catch(function (e) {
                if (/timeout/i.test(errmsg(e))) return;
                toast("导入失败：" + errmsg(e));
            });
        });
    }

    // 导入音源插件（与视频源一样是模态对话框：回执要等用户关闭，
    // 超时属预期，静默处理）
    var btnImportMusic = $("btn-import-music");
    if (btnImportMusic) {
        btnImportMusic.addEventListener("click", function () {
            B.send("music.import", {}).then(function (r) {
                if (r && r.opened) toast("已打开音源导入对话框");
            }).catch(function (e) {
                if (/timeout/i.test(errmsg(e))) return;   // 模态对话框，等关闭
                toast("导入音源失败：" + errmsg(e));
            });
        });
    }

    // ================= 握手 =================
    var readyWatchdog = setTimeout(function () {
        if (!inited) setStatus("error", "等待宿主就绪超时（bridge.ready 未到达），请重启音乐窗");
    }, 12000);

    function handshake() {
        if (inited) return;
        inited = true; // 置位先行：app.init 失败时也留在真实模式并显示错误行
        clearTimeout(readyWatchdog);
        setStatus("info", "已连接宿主，正在初始化…");
        B.send("app.init", { ua: navigator.userAgent || "", version: "webui 1.0" })
            .then(function () {
                toast("已连接宿主");
                // 初始数据依次拉取（单步失败不阻断后续）
                return B.send("tab.switch", { tab: curTab }).catch(function () {});
            })
            .then(function () {
                // 只拉音乐首页：视频首页要 72 个站点并发聚合，切到「视频」页签时
                // 由 switchTab 按需触发（见 loadVideoHome 的调用点）。
                loadMusicDiscover();   // 音乐首页榜单/推荐（导入过音源才会有内容）
            })
            .catch(function (e) {
                setStatus("error", "初始化失败：" + errmsg(e));
            });
    }

    // 订阅就位（bridge.ready 先到则 on() 会补投存量事件）
    B.on("bridge.ready", handshake);
    B.ready.then(handshake);

    // ================= 真实模式初始占位（防白屏） =================
    setStatus("info", "正在连接宿主…");
    showSectionsLoading("正在连接宿主…");
    $("music-charts").innerHTML = '<div class="empty" style="grid-column:1/-1">正在连接宿主…</div>';
    $("track-list").innerHTML = '<li class="empty">暂无歌曲列表，可在右栏搜索歌曲播放</li>';
    $("episode-list").innerHTML = '<div class="empty" style="flex:1 1 100%">点击视频卡片加载选集</div>';
    searchEmpty("输入关键词开始搜索");
    setSearchMode(false);   // 初始显示曲目列表，搜索结果区隐藏
    videoSearchEmpty("输入关键词开始搜索");
    setVideoSearchMode(false);   // 初始显示选集列表，视频搜索结果区隐藏
    window.__REAL_RENDERED__ = true;
})();
