#!/usr/bin/env python3
"""Build script for 'A Heavily Commented ABOS Microkernel Source Code'.

Responsibilities:
  1. Verify all 19 chapter Markdown files in book/chapters/ exist in order.
  2. Perform a 100% byte-accurate / line-accurate verbatim source coverage audit
     across all 58 non-doc source, header, linker, and build files in lesson_20/.
  3. Assemble the complete single-file Markdown edition (book/ABOS_Book.md) with
     Front Matter, visual embeds, and a linked Table of Contents.
  4. Render the print-ready A4 PDF edition (book/ABOS_Book.pdf) modeled after
     Dr. Zhao Jiong's 'A Heavily Commented Linux Kernel Source Code (ECLK 5.0)',
     using a two-pass headless Chrome build for exact two-column TOC page numbers,
     per-chapter running headers, and blue-underlined source line numbers.

Pure Python 3 standard library only (requires google-chrome/chromium + pdftotext or read_pdf_cli).
"""

from __future__ import annotations

import argparse
import html
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.parse

BOOK_DIR = os.path.dirname(os.path.abspath(__file__))
ABOS_ROOT = os.path.dirname(BOOK_DIR)
LESSON20_DIR = os.path.join(ABOS_ROOT, "lesson_20")
CHAPTERS_DIR = os.path.join(BOOK_DIR, "chapters")
ASSETS_DIR = os.path.join(BOOK_DIR, "assets")

OUT_MD = os.path.join(BOOK_DIR, "ABOS_Book.md")
OUT_PDF = os.path.join(BOOK_DIR, "ABOS_Book.pdf")

EXPECTED_CHAPTERS = [
    "00_preface.md",
    "01_overview.md",
    "02_hardware.md",
    "03_toolchain.md",
    "04_armv8_arch.md",
    "05_microkernel_arch.md",
    "06_booting.md",
    "07_init_main.md",
    "08_exceptions_sched.md",
    "09_memory_mgmt.md",
    "10_objects_ipc.md",
    "11_syscalls.md",
    "12_headers.md",
    "13_ulibc.md",
    "14_system_servers.md",
    "15_shell_apps.md",
    "16_build_system.md",
    "17_lab_debugging.md",
    "18_references_appendix.md",
]

SKIP_EXTS = {
    ".o",
    ".a",
    ".elf",
    ".bin",
    ".img",
    ".tar",
    ".dtb",
    ".d",
    ".map",
    ".sym",
}


# ---------------------------------------------------------------------------
# 1. Verbatim Source Coverage Audit
# ---------------------------------------------------------------------------


def discover_lesson20_files() -> dict[str, list[str]]:
    """Return {rel_path: [line1, line2, ...]} for all source/build files in lesson_20/."""
    inventory: dict[str, list[str]] = {}
    for dirpath, dirnames, filenames in os.walk(LESSON20_DIR):
        dirnames[:] = [
            d for d in sorted(dirnames) if d not in ("build", "__pycache__", ".git")
        ]
        for fn in sorted(filenames):
            if fn == "STUDY_BOOK.md" or fn.startswith("."):
                continue
            ext = os.path.splitext(fn)[1]
            if ext in SKIP_EXTS:
                continue
            abs_p = os.path.join(dirpath, fn)
            rel_p = os.path.relpath(abs_p, LESSON20_DIR).replace(os.sep, "/")
            with open(abs_p, "r", encoding="utf-8") as fh:
                inventory[rel_p] = [ln.rstrip("\r\n") for ln in fh.readlines()]
    return inventory


ECLK_META_RE = re.compile(
    r'<!--\s*ECLK_CODE_LISTING\s+file="([^"]+)"\s+'
    r'start="(\d+)"\s+end="(\d+)"\s+total="(\d+)"\s*-->'
)
CODE_LINE_RE = re.compile(r"^\s*(\d+)\s*\|(?: (.*))?$")


def audit_source_coverage(chapters_text: list[tuple[str, str]]) -> tuple[bool, str]:
    """Verify every file and line in lesson_20/ appears verbatim in ECLK_CODE_LISTING blocks."""
    expected = discover_lesson20_files()
    # Map rel_path -> {line_no: (chapter_name, extracted_text)}
    covered: dict[str, dict[int, tuple[str, str]]] = {k: {} for k in expected}
    errors: list[str] = []
    listing_count = 0

    for chap_fn, md in chapters_text:
        lines = md.splitlines()
        i = 0
        n = len(lines)
        while i < n:
            m = ECLK_META_RE.search(lines[i])
            if not m:
                i += 1
                continue

            listing_count += 1
            raw_file, s_str, e_str, tot_str = m.groups()
            start_ln, end_ln, decl_total = int(s_str), int(e_str), int(tot_str)
            rel_path = raw_file
            if rel_path.startswith("lesson_20/"):
                rel_path = rel_path[len("lesson_20/") :]

            if rel_path not in expected:
                errors.append(
                    f"[{chap_fn}] Unknown file in ECLK_CODE_LISTING: '{raw_file}'"
                )
                i += 1
                continue

            disk_lines = expected[rel_path]
            if decl_total != len(disk_lines):
                errors.append(
                    f"[{chap_fn}] {rel_path}: declared total={decl_total}, disk has {len(disk_lines)} lines"
                )

            # Advance to opening code fence
            i += 1
            while i < n and lines[i].strip() == "":
                i += 1
            if i >= n or not lines[i].lstrip().startswith("```"):
                errors.append(
                    f"[{chap_fn}] {rel_path} ({start_ln}..{end_ln}): missing opening ``` fence after ECLK_CODE_LISTING"
                )
                continue

            i += 1  # step inside code block
            seen_in_block: list[int] = []
            while i < n and not lines[i].lstrip().startswith("```"):
                cl = CODE_LINE_RE.match(lines[i])
                if cl:
                    lno = int(cl.group(1))
                    code_str = cl.group(2) if cl.group(2) is not None else ""
                    seen_in_block.append(lno)
                    if not (1 <= lno <= len(disk_lines)):
                        errors.append(
                            f"[{chap_fn}] {rel_path}: line number {lno} out of range 1..{len(disk_lines)}"
                        )
                    else:
                        disk_str = disk_lines[lno - 1]
                        if code_str.rstrip() != disk_str.rstrip():
                            errors.append(
                                f"[{chap_fn}] {rel_path}:{lno} mismatch:\n"
                                f"    book: {code_str!r}\n"
                                f"    disk: {disk_str!r}"
                            )
                        covered[rel_path][lno] = (chap_fn, code_str)
                i += 1

            expected_slice = list(range(start_ln, end_ln + 1))
            if seen_in_block != expected_slice:
                errors.append(
                    f"[{chap_fn}] {rel_path}: expected lines {start_ln}..{end_ln} "
                    f"({len(expected_slice)} lines), got {len(seen_in_block)} lines "
                    f"({seen_in_block[:3]}..{seen_in_block[-3:] if seen_in_block else []})"
                )
            i += 1

    # Verify complete coverage across all expected files
    total_expected_lines = 0
    total_covered_lines = 0
    for rel_path, disk_lines in sorted(expected.items()):
        n_lines = len(disk_lines)
        total_expected_lines += n_lines
        cov_map = covered[rel_path]
        missing = [ln for ln in range(1, n_lines + 1) if ln not in cov_map]
        total_covered_lines += n_lines - len(missing)
        if missing:
            if len(missing) == n_lines:
                errors.append(
                    f"[MISSING FILE] lesson_20/{rel_path} ({n_lines} lines) is completely absent!"
                )
            else:
                preview = ", ".join(str(x) for x in missing[:10])
                more = f" (+{len(missing) - 10} more)" if len(missing) > 10 else ""
                errors.append(
                    f"[INCOMPLETE FILE] lesson_20/{rel_path}: missing {len(missing)}/{n_lines} lines: {preview}{more}"
                )

    summary = (
        f"Source Coverage Audit: {len(expected)} files, "
        f"{total_covered_lines}/{total_expected_lines} lines covered across "
        f"{listing_count} Program listings."
    )
    if errors:
        detail = "\n".join(["  - " + e for e in errors[:40]])
        if len(errors) > 40:
            detail += f"\n  ... and {len(errors) - 40} additional errors."
        return False, f"{summary}\nAUDIT FAILED ({len(errors)} issues):\n{detail}"
    return True, f"{summary}\nAUDIT PASSED: 100% verbatim coverage verified!"


