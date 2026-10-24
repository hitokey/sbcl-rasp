# Browsered in sbcl-rasp

Browsered is an editor with Emacs keys that runs in Chrome or Edge, scripted
in femtoscheme (a small Scheme compiled to WebAssembly). In sbcl-rasp it is
the editor for things that need a window: showing RASP's analyses to people
who are not linguists.

This folder holds the finished editor and the helper's source, taken from
[FemtoEmacs/browsered](https://github.com/FemtoEmacs/browsered) at commit
a751c12. Changing the editor itself (its C interpreter, the build of
`Browsered.html`) is done in that repository, which needs Emscripten.

- **macOS:** `make` (at the top of sbcl-rasp, or here) builds `Browsered.app`.
  Double-click it: it starts the helper and opens the editor at
  http://127.0.0.1:8766/.
- **Linux:** `make` builds `Browsered-linux/bin/browsered`; run it to start
  the helper and open the editor at http://127.0.0.1:8766/. PDF export and
  the NAIA/autoclave routes are not available on this helper (see
  OPERATIONS.md).
- **Windows:** `make windows` (needs a mingw-w64 cross-compiler) cross-builds
  the helper with `windows/build.sh`; `windows/browsered.iss` is the Inno
  Setup installer script, run separately with `iscc`.
- **Tailoring:** key bindings and commands are in `editor.scm` and
  `init.scm`, loaded as text; changing them needs no rebuild.

femtoscheme derives from femtolisp by Jeff Bezanson (BSD licence,
`femtoscheme/LICENSE`).

## The autoclave (proof of concept, macOS)

Browsered can test another medical AI, NAIA, and check its answers.

1. Ask Claude for a test, naming the source: *Create a test about the
   contents of https://...pdf*. Browsered downloads the source (or reads a
   file you name, such as `~/Downloads/paper.pdf`), extracts its text, and
   Claude writes the test into the buffer: questions `Q1:`, expected answers
   `KEY1:` with a quotation from the source, and empty `NAIA1:` lines.
2. Open NAIA (naia.med.br/app) in Chrome and press **NAIA**. Browsered types
   each question into NAIA's page and writes NAIA's answer after `NAIAn:`.
   Chrome must allow it once: View › Developer › Allow JavaScript from Apple
   Events; macOS asks once whether Browsered may control Chrome.
3. Press **Autoclave**. Every quotation in the KEY and NAIA lines is searched
   for in the source text, letter by letter, with no AI involved; the report
   goes at the end of the buffer, and quotations not found in the source are
   marked.
4. Ask Claude to score the answers; it adds a `SCOREn:` line after each.

Every change to the buffer can be undone with Ctrl+/.

## Notice

**English.** This is not a product and must be used only to test the software,
Claude Code, Codex, OpenEvidence and the algorithms. Anyone who wishes to
develop the product for sale, distribution or their own use is free to do so.

**Português (original).** Isto não é um produto e deve ser usado apenas para testar o software, Claude Code, Codex, OpenEvidence e os algoritmos. Se alguém quiser desenvolver o produto para venda, distribuição ou uso próprio, sinta-se à vontade para fazê-lo.

**中文.** 本项目不是产品，仅可用于测试软件、Claude Code、Codex、OpenEvidence 及相关算法。任何人如欲开发该产品用于销售、分发或自用，均可自由进行。

**Latine.** Hoc opus merx non est; adhibendum est tantummodo ad programmata,
Claude Code, Codex, OpenEvidence algorithmosque probanda. Si quis ex eo
mercem facere voluerit, ut vendat, distribuat vel ipse utatur, libere faciat.


## End-user tools: policy update (2026-10-17)

The earlier tool-denial/read-only adapter description is superseded.
Development dependencies do not restrict end-user tasks. All available tools
may be used under normal provider permissions. The policy is automatically
prepended to EVERY request, and fresh macOS request workspaces get AGENTS.md
and CLAUDE.md containing it. Source copies no longer carry a file whitelist.
See USER-TOOLS.md for details. Restart the native helper after rebuilding.

Generated-file links now use an extension and MIME type matching the actual
format: Markdown is .md and plain text is .txt. A supplied .docx name cannot
turn text into a Word file. Each generated artifact also has a 'Ver texto'
preview in the answer dialog, where its source text can be read and selected.
This fixes export correctness; it does not establish the cause of a browser
security block or change Chrome's download protections.

### NAIA: pergunta acompanhada do artigo em texto (2026-10-18)

Abra no buffer o documento com a pergunta e o artigo completo em Markdown.
O botão NAIA agora também aceita esse documento comum, além do formato de
Autoclave. A confirmação informa o destino e o número de caracteres; o
helper envia uma mensagem de texto e acrescenta a resposta sem substituir
o artigo. Não há anexação automática. Erros preservam o buffer e o caminho
de diagnóstico, sem repetir o envio automaticamente.

Nos pedidos ao ChatGPT/Claude, "Submeter a pergunta com o paper ao NAIA"
é reconhecido também em português. Uma única Q no buffer é identificada
sem exigir repetir seu número no pedido. Quando solicitado o artigo, o
texto completo segue junto, em vez de apenas a primeira linha da pergunta.
Uma recusa externa de permissão deve ser preservada e informada; esta
implementação não desativa nem altera políticas do navegador/provedor.

As respostas do NAIA agora oferecem Markdown para salvar. Pelo diálogo do
ChatGPT/Claude, a resposta vem com o arquivo NAIA-Q<num>.md. Pelo botão NAIA,
o link "Salvar Markdown" aparece ao lado do botão. No Autoclave, o arquivo
inclui o teste completo com suas respostas. O buffer continua disponível.

Atualização: as respostas do NAIA NÃO alteram nem são acrescentadas ao buffer.
O diálogo oferece NAIA-Q<num>.md para salvar; o botão NAIA mostra "Salvar
Markdown". No Autoclave, o teste preenchido é disponibilizado somente no
arquivo Markdown. Isso substitui o comportamento de inserção descrito acima.

Toolbar repair (2026-10-18): escape HTML comment/script opening tokens in embedded JavaScript. Validate the standalone page as well as the separate-script test harness.

All NAIA requests explicitly require complete Markdown in the chat, without attachments or download links. Browsered creates the downloadable .md locally from that reply and preserves the buffer.
