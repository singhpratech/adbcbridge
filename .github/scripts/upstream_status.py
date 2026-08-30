#!/usr/bin/env python3
"""Build upstream/status.json for the landing page's live report tracker.

Reads the "Reported" table in docs/UPSTREAM.md, fetches every linked GitHub issue and
keeps only what the *project side* did with it: comments by accounts with an
owner/member/collaborator/contributor association (never the reporter, never bots),
label changes, close/reopen events and pull requests that reference the issue.
The reporter's own comments are counted but not shown.  Output is one JSON file the
page fetches; nothing on the page talks to api.github.com.

Usage: GITHUB_TOKEN=... upstream_status.py [--out upstream/status.json] [--md docs/UPSTREAM.md]
Only the standard library is used so the script runs unchanged in a GitHub Action.
"""
import argparse
import datetime as dt
import json
import os
import re
import sys
import urllib.error
import urllib.request

API = "https://api.github.com"
PROJECT_SIDE = {"OWNER", "MEMBER", "COLLABORATOR", "CONTRIBUTOR"}
TEXT_LIMIT = 400          # characters of a comment kept for the card
PULSE_DAYS = 7            # "recent activity" window


def get(url, token):
    req = urllib.request.Request(url, headers={
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "adbcbridge-upstream-status",
        **({"Authorization": f"Bearer {token}"} if token else {}),
    })
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.load(r), r.headers.get("Link", "")
    except urllib.error.HTTPError as e:
        sys.stderr.write(f"{url}: HTTP {e.code}\n")
        return None, ""


def paged(url, token):
    out = []
    while url:
        data, link = get(url, token)
        if data is None:
            break
        out.extend(data)
        m = re.search(r'<([^>]+)>;\s*rel="next"', link)
        url = m.group(1) if m else None
    return out


def reported_rows(md_path):
    """Yield (date, project, issue_url, what) for every row of the Reported table."""
    in_table = False
    for line in open(md_path, encoding="utf-8"):
        if line.startswith("## Reported"):
            in_table = True
            continue
        if in_table and line.startswith("## "):
            break
        if not in_table or not line.startswith("| 20"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split(" | ")]
        if len(cells) < 4:
            continue
        for m in re.finditer(r"\((https://github\.com/[^/]+/[^/]+/(?:issues|pull)/\d+)(#[^)]*)?\)", cells[2]):
            yield cells[0], cells[1], m.group(1), (m.group(2) or ""), cells[3]


def clean(text):
    text = re.sub(r"```.*?```", "[code]", text or "", flags=re.S)
    text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    text = re.sub(r"\s+", " ", text).strip()
    return text[:TEXT_LIMIT] + ("…" if len(text) > TEXT_LIMIT else "")


def iso(s):
    return s if s else None


