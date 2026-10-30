# A Heavily Commented ABOS Microkernel Source Code

**Author:** Liping Liu (`lipingliu@google.com`)  
**Target Edition:** ABOS Version 1.0 (`lesson_20/` — Complete 4-Core SMP ARMv8-A Capability Microkernel)

Modeled in structure, visual layout, and pedagogical spirit after Dr. Zhao Jiong's classic *"A Heavily Commented Linux Kernel Source Code (Kernel Version 0.12)"*, this directory contains the complete book for **ABOS** in both Markdown and print-ready A4 PDF formats.

## Deliverables

- **[`ABOS_Book.md`](ABOS_Book.md)**: Single-file complete Markdown edition (Front Matter, linked Table of Contents, Chapters 1–17, References, and Appendices A1–A4).
- **[`ABOS_Book.pdf`](ABOS_Book.pdf)**: Typeset A4 PDF edition featuring cover & inner title page artwork, two-column Table of Contents with exact page numbers, per-chapter running headers, vector architecture diagrams, and blue-underlined verbatim source line numbers interleaved with author commentary.
- **[`chapters/`](chapters/)**: Individual per-chapter Markdown manuscripts (`00_preface.md` through `18_references_appendix.md`).
- **[`assets/`](assets/)**: Cover photography, ABOS armadillo mascot illustration, and vector SVG system architecture diagrams.
- **[`build_book.py`](build_book.py)**: Reproducible pure-Python 3 build & verification pipeline.
- **[`weave_helper.py`](weave_helper.py)**: Helper module that weaves verbatim `lesson_20/` source files (`1..N`) with `////` and `##` author commentary blocks.

## Rebuilding & Verifying Source Fidelity

From the repository root (or inside `book/`), run:

```bash
python3 book/build_book.py
```

`build_book.py` automatically:
1. Audits all **58 non-documentation source, header, assembly, linker, and build files** in `../lesson_20/` (**9,526 lines** total) to verify **100% contiguous, verbatim line-for-line coverage** across the `Program X-Y` listings.
2. Rebuilds `book/ABOS_Book.md`.
3. Runs a two-pass headless Chrome build to resolve exact Table of Contents page numbers and generate `book/ABOS_Book.pdf`.
