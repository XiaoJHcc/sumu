# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Directory-index + player pages for the web-streaming server. The layout/CSS is adapted from
# D:\Git\simple-http-video-server (the user's reference front-end, kept visually in sync); the
# one semantic difference is that a video card links to an HLS *live transcode* endpoint
# (/play/<relpath>) instead of a raw file, since sumu decensors on the fly.
from __future__ import annotations

import html as _html
import json as _json
from urllib.parse import quote as _quote


def _esc(s) -> str:
    return _html.escape(str(s))


VIDEO_EXT = {
    ".mp4", ".mkv", ".webm", ".avi", ".mov", ".m4v", ".ts", ".m2ts", ".wmv", ".flv",
    ".mpg", ".mpeg", ".3gp", ".ogv",
}


def is_video(name: str) -> bool:
    dot = name.rfind(".")
    if dot < 0:
        return False
    return name[dot:].lower() in VIDEO_EXT


def _qs(token: str) -> str:
    """`?token=...` query suffix (or '' when auth is off)."""
    return ("?token=" + _quote(token)) if token else ""


def _thumb_url(rel: str, token: str) -> str:
    """URL of a video's on-demand thumbnail endpoint, URL-encoded + token-qualified."""
    q = "/__thumb__/" + "/".join(_quote(seg) for seg in rel.split("/"))
    if token:
        q += "?token=" + _quote(token)
    return q


_PLACEHOLDER = (
    "data:image/svg+xml,"
    + _quote(
        '<svg xmlns="http://www.w3.org/2000/svg" width="480" height="300" viewBox="0 0 480 300">'
        '<rect width="480" height="300" fill="#121722"/>'
        '<circle cx="240" cy="135" r="36" fill="#2a3344"/>'
        '<path d="M230 117v36l30-18z" fill="#8ab4f8"/>'
        '<text x="240" y="205" text-anchor="middle" fill="#6b7280" '
        'font-family="system-ui,sans-serif" font-size="16">sumu</text></svg>',
        safe="",
    )
)


def _mode_button_html() -> str:
    """The 去码/原片 mode toggle button (shared by the directory index and the player page)."""
    return ('<button id="modebtn" class="pbtn modebtn" title="切换 AI 去码 / 原片直出" '
            'aria-label="切换去码">…</button>')


def _mode_js(token_js: str) -> str:
    """JS for #modebtn: fetch /mode to show the current mode, and toggle it via
    /mode?passthrough=0|1. A 503 (AI engine still warming up) shows a "正在预热…" hint. After a
    successful switch it calls window.onModeSwitch (if defined) so the player page can reload the
    stream at the current position."""
    return (
        "var MODE_TOK=%s;" % token_js
        + "function modeUrl(extra){var u='/mode',s='?';"
        + "if(MODE_TOK){u+=s+'token='+encodeURIComponent(MODE_TOK);s='&';}"
        + "if(extra!==undefined){u+=s+extra;}"
        + "return u;}"
        + "function setModeLabel(p){var b=document.getElementById('modebtn');"
        + "if(!b)return;b.textContent=p?'原片直出':'AI 去码';b.dataset.passthrough=p?'1':'0';"
        + "b.classList.toggle('on',!p);}"
        + "function showWarmup(){var b=document.getElementById('modebtn');"
        + "if(!b)return;b.textContent='正在预热…';b.dataset.passthrough='1';"
        + "setTimeout(function(){fetch(modeUrl()).then(function(r){return r.json();})"
        + ".then(function(m){setModeLabel(m.passthrough);}).catch(function(){});},2000);}"
        + "var _mb=document.getElementById('modebtn');"
        + "_mb.addEventListener('click',function(){"
        + "var p=_mb.dataset.passthrough==='1'?0:1;"
        + "fetch(modeUrl('passthrough='+p)).then(function(r){"
        + "if(r.status===503){showWarmup();return;}"
        + "if(!r.ok)return;"
        + "return r.json();})"
        + ".then(function(m){if(!m)return;setModeLabel(m.passthrough);"
        + "if(window.onModeSwitch){window.onModeSwitch(m.passthrough);}})"
        + ".catch(function(){});});"
        + "fetch(modeUrl()).then(function(r){return r.json();})"
        + ".then(function(m){setModeLabel(m.passthrough);}).catch(function(){});"
    )