# ---------------------------------------------------------------------------
# 2. Heading & Slug Utilities + Combined Markdown Generation
# ---------------------------------------------------------------------------


def slugify(text: str, used: set[str]) -> str:
    clean = re.sub(r"`([^`]*)`", r"\1", text)
    clean = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", clean)
    clean = re.sub(r"[^a-zA-Z0-9]+", "-", clean.strip().lower()).strip("-")
    if not clean:
        clean = "section"
    candidate = clean
    idx = 2
    while candidate in used:
        candidate = f"{clean}-{idx}"
        idx += 1
    used.add(candidate)
    return candidate


def normalize_chapter_assets_for_book_md(md: str) -> str:
    """Rewrite ../assets/... or absolute book/assets/... paths to assets/... for book/ABOS_Book.md."""
    md = md.replace("](../assets/", "](assets/")
    md = md.replace(f"]({ASSETS_DIR}/", "](assets/")
    md = md.replace(f"](file://{ASSETS_DIR}/", "](assets/")
    return md


def extract_toc_headings_from_chapters(
    chapters_text: list[tuple[str, str]],
) -> list[tuple[int, str, str]]:
    """Extract (level, raw_title, github_anchor) for H1 and H2 headings for ABOS_Book.md TOC."""
    entries: list[tuple[int, str, str]] = []
    used_slugs: set[str] = set()
    in_fence = False
    for _, md in chapters_text:
        for line in md.splitlines():
            s = line.lstrip()
            if s.startswith("```") or s.startswith("~~~"):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            m = re.match(r"^(#{1,4})\s+(.+?)\s*$", line)
            if m:
                lvl = len(m.group(1))
                title = m.group(2).strip()
                slug = slugify(title, used_slugs)
                if lvl in (1, 2):
                    entries.append((lvl, title, slug))
    return entries


def build_combined_markdown(chapters_text: list[tuple[str, str]]) -> str:
    """Assemble book/ABOS_Book.md with Front Matter, Markdown TOC, and all 19 chapters."""
    normalized_chaps = [
        (fn, normalize_chapter_assets_for_book_md(md)) for fn, md in chapters_text
    ]
    toc_entries = extract_toc_headings_from_chapters(normalized_chaps)

    front = [
        "# A Heavily Commented ABOS Microkernel Source Code",
        "",
        "**ABOS Version 1.0 (Lesson 20 Complete Edition) — ARMv8-A AArch64 Capability Microkernel**",
        "",
        "**Author:** Liping Liu (`lipingliu@google.com`)  ",
        "**Date:** 2026-10-29",
        "",
        "![Cover Photo](assets/cover_photo.jpg)",
        "",
        "---",
        "",
        "<div align=\"center\">",
        "",
        "![ABOS Mascot](assets/abos_mascot.jpg)",
        "",
        "### *“RTFSC – Read The F\\*\\*king Source Code :)!”*",
        "— *Linus Benedict Torvalds*",
        "",
        "</div>",
        "",
        "---",
        "",
        "## Abstract",
        "",
        "This book provides an exhaustive, line-by-line architectural and source-code commentary on **ABOS** "
        "(ARM Bare-metal Operating System, Version 1.0 / Lesson 20 Complete Edition) — a two-layer, capability-based, "
        "4-core SMP ARMv8-A (AArch64) microkernel operating system inspired by Google's Fuchsia Zircon architecture "
        "and modeled in structure and spirit after Dr. Zhao Jiong's classic *A Heavily Commented Linux Kernel Source Code*.",
        "",
        "Every single source file, header file, assembly routine, linker script, build Makefile, and image-packaging tool "
        "in `lesson_20/` (58 files, 9,526 lines in total) is reproduced verbatim with numbered lines and interleaved "
        "explanation blocks covering hardware register protocols, SMP synchronization invariants, 4-level page-table "
        "management, capability rights enforcement, asynchronous port/channel IPC, and user-space server design.",
        "",
        "**Copyright Statement:** © 2026 by Liping Liu (`lipingliu@google.com`). Prepared for OS learners, systems engineers, "
        "and microkernel researchers.",
        "",
        "---",
        "",
        "## Table of Contents",
        "",
    ]

    for lvl, title, slug in toc_entries:
        disp = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", title)
        if lvl == 1:
            front.append(f"- **[{disp}](#{slug})**")
        else:
            front.append(f"  - [{disp}](#{slug})")

    front.append("")
    front.append("---")
    front.append("")

    parts = ["\n".join(front)]
    for _, md in normalized_chaps:
        parts.append(md.strip() + "\n\n---\n")

    return "\n".join(parts)


# ---------------------------------------------------------------------------
# 3. Markdown -> ECLK-Styled HTML Converter (Stdlib Only)
# ---------------------------------------------------------------------------


def resolve_img_src_for_html(src: str) -> str:
    """Resolve relative image paths to absolute file:// URIs for headless Chrome."""
    src = src.strip()
    if src.startswith(("http://", "https://", "data:", "file://")):
        return src
    cleaned = src
    while cleaned.startswith("../"):
        cleaned = cleaned[3:]
    if cleaned.startswith("./"):
        cleaned = cleaned[2:]
    cand = os.path.join(BOOK_DIR, cleaned)
    if os.path.exists(cand):
        return "file://" + urllib.parse.quote(os.path.abspath(cand))
    cand2 = os.path.join(ABOS_ROOT, cleaned)
    if os.path.exists(cand2):
        return "file://" + urllib.parse.quote(os.path.abspath(cand2))
    return src


