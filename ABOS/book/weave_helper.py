#!/usr/bin/env python3
"""Helper for weaving verbatim ABOS lesson_20 source files with ECLK-style author commentary."""

import os
import textwrap

BOOK_DIR = os.path.dirname(os.path.abspath(__file__))
ABOS_ROOT = os.path.dirname(BOOK_DIR)
LESSON20_DIR = os.path.join(ABOS_ROOT, "lesson_20")


def _lang_and_prefix(rel_path: str) -> tuple[str, str]:
    base = os.path.basename(rel_path)
    if rel_path.endswith((".c", ".h")):
        return "c", "////"
    if rel_path.endswith(".S"):
        return "asm", "##"
    if rel_path.endswith(".ld"):
        return "ld", "////"
    if rel_path.endswith(".py"):
        return "python", "##"
    if rel_path.endswith(".sh"):
        return "bash", "##"
    if base == "Makefile" or rel_path.endswith(".mk"):
        return "makefile", "##"
    return "text", "##"


def _format_comment_block(comment, prefix: str) -> list[str]:
    if isinstance(comment, str):
        paragraphs = comment.strip().split("\n")
    else:
        paragraphs = list(comment)

    out = []
    indent = f"       {prefix} "
    for para in paragraphs:
        s = para.rstrip()
        if not s:
            out.append(f"       {prefix}")
            continue
        # Preserve explicit pre-indented ASCII tables/diagrams inside comments
        if s.startswith(("  ", "|", "+--", "* ", "- ")):
            out.append(f"{indent}{s}")
        else:
            wrapped = textwrap.wrap(
                s,
                width=78,
                break_long_words=False,
                break_on_hyphens=False,
            )
            for w in wrapped:
                out.append(f"{indent}{w}")
    return out


def weave_file(
    rel_path: str,
    prog_label: str,
    annotations: dict | None = None,
    line_range: tuple[int, int] | None = None,
) -> str:
    """Return a Markdown ECLK-style annotated code listing for lesson_20/<rel_path>.

    Args:
        rel_path: Path relative to lesson_20/, e.g. 'kernel/core/start.S'.
        prog_label: E.g. 'Program 6-2'.
        annotations: Dict mapping 1-based line number (int) -> str or list[str] of
            author commentary inserted immediately BEFORE that line. Optional key
            'eof' appends commentary after the last line in the range.
        line_range: Optional (start_line, end_line) 1-based inclusive slice for
            splitting large files (such as object.c or syscalls.c) across sections.
    """
    annotations = annotations or {}
    full_path = os.path.join(LESSON20_DIR, rel_path)
    with open(full_path, "r", encoding="utf-8") as fh:
        raw_lines = [ln.rstrip("\r\n") for ln in fh.readlines()]

    total = len(raw_lines)
    start_ln = line_range[0] if line_range else 1
    end_ln = line_range[1] if line_range else total
    assert 1 <= start_ln <= end_ln <= total, (
        f"Invalid line_range {line_range} for {rel_path} ({total} lines)"
    )

    for k in annotations:
        if k == "eof":
            continue
        assert isinstance(k, int) and start_ln <= k <= end_ln, (
            f"Annotation line {k} outside range {start_ln}..{end_ln} for {rel_path}"
        )

    lang, prefix = _lang_and_prefix(rel_path)
    range_suffix = (
        f" (Lines {start_ln}–{end_ln})" if (start_ln != 1 or end_ln != total) else ""
    )
    title = f"#### {prog_label}: `lesson_20/{rel_path}`{range_suffix}"
    meta = (
        f'<!-- ECLK_CODE_LISTING file="lesson_20/{rel_path}" '
        f'start="{start_ln}" end="{end_ln}" total="{total}" -->'
    )

    body = [title, "", meta, f"```{lang}"]
    for ln in range(start_ln, end_ln + 1):
        if ln in annotations:
            body.extend(_format_comment_block(annotations[ln], prefix))
        body.append(f"{ln:4d} | {raw_lines[ln - 1]}")

    if "eof" in annotations:
        body.extend(_format_comment_block(annotations["eof"], prefix))

    body.append("```")
    body.append("")
    return "\n".join(body)