def render_index(title: str, crumbs: list[tuple[str, str]], items: list[dict], token: str) -> str:
    """`items`: list of {name, is_dir, rel, size} sorted dirs-first. `crumbs`: [(label, rel)]."""
    root_href = "/" + ("?token=" + _quote(token) if token else "")
    crumb_html = '<a href="%s">root</a>' % root_href
    acc = ""
    for label, _rel in crumbs:
        acc = (acc + "/" if acc else "") + label
        crumb_html += ' <span>/</span> <a href="/browse/%s%s">%s</a>' % (
            _quote(acc), ("?token=" + _quote(token)) if token else "", _esc(label))

    cards = []
    for it in items:
        name = _esc(it["name"])
        badge = "文件夹" if it["is_dir"] else "视频"
        if it["is_dir"]:
            href = "/browse/%s%s" % (_quote(it["rel"]), _qs(token))
        else:
            href = "/play/%s%s" % (_quote(it["rel"]), _qs(token))
        size = "" if it["is_dir"] else _fmt_size(it.get("size"))
        thumb_rel = it.get("thumb")
        if thumb_rel:
            src = _thumb_url(thumb_rel, token)
            img = ('<img class="thumb" src="%s" alt="" loading="lazy" decoding="async" '
                   'onerror="this.onerror=null;this.src=\'%s\'" />' % (src, _PLACEHOLDER))
        else:
            img = '<img class="thumb" src="%s" alt="" />' % _PLACEHOLDER
        cards.append(
            '<a class="card" href="%s" title="%s">'
            '<div class="cover">%s<span class="badge">%s</span></div>'
            '<div class="meta"><div class="name">%s</div>%s</div></a>'
            % (href, name, img, badge, name,
               ('<div class="sub">%s</div>' % size) if size else "")
        )

    body = (
        '<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8"/>'
        '<meta name="viewport" content="width=device-width, initial-scale=1"/>'
        '<title>%s · sumu</title>%s</head><body>'
        '<main><header><div class="hrow"><h1>sumu 去码流媒体</h1>%s</div>'
        '<div class="crumb">%s</div></header>'
        '<div class="grid">%s</div>'
        '<footer>点击视频即实时去码直播（HLS）· 仅限同一局域网 · 端口请勿暴露公网</footer>'
        '</main><script>%s</script></body></html>'
    ) % (_esc(title), _CSS, _mode_button_html(), crumb_html,
         "\n".join(cards) if cards else '<div class="empty">空目录</div>',
         _mode_js(_json.dumps(token)))

    return body


def render_player(title: str, stream_rel: str, token: str) -> str:
    """Player page: hls.js (bundled) on desktop Chrome/Firefox/Edge, native HLS on Safari, over a
    custom seekbar that shows the FULL duration immediately (from /meta) and jumps by server
    reposition (`?start=<seconds>`), so seek works right away -- not only once the whole file has
    finished transcoding. Absolute time = basePos (server start/seek position) + video.currentTime
    (a 0-based offset thanks to EXT-X-START:TIME-OFFSET=0 injected by the server)."""
    qs = ("?token=" + _quote(token)) if token else ""
    hls_src = "/static/hls.min.js" + qs
    back = ("/?token=" + _quote(token)) if token else "/"
    rel_js = _json.dumps(stream_rel)
    token_js = _json.dumps(token)
    return (
        '<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8"/>'
        '<meta name="viewport" content="width=device-width, initial-scale=1"/>'
        '<title>%s · sumu</title>%s%s</head><body class="player">'
        '<main><header><h1>%s</h1><a class="back" href="%s">← 返回目录</a></header>'
        '<div class="stage"><video id="v" playsinline preload="auto"></video>'
        '<button id="bigplay" class="bigplay" title="播放" aria-label="播放">▶</button></div>'
        '<div class="ctl">'
        '<button id="playbtn" class="pbtn">▶ 播放</button>'
        '<span id="tcur" class="t">0:00</span>'
        '<input id="seek" type="range" min="0" max="100" step="0.1" value="0" aria-label="进度条"/>'
        '<span id="tdur" class="t">0:00</span>'
        '<button id="fsbtn" class="pbtn" title="全屏" aria-label="全屏">⛶</button>'
        '%s'
        '</div>'
        '<div class="ctl subrow"><span id="st" class="st">就绪</span></div>'
        '<p class="hint">桌面浏览器由内置 hls.js 播放（iOS Safari 原生 HLS）；拖动进度条即跳转，'
        '暂停/关闭页面即停止转码。右上角「AI 去码 / 原片直出」可随时切换。</p>'
        '</main>'
        '<script src="%s"></script>'
        '<script>%s</script>'
        '<script>%s</script>'
        '</body></html>'
    ) % (_esc(title), _CSS, _PLAYER_CSS, _esc(title), back, _mode_button_html(), hls_src,
         _player_js(rel_js, token_js), _mode_js(token_js))