def format_inline(text: str) -> str:
    """Convert inline Markdown (code spans, links, images, bold, italic) safely to HTML."""
    # Strip HTML comments
    text = re.sub(r"<!--.*?-->", "", text)

    # Extract inline code spans (`...` or ``...``) first so their contents are untouched
    code_tokens: list[str] = []

    def _save_code(m: re.Match) -> str:
        inner = m.group(2)
        if inner.startswith(" ") and inner.endswith(" ") and len(inner) > 2:
            inner = inner[1:-1]
        idx = len(code_tokens)
        code_tokens.append(f"<code>{html.escape(inner)}</code>")
        return f"\x00CODE{idx}\x00"

    text = re.sub(r"(`+)(.+?)\1", _save_code, text)

    # Extract inline images ![alt](src)
    img_tokens: list[str] = []

    def _save_img(m: re.Match) -> str:
        alt_txt = html.escape(m.group(1))
        src_url = html.escape(resolve_img_src_for_html(m.group(2)))
        idx = len(img_tokens)
        img_tokens.append(f'<img src="{src_url}" alt="{alt_txt}" class="inline-img">')
        return f"\x00IMG{idx}\x00"

    text = re.sub(r"!\[([^\]]*)\]\(([^)]+)\)", _save_img, text)

    # Extract links [label](url)
    link_tokens: list[str] = []

    def _save_link(m: re.Match) -> str:
        lbl = format_inline(m.group(1))
        href = m.group(2).strip()
        # Keep internal #anchor links working; external file/http links rendered cleanly
        href_esc = html.escape(href)
        idx = len(link_tokens)
        if href.startswith("#"):
            link_tokens.append(f'<a href="{href_esc}">{lbl}</a>')
        else:
            link_tokens.append(f'<a href="{href_esc}" class="ext-link">{lbl}</a>')
        return f"\x00LINK{idx}\x00"

    text = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", _save_link, text)

    # Escape HTML special chars in remaining prose
    text = html.escape(text, quote=False)

    # Bold + Italic (***...***)
    text = re.sub(r"\*\*\*(.+?)\*\*\*", r"<strong><em>\1</em></strong>", text)
    # Bold (**...**)
    text = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", text)
    # Italic (*...*), cautious not to match spaced multiplication a * b * c
    text = re.sub(
        r"(?<!\*)\*(?!\s)([^*\n]+?)(?<!\s)\*(?!\*)", r"<em>\1</em>", text
    )

    # Restore links, images, and code spans
    for idx, val in enumerate(link_tokens):
        text = text.replace(f"\x00LINK{idx}\x00", val)
    for idx, val in enumerate(img_tokens):
        text = text.replace(f"\x00IMG{idx}\x00", val)
    for idx, val in enumerate(code_tokens):
        text = text.replace(f"\x00CODE{idx}\x00", val)

    return text


def split_gfm_table_row(line: str) -> list[str]:
    """Split a GFM table row on unescaped '|' outside backtick spans."""
    s = line.strip()
    if s.startswith("|"):
        s = s[1:]
    if s.endswith("|") and not s.endswith("\\|"):
        s = s[:-1]

    cells: list[str] = []
    cur: list[str] = []
    in_tick = False
    tick_len = 0
    i = 0
    n = len(s)
    while i < n:
        ch = s[i]
        if ch == "\\" and i + 1 < n:
            if s[i + 1] == "|":
                cur.append("|")
                i += 2
                continue
            cur.append(ch)
            cur.append(s[i + 1])
            i += 2
            continue
        if ch == "`":
            j = i
            while j < n and s[j] == "`":
                j += 1
            run = j - i
            if not in_tick:
                in_tick = True
                tick_len = run
            elif run == tick_len:
                in_tick = False
            cur.append(s[i:j])
            i = j
            continue
        if ch == "|" and not in_tick:
            cells.append("".join(cur).strip())
            cur = []
            i += 1
            continue
        cur.append(ch)
        i += 1
    cells.append("".join(cur).strip())
    return cells


def is_table_separator(line: str) -> bool:
    s = line.strip()
    if "|" not in s:
        return False
    cells = split_gfm_table_row(s)
    if not cells:
        return False
    return all(bool(re.match(r"^:?-{2,}:?$", c.strip())) for c in cells)


def render_eclk_listing_html(prog_heading_md: str | None, code_lines: list[str]) -> str:
    """Render an ECLK Program X-Y annotated code listing with blue underlined line numbers."""
    out: list[str] = ['<div class="eclk-listing">']
    if prog_heading_md:
        clean_hdr = re.sub(r"^#{1,6}\s+", "", prog_heading_md).strip()
        # Strip backticks in Program X-Y header to match ECLK clean title bar
        clean_hdr_plain = clean_hdr.replace("`", "")
        out.append(f'<div class="prog-title">{html.escape(clean_hdr_plain)}</div>')

    out.append('<div class="prog-code">')
    for raw in code_lines:
        m = CODE_LINE_RE.match(raw)
        if m:
            lno = m.group(1)
            code_part = m.group(2) if m.group(2) is not None else ""
            esc = html.escape(code_part) if code_part != "" else "&nbsp;"
            out.append(
                f'<div class="cl"><span class="ln">{lno}</span><span class="cd">{esc}</span></div>'
            )
        else:
            # Author commentary line (e.g. '       //// ...' or '       ## ...')
            stripped = re.sub(r"^\s{1,8}(?=(?:////|##))", "", raw)
            esc = html.escape(stripped) if stripped != "" else "&nbsp;"
            out.append(
                f'<div class="ca"><span class="ln-pad"></span><span class="an">{esc}</span></div>'
            )
    out.append("</div></div>")
    return "\n".join(out)


class TocItem:
    def __init__(self, idx: int, level: int, title_plain: str, slug: str, chap_idx: int):
        self.idx = idx
        self.level = level  # 1 = chapter/preface/appendix-root, 2 = X.Y section
        self.title_plain = title_plain
        self.slug = slug
        self.chap_idx = chap_idx
        self.page_num: int | None = None


def strip_md_inline(text: str) -> str:
    t = re.sub(r"<!--.*?-->", "", text)
    t = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", t)
    t = re.sub(r"`([^`]*)`", r"\1", t)
    t = re.sub(r"\*\*\*(.+?)\*\*\*", r"\1", t)
    t = re.sub(r"\*\*(.+?)\*\*", r"\1", t)
    t = re.sub(r"(?<!\*)\*(?!\s)([^*\n]+?)(?<!\s)\*(?!\*)", r"\1", t)
    return t.strip()


