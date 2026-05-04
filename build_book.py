"""
将 novel/ 目录下的 chapter_1 到 chapter_100 合并，
生成一个自包含的精美 HTML 在线阅读网站。
"""
import os
import re
import html

NOVEL_DIR = os.path.join(os.path.dirname(__file__), "novel")
OUTPUT_HTML = os.path.join(os.path.dirname(__file__), "index.html")

# ── 书名 & 作者 ──
BOOK_TITLE = "重生之：满血 Gemini 3.1 Pro 降临 2015"
BOOK_AUTHOR = "匿名作者"

def extract_chapter_number(filename: str) -> int:
    """从文件名提取章节序号，如 chapter_42_aftermath.md → 42"""
    m = re.match(r"chapter_(\d+)", filename, re.IGNORECASE)
    return int(m.group(1)) if m else 9999

def find_chapter_files() -> list[str]:
    """按序号升序返回 chapter_1 .. chapter_100 的文件名列表"""
    all_md = [f for f in os.listdir(NOVEL_DIR) if f.endswith(".md")]
    # 只保留 chapter_ 开头的
    chapters = [f for f in all_md if re.match(r"chapter_\d+", f, re.IGNORECASE)]
    chapters.sort(key=extract_chapter_number)
    # 只取 1-100
    chapters = [c for c in chapters if 1 <= extract_chapter_number(c) <= 100]
    return chapters

def clean_title(raw_title: str) -> str:
    """去掉 # 号和多余空格，保留纯标题文本"""
    t = re.sub(r"^#+\s*", "", raw_title).strip()
    # 去掉可能的 html 标签
    t = html.escape(t)
    return t