def _player_js(rel_js: str, token_js: str) -> str:
    """Vanilla-JS player controller (rel/token are already JSON-encoded string literals).

    Seek is a *server reposition carried on the m3u8 URL* (?start=<seconds>): dragging the bar
    beyond the current stream's buffered/seekable range reloads the stream at the new absolute
    position synchronously inside the user gesture (video.load()+play()), which keeps iOS Safari
    autoplay working. Within the already-available range we seek locally (instant). Pause/end/close
    stop the transcode via /stop.
    """
    return (
        "var REL=%s,TOKEN=%s;" % (rel_js, token_js)
        + "var QS=TOKEN?('?token='+encodeURIComponent(TOKEN)):'';"
        + "var video=document.getElementById('v');"
        + "var seek=document.getElementById('seek');"
        + "var tcur=document.getElementById('tcur');"
        + "var tdur=document.getElementById('tdur');"
        + "var st=document.getElementById('st');"
        + "var playbtn=document.getElementById('playbtn');"
        + "var bigplay=document.getElementById('bigplay');"
        + "var fsbtn=document.getElementById('fsbtn');"
        + "var duration=0,basePos=0,hls=null,scrubbing=false,reloading=false,ended=false,isLive=true;"
        + "function fmt(s){s=Math.max(0,Math.floor(s||0));"
        + "var h=Math.floor(s/3600),m=Math.floor(s/60)-h*60,ss=s-h*3600-m*60;"
        + "function p(n){return (n<10?'0':'')+n;}"
        + "return h>0?(h+':'+p(m)+':'+p(ss)):(m+':'+p(ss));}"
        + "function clamp(t){t=parseFloat(t);if(!isFinite(t))t=0;t=Math.max(0,t);"
        + "if(duration>0)t=Math.min(duration,t);return t;}"
        + "function m3u8Url(pos){var u='/stream/'+REL+'/index.m3u8',s='?';"
        + "if(TOKEN){u+=s+'token='+encodeURIComponent(TOKEN);s='&';}"
        + "u+=s+'start='+pos+'&_='+Date.now();return u;}"
        + "function destroyHls(){if(hls){try{hls.destroy();}catch(e){}hls=null;}}"
        + "function showBig(){bigplay.style.display='block';}"
        + "function hideBig(){bigplay.style.display='none';}"
        + "function setUi(s){"
        + "if(s==='playing'){playbtn.textContent='❚❚ 暂停';hideBig();st.textContent='播放中';}"
        + "else if(s==='paused'){playbtn.textContent='▶ 播放';showBig();st.textContent='已暂停';}"
        + "else if(s==='buffering'){playbtn.textContent='❚❚ 暂停';st.textContent='缓冲中…';}"
        + "else if(s==='ended'){playbtn.textContent='↻ 重播';showBig();st.textContent='已结束';}"
        + "else{playbtn.textContent='▶ 播放';showBig();st.textContent='播放失败';}}"
        + "function playFrom(pos){"
        + "pos=clamp(pos);basePos=pos;ended=false;reloading=true;isLive=true;destroyHls();"
        + "video.removeAttribute('src');"
        + "var url=m3u8Url(basePos);"
        + "if(window.Hls&&Hls.isSupported()){"
        + "hls=new Hls({enableWorker:true});"
        + "hls.loadSource(url);hls.attachMedia(video);"
        + "hls.on(Hls.Events.LEVEL_LOADED,function(e,d){"
        + "if(d&&d.details){isLive=!!d.details.live;}});"
        + "hls.on(Hls.Events.MANIFEST_PARSED,function(){reloading=false;"
        + "var p=video.play();if(p&&p.then){p.then(function(){setUi('playing');},"
        + "function(){setUi('paused');});}else{setUi('playing');}});"
        + "hls.on(Hls.Events.ERROR,function(e,d){"
        + "if(d&&d.fatal){reloading=false;setUi('error');}});"
        + "setUi('buffering');}"
        + "else if(video.canPlayType('application/vnd.apple.mpegurl')){"
        + "video.src=url;video.load();reloading=false;"
        + "var p=video.play();if(p&&p.then){p.then(function(){setUi('playing');},"
        + "function(){setUi('paused');});}else{setUi('playing');}"
        + "setUi('buffering');}"
        + "else{reloading=false;setUi('error');"
        + "st.textContent='此浏览器不支持 HLS，请用 Chrome/Edge/Firefox 或 Safari';}}"
        + "function syncTime(){if(scrubbing||reloading)return;"
        + "var a=basePos+video.currentTime;if(!isFinite(a))return;"
        + "seek.value=a;tcur.textContent=fmt(a);}"
        + "function seekTo(t){t=clamp(t);var off=t-basePos;"
        + "if(!isLive&&video.seekable&&video.seekable.length){"
        + "for(var i=0;i<video.seekable.length;i++){"
        + "if(off>=video.seekable.start(i)-0.5&&off<=video.seekable.end(i)+0.5){"
        + "video.currentTime=off;syncTime();return;}}}"
        + "playFrom(t);}"
        + "bigplay.addEventListener('click',function(){"
        + "playFrom(ended?0:(basePos+video.currentTime));});"
        + "playbtn.addEventListener('click',function(){"
        + "if(video.paused||ended){playFrom(ended?0:(basePos+video.currentTime));}"
        + "else{video.pause();}});"
        + "fsbtn.addEventListener('click',function(){"
        + "var el=video.parentNode;"
        + "if(document.fullscreenElement||document.webkitFullscreenElement){"
        + "(document.exitFullscreen||document.webkitExitFullscreen).call(document);}"
        + "else if(el.requestFullscreen){el.requestFullscreen();}"
        + "else if(el.webkitRequestFullscreen){el.webkitRequestFullscreen();}});"
        + "video.addEventListener('timeupdate',syncTime);"
        + "video.addEventListener('progress',syncTime);"
        + "video.addEventListener('playing',function(){setUi('playing');});"
        + "video.addEventListener('waiting',function(){setUi('buffering');});"
        + "video.addEventListener('pause',function(){if(reloading)return;setUi('paused');"
        + "fetch('/stream/'+REL+'/stop'+QS).catch(function(){});});"
        + "video.addEventListener('ended',function(){ended=true;setUi('ended');"
        + "fetch('/stream/'+REL+'/stop'+QS).catch(function(){});});"
        + "seek.addEventListener('input',function(){scrubbing=true;"
        + "tcur.textContent=fmt(parseFloat(seek.value)||0);});"
        + "seek.addEventListener('change',function(){scrubbing=false;"
        + "var t=parseFloat(seek.value);if(isFinite(t))seekTo(t);});"
        + "window.addEventListener('pagehide',function(){"
        + "if(navigator.sendBeacon){navigator.sendBeacon('/stream/'+REL+'/stop'+QS);}});"
        + "fetch('/stream/'+REL+'/meta'+QS).then(function(r){return r.json();})"
        + ".then(function(m){if(m.duration&&m.duration>0){duration=m.duration;"
        + "seek.max=duration;tdur.textContent=fmt(duration);}"
        + "basePos=m.position||0;playFrom(basePos);})"
        + ".catch(function(){playFrom(0);});"
        + "window.onModeSwitch=function(){"
        + "playFrom(ended?0:(basePos+video.currentTime));};"
    )