def convert_chapter_markdown_to_html(
    md: str,
    chap_idx: int,
    used_slugs: set[str],
    toc_items: list[TocItem],
    emit_page_markers: bool,
) -> tuple[str, str]:
    """Convert one chapter's Markdown to HTML and return (running_header_title, html_body)."""
    lines = md.splitlines()
    n = len(lines)
    i = 0
    html_blocks: list[str] = []
    running_header = f"Chapter {chap_idx}"
    found_h1 = False

    while i < n:
        line = lines[i]
        stripped = line.strip()

        # Blank line
        if not stripped:
            i += 1
            continue

        # Horizontal rule
        if re.match(r"^(?:-{3,}|\*{3,}|_{3,})$", stripped):
            html_blocks.append('<hr class="sec-rule">')
            i += 1
            continue

        # Check for #### Program X-Y immediately followed by <!-- ECLK_CODE_LISTING ... -->
        if re.match(r"^#{3,5}\s+Program\b", stripped):
            look = i + 1
            while look < n and not lines[look].strip():
                look += 1
            if look < n and ECLK_META_RE.search(lines[look]):
                prog_hdr = stripped
                i = look + 1
                while i < n and not lines[i].strip():
                    i += 1
                if i < n and lines[i].lstrip().startswith("```"):
                    i += 1
                    c_lines: list[str] = []
                    while i < n and not lines[i].lstrip().startswith("```"):
                        c_lines.append(lines[i])
                        i += 1
                    if i < n:
                        i += 1
                    html_blocks.append(render_eclk_listing_html(prog_hdr, c_lines))
                    continue

        # Standalone <!-- ECLK_CODE_LISTING ... -->
        if ECLK_META_RE.search(stripped):
            i += 1
            while i < n and not lines[i].strip():
                i += 1
            if i < n and lines[i].lstrip().startswith("```"):
                i += 1
                c_lines = []
                while i < n and not lines[i].lstrip().startswith("```"):
                    c_lines.append(lines[i])
                    i += 1
                if i < n:
                    i += 1
                html_blocks.append(render_eclk_listing_html(None, c_lines))
                continue

        # Other HTML comments
        if stripped.startswith("<!--"):
            while i < n and "-->" not in lines[i]:
                i += 1
            i += 1
            continue

        # Standard fenced code block (``` or ~~~)
        fence_m = re.match(r"^(\s*)(`{3,}|~{3,})(.*)$", line)
        if fence_m:
            fence_chars = fence_m.group(2)
            fence_char = fence_chars[0]
            fence_len = len(fence_chars)
            i += 1
            box_lines: list[str] = []
            while i < n:
                close_m = re.match(rf"^\s*{re.escape(fence_char)}{{{fence_len},}}\s*$", lines[i])
                if close_m:
                    i += 1
                    break
                box_lines.append(lines[i])
                i += 1
            esc_box = html.escape("\n".join(box_lines))
            html_blocks.append(f'<pre class="code-box"><code>{esc_box}</code></pre>')
            continue

        # Headings (# .. ######)
        hdr_m = re.match(r"^(#{1,6})\s+(.+?)\s*$", line)
        if hdr_m:
            lvl = len(hdr_m.group(1))
            raw_title = hdr_m.group(2).strip()
            slug = slugify(raw_title, used_slugs)
            plain_title = strip_md_inline(raw_title)

            if lvl == 1 and not found_h1:
                found_h1 = True
                # Clean running header (e.g. "Chapter 6: Booting System" -> "Chapter 6  Booting System")
                running_header = plain_title

            marker_html = ""
            if lvl in (1, 2):
                toc_idx = len(toc_items)
                toc_items.append(TocItem(toc_idx, lvl, plain_title, slug, chap_idx))
                if emit_page_markers:
                    marker_html = f' <span class="pgmark">PGMK_{toc_idx}_END</span>'

            html_blocks.append(
                f'<h{lvl} id="{slug}">{format_inline(raw_title)}{marker_html}</h{lvl}>'
            )
            i += 1
            continue

        # Standalone Figure Image: ![alt](src)
        img_m = re.match(r"^!\[([^\]]*)\]\(([^)]+)\)\s*$", stripped)
        if img_m:
            alt_raw = img_m.group(1).strip()
            src_resolved = html.escape(resolve_img_src_for_html(img_m.group(2)))
            caption_html = format_inline(alt_raw) if alt_raw else ""
            i += 1
            # Check if the immediately following line (or after 1 blank line) is an explicit italic/bold Figure caption
            look = i
            while look < n and not lines[look].strip():
                look += 1
            if look < n and re.match(r"^(?:\*+|\b)Figure\s+\d+", lines[look].strip()):
                cap_line = lines[look].strip()
                cap_line = re.sub(r"^\*+|\*+$", "", cap_line).strip()
                caption_html = format_inline(cap_line)
                i = look + 1

            fig_parts = [
                '<figure class="eclk-fig">',
                f'  <img src="{src_resolved}" alt="{html.escape(alt_raw)}">',
            ]
            if caption_html:
                fig_parts.append(f"  <figcaption>{caption_html}</figcaption>")
            fig_parts.append("</figure>")
            html_blocks.append("\n".join(fig_parts))
            continue

        # GFM Table
        if "|" in stripped and i + 1 < n and is_table_separator(lines[i + 1]):
            headers = split_gfm_table_row(lines[i])
            align_specs = split_gfm_table_row(lines[i + 1])
            aligns: list[str] = []
            for sp in align_specs:
                sp = sp.strip()
                if sp.startswith(":") and sp.endswith(":"):
                    aligns.append("center")
                elif sp.endswith(":"):
                    aligns.append("right")
                else:
                    aligns.append("left")
            i += 2
            rows: list[list[str]] = []
            while i < n and lines[i].strip() and "|" in lines[i]:
                if lines[i].lstrip().startswith(("```", "#", ">")):
                    break
                rows.append(split_gfm_table_row(lines[i]))
                i += 1

            tbl = ['<table class="eclk-table"><thead><tr>']
            for c_idx, h_cell in enumerate(headers):
                al = aligns[c_idx] if c_idx < len(aligns) else "left"
                tbl.append(f'<th style="text-align:{al}">{format_inline(h_cell)}</th>')
            tbl.append("</tr></thead><tbody>")
            for r in rows:
                tbl.append("<tr>")
                for c_idx in range(len(headers)):
                    cell_val = r[c_idx] if c_idx < len(r) else ""
                    al = aligns[c_idx] if c_idx < len(aligns) else "left"
                    tbl.append(f'<td style="text-align:{al}">{format_inline(cell_val)}</td>')
                tbl.append("</tr>")
            tbl.append("</tbody></table>")
            html_blocks.append("".join(tbl))
            continue

        # Blockquote / GitHub Alert (> ...)
        if stripped.startswith(">"):
            q_lines: list[str] = []
            while i < n and lines[i].strip().startswith(">"):
                q_lines.append(re.sub(r"^\s*>\s?", "", lines[i]))
                i += 1
            alert_type = None
            if q_lines:
                am = re.match(r"^\[!(NOTE|TIP|IMPORTANT|WARNING|CAUTION)\]\s*(.*)$", q_lines[0].strip(), re.I)
                if am:
                    alert_type = am.group(1).upper()
                    rem = am.group(2).strip()
                    q_lines = ([rem] if rem else []) + q_lines[1:]
            # Render inner paragraphs inside blockquote
            paras: list[str] = []
            cur_p: list[str] = []
            for ql in q_lines:
                if not ql.strip():
                    if cur_p:
                        paras.append(f"<p>{format_inline(' '.join(cur_p))}</p>")
                        cur_p = []
                else:
                    cur_p.append(ql.strip())
            if cur_p:
                paras.append(f"<p>{format_inline(' '.join(cur_p))}</p>")
            inner_html = "\n".join(paras)
            if alert_type:
                cls = f"callout callout-{alert_type.lower()}"
                html_blocks.append(
                    f'<div class="{cls}"><div class="callout-title">{alert_type}</div>{inner_html}</div>'
                )
            else:
                html_blocks.append(f"<blockquote>{inner_html}</blockquote>")
            continue

        # Unordered or Ordered Lists (supports nested items)
        list_m = re.match(r"^(\s*)(?:([-*+])|(\d+)\.)\s+(.+)$", line)
        if list_m:
            list_html, i = _parse_list_block(lines, i)
            html_blocks.append(list_html)
            continue

        # Regular Paragraph
        p_lines: list[str] = []
        while i < n:
            cur = lines[i]
            cur_s = cur.strip()
            if not cur_s:
                break
            if (
                cur_s.startswith(("#", "```", "~~~", ">", "<!--"))
                or re.match(r"^(?:-{3,}|\*{3,}|_{3,})$", cur_s)
                or re.match(r"^!\[([^\]]*)\]\(([^)]+)\)\s*$", cur_s)
                or re.match(r"^(\s*)(?:([-*+])|(\d+)\.)\s+(.+)$", cur)
                or ("|" in cur_s and i + 1 < n and is_table_separator(lines[i + 1]))
            ):
                break
            # Preserve intentional Markdown hard line breaks (two trailing spaces)
            if cur.endswith("  "):
                p_lines.append(format_inline(cur.rstrip()) + "<br>")
            else:
                p_lines.append(format_inline(cur_s))
            i += 1
        if p_lines:
            html_blocks.append("<p>" + " ".join(p_lines) + "</p>")

    return running_header, "\n".join(html_blocks)