def build(md_path, token):
    items = []
    now = dt.datetime.now(dt.timezone.utc)
    for reported_on, project, url, anchor, what in reported_rows(md_path):
        owner, repo, kind, num = re.match(r"https://github\.com/([^/]+)/([^/]+)/(issues|pull)/(\d+)", url).groups()
        issue, _ = get(f"{API}/repos/{owner}/{repo}/issues/{num}", token)
        if issue is None:
            continue
        reporter = issue["user"]["login"]
        is_comment_report = bool(anchor)   # e.g. clickhouse-odbc#335 (comment): our report is a comment on an older issue
        since = None
        events = []
        comments = paged(f"{API}/repos/{owner}/{repo}/issues/{num}/comments?per_page=100", token)
        if is_comment_report:
            cid = re.search(r"issuecomment-(\d+)", anchor)
            ours = next((c for c in comments if cid and str(c["id"]) == cid.group(1)), None)
            reporter = ours["user"]["login"] if ours else reporter
            since = ours["created_at"] if ours else None
        own_comments = 0
        community = 0
        for c in comments:
            if since and c["created_at"] <= since:
                continue
            login = c["user"]["login"]
            if login == reporter:
                own_comments += 1
                continue
            if c["user"].get("type") == "Bot" or login.endswith("[bot]"):
                continue
            assoc = c.get("author_association", "NONE")
            if assoc in PROJECT_SIDE:
                events.append({"type": "comment", "at": c["created_at"], "actor": login, "association": assoc.lower(),
                               "text": clean(c["body"]), "url": c["html_url"]})
            else:
                community += 1
        timeline = paged(f"{API}/repos/{owner}/{repo}/issues/{num}/timeline?per_page=100", token)
        for ev in timeline:
            at = ev.get("created_at")
            if since and at and at <= since:
                continue
            actor = (ev.get("actor") or {}).get("login")
            if actor == reporter:
                continue
            t = ev.get("event")
            if t in ("labeled", "unlabeled"):
                events.append({"type": t, "at": at, "actor": actor, "label": ev["label"]["name"]})
            elif t in ("closed", "reopened"):
                events.append({"type": t, "at": at, "actor": actor, "reason": ev.get("state_reason"),
                               "commit": (ev.get("commit_url") or "").replace(API + "/repos/", "https://github.com/").replace("/commits/", "/commit/") or None})
            elif t == "cross-referenced" and ev.get("source", {}).get("issue", {}).get("pull_request") \
                    and ev["source"]["issue"].get("repository", {}).get("full_name", "").lower() == f"{owner}/{repo}".lower():
                src = ev["source"]["issue"]      # a PR in the project's own repo (forks' PRs are noise)
                events.append({"type": "pull_request", "at": at, "actor": (src.get("user") or {}).get("login"),
                               "title": src["title"], "url": src["html_url"], "state": "merged" if src.get("pull_request", {}).get("merged_at") else src["state"]})
            elif t == "referenced" and ev.get("commit_id"):
                events.append({"type": "commit", "at": at, "actor": actor,
                               "url": f"https://github.com/{owner}/{repo}/commit/{ev['commit_id']}"})
            elif t == "milestoned":
                events.append({"type": "milestoned", "at": at, "actor": actor, "milestone": ev.get("milestone", {}).get("title")})
        events.sort(key=lambda e: e.get("at") or "")
        last = max([e.get("at") for e in events if e.get("at")] + [issue.get("closed_at") or ""] + [""])
        state = "closed" if issue["state"] == "closed" else "open"
        if state == "closed":
            state = {"completed": "fixed", "not_planned": "closed (not planned)", "duplicate": "closed (duplicate)"}.get(issue.get("state_reason"), "closed")
        pulse = bool(last) and (now - dt.datetime.fromisoformat(last.replace("Z", "+00:00"))).days <= PULSE_DAYS
        items.append({
            "project": project, "repo": f"{owner}/{repo}", "number": int(num), "url": url + anchor,
            "title": issue["title"], "reported": reported_on, "what": what,
            "kind": "comment on existing issue" if is_comment_report else "issue",
            "state": state, "closed_at": iso(issue.get("closed_at")),
            "labels": [l["name"] for l in issue.get("labels", []) if not is_comment_report],
            "reactions": issue.get("reactions", {}).get("total_count", 0),
            "maintainer_events": events, "reporter_comments": own_comments, "community_comments": community,
            "last_activity": last or None, "pulse": pulse,
        })
    return {"as_of": now.strftime("%Y-%m-%dT%H:%M:%SZ"), "window_days": PULSE_DAYS, "count": len(items),
            "with_project_activity": sum(1 for i in items if any(not (e.get("actor") or "").endswith("[bot]") for e in i["maintainer_events"])),
            "fixed": sum(1 for i in items if i["state"] == "fixed"), "items": items}


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--md", default="docs/UPSTREAM.md")
    ap.add_argument("--out", default="upstream/status.json")
    a = ap.parse_args()
    data = build(a.md, os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN"))
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
    print(f"{a.out}: {data['count']} reports, {data['with_project_activity']} with project-side activity, {data['fixed']} fixed, as of {data['as_of']}")
