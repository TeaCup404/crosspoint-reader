"""'Story so far' recaps for reader-hub (homebot's local LLM via the GPU arbiter).

The book's text comes from the Calibre-Web library (metadata.db + the EPUB),
split into ~12k-character sections. Each section is summarised once, in the
background, and cached on disk; groups of 10 section summaries are compressed
again so a long book still fits one prompt. A recap for a reading position
uses the summaries of everything before it plus the last pages read, and the
model is told not to go past that point.

Every request is non-blocking: the reader gets {"status": "preparing", ...}
until the recap is cached, then {"status": "ready", "recap": ...}.
"""
import html.parser
import json
import os
import posixpath
import queue
import re
import sqlite3
import threading
import time
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

LIBRARY = os.environ.get("READER_HUB_LIBRARY", "/srv/media/books/library")
CACHE = os.path.join(os.environ.get("READER_HUB_ROOT", "/srv/fast/reader-hub"), "recap")
OLLAMA = os.environ.get("READER_HUB_OLLAMA", "http://127.0.0.1:11434")  # gpu-arbiter
# The arbiter keeps one model on the 6 GB card; qwen2.5:7b is the one other
# homebot services already keep resident, so recaps don't force a swap.
MODEL = os.environ.get("READER_HUB_MODEL", "qwen2.5:7b")
SECTION_CHARS = 12000
GROUP = 10
TAIL_CHARS = 2500
RECAP_VERSION = 3  # bump when the recap prompt changes (old cached recaps are ignored)
PREFETCH_AHEAD = 10

_jobs = queue.Queue()
_pending = set()
_lock = threading.Lock()


# ---------------------------------------------------------------- library

def find_book(title, author):
    db = sqlite3.connect(f"file:{os.path.join(LIBRARY, 'metadata.db')}?mode=ro", uri=True)
    try:
        rows = db.execute(
            "SELECT b.id, b.title, b.path, group_concat(a.name, ' & ') FROM books b "
            "LEFT JOIN books_authors_link l ON l.book = b.id LEFT JOIN authors a ON a.id = l.author "
            "GROUP BY b.id").fetchall()
    finally:
        db.close()

    def norm(s):
        return re.sub(r"[^a-z0-9]+", " ", (s or "").lower()).strip()

    t, a = norm(title), norm(author)
    best = None
    for book_id, btitle, path, bauthor in rows:
        bt = norm(btitle)
        score = 3 if bt == t else 2 if t and (t in bt or bt in t) else 0
        if score and a and norm(bauthor) and (a in norm(bauthor) or norm(bauthor) in a):
            score += 1
        if score and (best is None or score > best[0]):
            best = (score, book_id, btitle, bauthor, path)
    if not best:
        return None
    _, book_id, btitle, bauthor, path = best
    folder = os.path.join(LIBRARY, path)
    epubs = [f for f in os.listdir(folder) if f.lower().endswith(".epub")]
    if not epubs:
        return None
    return {"id": book_id, "title": btitle, "author": bauthor or "", "epub": os.path.join(folder, epubs[0])}


class _Text(html.parser.HTMLParser):
    BLOCK = {"p", "div", "br", "h1", "h2", "h3", "h4", "li", "blockquote", "section", "tr"}

    def __init__(self):
        super().__init__()
        self.out, self.skip = [], 0

    def handle_starttag(self, tag, attrs):
        if tag in ("script", "style", "head"):
            self.skip += 1
        elif tag in self.BLOCK:
            self.out.append("\n")

    def handle_endtag(self, tag):
        if tag in ("script", "style", "head") and self.skip:
            self.skip -= 1

    def handle_data(self, data):
        if not self.skip:
            self.out.append(data)