def _parse_list_block(lines: list[str], start_i: int) -> tuple[str, int]:
    """Parse contiguous unordered/ordered list items (with 1-level nesting support)."""
    n = len(lines)
    first_m = re.match(r"^(\s*)(?:([-*+])|(\d+)\.)\s+(.+)$", lines[start_i])
    assert first_m is not None
    base_indent = len(first_m.group(1).expandtabs(4))
    is_ordered = first_m.group(3) is not None
    tag = "ol" if is_ordered else "ul"
    start_attr = ""
    if is_ordered and first_m.group(3) != "1":
        start_attr = f' start="{first_m.group(3)}"'

    items_html: list[str] = []
    i = start_i

    while i < n:
        if not lines[i].strip():
            # Look ahead past single blank line to see if list continues
            look = i + 1
            while look < n and not lines[look].strip():
                look += 1
            if look < n:
                nm = re.match(r"^(\s*)(?:([-*+])|(\d+)\.)\s+(.+)$", lines[look])
                if nm and len(nm.group(1).expandtabs(4)) >= base_indent:
                    i = look
                else:
                    break
            else:
                break

        m = re.match(r"^(\s*)(?:([-*+])|(\d+)\.)\s+(.+)$", lines[i])
        if not m:
            break
        indent = len(m.group(1).expandtabs(4))
        if indent < base_indent:
            break
        item_ordered = m.group(3) is not None
        if indent == base_indent and item_ordered != is_ordered:
            break

        if indent > base_indent:
            sub_html, i = _parse_list_block(lines, i)
            if items_html:
                items_html[-1] = items_html[-1][:-5] + "\n" + sub_html + "</li>"
            else:
                items_html.append(f"<li>{sub_html}</li>")
            continue

        # Collect continuation lines belonging to this list item
        item_parts = [format_inline(m.group(4).strip())]
        i += 1
        while i < n:
            nxt = lines[i]
            nxt_s = nxt.strip()
            if not nxt_s:
                break
            if re.match(r"^(\s*)(?:([-*+])|(\d+)\.)\s+(.+)$", nxt):
                break
            if nxt_s.startswith(("#", "```", "~~~", ">", "<!--")):
                break
            nxt_indent = len(nxt.expandtabs(4)) - len(nxt.lstrip())
            if nxt_indent > base_indent:
                item_parts.append(format_inline(nxt_s))
                i += 1
            else:
                break

        items_html.append("<li>" + " ".join(item_parts) + "</li>")

    return f"<{tag}{start_attr}>\n" + "\n".join(items_html) + f"\n</{tag}>", i


# ---------------------------------------------------------------------------
# 4. ECLK 5.0 Complete HTML Document & CSS Generator
# ---------------------------------------------------------------------------


def split_md_by_top_h1(md: str) -> list[str]:
    """Split a Markdown document containing multiple top-level '# ' headings (outside code fences)."""
    chunks: list[list[str]] = []
    cur: list[str] = []
    in_fence = False
    for line in md.splitlines():
        s = line.lstrip()
        if s.startswith("```") or s.startswith("~~~"):
            in_fence = not in_fence
            cur.append(line)
            continue
        if not in_fence and line.startswith("# ") and any(x.strip() for x in cur):
            # Strip trailing '---' from previous chunk if present right before the new H1
            while cur and cur[-1].strip() in ("", "---"):
                cur.pop()
            chunks.append(cur)
            cur = [line]
        else:
            cur.append(line)
    if cur:
        chunks.append(cur)
    return ["\n".join(c) for c in chunks]