def render_message(title: str, text: str) -> str:
    return (
        '<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8"/>'
        '<meta name="viewport" content="width=device-width, initial-scale=1"/>'
        '<title>%s</title>%s</head><body class="player">'
        '<main><header><h1>%s</h1></header><p class="hint">%s</p></main></body></html>'
    ) % (_esc(title), _CSS, _esc(title), _esc(text))


def _fmt_size(n) -> str:
    if n is None:
        return ""
    u = ["B", "KB", "MB", "GB", "TB"]
    i = 0
    v = float(n)
    while v >= 1024 and i < len(u) - 1:
        v /= 1024
        i += 1
    return ("%.1f" % v if (v < 10 and i > 0) else "%d" % round(v)) + " " + u[i]


_CSS = """
<style>
:root{color-scheme:dark;--bg:#0b0d12;--fg:#eef1f6;--muted:#8b93a7;--line:#232a3a;
--card:#121722;--card-hover:#182033;--link:#9ec1ff}
*{box-sizing:border-box}body{margin:0;font:14px/1.45 system-ui,"Segoe UI",sans-serif;
background:radial-gradient(1200px 500px at 10% -10%,rgba(59,130,246,.16),transparent 60%),
radial-gradient(900px 400px at 100% 0%,rgba(168,85,247,.10),transparent 55%),var(--bg);
color:var(--fg);min-height:100vh}
main{max-width:1280px;margin:0 auto;padding:28px 18px 56px}header{margin-bottom:22px}
h1{font-size:22px;font-weight:650;margin:0 0 8px;letter-spacing:-.02em}
.crumb{color:var(--muted);display:flex;flex-wrap:wrap;gap:6px;align-items:center}
.crumb a{color:var(--link);text-decoration:none}.crumb a:hover{text-decoration:underline}
.hrow{display:flex;align-items:center;gap:12px;flex-wrap:wrap}
.hrow h1{margin:0}.hrow .modebtn{margin-left:auto}
.modebtn.on{background:rgba(59,130,246,.18);border-color:#8ab4f8;color:#dbe7ff}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(168px,1fr));gap:14px}
.card{display:flex;flex-direction:column;background:var(--card);border:1px solid var(--line);
border-radius:14px;overflow:hidden;text-decoration:none;color:inherit;transition:transform .15s ease}
.card:hover{transform:translateY(-2px);background:var(--card-hover);border-color:#334155}
.cover{position:relative;aspect-ratio:16/10;background:#0e131c;overflow:hidden}
.thumb{width:100%;height:100%;object-fit:cover;display:block;background:#0e131c}
.badge{position:absolute;left:8px;top:8px;padding:2px 8px;border-radius:999px;font-size:11px;
color:#dbe7ff;background:rgba(15,23,42,.72);border:1px solid rgba(148,163,184,.25)}
.meta{padding:10px 11px 12px}.name{font-size:13px;font-weight:560;line-height:1.35;
display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical;overflow:hidden;
word-break:break-word;min-height:2.7em}.sub{margin-top:4px;color:var(--muted);font-size:12px}
.empty{grid-column:1/-1;color:var(--muted);text-align:center;padding:48px 16px;
border:1px dashed var(--line);border-radius:14px}footer{margin-top:22px;color:var(--muted);font-size:12px}
.player video{width:100%;max-height:70vh;background:#000;border-radius:12px;border:1px solid var(--line)}
.back{color:var(--link);text-decoration:none;font-size:14px}.hint{color:var(--muted);font-size:13px}
@media(max-width:560px){.grid{grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:10px}
main{padding:18px 12px 40px}}
</style>
"""