def chapter_to_html(md_path: str) -> str:
    """将单个 markdown 章节转为 HTML 片段（极简解析）"""
    with open(md_path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    html_parts = []
    # 找第一行 # 标题
    title = ""
    content_start = 0
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("# ") and not title:
            title = clean_title(stripped)
            content_start = i + 1
            break

    if not title:
        title = os.path.basename(md_path)

    html_parts.append(f'<h2 class="ch-title">{title}</h2>')

    # 处理内容行
    in_paragraph = False
    for line in lines[content_start:]:
        stripped = line.strip()

        # 空行 → 结束当前段落
        if not stripped:
            if in_paragraph:
                html_parts.append("</p>")
                in_paragraph = False
            continue

        # 子标题 ### / ##
        if stripped.startswith("## ") or stripped.startswith("### "):
            if in_paragraph:
                html_parts.append("</p>")
                in_paragraph = False
            level = 3 if stripped.startswith("### ") else 2
            sub = clean_title(stripped)
            html_parts.append(f'<h{level} class="ch-subtitle">{sub}</h{level}>')
            continue

        # 分隔线
        if stripped == "---" or stripped == "***" or stripped == "* * *":
            if in_paragraph:
                html_parts.append("</p>")
                in_paragraph = False
            html_parts.append('<hr class="ch-divider">')
            continue

        # 粗体 **text**
        text = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", stripped)
        text = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", text)  # 去掉链接
        text = html.escape(text, quote=False)
        # 重新补回 <strong>
        text = text.replace("&lt;strong&gt;", "<strong>").replace("&lt;/strong&gt;", "</strong>")

        if not in_paragraph:
            html_parts.append('<p class="ch-text">')
            in_paragraph = True
            html_parts.append(text)
        else:
            html_parts.append("<br>" + text)

    if in_paragraph:
        html_parts.append("</p>")

    return "\n".join(html_parts)


def generate_book_html():
    chapters = find_chapter_files()
    print(f"Found {len(chapters)} chapters (1-100)")

    # 构建目录 HTML
    toc_items = []
    chapter_bodies = []
    for i, fname in enumerate(chapters):
        num = extract_chapter_number(fname)
        # 读取第一行拿标题
        path = os.path.join(NOVEL_DIR, fname)
        with open(path, "r", encoding="utf-8") as f:
            first = f.readline().strip()
        title = clean_title(first) if first.startswith("#") else f"第{num}章"
        anchor = f"ch{num}"

        toc_items.append(f'<li><a href="#{anchor}" onclick="jumpTo(\'{anchor}\')">{title}</a></li>')

        body_html = chapter_to_html(path)
        chapter_bodies.append(f'<section id="{anchor}" class="chapter">\n{body_html}\n</section>')

    toc_html = "\n".join(toc_items)
    bodies_html = "\n\n".join(chapter_bodies)

    # ── 完整 HTML ──
    html_content = f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>{BOOK_TITLE}</title>
<style>
  :root {{
    --bg: #f5f0e8;
    --text: #2c2416;
    --accent: #8b4513;
    --sidebar-bg: #1a1a2e;
    --sidebar-text: #e0d7c6;
    --sidebar-hover: #e6a817;
    --card-bg: #fefcf7;
    --shadow: 0 2px 12px rgba(0,0,0,.08);
    --border: #d4c5a9;
  }}
  * {{ margin:0; padding:0; box-sizing:border-box; }}
  body {{
    font-family: "Noto Serif SC", "Source Han Serif SC", "SimSun", "STSong", Georgia, "Times New Roman", serif;
    background: var(--bg);
    color: var(--text);
    display: flex;
    min-height: 100vh;
  }}

  /* ── 侧边栏 ── */
  .sidebar {{
    width: 280px;
    min-width: 280px;
    background: var(--sidebar-bg);
    color: var(--sidebar-text);
    position: fixed;
    top: 0; left: 0; bottom: 0;
    overflow-y: auto;
    z-index: 100;
    padding: 20px 0;
    box-shadow: 2px 0 20px rgba(0,0,0,.3);
  }}
  .sidebar-header {{
    text-align: center;
    padding: 0 16px 16px;
    border-bottom: 1px solid rgba(255,255,255,.15);
    margin-bottom: 8px;
  }}
  .sidebar-header h1 {{
    font-size: 1.25rem;
    font-weight: 700;
    color: var(--sidebar-hover);
    line-height: 1.5;
  }}
  .sidebar-header .author {{
    font-size: .85rem;
    opacity: .7;
    margin-top: 4px;
  }}
  .sidebar nav ul {{
    list-style: none;
  }}
  .sidebar nav ul li a {{
    display: block;
    padding: 6px 20px;
    color: var(--sidebar-text);
    text-decoration: none;
    font-size: .9rem;
    transition: all .2s;
    border-left: 3px solid transparent;
  }}
  .sidebar nav ul li a:hover {{
    background: rgba(255,255,255,.08);
    color: var(--sidebar-hover);
    border-left-color: var(--sidebar-hover);
  }}

  /* ── 汉堡菜单 (移动端) ── */
  .hamburger {{
    display: none;
    position: fixed;
    top: 12px; left: 12px;
    z-index: 200;
    background: var(--sidebar-bg);
    color: #fff;
    border: none;
    font-size: 1.5rem;
    width: 40px; height: 40px;
    border-radius: 6px;
    cursor: pointer;
    box-shadow: 0 2px 8px rgba(0,0,0,.3);
  }}

  /* ── 主内容区 ── */
  .main {{
    margin-left: 280px;
    flex: 1;
    padding: 40px 48px;
    max-width: 900px;
  }}
  .main .book-title-page {{
    text-align: center;
    padding: 60px 20px 40px;
    border-bottom: 2px solid var(--border);
    margin-bottom: 40px;
  }}
  .main .book-title-page h1 {{
    font-size: 2rem;
    color: var(--accent);
    margin-bottom: 8px;
  }}
  .main .book-title-page .author {{
    font-size: 1.1rem;
    opacity: .6;
  }}

  /* ── 章节样式 ── */
  .chapter {{
    background: var(--card-bg);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 32px 36px;
    margin-bottom: 32px;
    box-shadow: var(--shadow);
  }}
  .ch-title {{
    font-size: 1.5rem;
    color: var(--accent);
    margin-bottom: 20px;
    padding-bottom: 10px;
    border-bottom: 1px solid var(--border);
    text-align: center;
  }}
  .ch-subtitle {{
    font-size: 1.15rem;
    color: #5a3e1a;
    margin: 18px 0 10px;
  }}
  .ch-text {{
    font-size: 1.05rem;
    line-height: 2;
    text-indent: 2em;
    margin-bottom: 8px;
    text-align: justify;
  }}
  .ch-divider {{
    border: none;
    border-top: 1px dashed var(--border);
    margin: 16px 0;
  }}

  /* ── 回到顶部 ── */
  .back-top {{
    position: fixed;
    bottom: 24px; right: 24px;
    width: 44px; height: 44px;
    background: var(--accent);
    color: #fff;
    border: none;
    border-radius: 50%;
    font-size: 1.2rem;
    cursor: pointer;
    box-shadow: 0 2px 10px rgba(0,0,0,.2);
    display: none;
    z-index: 150;
  }}
  .back-top:hover {{ opacity: .85; }}

  /* ── 响应式 ── */
  @media (max-width: 768px) {{
    .sidebar {{
      transform: translateX(-100%);
      transition: transform .3s;
    }}
    .sidebar.open {{
      transform: translateX(0);
    }}
    .hamburger {{ display: block; }}
    .main {{
      margin-left: 0;
      padding: 20px 12px;
    }}
    .chapter {{
      padding: 18px 14px;
    }}
    .ch-title {{ font-size: 1.25rem; }}
    .ch-text {{ font-size: 1rem; line-height: 1.9; }}
  }}
</style>
</head>
<body>

<button class="hamburger" onclick="toggleSidebar()" aria-label="目录">☰</button>

<aside class="sidebar" id="sidebar">
  <div class="sidebar-header">
    <h1>{BOOK_TITLE}</h1>
    <div class="author">{BOOK_AUTHOR}</div>
  </div>
  <nav>
    <ul>
{toc_html}
    </ul>
  </nav>
</aside>

<main class="main" id="main">
  <div class="book-title-page">
    <h1>{BOOK_TITLE}</h1>
    <div class="author">{BOOK_AUTHOR}</div>
    <p style="margin-top:12px;opacity:.5;font-size:.9rem;">全 100 章 · 在线阅读</p>
  </div>

{bodies_html}
</main>

<button class="back-top" id="backTop" onclick="scrollToTop()" title="回到顶部">↑</button>

<div class="sidebar-overlay" id="overlay" onclick="toggleSidebar()"></div>

<script>
  // ── 侧边栏切换 ──
  function toggleSidebar() {{
    document.getElementById('sidebar').classList.toggle('open');
  }}

  // ── 跳转到章节 ──
  function jumpTo(anchor) {{
    var el = document.getElementById(anchor);
    if (el) {{
      el.scrollIntoView({{ behavior: 'smooth', block: 'start' }});
    }}
    // 移动端自动关闭侧边栏
    document.getElementById('sidebar').classList.remove('open');
  }}

  // ── 回到顶部按钮 ──
  var backTop = document.getElementById('backTop');
  window.addEventListener('scroll', function() {{
    backTop.style.display = window.scrollY > 400 ? 'block' : 'none';
  }});
  function scrollToTop() {{
    window.scrollTo({{ top: 0, behavior: 'smooth' }});
  }}

  // ── 记住阅读位置 ──
  (function() {{
    var saved = localStorage.getItem('novel_scroll');
    if (saved) {{
      var pos = parseInt(saved, 10);
      if (pos > 100) setTimeout(function() {{ window.scrollTo(0, pos); }}, 200);
    }}
    window.addEventListener('beforeunload', function() {{
      localStorage.setItem('novel_scroll', window.scrollY);
    }});
  }})();
</script>

</body>
</html>"""

    with open(OUTPUT_HTML, "w", encoding="utf-8") as f:
        f.write(html_content)

    # 统计大小
    size_kb = os.path.getsize(OUTPUT_HTML) / 1024
    print(f"✅ 生成完成: {OUTPUT_HTML}  ({size_kb:.1f} KB)")


if __name__ == "__main__":
    generate_book_html()