def epub_text(path):
    with zipfile.ZipFile(path) as z:
        container = ET.fromstring(z.read("META-INF/container.xml"))
        opf_path = container.find(".//{*}rootfile").get("full-path")
        opf = ET.fromstring(z.read(opf_path))
        base = posixpath.dirname(opf_path)
        manifest = {i.get("id"): i.get("href") for i in opf.find("{*}manifest")}
        parts = []
        for ref in opf.find("{*}spine"):
            href = manifest.get(ref.get("idref"))
            if not href:
                continue
            name = posixpath.normpath(posixpath.join(base, href.split("#")[0]))
            try:
                raw = z.read(name).decode("utf-8", errors="replace")
            except KeyError:
                continue
            p = _Text()
            p.feed(raw)
            text = re.sub(r"[ \t\r\f\v]+", " ", "".join(p.out))
            text = re.sub(r"\n\s*\n+", "\n\n", text).strip()
            if text:
                parts.append(text)
    return "\n\n".join(parts)


def sections(text):
    out, start = [], 0
    while start < len(text):
        end = min(len(text), start + SECTION_CHARS)
        if end < len(text):
            cut = text.rfind("\n\n", start + SECTION_CHARS // 2, end)
            end = cut if cut > 0 else end
        out.append(text[start:end])
        start = end
    return out


# ---------------------------------------------------------------- model

def ask(system, prompt, max_tokens):
    body = json.dumps({
        "model": MODEL, "stream": False,
        "messages": [{"role": "system", "content": system}, {"role": "user", "content": prompt}],
        "options": {"num_ctx": 8192, "num_predict": max_tokens, "temperature": 0.3},
    }).encode()
    req = urllib.request.Request(f"{OLLAMA}/api/chat", body, {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=300) as r:
        return json.load(r)["message"]["content"].strip()


SECTION_SYSTEM = ("You summarise one passage of a novel for a reader's private notes. "
                  "Write 3-5 plain sentences: who appears, what happens, what changes. "
                  "Only use the passage. No preamble.")
GROUP_SYSTEM = ("You merge consecutive passage summaries of a novel into one summary of "
                "5-8 sentences, keeping names, key events and their order. No preamble.")
RECAP_SYSTEM = ("You help a reader pick a novel back up after a break. Using only the notes given, "
                "write about 100 words of 'story so far': who the main characters are and the key "
                "events in order, drawn from the notes. Then a new paragraph of one or two sentences "
                "starting 'Where you left off:' saying exactly where the last pages end. Never reveal, "
                "hint at or guess anything that happens later. Plain prose, no headings, no labels "
                "on the first paragraph, and keep the last pages out of the first paragraph.")


# ---------------------------------------------------------------- cache

class Book:
    def __init__(self, info):
        self.info = info
        self.dir = os.path.join(CACHE, str(info["id"]))
        os.makedirs(self.dir, exist_ok=True)
        self.state_path = os.path.join(self.dir, "state.json")
        try:
            with open(self.state_path) as f:
                self.state = json.load(f)
        except (OSError, ValueError):
            self.state = {}
        mtime = os.path.getmtime(info["epub"])
        if self.state.get("mtime") != mtime:
            self.state = {"mtime": mtime, "sections": {}, "groups": {}, "recaps": {}}
        self._text = None

    def save(self):
        tmp = self.state_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.state, f)
        os.replace(tmp, self.state_path)

    def parts(self):
        if self._text is None:
            self._text = sections(epub_text(self.info["epub"]))
        return self._text

    def section_summary(self, i):
        key = str(i)
        if key not in self.state["sections"]:
            prompt = f"Novel: {self.info['title']} by {self.info['author']}\n\nPassage:\n{self.parts()[i]}"
            self.state["sections"][key] = ask(SECTION_SYSTEM, prompt, 220)
            self.save()
        return self.state["sections"][key]

    def group_summary(self, g):
        key = str(g)
        if key not in self.state["groups"]:
            notes = "\n\n".join(self.section_summary(i) for i in range(g * GROUP, (g + 1) * GROUP))
            self.state["groups"][key] = ask(GROUP_SYSTEM, notes, 350)
            self.save()
        return self.state["groups"][key]

    def recap(self, pos):
        """pos = character offset of the reading position."""
        parts = self.parts()
        offsets, acc = [], 0
        for p in parts:
            offsets.append(acc)
            acc += len(p)
        cur = max(i for i, o in enumerate(offsets) if o <= pos)
        key = f"v{RECAP_VERSION}:{cur}:{(pos - offsets[cur]) * 10 // max(1, len(parts[cur]))}"
        if key in self.state["recaps"]:
            return self.state["recaps"][key]
        notes = [self.group_summary(g) for g in range(cur // GROUP)]
        notes += [self.section_summary(i) for i in range((cur // GROUP) * GROUP, cur)]
        tail = parts[cur][: pos - offsets[cur]][-TAIL_CHARS:]
        prompt = (f"Novel: {self.info['title']} by {self.info['author']}\n\n"
                  f"Notes on everything read so far, in order:\n" + "\n\n".join(notes) +
                  f"\n\nThe last pages the reader read:\n{tail}")
        self.state["recaps"][key] = ask(RECAP_SYSTEM, prompt, 260)
        self.save()
        return self.state["recaps"][key]

    def progress(self, pos):
        """(done, total) model calls still needed for this position."""
        parts = self.parts()
        acc, cur = 0, 0
        for i, p in enumerate(parts):
            if acc + len(p) > pos:
                cur = i
                break
            acc += len(p)
            cur = i
        need_sections = range((cur // GROUP) * GROUP, cur)
        groups = range(cur // GROUP)
        total = len(need_sections) + len(groups) + sum(GROUP for _ in groups) + 1
        done = sum(1 for i in need_sections if str(i) in self.state["sections"])
        for g in groups:
            if str(g) in self.state["groups"]:
                done += GROUP + 1
            else:
                done += sum(1 for i in range(g * GROUP, (g + 1) * GROUP) if str(i) in self.state["sections"])
        return done, total


def _prefetch(book, pos):
    """While nothing else is queued, summarise the sections just ahead of the
    reader so the next recap is quick."""
    parts = book.parts()
    acc, cur = 0, 0
    for i, p in enumerate(parts):
        if acc + len(p) > pos:
            cur = i
            break
        acc += len(p)
    for i in range(cur, min(len(parts), cur + PREFETCH_AHEAD)):
        if not _jobs.empty():
            return
        book.section_summary(i)


def _worker():
    while True:
        book_id, info, pos = _jobs.get()
        try:
            book = Book(info)
            book.recap(pos)
            with _lock:
                _pending.discard((book_id, pos))
            _prefetch(book, pos)
        except Exception as e:  # keep the worker alive; the next request retries
            print(f"recap: {info['title']}: {e}", flush=True)
        finally:
            with _lock:
                _pending.discard((book_id, pos))


threading.Thread(target=_worker, daemon=True).start()


def handle(title, author, pct):
    started = time.time()
    info = find_book(title, author)
    if not info:
        return 404, {"status": "error", "message": "This book isn't in your library yet"}
    book = Book(info)
    total_chars = sum(len(p) for p in book.parts())
    pct = min(max(pct, 0.0), 1.0)
    pos = int(total_chars * pct)
    parts = book.parts()
    offsets, acc = [], 0
    for p in parts:
        offsets.append(acc)
        acc += len(p)
    cur = max(i for i, o in enumerate(offsets) if o <= pos)
    key = f"v{RECAP_VERSION}:{cur}:{(pos - offsets[cur]) * 10 // max(1, len(parts[cur]))}"
    if key in book.state["recaps"]:
        return 200, {"status": "ready", "title": info["title"], "pct": f"{pct * 100:.0f}%",
                     "recap": book.state["recaps"][key], "ms": int((time.time() - started) * 1000)}
    with _lock:
        if (info["id"], pos) not in _pending:
            _pending.add((info["id"], pos))
            _jobs.put((info["id"], info, pos))
    done, total = book.progress(pos)
    return 202, {"status": "preparing", "title": info["title"], "done": done, "total": total}
