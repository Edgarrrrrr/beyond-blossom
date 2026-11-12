#!/usr/bin/env python3
"""Export an immutable Git or explicitly labelled snapshot diff for offline review.

Standard library only. Does not execute source, modify Git, follow snapshot
symlinks, fetch dependencies, or silently truncate a comparison.
"""
from __future__ import annotations

import argparse
import difflib
import hashlib
import html
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys

VERSION = 3
DEFAULT_LIMIT = 500_000


class DiffError(Exception):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest()


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def esc(value):
    return html.escape(str(value), quote=True)


def filters(values):
    result = []
    for value in values:
        if not value or "\x00" in value or "\n" in value or "\r" in value:
            raise DiffError("--path must be a nonempty relative file or directory")
        p = PurePosixPath(value)
        if p.is_absolute() or ".." in p.parts or value.startswith(":"):
            raise DiffError("--path must be literal and relative, without '..' or ':' prefix")
        normalized = p.as_posix().rstrip("/")
        if normalized not in result:
            result.append(normalized)
    return result


def selected(path, scope):
    return not scope or any(p == "." or path == p or path.startswith(p + "/") for p in scope)


def decode_path(value):
    try:
        return value.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise DiffError("Non-UTF-8 filenames cannot be rendered; use a narrower --path scope") from exc


def git_run(repo, arguments, commands):
    command = ["git", "--no-pager", "--literal-pathspecs", "-c", "color.ui=false",
               "-c", "core.quotePath=true", "-C", str(repo), *arguments]
    commands.append(command)
    process = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if process.returncode:
        raise DiffError("Git command failed: " + process.stderr.decode("utf-8", errors="replace").strip())
    return process.stdout


def git_revision(repo, revision, commands):
    value = git_run(repo, ["rev-parse", "--verify", "--end-of-options", revision + "^{commit}"], commands)
    full = value.decode("ascii").strip()
    if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", full):
        raise DiffError("Git did not return a full commit object ID")
    return full


def git_content(repo, mode, oid, commands):
    if mode == "000000":
        return None
    if mode == "160000":
        # A gitlink is an object ID, never dereferenced as source.
        return ("Subproject commit " + oid + "\n").encode("ascii")
    return git_run(repo, ["cat-file", "blob", oid], commands)


def side(mode, oid, data):
    if data is None:
        return None
    return {"mode": mode, "git_object": oid, "bytes": len(data),
            "sha256": digest(data), "kind": "gitlink" if mode == "160000" else
            "symlink" if mode == "120000" else "file"}


def git_comparison(args, scope):
    repo = Path(args.repo).resolve(strict=True)
    commands = []
    base = git_revision(repo, args.base, commands)
    candidate = git_revision(repo, args.candidate, commands)
    common = ["--no-ext-diff", "--no-textconv", "--find-renames", base, candidate, "--", *scope]
    raw = git_run(repo, ["diff", "--raw", "-z", "--no-abbrev", *common], commands)
    patch = git_run(repo, ["diff", "--binary", "--full-index", *common], commands)
    tokens = raw.split(b"\0")
    records = []
    i = 0
    while i < len(tokens) and tokens[i]:
        metadata = tokens[i].decode("ascii").split()
        i += 1
        if len(metadata) != 5 or not metadata[0].startswith(":"):
            raise DiffError("Unsupported Git --raw record")
        old_mode, new_mode, old_oid, new_oid, status = metadata
        old_mode = old_mode[1:]
        old_path = decode_path(tokens[i])
        i += 1
        new_path = old_path
        if status[0] in "RC":
            new_path = decode_path(tokens[i])
            i += 1
        old_data = git_content(repo, old_mode, old_oid, commands)
        new_data = git_content(repo, new_mode, new_oid, commands)
        records.append({"status": status, "old_path": old_path if old_data is not None else None,
                        "new_path": new_path if new_data is not None else None,
                        "base": side(old_mode, old_oid, old_data),
                        "candidate": side(new_mode, new_oid, new_data),
                        "_old": old_data, "_new": new_data})
    identity = {"kind": "git_commits", "repository": str(repo),
                "base": {"requested": args.base, "commit": base},
                "candidate": {"requested": args.candidate, "commit": candidate}}
    return identity, records, patch, commands


def snapshot_inventory(root, scope):
    records = {}
    for directory, dirs, files in os.walk(root, followlinks=False):
        dirs[:] = sorted(d for d in dirs if d != ".git")
        links = [d for d in dirs if (Path(directory) / d).is_symlink()]
        dirs[:] = [d for d in dirs if d not in links]
        for filename in sorted(files + links):
            path = Path(directory) / filename
            relative = path.relative_to(root).as_posix()
            if filename == ".git" or not selected(relative, scope):
                continue
            try:
                relative.encode("utf-8")
            except UnicodeEncodeError as exc:
                raise DiffError("Non-UTF-8 snapshot filename; narrow --path scope") from exc
            before = path.lstat()
            if stat.S_ISLNK(before.st_mode):
                data = os.fsencode(os.readlink(path))
                mode = "120000"
            elif stat.S_ISREG(before.st_mode):
                data = path.read_bytes()
                mode = "100755" if before.st_mode & 0o111 else "100644"
            else:
                raise DiffError("Snapshot contains special file: " + relative)
            after = path.lstat()
            if (before.st_ino, before.st_size, before.st_mtime_ns, before.st_mode) != (
                    after.st_ino, after.st_size, after.st_mtime_ns, after.st_mode):
                raise DiffError("Snapshot changed while being read: " + relative)
            records[relative] = {**side(mode, None, data), "_data": data}
    return records