_PLAYER_CSS = """
<style>
.stage{position:relative;background:#000;border-radius:12px;border:1px solid var(--line);overflow:hidden}
.stage video{width:100%;max-height:70vh;display:block;background:#000;object-fit:contain}
.stage:fullscreen video,.stage:-webkit-full-screen video{max-height:100vh;height:100vh}
.bigplay{position:absolute;left:50%;top:50%;transform:translate(-50%,-50%);width:72px;height:72px;
border-radius:50%;border:1px solid rgba(255,255,255,.35);background:rgba(0,0,0,.55);color:#fff;
font-size:26px;cursor:pointer;line-height:1}
.bigplay:hover{background:rgba(0,0,0,.75)}
.ctl{display:flex;align-items:center;gap:10px;margin-top:12px;flex-wrap:wrap}
.ctl.subrow{margin-top:6px}
.pbtn{background:var(--card);color:var(--fg);border:1px solid var(--line);border-radius:8px;
padding:6px 14px;font-size:14px;cursor:pointer}
.pbtn:hover{border-color:#334155}
#seek{flex:1;min-width:180px;accent-color:#8ab4f8}
.t{font-variant-numeric:tabular-nums;color:var(--muted);font-size:13px}
.st{font-size:12px;color:var(--muted);min-width:64px;text-align:right}
</style>
"""
