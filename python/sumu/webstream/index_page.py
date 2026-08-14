# SPDX-FileCopyrightText: sumu Authors
# SPDX-License-Identifier: AGPL-3.0
#
# Directory-index + player pages for the web-streaming server. The layout/CSS is adapted from
# D:\Git\simple-http-video-server (the user's reference front-end, kept visually in sync); the
# one semantic difference is that a video card links to an HLS *live transcode* endpoint
# (/play/<relpath>) instead of a raw file, since sumu decensors on the fly.
from __future__ import annotations

import html as _html
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


def _url(rel_path: str, token: str) -> str:
    """URL-encode a relative path and append the token query (if any)."""
    q = "/" + "/".join(_quote(seg) for seg in rel_path.split("/"))
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
            href = "/browse/%s%s" % (_quote(it["rel"]), ("?token=" + _quote(token)) if token else "")
        else:
            href = "/play/%s%s" % (_quote(it["rel"]), ("?token=" + _quote(token)) if token else "")
        size = "" if it["is_dir"] else _fmt_size(it.get("size"))
        cards.append(
            '<a class="card" href="%s" title="%s">'
            '<div class="cover"><img src="%s" alt=""/><span class="badge">%s</span></div>'
            '<div class="meta"><div class="name">%s</div>%s</div></a>'
            % (href, name, _PLACEHOLDER, badge, name,
               ('<div class="sub">%s</div>' % size) if size else "")
        )

    body = (
        '<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8"/>'
        '<meta name="viewport" content="width=device-width, initial-scale=1"/>'
        '<title>%s · sumu</title>%s</head><body>'
        '<main><header><h1>sumu 去码流媒体</h1><div class="crumb">%s</div></header>'
        '<div class="grid">%s</div>'
        '<footer>点击视频即实时去码直播（HLS）· 仅限同一局域网 · 端口请勿暴露公网</footer>'
        '</main></body></html>'
    ) % (_esc(title), _CSS, crumb_html,
         "\n".join(cards) if cards else '<div class="empty">空目录</div>')

    return body


def render_player(title: str, stream_rel: str, token: str) -> str:
    src = "/stream/%s/index.m3u8%s" % (_quote(stream_rel), ("?token=" + _quote(token)) if token else "")
    back = ("/?token=" + _quote(token)) if token else "/"
    return (
        '<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8"/>'
        '<meta name="viewport" content="width=device-width, initial-scale=1"/>'
        '<title>%s · sumu</title>%s</head><body class="player">'
        '<main><header><h1>%s</h1><a class="back" href="%s">← 返回目录</a></header>'
        '<video controls autoplay playsinline src="%s"></video>'
        '<p class="hint">iOS Safari 原生播放 HLS；桌面浏览器需支持 HLS（Safari）或安装 hls.js。</p>'
        '</main></body></html>'
    ) % (_esc(title), _CSS, _esc(title), back, src)


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
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(168px,1fr));gap:14px}
.card{display:flex;flex-direction:column;background:var(--card);border:1px solid var(--line);
border-radius:14px;overflow:hidden;text-decoration:none;color:inherit;transition:transform .15s ease}
.card:hover{transform:translateY(-2px);background:var(--card-hover);border-color:#334155}
.cover{position:relative;aspect-ratio:16/10;background:#0e131c;overflow:hidden}
.cover img{width:100%;height:100%;object-fit:cover;display:block}
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