def snapshot_tree(records):
    inventory = [{"path": p, **{k: v for k, v in record.items() if not k.startswith("_")}}
                 for p, record in sorted(records.items())]
    return {"scoped_tree_sha256": digest(json_bytes(inventory)), "file_count": len(inventory),
            "inventory": inventory}


def quote_patch_path(path):
    # JSON quoting makes unusual names unambiguous in the human-readable snapshot patch.
    return json.dumps(path, ensure_ascii=False)


def snapshot_patch(records):
    pieces = [b"# Labelled snapshot comparison; this is not a Git commit diff.\n"]
    for record in records:
        old_path, new_path = record["old_path"], record["new_path"]
        pieces.append(("diff --snapshot " + quote_patch_path(old_path) + " " +
                       quote_patch_path(new_path) + "\n").encode("utf-8"))
        for label in ("base", "candidate"):
            item = record[label]
            pieces.append((label + ": " + (json.dumps(item, sort_keys=True) if item else "absent") + "\n").encode())
        old, new = record["_old"] or b"", record["_new"] or b""
        if text_content(old) is None or text_content(new) is None:
            pieces.append(b"Binary or non-UTF-8 content differs; see SHA256 and byte counts above.\n")
            continue
        lines = difflib.diff_bytes(difflib.unified_diff, old.splitlines(keepends=True),
                                  new.splitlines(keepends=True),
                                  fromfile=(old_path or "/dev/null").encode("utf-8"),
                                  tofile=(new_path or "/dev/null").encode("utf-8"))
        for line in lines:
            pieces.append(line)
            if not line.endswith(b"\n"):
                pieces.append(b"\n\\ No newline at end of file\n")
    return b"".join(pieces)


def snapshot_comparison(args, scope):
    base = Path(args.base_dir).resolve(strict=True)
    candidate = Path(args.candidate_dir).resolve(strict=True)
    out = Path(args.out).resolve()
    for root in (base, candidate):
        if not root.is_dir():
            raise DiffError("Snapshot inputs must be directories")
        if out == root or root in out.parents:
            raise DiffError("--out must be outside both snapshot directories")
    old = snapshot_inventory(base, scope)
    new = snapshot_inventory(candidate, scope)
    records = []
    for path in sorted(old.keys() | new.keys()):
        a, b = old.get(path), new.get(path)
        if a and b and a["sha256"] == b["sha256"] and a["mode"] == b["mode"]:
            continue
        status = "A" if not a else "D" if not b else "T" if a["kind"] != b["kind"] else "M"
        records.append({"status": status, "old_path": path if a else None,
                        "new_path": path if b else None,
                        "base": {k: v for k, v in a.items() if not k.startswith("_")} if a else None,
                        "candidate": {k: v for k, v in b.items() if not k.startswith("_")} if b else None,
                        "_old": a["_data"] if a else None, "_new": b["_data"] if b else None})
    # Detect identical renames only; ambiguous duplicates remain explicit A/D records.
    removed = [r for r in records if r["status"] == "D"]
    added = [r for r in records if r["status"] == "A"]
    rename_records = []
    consumed = set()
    for a in removed:
        match = [b for b in added if b["candidate"]["sha256"] == a["base"]["sha256"] and
                 b["candidate"]["mode"] == a["base"]["mode"]]
        reverse = [r for r in removed if r["base"]["sha256"] == a["base"]["sha256"] and
                   r["base"]["mode"] == a["base"]["mode"]]
        if len(match) == len(reverse) == 1:
            b = match[0]
            consumed.update((id(a), id(b)))
            rename_records.append({**a, "status": "R100", "new_path": b["new_path"],
                                   "candidate": b["candidate"], "_new": b["_new"]})
    records = sorted([r for r in records if id(r) not in consumed] + rename_records,
                     key=lambda r: r["new_path"] or r["old_path"])
    identity = {"kind": "labelled_snapshots", "base": {"label": args.base_label,
                "directory": str(base), **snapshot_tree(old)}, "candidate": {
                "label": args.candidate_label, "directory": str(candidate), **snapshot_tree(new)}}
    return identity, records, snapshot_patch(records), []


def text_content(data):
    if b"\0" in data:
        return None
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def text_lines(text):
    # Git considers LF the line separator; retain CR and the final newline state.
    lines = text.split("\n")
    if lines[-1] == "":
        lines.pop()
    return [line + ("\n" if i < len(lines) - 1 or text.endswith("\n") else "")
            for i, line in enumerate(lines)]


