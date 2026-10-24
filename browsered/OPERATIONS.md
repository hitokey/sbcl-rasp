# Browsered: how the system works

This document is for anyone — human or AI — who needs to operate, debug or
extend Browsered without reading all of the C and JavaScript first. For the
development/build rules, see `AGENTS.md`; for the end-user tool policy, see
`USER-TOOLS.md`; for the short pitch and the autoclave walkthrough, see
`README.md`.

## System map

| File(s) | Role |
| --- | --- |
| `browsered.c` / `windows/browsered-win.c` | Native helper: a loopback-only HTTP server on `127.0.0.1:8766` that serves the editor's files and routes its POST actions. |
| `assistant.c` | Spawns Codex/Claude Code CLIs for `/assistant` and `/claude`, builds the system prompt (with the autoclave instructions), and the NAIA/Chrome bridge (`/naia`). macOS only. |
| `sources.c` | Finds and fetches sources a request names (links, local files), extracts PDF text, remembers the last source, and runs the autoclave's mechanical quote check (`/autoclave`). macOS only. |
| `pdf.c` | Renders a Markdown artifact to PDF via CoreText/CoreGraphics (`/pdf`). macOS only. |
| `runtime-policy.h` | The `BROWSERED_USER_TOOL_POLICY` string: the tool-use policy prepended to native-helper assistant requests. |
| `Browsered.html`, `keys.js`, `scheme.js`, `editor.scm`, `init.scm`, `femtoscheme/` | The in-browser editor: Emacs-style keys and commands (femtoscheme, a Scheme compiled to WASM) driving a `<textarea>`. `keys.js` is the same logic as `Browsered.html`'s inline script, split out so `browser-tests.html` can load it as a normal script. |
| `assistant-schema.json` | The JSON schema given to `codex exec --output-schema`. |
| `formatting.xml` | Per-element font/size/alignment/spacing rules `pdf.c` applies when rendering a PDF. |
| `windows/build.sh`, `windows/browsered.iss`, `windows/browsered.rc`, `windows/browsered.ico` | Windows packaging: mingw-w64 cross-compile script and Inno Setup installer. |

## Request flow, end to end

1. The user opens the question dialog (Claude or ChatGPT button) in `keys.js`
   and types a request.
2. On submit, `keys.js` decides where the request goes:
   - If a native helper answers at `127.0.0.1:8766` (`nativeHelper` is true),
     it `fetch()`es `/assistant` (Codex) or `/claude` (Claude Code) on the
     helper, sending the request and the current buffer text.
   - Otherwise (hosted website, no native helper) it calls the Anthropic or
     OpenAI API directly from the browser with a user-supplied API key
     (`askApi`, using `webInstructions` as the system prompt).
   - If the request is recognized as a NAIA question (`naiaSubmission`), it
     instead `fetch()`es `/naia` and never touches `/assistant`/`/claude`.
3. For the native-helper path, `assistant.c` creates a private, per-request
   temporary workspace, fetches any sources the request names
   (`browsered_fetch_sources`, macOS only), writes a prompt file containing
   `instructions` (the tool policy + task rules, plus the autoclave
   instructions) followed by the request, and spawns `codex exec` or
   `claude -p` with that file as input. The CLI's JSON answer
   (`{reply, buffer, artifacts}`) is read back and the workspace is deleted.
4. The browser applies the answer: `result.buffer`, if non-null, replaces the
   editor's text via `document.execCommand("insertText"/"delete")` (a normal,
   undoable edit — Ctrl+/). Each `result.artifacts[i]` becomes a downloadable
   file, with its extension/MIME type forced to match its declared `format`
   (so a model-requested `.docx` name can't turn Markdown into a fake Word
   file), plus a "Ver texto" preview. A `format: "pdf"` artifact is rendered
   server-side via `/pdf` when available, or offered as a printable HTML page
   plus a raw `.md` download otherwise.
5. The clipboard (`navigator.clipboard`, Ctrl+W/Ctrl+Y and the toolbar's Copy
   all/Paste) is a separate, generic browser↔OS bridge for the human to move
   text in and out of the editor. It plays no part in reaching Claude
   Code/Codex: that always goes through `/assistant`/`/claude` and a spawned
   subprocess, never the clipboard.

## The two assistant paths, and why they differ

- **Native helper path**: has the `runtime-policy.h` tool-use policy
  prepended to every request (and, on macOS, written into fresh
  `AGENTS.md`/`CLAUDE.md` files in the request workspace), runs under the
  user's own installed CLI and its login/subscription, and — on macOS — can
  fetch sources the request names and read files beyond the request text.
