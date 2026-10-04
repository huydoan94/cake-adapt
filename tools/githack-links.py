#!/usr/bin/env python3
"""Point every Markdown link to a repository HTML file at one branch on githack.

GitHub shows a linked .html file as source, so rendered pages are linked
through raw.githack.com, whose URLs name a branch. This rewrites, in every
tracked Markdown file, both relative links to .html files and existing
githack links to this repository, so they all name the given branch.

The GitHub Action passes --repo and --branch for the pushed branch; locally,
--branch defaults to the current branch. --check lists changes without
writing them and exits 1 when there are any.
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys

GITHACK = "https://raw.githack.com/"
LINK = re.compile(r"\]\(([^)\s]+)\)")
HTML_SUFFIX = ".html"


def run_git(root, *arguments):
    return subprocess.run(
        ["git", *arguments], cwd=root, check=True, capture_output=True, text=True
    ).stdout


def split_suffix(target):
    """Splits "page.html#part" into ("page.html", "#part")."""
    for marker in ("#", "?"):
        index = target.find(marker)
        if index >= 0:
            return target[:index], target[index:]
    return target, ""


def html_path(root, document, target, repo):
    """The repository path of the HTML file a link points to, or None."""
    path, _ = split_suffix(target)
    prefix = f"{GITHACK}{repo}/"
    if path.startswith(prefix):
        rest = path[len(prefix):]
        # A branch name may contain "/": the branch ends where a real file begins.
        for index, character in enumerate(rest):
            if character == "/":
                candidate = rest[index + 1:]
                if candidate.endswith(HTML_SUFFIX) and (root / candidate).is_file():
                    return candidate
        return None
    if "://" in path or path.startswith(("#", "/", "mailto:")) or not path.endswith(HTML_SUFFIX):
        return None
    resolved = (root / document.parent / path).resolve()
    try:
        relative = resolved.relative_to(root.resolve())
    except ValueError:
        return None
    return relative.as_posix() if resolved.is_file() else None


def rewrite(root, document, repo, branch):
    text = (root / document).read_text(encoding="utf-8")

    def replace(match):
        target = match.group(1)
        path = html_path(root, document, target, repo)
        if path is None:
            return match.group(0)
        _, suffix = split_suffix(target)
        return f"]({GITHACK}{repo}/{branch}/{path}{suffix})"

    return text, LINK.sub(replace, text)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY"),
                        help="owner/name on GitHub (default: $GITHUB_REPOSITORY)")
    parser.add_argument("--branch", default=os.environ.get("GITHUB_REF_NAME"),
                        help="branch to link (default: $GITHUB_REF_NAME, else the current branch)")
    parser.add_argument("--check", action="store_true",
                        help="list files that would change, without writing them")
    arguments = parser.parse_args()

    root = pathlib.Path(run_git(pathlib.Path.cwd(), "rev-parse", "--show-toplevel").strip())
    branch = arguments.branch or run_git(root, "branch", "--show-current").strip()
    if not arguments.repo or not branch:
        parser.error("--repo and a branch are required")

    changed = []
    for name in run_git(root, "ls-files", "*.md").splitlines():
        document = pathlib.Path(name)
        before, after = rewrite(root, document, arguments.repo, branch)
        if before != after:
            changed.append(name)
            if not arguments.check:
                (root / document).write_text(after, encoding="utf-8")
    for name in changed:
        print(name)
    return 1 if arguments.check and changed else 0


if __name__ == "__main__":
    sys.exit(main())