def code_cell(line):
    if line is None:
        return ""
    suffix = '<span class="eol" title="No newline at end of file"> [no LF]</span>' if not line.endswith("\n") else ""
    return esc(line.rstrip("\n")) + suffix


def row_side(old_num, old, new_num, new, old_class="", new_class=""):
    return (f'<tr><td class="ln">{old_num or ""}</td><td class="code {old_class}">' +
            ("<span class=marker>−</span>" if old_class == "del" else "") + code_cell(old) +
            f'</td><td class="ln">{new_num or ""}</td><td class="code {new_class}">' +
            ("<span class=marker>+</span>" if new_class == "add" else "") + code_cell(new) + '</td></tr>')


def row_unified(old_num, new_num, line, cls=""):
    return (f'<tr><td class="ln">{old_num or ""}</td><td class="ln">{new_num or ""}</td>'
            f'<td class="code {cls}"><span class="marker">' +
            ("−" if cls == "del" else "+" if cls == "add" else " ") +
            '</span>' + code_cell(line) + '</td></tr>')


def render_file(record, index):
    old = record["_old"] or b""
    new = record["_new"] or b""
    a, b = text_content(old), text_content(new)
    label = record["new_path"] or record["old_path"]
    if record["old_path"] and record["new_path"] and record["old_path"] != record["new_path"]:
        label = record["old_path"] + " → " + record["new_path"]
    record["display_path"] = label
    record["html_id"] = f"file-{index}"
    meta = []
    for key in ("base", "candidate"):
        entry = record[key]
        meta.append(key + ": " + (f'{entry["kind"]}, mode {entry["mode"]}, {entry["bytes"]} bytes, SHA256 {entry["sha256"]}' if entry else "absent"))
    hunks = []
    added = deleted = 0
    if a is None or b is None:
        record["representation"] = "binary_or_non_utf8"
        record["added_lines"] = record["deleted_lines"] = None
        message = "二进制或非 UTF-8 内容；未做文本转换。上方保留身份与字节数，原始对照见 diff.patch。"
    elif old == new:
        record["representation"] = "metadata_only"
        record["added_lines"] = record["deleted_lines"] = 0
        message = "内容相同；本项变更是重命名、文件类型或权限，请核对上方元数据。"
    else:
        record["representation"] = "utf8_text"
        aa, bb = text_lines(a), text_lines(b)
        matcher = difflib.SequenceMatcher(None, aa, bb, autojunk=False)
        for tag, i1, i2, j1, j2 in matcher.get_opcodes():
            if tag != "equal":
                deleted += i2 - i1
                added += j2 - j1
        for number, group in enumerate(matcher.get_grouped_opcodes(3), 1):
            side_rows, unified_rows = [], []
            for tag, i1, i2, j1, j2 in group:
                if tag == "equal":
                    for i, j in zip(range(i1, i2), range(j1, j2)):
                        side_rows.append(row_side(i + 1, aa[i], j + 1, bb[j]))
                        unified_rows.append(row_unified(i + 1, j + 1, aa[i]))
                else:
                    for offset in range(max(i2 - i1, j2 - j1)):
                        i, j = i1 + offset, j1 + offset
                        side_rows.append(row_side(i + 1 if i < i2 else None, aa[i] if i < i2 else None,
                                                  j + 1 if j < j2 else None, bb[j] if j < j2 else None,
                                                  "del" if i < i2 else "", "add" if j < j2 else ""))
                    unified_rows += [row_unified(i + 1, None, aa[i], "del") for i in range(i1, i2)]
                    unified_rows += [row_unified(None, j + 1, bb[j], "add") for j in range(j1, j2)]
            old_count = group[-1][2] - group[0][1]
            new_count = group[-1][4] - group[0][3]
            old_start = group[0][1] + (1 if old_count else 0)
            new_start = group[0][3] + (1 if new_count else 0)
            label_hunk = f'@@ -{old_start},{old_count} +{new_start},{new_count} @@'
            initial = record.get("initial_candidate_line")
            initial_attr = ' data-initial="true"' if initial and new_start <= initial < new_start + new_count else ''
            if initial_attr:
                record["initial_hunk_id"] = f"change-{index}-{number}"
            hunks.append(f'<div class="change" id="change-{index}-{number}" tabindex="-1"{initial_attr}><p class="hunk">{label_hunk}</p>'
                         '<div class="scroll side"><table><caption>基线 / 候选</caption><tbody>' +
                         "".join(side_rows) + '</tbody></table></div><div class="scroll unified"><table>'
                         '<caption>统一视图：基线行号 / 候选行号</caption><tbody>' +
                         "".join(unified_rows) + '</tbody></table></div></div>')
        record["added_lines"], record["deleted_lines"] = added, deleted
        message = f"新增 {added} 行 / 删除 {deleted} 行；UTF-8 文本，每处变更附 3 行上下文。"
    if record.get("initial_candidate_line") and not record.get("initial_hunk_id"):
        raise DiffError("--initial-line must fall within a rendered candidate hunk of --initial-path")
    short_label = PurePosixPath(record["new_path"] or record["old_path"]).name
    return (f'<details class="file" id="file-{index}" open><summary title="{esc(label)}">'
            f'<span class="badge">{esc(record["status"])}</span> {esc(short_label)}</summary>'
            '<details class="file-meta"><summary>文件来源 · ' + esc(message) + '</summary>'
            '<div class="meta">' + esc(label) + '<br>' + '<br>'.join(esc(m) for m in meta) +
            '</div></details>' + "".join(hunks) + '</details>')