def build_eclk_html_document(
    chapters_text: list[tuple[str, str]],
    page_map: dict[int, int] | None,
    emit_page_markers: bool,
) -> tuple[str, list[TocItem]]:
    """Build the full ECLK-5.0 HTML document for headless Chrome PDF printing."""
    used_slugs: set[str] = set()
    toc_items: list[TocItem] = []
    chapter_sections: list[tuple[int, str, str]] = []

    sec_idx = 0
    for _, md in chapters_text:
        for sub_md in split_md_by_top_h1(md):
            hdr_title, body_html = convert_chapter_markdown_to_html(
                sub_md,
                chap_idx=sec_idx,
                used_slugs=used_slugs,
                toc_items=toc_items,
                emit_page_markers=emit_page_markers,
            )
            chapter_sections.append((sec_idx, hdr_title, body_html))
            sec_idx += 1

    # Populate known page numbers if provided (Pass 2)
    if page_map:
        for item in toc_items:
            item.page_num = page_map.get(item.idx)

    # Generate named @page rules per chapter so each chapter gets its own top running header
    named_page_css: list[str] = []
    for chap_idx, hdr_title, _ in chapter_sections:
        css_hdr = hdr_title.replace("\\", "\\\\").replace('"', '\\"')
        named_page_css.append(
            f"""
@page chap_{chap_idx} {{
  size: A4;
  margin: 22mm 17mm 19mm 17mm;
  @top-center {{
    content: "{css_hdr}";
    font-family: "Liberation Serif", "Noto Serif", "Times New Roman", serif;
    font-size: 9pt;
    color: #222222;
    width: 100%;
    vertical-align: bottom;
    border-bottom: 0.6pt solid #222222;
    padding-bottom: 2.5pt;
    margin-bottom: 4.5mm;
  }}
  @bottom-center {{
    content: counter(page);
    font-family: "Liberation Serif", "Noto Serif", "Times New Roman", serif;
    font-size: 9.5pt;
    color: #222222;
    vertical-align: top;
    margin-top: 3mm;
  }}
}}
section.chap-sec-{chap_idx} {{
  page: chap_{chap_idx};
  break-before: page;
}}"""
        )

    cover_img_uri = resolve_img_src_for_html("assets/cover_photo.jpg")
    mascot_img_uri = resolve_img_src_for_html("assets/abos_mascot.jpg")

    # Build two-column ECLK Table of Contents rows
    toc_rows_html: list[str] = []
    for item in toc_items:
        pg_str = str(item.page_num) if item.page_num is not None else "000"
        cls = "toc-l1" if item.level == 1 else "toc-l2"
        disp = html.escape(item.title_plain)
        toc_rows_html.append(
            f'<div class="toc-row {cls}">'
            f'<a href="#{item.slug}" class="toc-link">'
            f'<span class="toc-title">{disp}</span>'
            f'<span class="toc-dots"></span>'
            f'<span class="toc-pg">{pg_str}</span>'
            f"</a></div>"
        )

    body_sections_html = "\n".join(
        f'<section class="chap-sec chap-sec-{c_idx}">\n{b_html}\n</section>'
        for c_idx, _, b_html in chapter_sections
    )

    css = f"""
@page {{
  size: A4;
  margin: 22mm 17mm 19mm 17mm;
}}
@page cover_page {{
  size: A4;
  margin: 0;
  @top-center {{ content: none; }}
  @bottom-center {{ content: none; }}
}}
@page front_matter {{
  size: A4;
  margin: 24mm 22mm 24mm 22mm;
  @top-center {{ content: none; }}
  @bottom-center {{ content: none; }}
}}
@page toc_page {{
  size: A4;
  margin: 22mm 17mm 19mm 17mm;
  @top-center {{
    content: "Table of Contents";
    font-family: "Liberation Serif", "Noto Serif", "Times New Roman", serif;
    font-size: 9pt;
    color: #222222;
    width: 100%;
    vertical-align: bottom;
    border-bottom: 0.6pt solid #222222;
    padding-bottom: 2.5pt;
    margin-bottom: 4.5mm;
  }}
  @bottom-center {{
    content: "- " counter(page, upper-roman) " -";
    font-family: "Liberation Serif", "Noto Serif", "Times New Roman", serif;
    font-size: 9.5pt;
    color: #222222;
    vertical-align: top;
    margin-top: 3mm;
  }}
}}
{"".join(named_page_css)}

* {{
  box-sizing: border-box;
  -webkit-print-color-adjust: exact !important;
  print-color-adjust: exact !important;
}}
html, body {{
  margin: 0;
  padding: 0;
  font-family: "Liberation Serif", "Noto Serif", "Times New Roman", Georgia, serif;
  font-size: 10.2pt;
  line-height: 1.52;
  color: #111111;
  background: #ffffff;
}}

/* --- Page 1: ECLK Homage Full-Bleed Cover --- */
.cover-page {{
  page: cover_page;
  width: 210mm;
  height: 296.5mm;
  overflow: hidden;
  display: flex;
  flex-direction: column;
  justify-content: space-between;
  background: #ffffff;
  padding: 26mm 0 13mm 0;
}}
.cover-top {{
  text-align: center;
  padding: 0 14mm;
}}
.cover-main-title {{
  font-family: "Liberation Sans Narrow", "Arial Narrow", "Liberation Sans", Arial, sans-serif;
  font-size: 35pt;
  font-weight: 800;
  line-height: 1.24;
  color: #cc0000;
  text-shadow: 2.2px 2.2px 0px #c4c4c4;
  letter-spacing: -0.4px;
  margin: 0 0 14mm 0;
}}
.cover-author {{
  font-family: "Liberation Sans", Arial, sans-serif;
  font-size: 20pt;
  font-weight: 600;
  color: #111111;
  margin: 0 0 8mm 0;
}}
.cover-photo-wrap {{
  width: 210mm;
  height: 146mm;
  overflow: hidden;
  border-top: 0.5pt solid #d0d7de;
  border-bottom: 0.5pt solid #d0d7de;
}}
.cover-photo-wrap img {{
  width: 100%;
  height: 100%;
  object-fit: cover;
  display: block;
}}
.cover-footer {{
  display: flex;
  justify-content: space-around;
  align-items: center;
  padding: 4mm 18mm 0 18mm;
  font-family: "Liberation Sans", Arial, sans-serif;
  font-size: 12.5pt;
  font-weight: 700;
}}
.cover-footer span {{
  color: #000000;
  text-decoration: underline;
  text-underline-offset: 3px;
}}

/* --- Page 2: Inner Title Page --- */
.inner-title-page {{
  page: front_matter;
  break-before: page;
  height: 248mm;
  display: flex;
  flex-direction: column;
  justify-content: space-between;
  align-items: center;
  text-align: center;
  padding: 16mm 6mm 10mm 6mm;
}}
.itp-title {{
  font-family: "Liberation Serif", "Times New Roman", serif;
  font-size: 25pt;
  font-weight: 700;
  line-height: 1.32;
  color: #000000;
  margin-bottom: 3mm;
}}
.itp-ver {{
  font-size: 14pt;
  color: #111111;
  margin-bottom: 9mm;
}}
.itp-ed {{
  font-size: 13pt;
  color: #222222;
  margin-bottom: 5mm;
}}
.itp-mascot {{
  width: 48mm;
  height: auto;
  margin: 2mm auto 6mm auto;
  display: block;
}}
.itp-author {{
  font-size: 14pt;
  line-height: 1.55;
  color: #111111;
}}
.itp-bottom {{
  font-size: 12.5pt;
  line-height: 1.7;
  letter-spacing: 0.4px;
  color: #111111;
}}

/* --- Page 3: Abstract & Copyright --- */
.abstract-page {{
  page: front_matter;
  break-before: page;
  padding-top: 8mm;
}}
.abstract-page h2 {{
  font-family: "Liberation Sans", Arial, sans-serif;
  font-size: 15pt;
  border-bottom: 1pt solid #222;
  padding-bottom: 3pt;
  margin-top: 8mm;
  margin-bottom: 4mm;
}}
.abstract-page p {{
  text-align: justify;
  margin: 0 0 3.5mm 0;
}}

/* --- Page 4: Epigraph --- */
.epigraph-page {{
  page: front_matter;
  break-before: page;
  height: 245mm;
  display: flex;
  flex-direction: column;
  justify-content: center;
  align-items: center;
  text-align: center;
}}
.epigraph-quote {{
  font-size: 15pt;
  font-style: italic;
  color: #111111;
  margin-bottom: 5mm;
}}
.epigraph-attr {{
  font-size: 12pt;
  color: #333333;
}}

/* --- Pages 5+: Two-Column Table of Contents --- */
.toc-section {{
  page: toc_page;
  break-before: page;
}}
.toc-heading {{
  text-align: center;
  font-family: "Liberation Serif", "Times New Roman", serif;
  font-size: 22pt;
  font-weight: 700;
  margin: 2mm 0 9mm 0;
}}
.toc-columns {{
  column-count: 2;
  column-gap: 8mm;
}}
.toc-row {{
  break-inside: avoid;
  margin-bottom: 2.2pt;
}}
.toc-link {{
  display: flex;
  align-items: baseline;
  text-decoration: none;
  color: #000000;
}}
.toc-l1 {{
  margin-top: 5.5pt;
  margin-bottom: 3pt;
  font-family: "Liberation Serif", "Times New Roman", serif;
  font-weight: 700;
  font-size: 9.2pt;
  text-transform: uppercase;
}}
.toc-l2 {{
  padding-left: 3.5mm;
  font-size: 8.6pt;
  font-variant: small-caps;
}}
.toc-title {{
  flex: 0 1 auto;
  padding-right: 2pt;
}}
.toc-dots {{
  flex: 1 1 8mm;
  border-bottom: 1px dotted #444444;
  margin: 0 2pt;
  min-width: 5mm;
}}
.toc-l1 .toc-dots {{
  border-bottom: 1.4px dotted #111111;
}}
.toc-pg {{
  flex: 0 0 auto;
  text-align: right;
  font-variant-numeric: tabular-nums;
}}

/* --- Main Body Typography --- */
.chap-sec h1 {{
  font-family: "Liberation Sans", "Noto Sans", Arial, sans-serif;
  font-size: 20pt;
  font-weight: 700;
  color: #0f172a;
  border-bottom: 1.6pt solid #0f172a;
  padding-bottom: 4pt;
  margin: 2mm 0 6mm 0;
  break-after: avoid;
}}
.chap-sec h2 {{
  font-family: "Liberation Sans", "Noto Sans", Arial, sans-serif;
  font-size: 14pt;
  font-weight: 700;
  color: #111827;
  margin: 6.5mm 0 3mm 0;
  break-after: avoid;
}}
.chap-sec h3 {{
  font-family: "Liberation Sans", "Noto Sans", Arial, sans-serif;
  font-size: 11.6pt;
  font-weight: 700;
  color: #1e293b;
  margin: 5mm 0 2.2mm 0;
  break-after: avoid;
}}
.chap-sec h4 {{
  font-family: "Liberation Sans", "Noto Sans", Arial, sans-serif;
  font-size: 10.4pt;
  font-weight: 700;
  color: #1e293b;
  margin: 4mm 0 1.8mm 0;
  break-after: avoid;
}}
.chap-sec p {{
  margin: 0 0 2.8mm 0;
  text-align: justify;
  orphans: 2;
  widows: 2;
}}
.chap-sec ul, .chap-sec ol {{
  margin: 1.5mm 0 3.2mm 0;
  padding-left: 6.5mm;
}}
.chap-sec li {{
  margin-bottom: 1.4mm;
  text-align: justify;
}}
.chap-sec code {{
  font-family: "Liberation Mono", "DejaVu Sans Mono", "Courier New", monospace;
  font-size: 8.8pt;
  background: #f1f5f9;
  padding: 0.5pt 2.5pt;
  border-radius: 2pt;
  color: #0f172a;
}}
.chap-sec a {{
  color: #0f3d91;
  text-decoration: none;
}}
.sec-rule {{
  border: none;
  border-top: 0.6pt solid #cbd5e1;
  margin: 5mm 0;
}}

/* --- Figures & Diagrams --- */
.eclk-fig {{
  margin: 4.5mm 0 5mm 0;
  padding: 3mm 3mm 2.5mm 3mm;
  border: 0.75pt solid #334155;
  background: #ffffff;
  text-align: center;
  break-inside: avoid;
}}
.eclk-fig img {{
  max-width: 100%;
  max-height: 115mm;
  height: auto;
  display: block;
  margin: 0 auto;
}}
.eclk-fig figcaption {{
  margin-top: 2.5mm;
  font-family: "Liberation Sans", Arial, sans-serif;
  font-size: 9pt;
  font-weight: 600;
  color: #1e293b;
}}

/* --- Standard Code / ASCII Diagram Blocks --- */
pre.code-box {{
  font-family: "Liberation Mono", "DejaVu Sans Mono", "Courier New", monospace;
  font-size: 7.9pt;
  line-height: 1.28;
  background: #f8fafc;
  border: 0.6pt solid #cbd5e1;
  border-radius: 2pt;
  padding: 2.8mm 3.2mm;
  margin: 2.8mm 0 3.8mm 0;
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}}
pre.code-box code {{
  font-size: inherit;
  background: transparent;
  padding: 0;
}}

/* --- ECLK Program X-Y Heavily Commented Source Listings --- */
.eclk-listing {{
  margin: 4.5mm 0 5.5mm 0;
}}
.prog-title {{
  font-family: "Liberation Sans", Arial, sans-serif;
  font-size: 9.8pt;
  font-weight: 700;
  text-align: center;
  color: #000000;
  border-bottom: 1.2pt solid #222222;
  padding-bottom: 2.5pt;
  margin-bottom: 2.5mm;
  break-after: avoid;
}}
.prog-code {{
  font-family: "Liberation Mono", "DejaVu Sans Mono", "Courier New", monospace;
  font-size: 8.1pt;
  line-height: 1.32;
  border-bottom: 0.8pt solid #475569;
  padding-bottom: 2mm;
}}
.cl, .ca {{
  display: flex;
  align-items: flex-start;
}}
.ln {{
  flex: 0 0 9.5mm;
  width: 9.5mm;
  padding-right: 2.2mm;
  text-align: right;
  color: #0022cc;
  text-decoration: underline;
  font-variant-numeric: tabular-nums;
  user-select: none;
}}
.ln-pad {{
  flex: 0 0 9.5mm;
  width: 9.5mm;
}}
.cd {{
  flex: 1 1 auto;
  min-width: 0;
  white-space: pre-wrap;
  overflow-wrap: break-word;
  color: #0f172a;
}}
.ca {{
  background: #f4f8f5;
}}
.an {{
  flex: 1 1 auto;
  min-width: 0;
  white-space: pre-wrap;
  overflow-wrap: break-word;
  color: #14532d;
}}

/* --- GFM Tables --- */
table.eclk-table {{
  width: 100%;
  border-collapse: collapse;
  margin: 3.2mm 0 4.5mm 0;
  font-size: 8.8pt;
  line-height: 1.35;
}}
table.eclk-table th, table.eclk-table td {{
  border: 0.6pt solid #64748b;
  padding: 1.6mm 2.2mm;
  vertical-align: top;
}}
table.eclk-table th {{
  background: #e2e8f0;
  font-family: "Liberation Sans", Arial, sans-serif;
  font-weight: 700;
  color: #0f172a;
}}
table.eclk-table tr:nth-child(even) td {{
  background: #f8fafc;
}}
table.eclk-table code {{
  font-size: 8.1pt;
}}

/* --- Callouts & Blockquotes --- */
blockquote {{
  margin: 3mm 0 3.5mm 0;
  padding: 2mm 3.5mm;
  border-left: 2.5pt solid #475569;
  background: #f8fafc;
  color: #1e293b;
}}
.callout {{
  margin: 3.2mm 0 4mm 0;
  padding: 2.5mm 3.5mm;
  border-left: 3pt solid #1d4ed8;
  background: #eff6ff;
  break-inside: avoid;
}}
.callout-title {{
  font-family: "Liberation Sans", Arial, sans-serif;
  font-size: 8.8pt;
  font-weight: 700;
  letter-spacing: 0.4px;
  color: #1e40af;
  margin-bottom: 1mm;
}}
.callout-tip {{ border-left-color: #15803d; background: #f0fdf4; }}
.callout-tip .callout-title {{ color: #166534; }}
.callout-important {{ border-left-color: #6d28d9; background: #f5f3ff; }}
.callout-important .callout-title {{ color: #5b21b6; }}
.callout-warning, .callout-caution {{ border-left-color: #b45309; background: #fffbeb; }}
.callout-warning .callout-title, .callout-caution .callout-title {{ color: #92400e; }}

/* Invisible Pass-1 page number locator marker */
.pgmark {{
  font-size: 0.8pt;
  color: #ffffff;
  font-weight: normal;
}}
"""

    doc = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>A Heavily Commented ABOS Microkernel Source Code</title>
