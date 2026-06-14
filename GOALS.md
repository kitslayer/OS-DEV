# North-star goals — an honest difficulty breakdown

You named three end goals: **(1) a web browser, (2) play music from the NAS,
(3) run Claude Code.** They are all reachable in *some* form, but they differ
enormously in difficulty, and one of them is genuinely a multi-year stretch.
Here's the honest picture so expectations match reality.

All three sit on top of the same **foundation** (interrupts → memory →
scheduler → userspace → libc → filesystem). That foundation is the achievable,
fun, well-documented part — months of work for a determined hobbyist. The goals
below are what comes *after* it, and each adds large subsystems.

---

## Update (2026-06-02): where we actually landed

- **🌐 Web browser — substantially achieved (for HTTP).** OS-DEV has a
  from-scratch TCP/HTTP stack and its own HTML renderer drawing to the
  framebuffer: headings, bold/italic, links, lists, tables, `<pre>`, `<img>`
  alt text, form-field placeholders, and HTML entities — fully keyboard-driven
  (link nav, in-page find, bookmarks, history, save). It renders real sites like
  info.cern.ch and example.com. **HTTPS/TLS is the remaining gate** to the modern
  (mostly-HTTPS, JS-heavy) web — see that section below; it's the honest reason
  the browser is HTTP-only.
- **🎵 Music from the NAS — shelved.** Miles deprioritized this ("idc about music
  i want a good DE"). The networking foundation it needed exists; an audio driver
  + decoder were never built.
- **🤖 Run Claude Code — still a multi-year stretch.** Unchanged: needs a Linux
  syscall ABI + a JS runtime. Not attempted.

The detailed original analysis follows.

---

## 🎵 Play music from the NAS — **the most achievable goal**

What it needs:
- **NIC driver** — QEMU emulates `e1000` / `virtio-net`. Moderate.
- **TCP/IP stack** — ARP, IPv4, ICMP, UDP, TCP, DNS. Big but well-trodden; we
  can write it or port **lwIP**.
- **A way to reach the NAS:**
  - If the NAS exposes **HTTP/DLNA** → just an HTTP client. *Much* easier.
  - **SMB/CIFS** or **NFS** client → considerably more work.
- **Audio driver** — QEMU emulates Intel HD Audio (`intel-hda`) or `AC'97`.
  Push PCM samples to the codec. Moderate.
- **Decoding** — WAV is raw PCM (trivial). MP3/FLAC = port a small decoder
  (`minimp3`, `dr_flac`).

**Realistic milestone:** stream a WAV (or MP3 via a ported decoder) over HTTP
from the NAS and play it through the AC'97/HDA codec. This is genuinely
attainable and a fantastic mid-term target.

---

## 🌐 A web browser — **achievable in a limited form**

Two routes:

- **(a) Write your own engine.** A minimal HTML/CSS renderer (little or no
  JavaScript) drawing to a framebuffer with a font rasterizer. Large but
  bounded — you can render simple pages. This is the SerenityOS path; they
  spent *years* building their engine (Ladybird).
- **(b) Port an existing engine.** The realistic target is **NetSurf** — a
  lightweight browser with its own engine and few dependencies, designed to be
  portable. Porting **Firefox/Chromium is effectively infeasible solo**:
  millions of lines, needs a near-complete Linux userland, GPU, threads, and
  sandboxing.

Either route also needs: **framebuffer graphics** (VBE or QEMU virtio-gpu),
**fonts** (port FreeType or a bitmap font), **TLS** (port BearSSL/mbedTLS), and
the TCP stack from the music goal.

**Realistic milestone:** a simple browser (your own renderer, or a NetSurf
port) that loads basic HTTPS pages. Not "modern Chrome."

---

## 🤖 Run Claude Code — **the hardest by far; a long-horizon stretch goal**

Claude Code is a **Node.js** application. Running it natively means running
Node (V8 + libuv) on this OS, which requires:

- A substantial **POSIX libc** (port **musl** or **newlib**), **threads**
  (pthreads/futexes), `mmap`, signals, a **PTY/terminal**, a real process model.
- A **TCP/IP stack with working TLS 1.2/1.3** (it calls the Anthropic API over
  HTTPS).
- Then either:
  - **(a) compile Node for the OS** — Node's build is enormous and assumes a
    lot of POSIX; very hard, or
  - **(b) implement enough of the Linux syscall ABI to run a *prebuilt* Linux
    Node binary** (a "Linux compatibility layer") — the pragmatic route, and how
    some hobby OSes run real software. Still means implementing dozens of
    syscalls precisely (`clone`, `futex`, `epoll`, `mmap`…) plus a dynamic
    linker, or running a static build.

**Honest assessment:** this is essentially *"make the OS Linux-compatible
enough to run Node and do TLS networking."* It's a multi-year goal and may stay
aspirational — but it's the single best reason to make one **early
architectural decision** (below).

---

## The one early decision these goals force

Most of the foundation is identical no matter what. But around the
**userspace/libc** stage we choose a philosophy:

- **Pure from-scratch** ("SerenityOS style"): write your own libc, your own
  browser, your own everything. Music ✅, your own simple browser ✅, but you'd
  build your *own* tools rather than run Node — so **Claude Code wouldn't run
  natively** this way.
- **Compatibility-oriented** ("run real software"): aim early for a **musl
  libc + Linux-compatible syscall ABI**, so the long-term payoff is running
  unmodified third-party binaries — eventually Node (→ Claude Code) and a
  ported browser. More constraining, lots of unglamorous syscall work, but the
  only realistic path to literally running Claude Code.

**We do not need to decide today.** Milestones 1–7 are the same either way.
We pick the fork when we reach userspace (milestone 8).