CSS = """
:root{font-family:system-ui,sans-serif;color:#17243a;background:#fff;color-scheme:light;--code-size:15px}
*{box-sizing:border-box}html,body{margin:0;width:100%;max-width:100%}body{padding:0}.viewer{height:1000px;min-height:480px;max-width:100%;display:flex;flex-direction:column;border:1px solid #c9d2df;border-radius:7px;overflow:hidden;resize:vertical}header{flex:none;position:relative;background:#fff;border-bottom:1px solid #ced5e0;padding:7px 9px}.topbar,.toolbar{display:flex;align-items:center;gap:6px;flex-wrap:wrap}.topbar{margin-bottom:6px}h1{font-size:16px;margin:0}.short-id{font:12px ui-monospace,monospace;color:#56647b}.spacer{flex:1}.toolbar select,.toolbar input{min-width:0}.file-picker{flex:1.6;min-width:120px!important;max-width:420px}.search{flex:1;min-width:95px!important;max-width:260px}button,input,select{font:13px system-ui;line-height:1.25;padding:5px 6px;border:1px solid #aebacb;border-radius:4px;background:#fff;color:#17243a}button{cursor:pointer}button:hover{background:#edf3fc}button:focus-visible,a:focus-visible,summary:focus-visible,input:focus-visible,select:focus-visible{outline:3px solid #286ed4;outline-offset:1px}a{color:#1456a6}.small-control{padding:3px 6px;font-size:12px}.position{font-size:12px;min-width:42px;text-align:center}.font-size{font-size:12px;min-width:20px;text-align:center}.provenance>summary{list-style:none;cursor:pointer;font-size:12px;padding:4px 6px;border:1px solid #aebacb;border-radius:4px}.provenance>summary::-webkit-details-marker{display:none}.provenance[open]>summary{background:#e6effa}.provenance-panel{position:absolute;top:39px;left:8px;right:8px;z-index:10;max-height:360px;overflow:auto;background:#fff;border:1px solid #8b98aa;box-shadow:0 5px 18px #17243a33;padding:12px;font-size:12px;overflow-wrap:anywhere}.provenance-panel p{margin:4px 0 9px}.provenance-panel button{margin-right:6px}.identity{font:12px/1.5 ui-monospace,monospace;overflow-wrap:anywhere}.status{margin:0;font-size:12px;color:#415777;padding-top:5px}.status:empty{display:none}main{flex:1;min-height:0;min-width:0;width:100%;overflow:auto;overscroll-behavior:contain;padding:5px}.file{background:#fff;margin:0 0 8px;border:1px solid #d3dce8;border-radius:4px;overflow:hidden;scroll-margin-top:5px}.file>summary{cursor:pointer;font-size:13px;font-weight:600;padding:6px 8px;background:#eaf0f8;white-space:nowrap;text-overflow:ellipsis;overflow:hidden}.badge{font:11px ui-monospace,monospace;padding:1px 3px;background:#dbe5f2;margin-right:4px}.file-meta{font-size:11px;color:#56647b}.file-meta>summary{cursor:pointer;padding:4px 8px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.meta{font:11px/1.5 ui-monospace,monospace;padding:8px;overflow-wrap:anywhere}.hunk{background:#e6effa;color:#234269;font:12px ui-monospace,monospace;padding:4px 8px;margin:0}.scroll{overflow:auto}table{border-collapse:collapse;width:100%;font:var(--code-size)/1.55 ui-monospace,SFMono-Regular,Consolas,monospace;table-layout:fixed}caption{text-align:left;font:11px system-ui;padding:3px 8px;background:#f1f4f8}.ln{width:43px;background:#f0f3f7;color:#627089;text-align:right;padding:0 5px;vertical-align:top;user-select:none;font-size:12px}.code{white-space:pre-wrap;overflow-wrap:anywhere;vertical-align:top;padding:0 5px;tab-size:4}.del{background:#ffe9e8;color:#721d23}.add{background:#dff3e4;color:#15582c}.marker{display:inline-block;width:1em;font-weight:700}.eol{font-size:10px;color:#664b20;background:#fff2cc}.change{scroll-margin-top:5px}.unified{display:none}body.unified-mode .side{display:none}body.unified-mode .unified{display:block}[hidden]{display:none!important}.empty{padding:24px;background:#fff;font-size:14px}.parts{font-size:12px;margin:8px 0}.parts a{margin-right:14px}body.focus-mode .short-id,body.focus-mode .provenance,body.focus-mode .file-meta,body.focus-mode h1{display:none}body.focus-mode .topbar{margin-bottom:3px}body.index-mode main{padding:16px}body.index-mode h1{font-size:20px}.viewer:fullscreen{height:100vh;resize:none;border-radius:0}@media(max-width:560px){.toolbar{gap:4px}.file-picker{flex-basis:55%;max-width:none}.search{max-width:none}.short-id{display:none}.topbar{gap:4px}.ln{width:35px}.file>summary{font-size:12px}header{padding:6px}.provenance-panel{top:65px}}@media print{.viewer{height:auto;overflow:visible;resize:none;border:0}header{position:static}.toolbar,.topbar button,.provenance{display:none}main,.scroll{overflow:visible}.file{break-inside:avoid}}
"""
FULL_PAGE_CSS = """
html:has(body.full-page),body.full-page{height:100%;overflow:hidden}body.full-page .viewer{height:100vh;height:100dvh;min-height:0;border:0;border-radius:0;resize:none}body.full-page #fullscreen{display:none}body.full-page .reading-mode{font-size:11px;color:#516580;background:#edf3fa;border-radius:4px;padding:3px 6px}body.full-page .file-picker{max-width:650px}body.full-page .search{max-width:360px}body.full-page main{padding:5px 7px}body.full-page.index-mode{overflow:auto;height:auto;min-height:100vh}@media print{html:has(body.full-page),body.full-page{height:auto;overflow:visible}body.full-page .viewer{height:auto}}
"""
JS = """
(()=>{'use strict';const $=s=>document.querySelector(s);const files=[...document.querySelectorAll('details.file')];const view=$('#view');const main=$('main');const picker=$('#file-picker');let current=-1;let font=15;let manualView=false;
function setView(value){view.value=value;document.body.classList.toggle('unified-mode',value==='unified');}function adaptiveView(){if(!manualView)setView(window.innerWidth<1100?'unified':'side');}adaptiveView();window.addEventListener('resize',adaptiveView);view.addEventListener('change',e=>{manualView=true;setView(e.target.value);});
function scrollToFile(f){f.hidden=false;f.open=true;picker.value=f.id;main.scrollTop=f.offsetTop-main.offsetTop;f.querySelector('summary').focus({preventScroll:true});}picker.addEventListener('change',()=>{const f=document.getElementById(picker.value);if(f)scrollToFile(f);});
function changes(){return [...document.querySelectorAll('.change')].filter(c=>!c.closest('details.file').hidden);}function updatePosition(){const count=changes().length;$('#position').textContent=count?(current>=0?current+1:'–')+' / '+count:'0 / 0';}updatePosition();
$('#search').addEventListener('input',e=>{current=-1;const q=e.target.value.toLowerCase();for(const f of files){const hide=!f.textContent.toLowerCase().includes(q);f.hidden=hide;const option=picker.querySelector('option[value="'+f.id+'"]');if(option)option.disabled=hide;}const visible=files.find(f=>!f.hidden);$('#no-match').hidden=Boolean(visible)||!files.length;if(visible){visible.open=true;picker.value=visible.id;}main.scrollTop=0;updatePosition();});
$('#expand').onclick=()=>{files.forEach(f=>f.open=true);$('#provenance').open=false;};$('#collapse').onclick=()=>{files.forEach(f=>f.open=false);$('#provenance').open=false;};
function move(direction){const found=changes();if(!found.length)return;current=current<0?(direction>0?0:found.length-1):(current+direction+found.length)%found.length;const c=found[current];const f=c.closest('details.file');f.open=true;picker.value=f.id;main.scrollTop=c.offsetTop-main.offsetTop;c.focus({preventScroll:true});updatePosition();}$('#prev').onclick=()=>move(-1);$('#next').onclick=()=>move(1);
function adjustFont(delta){font=Math.max(12,Math.min(22,font+delta));document.documentElement.style.setProperty('--code-size',font+'px');$('#font-size').textContent=font;$('#font-size').setAttribute('aria-label','代码字号 '+font+' 像素');}$('#font-down').onclick=()=>adjustFont(-1);$('#font-up').onclick=()=>adjustFont(1);
function focus(on){document.body.classList.toggle('focus-mode',on);$('#focus').textContent=on?'退出专注':'专注';$('#focus').setAttribute('aria-pressed',String(on));}$('#focus').onclick=()=>focus(!document.body.classList.contains('focus-mode'));$('#fullscreen').onclick=async()=>{if(document.fullscreenElement){try{await document.exitFullscreen();}catch(e){$('#status').textContent='退出全屏受宿主限制，请使用浏览器退出控件。';}return;}focus(true);if(!document.fullscreenEnabled||!$('#viewer').requestFullscreen){$('#status').textContent='当前嵌入环境未开放浏览器全屏；已启用块内专注模式。';return;}try{await $('#viewer').requestFullscreen();$('#status').textContent='';}catch(e){$('#status').textContent='浏览器未允许全屏；已启用块内专注模式。';}};document.addEventListener('fullscreenchange',()=>{$('#fullscreen').textContent=document.fullscreenElement?'退出全屏':'全屏';});
const hash=document.getElementById(location.hash.slice(1));const initial=document.querySelector('.change[data-initial]');let initialPending=true;document.body.dataset.initialPosition='pending';
function initialPosition(){if(!initialPending)return;if(hash&&hash.matches('details.file'))scrollToFile(hash);else if(initial){const f=initial.closest('details.file');f.open=true;picker.value=f.id;main.scrollTop+=initial.getBoundingClientRect().top-main.getBoundingClientRect().top;current=changes().indexOf(initial);updatePosition();}else if(files.length)picker.value=files[0].id;initialPending=false;document.body.dataset.initialPosition='settled';}
function scheduleInitialPosition(){requestAnimationFrame(()=>requestAnimationFrame(initialPosition));}
// srcDoc can execute before its host iframe receives the final viewport size.
// Settle after load and two animation frames so the first selected hunk stays in view.
if(document.readyState==='complete')scheduleInitialPosition();else window.addEventListener('load',scheduleInitialPosition,{once:true});
for(const event of ['pointerdown','keydown','wheel','touchstart'])window.addEventListener(event,()=>{initialPending=false;document.body.dataset.initialPosition='user-controlled';},{once:true,passive:true});
})();
"""


