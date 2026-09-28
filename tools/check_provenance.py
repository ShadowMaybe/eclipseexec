#!/usr/bin/env python3
"""Verify this tree contains no text lifted from anything under ref/.

Written because "we read the reference implementations" and "we copied them"
are different claims, and only one of them is acceptable. The scanner answers a
narrow question: what is the *longest* run of tokens this project shares with
each reference file?

Counting shared fragments does not work — any two C programs share thousands of
10-token runs (`for ( size_t i = 0 ; i < n ; i ++ )` and friends), which is how
the first version of this produced 2802 false alarms. Longest-run length does
work: routine boilerplate tops out around 15 tokens, while a real copy is a
sentence long. So the output is a small, readable list with the actual shared
text attached, and a human decides.

Two streams per file, because they leak differently:
  code    comments and literals stripped, punctuation kept as tokens
  comment the comments themselves, as words (copied prose is still copying)

Usage: tools/check_provenance.py [ref-dir ...]
Exit 1 when a run reaches SUSPICIOUS_TOKENS in either stream.
"""

from __future__ import annotations

import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

SOURCE_SUFFIXES = {".c", ".h", ".cpp", ".cc", ".cxx", ".hpp", ".java", ".kt", ".gradle", ".pro"}
SHINGLE = 10             # tokens that make a candidate worth extending
CANDIDATE_MIN_HITS = 4   # distinct shingles before a pair is compared in detail
SUSPICIOUS_TOKENS = 25   # a code run this long is not boilerplate
SUSPICIOUS_WORDS = 20    # a comment run this long is not boilerplate