<style>{css}</style>
</head>
<body>

<!-- Page 1: Cover -->
<div class="cover-page">
  <div class="cover-top">
    <div class="cover-main-title">A Heavily Commented<br>ABOS Microkernel Source Code</div>
    <div class="cover-author">Liping Liu</div>
  </div>
  <div class="cover-photo-wrap">
    <img src="{html.escape(cover_img_uri)}" alt="Cover Photograph">
  </div>
  <div class="cover-footer">
    <span>ABOS Microkernel Project</span>
    <span>lipingliu@google.com</span>
  </div>
</div>

<!-- Page 2: Inner Title Page -->
<div class="inner-title-page">
  <div>
    <div class="itp-title">A Heavily Commented ABOS Microkernel<br>Source Code</div>
    <div class="itp-ver">Kernel Version 1.0 (Lesson 20)</div>
  </div>
  <div>
    <div class="itp-ed">(Complete Annotated Edition)</div>
    <img src="{html.escape(mascot_img_uri)}" alt="ABOS Armadillo Mascot" class="itp-mascot">
    <div class="itp-author">Liping Liu<br>lipingliu@google.com</div>
  </div>
  <div class="itp-bottom">
    ARMV8-A CAPABILITY MICROKERNEL SERIES<br>
    2026-10-29
  </div>
</div>

<!-- Page 3: Abstract & Copyright -->
<div class="abstract-page">
  <h2>Abstract</h2>
  <p>
    <strong>ABOS (ARM Bare-metal Operating System)</strong> is a clean-slate, two-layer ARMv8-A (AArch64)
    capability microkernel operating system built progressively across twenty hands-on engineering lessons
    and inspired by the architecture of Google's Fuchsia Zircon kernel. Running on a 4-CPU symmetric
    multiprocessing (SMP) QEMU <code>virt</code> machine with GICv2 interrupt routing, 4-level MMU address
    spaces, preemptive round-robin scheduling, object-capability handle tables, channels, ports, virtual
    memory objects (VMOs), and full user-space device/file servers, ABOS bridges the gap between toy kernels
    and production capability microkernels.
  </p>
  <p>
    Modeled after Dr. Zhao Jiong's renowned classic <em>A Heavily Commented Linux Kernel Source Code
    (Kernel Version 0.12)</em>, this book presents both the architectural foundations (Chapters 1–5) and a
    complete, 100% verbatim line-by-line annotation of every single source file, header file, assembly stub,
    linker script, build Makefile, and image-packaging script in <code>lesson_20/</code> (58 files, 9,526 lines
    in total, Chapters 6–16), followed by a hands-on QEMU &amp; GDB kernel debugging laboratory (Chapter 17)
    and quick-reference appendices (Appendices A1–A4).
  </p>

  <h2>Copyright &amp; Usage Statement</h2>
  <p>
    Copyright &copy; 2026 by <strong>Liping Liu</strong> (<code>lipingliu@google.com</code>).
  </p>
  <p>
    All ABOS source code annotated in this volume is part of the ABOS educational operating-system curriculum.
    Readers are encouraged to study, build, modify, experiment with, and step through every line under QEMU
    and GDB alongside this commentary.
  </p>