def identity_text(identity):
    if identity["kind"] == "git_commits":
        return "Git 提交：" + identity["base"]["commit"] + " → " + identity["candidate"]["commit"]
    return ("历史快照（非 Git 提交）：" + identity["base"]["label"] + " [" +
            identity["base"]["scoped_tree_sha256"] + "] → " + identity["candidate"]["label"] + " [" +
            identity["candidate"]["scoped_tree_sha256"] + "]")


def document(identity, scope, records, blocks, page, pages, total, presentation="embedded"):
    full_page = presentation == "full-page"
    css = CSS + (FULL_PAGE_CSS if full_page else "")
    mode = "viewport" if full_page else "auto"
    description = ("完整页面代码对照：阅读区随浏览器窗口伸展，15px 代码；支持搜索、文件切换及并排视图。" if full_page else
                   "版本代码差异：1000px 阅读区、15px 代码；文件选择、字体调整、专注模式、自动统一或并排视图。")
    reading_note = ("完整页面随当前浏览器窗口伸展；工具栏固定在顶部，代码区占据剩余高度并独立滚动。" if full_page else
                    "阅读区初始高 1000px，代码内部滚动；右下角可调整本地容器高度。嵌入宿主控制外框高度，内容变化不会自动扩展飞书外框。全屏需要浏览器及宿主允许。")
    options = ''.join(f'<option value="{r["html_id"]}">{esc(PurePosixPath(r["new_path"] or r["old_path"]).name)} · {esc(PurePosixPath(r["new_path"] or r["old_path"]).parent)}</option>' for r in records)
    page_links = ''
    if pages > 1:
        page_links = '<div class="parts"><a href="index.html">文件索引</a>' + ''.join(
            f'<a href="part-{p:03}.html">第 {p} 页</a>' for p in range(1, pages + 1)) + '</div>'
    scope_text = ", ".join(scope) if scope else "全部路径"
    if identity["kind"] == "labelled_snapshots":
        scope_text += "；排除 .git；符号链接仅记录链接文本"
    short_identity = (identity["base"]["commit"][:8] + " → " + identity["candidate"]["commit"][:8]
                      if identity["kind"] == "git_commits" else "历史快照对照")
    return ('<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
            f'<meta name="use-iframe" content="true"><meta name="html-box-height-mode" content="{mode}">'
            f'<meta name="description" content="{description}">'
            '<meta name="referrer" content="no-referrer"><title>版本代码对照</title><style>' + css +
            '</style></head><body' + (' class="full-page"' if full_page else '') + '><div id="viewer" class="viewer"><header><div class="topbar"><h1>版本代码对照</h1><span class="short-id">' + esc(short_identity) +
            '</span>' + ('<span class="reading-mode">完整页面</span>' if full_page else '') + '<span class="spacer"></span><details id="provenance" class="provenance"><summary>来源与范围</summary><div class="provenance-panel"><p class="identity">' + esc(identity_text(identity)) +
            f'</p><p>比较范围：{esc(scope_text)}。变更文件 {total} 个；本页 {len(records)} 个。'
            '行数按展示的文本差异计数；二进制不计行数。</p>' + page_links +
            '<p>离线查看 · 源码按文本转义 · 无外部脚本、字体或网络请求。原始对照：diff.patch；来源记录：manifest.json。</p>'
            '<p>' + reading_note + '</p>'
            '<button id="expand" type="button">全部展开</button><button id="collapse" type="button">全部折叠</button></div></details>'
            '<button id="font-down" class="small-control" type="button" aria-label="缩小代码字号">A−</button><span id="font-size" class="font-size" aria-label="代码字号 15 像素">15</span>'
            '<button id="font-up" class="small-control" type="button" aria-label="放大代码字号">A+</button>'
            '<button id="focus" class="small-control" type="button" aria-pressed="false">专注</button><button id="fullscreen" class="small-control" type="button">全屏</button></div>'
            '<div class="toolbar"><select id="file-picker" class="file-picker" aria-label="变更文件">' + options + '</select>'
            '<select id="view" aria-label="代码视图"><option value="unified">统一差异</option><option value="side">左右对照</option></select>'
            '<input id="search" class="search" type="search" placeholder="查找文件或代码" aria-label="查找文件或代码">'
            '<button id="prev" type="button" aria-label="上一处变更" title="上一处变更">↑</button><button id="next" type="button" aria-label="下一处变更" title="下一处变更">↓</button>'
            '<span id="position" class="position" aria-live="polite"></span></div><p id="status" class="status" role="status"></p></header><main>' +
            (''.join(blocks) if blocks else '<p class="empty">所选范围内没有源码变化。两个候选共用实现提交时，请另行比较已冻结的构建与运行配置。</p>') +
            '<p id="no-match" class="empty" hidden>未找到匹配的文件或代码；清空搜索可恢复全部文件。</p>'
            '</main></div><script>' + JS + '</script></body></html>')