TOKEN = re.compile(r"[A-Za-z_]\w*|\d+|[^\s\w]")
WORD = re.compile(r"[A-Za-z][A-Za-z'-]{3,}")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(?:[^"\\\n]|\\.)*"', ' " " ', text)
    text = re.sub(r"'(?:[^'\\\n]|\\.)*'", " ' ' ", text)
    return text


def comment_text(text: str) -> str:
    blocks = re.findall(r"/\*.*?\*/", text, flags=re.S)
    blocks += [m for m in re.findall(r"//[^\n]*", text)]
    return "\n".join(blocks)


def meaningful(tokens: list[str]) -> list[str]:
    """Drop #include blocks and punctuation-only noise from a candidate run.

    Measuring raw runs counts things nobody wrote. Five consecutive standard
    includes score 30 tokens, and every C file on earth shares them — that is
    how the first version of this flagged 98 pairs over a block of
    `#include <stdio.h>`. An array of blanked-out string literals scores the
    same way. What is left is the part a person would actually recognise as
    someone's writing, which is the only thing worth showing.
    """
    out: list[str] = []
    i = 0
    while i < len(tokens):
        if tokens[i] == "#" and i + 1 < len(tokens) and tokens[i + 1] == "include":
            i += 2
            if i < len(tokens) and tokens[i] == "<":
                while i < len(tokens) and tokens[i] != ">":
                    i += 1
                i += 1
            elif i < len(tokens) and tokens[i] == '"':
                while i < len(tokens) and tokens[i] != '"':
                    i += 1
                i += 1
            continue
        if tokens[i] not in ('"', ",", ";", "{", "}"):
            out.append(tokens[i])
        i += 1
    return out


def streams(path: Path) -> tuple[list[str], list[str]]:
    """Return (code tokens, comment words) for a file."""
    text = path.read_text(encoding="utf-8", errors="replace")
    code = [t.lower() for t in TOKEN.findall(strip_comments(text))]
    comment = [w.lower() for w in WORD.findall(comment_text(text))]
    return code, comment


def shingles(tokens: list[str]) -> set[tuple[str, ...]]:
    return {tuple(tokens[i : i + SHINGLE]) for i in range(len(tokens) - SHINGLE + 1)}


def longest_run(own: list[str], other: list[str]) -> tuple[int, int]:
    """Longest run of identical tokens shared by both, and where it starts."""
    index: dict[tuple[str, ...], list[int]] = defaultdict(list)
    for i in range(len(other) - SHINGLE + 1):
        index[tuple(other[i : i + SHINGLE])].append(i)

    best_len, best_at = 0, 0
    i = 0
    limit = len(own) - SHINGLE + 1
    while i < limit:
        hits = index.get(tuple(own[i : i + SHINGLE]))
        if hits:
            for start in hits:
                length = SHINGLE
                while (
                    i + length < len(own)
                    and start + length < len(other)
                    and own[i + length] == other[start + length]
                ):
                    length += 1
                if length > best_len:
                    best_len, best_at = length, i
            # Step past the part of this run we already measured.
            i += max(1, best_len - SHINGLE + 1) if best_len else 1
        else:
            i += 1
    return best_len, best_at


def snippet(tokens: list[str], start: int, length: int, limit: int = 160) -> str:
    text = " ".join(tokens[start : start + length])
    return text if len(text) <= limit else text[:limit] + " …"


def own_files() -> list[Path]:
    files = []
    for path in ROOT.rglob("*"):
        if not path.is_file():
            continue
        parts = set(path.relative_to(ROOT).parts)
        if ".git" in parts or "build" in parts or "ref" in parts:
            continue
        if path.suffix.lower() in SOURCE_SUFFIXES | {".md", ".sh", ".py"}:
            files.append(path)
    return sorted(files)


def main() -> int:
    ref_roots = [Path(arg) for arg in sys.argv[1:]] or [ROOT.parent / "ref"]
    ref_roots = [r for r in ref_roots if r.exists()]
    if not ref_roots:
        print(f"no reference tree at {ref_roots}", file=sys.stderr)
        return 2

    targets = own_files()
    print(f"checking {len(targets)} files against {', '.join(str(r) for r in ref_roots)}\n")

    # Phase one: cheap set membership to find which pairs are worth comparing.
    own_code = {p: shingles(streams(p)[0]) for p in targets}
    own_comment = {p: shingles(streams(p)[1]) for p in targets}
    candidate_hits: dict[tuple[Path, Path], int] = defaultdict(int)

    ref_files = [
        p for r in ref_roots for p in r.rglob("*") if p.is_file() and p.suffix.lower() in SOURCE_SUFFIXES
    ]
    print(f"indexing {len(ref_files)} reference files ...")
    for ref in ref_files:
        try:
            ref_code, ref_comment = streams(ref)
        except OSError:
            continue
        if not ref_code:
            continue
        ref_shingles = shingles(ref_code)
        ref_words = shingles(ref_comment)
        for path in targets:
            hits = len(ref_shingles & own_code[path])
            if hits >= CANDIDATE_MIN_HITS:
                candidate_hits[(path, ref)] += hits
            elif ref_words and own_comment[path]:
                word_hits = len(ref_words & own_comment[path])
                if word_hits >= CANDIDATE_MIN_HITS:
                    candidate_hits[(path, ref)] += 0  # candidate for the comment pass only

    print(f"{len(candidate_hits)} file pair(s) share enough to compare in detail\n")

    # Phase two: exact longest run for each candidate.
    worst_code, worst_comment = 0, 0
    findings = []
    for (path, ref), _ in sorted(candidate_hits.items(), key=lambda kv: str(kv[0])):
        own_tokens, own_words = streams(path)
        ref_tokens, ref_words = streams(ref)

        length, at = longest_run(own_tokens, ref_tokens)
        run = own_tokens[at : at + length]
        score = len(meaningful(run))
        if score > worst_code:
            worst_code = score
        if score >= SUSPICIOUS_TOKENS:
            findings.append(("CODE", score, path, ref, snippet(run, 0, len(run))))

        length, at = longest_run(own_words, ref_words)
        if length > worst_comment:
            worst_comment = length
        if length >= SUSPICIOUS_WORDS:
            findings.append(
                ("COMMENT", length, path, ref, snippet(own_words, at, length))
            )

    for kind, length, path, ref, text in sorted(findings, key=lambda f: -f[1]):
        print(f"[{kind} {length} tokens] {path.relative_to(ROOT)}")
        print(f"    also in: {ref}")
        print(f"    shared:  {text}\n")

    print("longest recognisable shared code run:    %d tokens (suspicious at %d)"
          % (worst_code, SUSPICIOUS_TOKENS))
    print("longest shared comment run:             %d words   (suspicious at %d)"
          % (worst_comment, SUSPICIOUS_WORDS))

    if findings:
        print(f"\n{len(findings)} run(s) long enough to be a copy — read them and judge")
        return 1
    print("\nnothing long enough to be copied text: every shared run is boilerplate")
    return 0


if __name__ == "__main__":
    sys.exit(main())