- **Hosted/API-key path** (`keys.js`'s `webInstructions`): browser-only, no
  native helper, bring-your-own API key, no `runtime-policy.h` text, no
  server-side source fetching.

## The native helper by operating system

| | macOS | Linux | Windows |
| --- | --- | --- | --- |
| Source | `browsered.c` + `assistant.c` + `sources.c` + `pdf.c` | `browsered.c` + `assistant.c` + `sources.c` (same files, Apple-only parts compiled out) | `windows/browsered-win.c` (separate implementation) |
| Concurrency | `fork()` per request | `fork()` per request | `CreateThread` per request |
| `/assistant`, `/claude` | yes | yes | yes |
| `/pdf` | renders a real PDF | `501 Not Implemented` | `501 Not Implemented` |
| `/naia`, `/autoclave` | yes | no | no |
| `/quit` (and `browsered --quit`) | yes | yes | yes |
| Source fetching (`browsered_fetch_sources`) | yes | yes | no |
| Portable package layout | `Browsered.app` bundle (`Contents/MacOS/`, `Contents/Resources/`) | flat `Browsered/` folder (executable and resources side by side) | flat folder (executable and resources side by side) |
| Build | `make` → `Browsered.app` | `make` → `Browsered-linux/bin/browsered` | `make windows` → `windows/build.sh` → `windows/stage` → `iscc windows/browsered.iss` |

## NAIA / autoclave workflow

Browsered can test another medical AI (NAIA) and mechanically check its
answers against a source, with no AI involved in the check itself:

1. Ask Claude/Codex for a test naming a source; it writes `Q1:`/`KEY1:`
   (expected answer plus a verbatim quotation)/`NAIA1:` (empty) lines into
   the buffer.
2. Open NAIA in Chrome and press **NAIA**: Browsered types each question into
   NAIA's page (via AppleScript/JavaScript for Automation) and fills in the
   `NAIAn:` lines — or, for a plain question-plus-document buffer, sends it
   as one message and offers the reply as a Markdown download (it is never
   written into the buffer).
3. Press **Autoclave**: every quotation in the `KEY`/`NAIA` lines is searched
   for in the source text, letter by letter; the report is appended to the
   buffer.
4. Ask Claude/Codex to score the answers (`SCOREn:` lines).

This is a proof of concept, and the server side of it (`/naia`, `/autoclave`)
exists only in the macOS helper. The **NAIA**/**Autoclave** toolbar buttons
are part of the shared UI, though, and currently appear whenever any native
helper is present — see inconsistency 4 below.

## Build and packaging

- **macOS**: `make` builds `Browsered.app` (CoreFoundation, CoreGraphics,
  CoreText, PDFKit, plus Homebrew's libxml2/cmark-gfm).
- **Linux**: `make` builds `Browsered-linux/bin/browsered` and
  `Browsered-linux/Resources/`, no extra libraries.
- **Windows**: `make windows` runs `windows/build.sh` (needs an
  `x86_64-w64-mingw32-gcc`/`windres` cross-compiler and a POSIX-ish shell),
  staging `windows/stage/browsered.exe` plus resources; `iscc
  windows/browsered.iss` builds the installer separately.
- **CI**: `.github/workflows/check.yml` runs `make` then `make check` on an
  `[ubuntu-latest, macos-latest]` matrix. There is no Windows job.

## Appendix: known inconsistencies

These were found while writing this document and are listed for future
reference; fixing them is out of scope here.

1. `windows/browsered-win.c` (around lines 324-325 and 343) still has
   comments describing the superseded "tool denial / read-only sandbox"
   behavior ("tools that read, write, run commands or browse are
   disallowed", "Codex in a read-only sandbox"), which `assistant.c:239-243`,
   `README.md:64` and `USER-TOOLS.md:19` say no longer applies.
2. `USER-TOOLS.md:13-14` says the `runtime-policy.h` text is embedded "at the
   START of every ChatGPT and Claude request" — but the hosted/API-key path
   (`keys.js`'s `webInstructions`) never includes it; only the native-helper
   path (`assistant.c:48`, `windows/browsered-win.c:61`) does.
3. `Browsered.html` references `manifest.webmanifest`, `icons/icon-192.png`
   and registers a `sw.js` service worker; none of the three files exist in
   the repository, and none are in the Makefile's `RESOURCES` or
   `windows/build.sh`'s copy list. The registration fails silently
   (`.catch(() => {})`).
4. `README.md:26` scopes the whole NAIA/autoclave workflow as "(proof of
   concept, macOS)", and indeed `/naia`/`/autoclave` exist only in the macOS
   helper. But the shared UI's NAIA/Autoclave toolbar buttons (`keys.js`)
   appear whenever any native helper is present, regardless of OS — a
   Windows (or now Linux) user sees clickable buttons that 404.
5. `USER-TOOLS.md`/`AGENTS.md` describe source-fetching (PDFs, user-named
   local files) as a general assistant capability, without noting that
   Browsered's own convenience fetcher (`browsered_fetch_sources` in
   `sources.c`) has no Windows port.
6. There are three independently maintained copies of the assistant's system
   prompt: `assistant.c:48-63` (macOS), `windows/browsered-win.c:61-75`
   (Windows), and `keys.js`'s `webInstructions` (hosted/API-key path) — no
   doc notes they must be kept in sync. Items 1 and 2 above are symptoms of
   exactly this drift.
7. `windows/build.sh`'s resource copy list omits `formatting.xml` and
   `USER-TOOLS.md`, which the macOS Makefile's `RESOURCES` variable includes.
8. `README.md` (before this change) said the Windows installer "is built by
   GitHub Actions" — `.github/workflows/check.yml` only runs `make`/`make
   check` on `[ubuntu-latest, macos-latest]`; there is no Windows job and no
   call to `windows/build.sh` or `iscc` anywhere in the repository's
   workflows.