def partition(identity, scope, records, blocks, limit, presentation="embedded"):
    whole = document(identity, scope, records, blocks, 1, 1, len(records), presentation).encode("utf-8")
    if len(whole) <= limit:
        return {"index.html": whole}
    # Reserve navigation for the maximum possible number of parts to enforce the cap.
    groups, current = [], []
    for i, block in enumerate(blocks):
        proposed = current + [i]
        body = document(identity, scope, [records[k] for k in proposed], [blocks[k] for k in proposed],
                        1, len(records), len(records), presentation).encode("utf-8")
        if len(body) <= limit:
            current = proposed
            continue
        if not current:
            raise DiffError(f'HTML for one file exceeds {limit} bytes: {records[i]["display_path"]}. '
                            'Select smaller literal --path scopes or review diff.patch locally; no content was truncated.')
        groups.append(current)
        current = [i]
        single = document(identity, scope, [records[i]], [block], 1, len(records), len(records), presentation).encode("utf-8")
        if len(single) > limit:
            raise DiffError(f'HTML for one file exceeds {limit} bytes: {records[i]["display_path"]}. '
                            'Review diff.patch locally or reduce source changes; no content was truncated.')
    if current:
        groups.append(current)
    output = {}
    index_rows = []
    for page, indices in enumerate(groups, 1):
        name = f"part-{page:03}.html"
        output[name] = document(identity, scope, [records[i] for i in indices], [blocks[i] for i in indices],
                                page, len(groups), len(records), presentation).encode("utf-8")
        index_rows += [f'<li><a href="{name}#{records[i]["html_id"]}">{esc(records[i]["status"])} {esc(records[i]["display_path"])}</a></li>' for i in indices]
    index = ('<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
             '<meta name="use-iframe" content="true"><meta name="html-box-height-mode" content="viewport">'
             '<meta name="description" content="完整代码差异文件索引及分块入口。">'
             '<title>代码差异文件索引</title><style>' + CSS + (FULL_PAGE_CSS if presentation == "full-page" else "") + '</style></head><body class="index-mode' + (' full-page' if presentation == "full-page" else '') + '"><main><h1>代码差异文件索引</h1><p class="identity">' +
             esc(identity_text(identity)) + '</p><p>所选范围完整对照：' + str(len(records)) +
             ' 个变更文件，分为 ' + str(len(groups)) + ' 个离线 HTML 页面。每页均包含独立样式和交互。'
             '本地查看时将索引和分块放在同一目录；飞书发布时保留所有分块。</p><p>比较范围：' +
             esc(", ".join(scope) if scope else "全部路径") + '</p><ul>' + ''.join(index_rows) + '</ul></main></body></html>')
    output["index.html"] = index.encode("utf-8")
    if any(len(value) > limit for value in output.values()):
        raise DiffError("HTML index/part exceeds limit; reduce --path scope. Raw comparison is preserved.")
    return output


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--repo", help="Git repository (reads commits only, never the working tree)")
    p.add_argument("--base", help="Baseline Git revision, resolved to full commit ID")
    p.add_argument("--candidate", help="Candidate Git revision, resolved to full commit ID")
    p.add_argument("--base-dir", help="Legacy baseline snapshot directory")
    p.add_argument("--candidate-dir", help="Legacy candidate snapshot directory")
    p.add_argument("--base-label", help="Explicit human label for baseline snapshot")
    p.add_argument("--candidate-label", help="Explicit human label for candidate snapshot")
    p.add_argument("--path", action="append", default=[], help="Literal relative file/directory; repeat to select several")
    p.add_argument("--initial-path", help="Show this changed literal file first (presentation only; patch is unchanged)")
    p.add_argument("--initial-line", type=int, help="Initially focus the hunk containing this candidate line; requires --initial-path")
    p.add_argument("--presentation", choices=("embedded", "full-page"), default="embedded",
                   help="Embedded keeps a resizable 1000px reader; full-page fills the browser viewport")
    p.add_argument("--out", required=True, help="New output directory; existing paths are never overwritten")
    p.add_argument("--max-html-bytes", type=int, default=DEFAULT_LIMIT, help="Per-HTML byte cap, 10000..500000 (default 500000)")
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    output = Path(args.out)
    manifest = None
    try:
        git_mode = any((args.repo, args.base, args.candidate))
        snapshot_mode = any((args.base_dir, args.candidate_dir, args.base_label, args.candidate_label))
        if git_mode == snapshot_mode:
            raise DiffError("Choose exactly one mode: Git commits or labelled snapshot directories")
        required = (args.repo, args.base, args.candidate) if git_mode else (
            args.base_dir, args.candidate_dir, args.base_label, args.candidate_label)
        if not all(required):
            raise DiffError("All inputs for the selected mode are required (see --help)")
        if not 10_000 <= args.max_html_bytes <= DEFAULT_LIMIT:
            raise DiffError("--max-html-bytes must be between 10000 and 500000")
        if output.exists() or output.is_symlink():
            raise DiffError("Output path already exists; choose a new analysis directory")
        scope = filters(args.path)
        identity, records, patch, commands = git_comparison(args, scope) if git_mode else snapshot_comparison(args, scope)
        initial_path = filters([args.initial_path])[0] if args.initial_path else None
        if args.initial_line is not None and (not initial_path or args.initial_line < 1):
            raise DiffError("--initial-line must be positive and requires --initial-path")
        if initial_path:
            if not any(initial_path in (r["new_path"], r["old_path"]) for r in records):
                raise DiffError("--initial-path must name a changed file inside the selected scope")
            records.sort(key=lambda r: initial_path not in (r["new_path"], r["old_path"]))
            if args.initial_line is not None:
                records[0]["initial_candidate_line"] = args.initial_line
        output.mkdir(parents=True, exist_ok=False)
        (output / "diff.patch").write_bytes(patch)
        manifest = {"schema_version": VERSION, "status": "preparing_html", "identity": identity,
                    "scope": {"literal_paths": scope, "all_paths": not scope,
                              "snapshot_exclusions": [".git"] if snapshot_mode else [],
                              "working_tree_included": False},
                    "generator_sha256": digest(Path(__file__).read_bytes()),
                    "invocation_argv": sys.argv if argv is None else [str(Path(__file__)), *argv],
                    "git_commands_argv": commands, "max_html_bytes": args.max_html_bytes,
                    "changed_files": len(records), "files": [],
                    "presentation": {"mode": args.presentation, "initial_path": initial_path, "initial_candidate_line": args.initial_line, "code_font_px": 15,
                                     "initial_height_px": None if args.presentation == "full-page" else 1000,
                                     "height_mode": "viewport" if args.presentation == "full-page" else "auto",
                                     "requires_hosted_top_level_link": args.presentation == "full-page",
                                     "unified_below_width_px": 1100,
                                     "provenance_initially_collapsed": True},
                    "artifacts": {"diff.patch": {"sha256": digest(patch), "bytes": len(patch)}},
                    "limitations": ["HTML shows changed hunks with three context lines; raw comparison is retained.",
                                    "Source diffs do not prove performance, build identity, or client rendering.",
                                    "Line counts describe the displayed difflib comparison, which can differ from Git's hunk alignment."]}
        if snapshot_mode:
            manifest["limitations"] += ["Snapshot hashes identify the selected path scope, not a historical Git commit.",
                                        "Snapshot inputs must stay frozen during export; individual reads are checked for concurrent changes.",
                                        "Snapshot binary content is represented by byte counts and SHA256; payloads are not embedded.",
                                        "Snapshot rename detection covers unambiguous identical content/mode only."]
        (output / "manifest.json").write_bytes(json_bytes(manifest))
        blocks = [render_file(record, i) for i, record in enumerate(records)]
        manifest["files"] = [{k: v for k, v in record.items() if not k.startswith("_")} for record in records]
        manifest["line_counts"] = {"added": sum(r["added_lines"] or 0 for r in records),
                                   "deleted": sum(r["deleted_lines"] or 0 for r in records),
                                   "unavailable_files": sum(r["added_lines"] is None for r in records)}
        pages = partition(identity, scope, records, blocks, args.max_html_bytes, args.presentation)
        for name, content in pages.items():
            (output / name).write_bytes(content)
            manifest["artifacts"][name] = {"sha256": digest(content), "bytes": len(content)}
        manifest["status"] = "complete"
        manifest["html_pages"] = list(pages)
        (output / "manifest.json").write_bytes(json_bytes(manifest))
        print(json.dumps({"status": "complete", "out": str(output.resolve()), "changed_files": len(records),
                          "html_pages": list(pages), "identity_kind": identity["kind"]}, ensure_ascii=False))
        return 0
    except (DiffError, OSError, ValueError) as exc:
        if manifest is not None:
            manifest["status"] = "html_failed"
            manifest["error"] = str(exc)
            (output / "manifest.json").write_bytes(json_bytes(manifest))
        print(json.dumps({"status": "failed", "error": str(exc),
                          "raw_evidence_preserved": manifest is not None}, ensure_ascii=False), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
