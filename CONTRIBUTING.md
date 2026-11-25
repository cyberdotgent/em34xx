# Contributing to em34xx

These rules apply to every pull request, whether a person or an agent wrote
it. A pull request is considered for review once it meets them. Reviewers,
human or agent, check against this file.

em34xx is a side project. Reviews happen when the maintainer has time, in an
order the maintainer sets.

## What a pull request must be

- **Complete.** A pull request is a working feature or a self-contained fix.
  A first slice that needs follow-up pull requests before the feature works
  stays a draft until it is whole. One larger pull request is preferred over
  a series of small ones.
- **Stated.** The description says what "works" means for this change, and
  what is out of scope.
- **Tested, and honest about it.** The description lists what was run and
  observed, what was only read, and what was not tested. A claim of testing
  is something that was actually run.
- **Current.** It merges onto `main` and CI passes on Linux, macOS and
  Windows.
- **Documented.** Tests and the documents in `docs/` change with the code.

## Fun features, and serious ones

em34xx is not a commercial product, and some fun is welcome: easter eggs
and the like, in the spirit of the Christmas hat on VLC's icon.

A pull request with a fun feature must:

- **declare it** in the description, and not hide it in other work;
- **let the user turn it off**, with a setting in the Settings dialogs and
  in the configuration file;
- **stay out of the way**: it must not harm or annoy the user, or get
  between them and their work.

The maintainer's test is "does it spark joy". That is subjective, and the
decision is the maintainer's alone. A feature that sparks joy may be
accepted even where it bends the rules above.

Fun does not crowd out serious use. Enterprise and business features, such
as central management and deployment, Kerberos or client certificates, are
just as welcome and are reviewed on whether they make sense for the
project.

## Front ends

The front ends are Cocoa (macOS), Win32 and Qt (once merged). macOS is a
first-class platform: a new feature must not leave it behind or break it.

A change that users see in a window must be:

- validated by hand on at least two of the three front ends, and
- present and building on the third, which the documentation marks as
  untested.

Until the Qt front end is merged this means Cocoa and Win32. A contributor
without a Mac waits for Qt and then validates on Qt and Win32.

## Accuracy

The goal is a terminal that behaves like the hardware, edge cases included.

- Emulate hardware. No code or data from ROM images in `src/`, and no ROM or
  overlay patches. ROM images are not committed; neither are disassembly
  listings or byte dumps. Analysis notes go in `docs/`.
- Other emulators and clients are behavioural references only, whatever
  their licence. Describe their behaviour and reimplement it; do not copy or
  port their code.
- Label every fact about a machine as **observed**, **documented** (name
  the source) or **hypothesis**, in `ModelInfo::notes` and the
  `docs/bringup-*.md` files.

## Technical rules

- C++17 only. Windows 7 x86 stays a viable target.
- Dependencies come through the vcpkg manifest.
- Build: `VCPKG_ROOT=~/vcpkg cmake --preset debug && cmake --build --preset debug`.
- Tests: `ctest --preset debug -LE slow`.
- Licence: GPL-3.0-or-later, with the SPDX line at the top of each source
  file.

## Code written with AI tools

AI tools are welcome. The rules:

- **You are the author.** You are responsible for every line you submit,
  however it was produced.
- **Understand it.** You must be able to explain, when asked in review, what
  each part does, why it exists and why it is done that way. "The model
  wrote it" is not an answer. A pull request whose author cannot justify its
  contents may be closed without a detailed review.
- **Keep it concise and readable.** Remove what the change does not need:
  speculative abstractions, dead code, checks for cases that cannot happen,
  and comments that restate the code. The same goes for descriptions and
  replies: short and factual.
- **Disclose it.** Disclosure is required. The pull request description
  names each tool, model and provider used, for example:

  `AI assistance: Claude Code, Claude Fable 5.2 (Anthropic)`

  Give the exact model version; list every model if several were used. If no
  AI tool was used, write `AI assistance: none`. The disclosure is also how
  problems tied to one model's habits are tracked, so keep it accurate.

## How review works

- A review states the problem and the behaviour expected, not a required
  design. A solution nobody thought of is welcome if it is honest about what
  the hardware does and it is accurate.
- A review may ask you to explain any part of the change.
- "Changes requested" is answered with commits on the same pull request,
  not a new one.

## Pull requests opened before these rules

Pull requests that were open on 25 November 2026, when these rules were
written, are not required to comply with them, though they are encouraged
to. This is a one-off relaxation to carry existing work over and keep the
load down for everyone; it is not a free pass. The maintainer has the final
say and may still ask such a pull request to comply.