</div>

<!-- Page 4: Epigraph -->
<div class="epigraph-page">
  <div class="epigraph-quote">&ldquo;RTFSC &ndash; Read The F**king Source Code :)!&rdquo;</div>
  <div class="epigraph-attr">&ndash; Linus Benedict Torvalds</div>
</div>

<!-- Pages 5+: Two-Column Table of Contents -->
<div class="toc-section">
  <div class="toc-heading">Table of Contents</div>
  <div class="toc-columns">
    {"".join(toc_rows_html)}
  </div>
</div>

<!-- Main Body Chapters -->
{body_sections_html}

</body>
</html>
"""
    return doc, toc_items


# ---------------------------------------------------------------------------
# 5. Chrome Headless Rendering & Two-Pass Page Number Extraction
# ---------------------------------------------------------------------------


def find_chrome_binary() -> str:
    for cand in ("google-chrome", "chromium", "chromium-browser", "google-chrome-stable"):
        path = shutil.which(cand)
        if path:
            return path
    raise RuntimeError("Could not find google-chrome or chromium in PATH.")


def render_html_to_pdf(chrome_bin: str, html_path: str, pdf_path: str) -> None:
    cmd = [
        chrome_bin,
        "--headless",
        "--no-sandbox",
        "--disable-gpu",
        "--allow-file-access-from-files",
        f"--print-to-pdf={pdf_path}",
        "--no-pdf-header-footer",
        html_path,
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0 or not os.path.exists(pdf_path):
        raise RuntimeError(
            f"Chrome PDF rendering failed (rc={res.returncode}):\n{res.stderr}"
        )


def extract_page_markers(pdf_path: str) -> dict[int, int]:
    """Extract {toc_idx: 1_based_pdf_page} from Pass-1 PDF using read_pdf_cli or pdftotext."""
    marker_re = re.compile(r"PGMK_(\d+)_END")
    page_map: dict[int, int] = {}

    read_pdf_cli = "/google/bin/releases/gemini-agents-pdf-reader/read_pdf_cli"
    if os.path.exists(read_pdf_cli):
        res = subprocess.run(
            [read_pdf_cli, f"--file={pdf_path}", "--text_only"],
            capture_output=True,
            text=True,
            check=True,
        )
        cur_page = 0
        for line in res.stdout.splitlines():
            pm = re.match(r"^---\s*PAGE\s+(\d+)\s*---$", line.strip())
            if pm:
                cur_page = int(pm.group(1))
                continue
            if cur_page > 0:
                for m in marker_re.finditer(line):
                    idx = int(m.group(1))
                    if idx not in page_map:
                        page_map[idx] = cur_page
        return page_map

    pdftotext_bin = shutil.which("pdftotext")
    if pdftotext_bin:
        res = subprocess.run(
            [pdftotext_bin, pdf_path, "-"],
            capture_output=True,
            text=True,
            check=True,
        )
        pages = res.stdout.split("\f")
        for pg_idx, pg_txt in enumerate(pages, start=1):
            for m in marker_re.finditer(pg_txt):
                idx = int(m.group(1))
                if idx not in page_map:
                    page_map[idx] = pg_idx
        return page_map

    raise RuntimeError("Neither read_pdf_cli nor pdftotext is available to extract TOC page numbers.")


def main() -> int:
    parser = argparse.ArgumentParser(description="Build ABOS_Book.md and ABOS_Book.pdf")
    parser.add_argument(
        "--no-strict",
        action="store_true",
        help="Continue building even if verbatim source coverage audit reports missing files/lines.",
    )
    parser.add_argument(
        "--md-only",
        action="store_true",
        help="Only run source audit and build ABOS_Book.md (skip PDF rendering).",
    )
    args = parser.parse_args()

    # 1. Load chapters in canonical order
    missing_chaps = [
        fn for fn in EXPECTED_CHAPTERS if not os.path.exists(os.path.join(CHAPTERS_DIR, fn))
    ]
    if missing_chaps and not args.no_strict:
        print(f"ERROR: Missing chapter files in {CHAPTERS_DIR}:", file=sys.stderr)
        for fn in missing_chaps:
            print(f"  - {fn}", file=sys.stderr)
        return 1

    present_chaps = [
        fn for fn in EXPECTED_CHAPTERS if os.path.exists(os.path.join(CHAPTERS_DIR, fn))
    ]
    chapters_text: list[tuple[str, str]] = []
    for fn in present_chaps:
        with open(os.path.join(CHAPTERS_DIR, fn), "r", encoding="utf-8") as fh:
            chapters_text.append((fn, fh.read()))

    print(f"Loaded {len(chapters_text)}/{len(EXPECTED_CHAPTERS)} chapter files.")

    # 2. Verbatim Source Coverage Audit
    audit_ok, audit_report = audit_source_coverage(chapters_text)
    print(audit_report)
    if not audit_ok and not args.no_strict:
        return 2

    # 3. Assemble combined Markdown book
    combined_md = build_combined_markdown(chapters_text)
    with open(OUT_MD, "w", encoding="utf-8") as fh:
        fh.write(combined_md)
    md_lines = combined_md.count("\n") + 1
    md_kb = os.path.getsize(OUT_MD) / 1024.0
    print(f"Wrote combined Markdown: {OUT_MD} ({md_lines:,} lines, {md_kb:,.1f} KB)")

    if args.md_only:
        return 0

    # 4. Two-Pass PDF Generation
    chrome_bin = find_chrome_binary()
    with tempfile.TemporaryDirectory(prefix="abos_book_build_") as tmpdir:
        pass1_html = os.path.join(tmpdir, "book_pass1.html")
        pass1_pdf = os.path.join(tmpdir, "book_pass1.pdf")
        pass2_html = os.path.join(tmpdir, "book_pass2.html")

        print("Pass 1/2: Rendering layout & measuring exact TOC page numbers...")
        html1, toc1 = build_eclk_html_document(
            chapters_text, page_map=None, emit_page_markers=True
        )
        with open(pass1_html, "w", encoding="utf-8") as fh:
            fh.write(html1)
        render_html_to_pdf(chrome_bin, pass1_html, pass1_pdf)

        page_map = extract_page_markers(pass1_pdf)
        print(
            f"  Resolved {len(page_map)}/{len(toc1)} TOC entries across Pass-1 PDF."
        )

        print("Pass 2/2: Rendering final print-ready A4 PDF...")
        html2, _ = build_eclk_html_document(
            chapters_text, page_map=page_map, emit_page_markers=False
        )
        with open(pass2_html, "w", encoding="utf-8") as fh:
            fh.write(html2)
        render_html_to_pdf(chrome_bin, pass2_html, OUT_PDF)

    pdf_mb = os.path.getsize(OUT_PDF) / (1024.0 * 1024.0)
    print(f"Wrote final PDF: {OUT_PDF} ({pdf_mb:,.2f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
