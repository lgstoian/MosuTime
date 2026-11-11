#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stoian Alexandru
# SPDX-License-Identifier: MIT
"""
Romanian pronunciation for the MosuTime clips, without a GPL phonemiser.

The Piper voice `ro_RO-mihai-medium` does not take letters: it takes a string
of IPA phonemes, and it was trained on the phoneme conventions of espeak-ng's
Romanian front end.  This module carries that pronunciation knowledge as a
plain lexicon for the closed vocabulary of `tools/gen_voice.py`, so the clips
can be rendered with onnxruntime alone.  No espeak-ng, no GPL code and no
phonemiser process is involved at any point.

The lexicon is data, not code: 38 words cover all 78 clips.  A phrase that
uses a word outside the lexicon raises instead of being guessed at, because a
wrong phoneme is silent - the clip still plays, it just says the wrong thing.

Conventions, all taken from the voice's training distribution:

* `ˈ` marks the stressed syllable, `ˌ` a secondary stress.  The model is
  sensitive to them: dropping `ˈ` changes the word.
* Final `-i` after a consonant is the non-syllabic `ʲ` (`cinci` -> `tʃintʃʲ`).
  `Luni` and `Marți` carry a doubled `ʲʲ`; that is what this voice was trained
  on and it is kept verbatim rather than "corrected".
* The diphthongs are spelled the way the voice expects, which is not textbook
  IPA: `trei` -> `tɾeɪ`, `șaisprezece` -> `ʃaɪspɾezetʃe`, `douăzeci` ->
  `doʊəzetʃʲ`.  Rewriting them as `ej`/`aj`/`owə` makes the model babble.
* `r` is the tap `ɾ`.
* `și` is `ʃi`, but `ʃiʲ` before a word that starts with a vowel
  (`și un minut` -> `ʃiʲ un minˈut`).  This sandhi is the one context rule.

Usage
-----
    python tools/mosu_g2p.py --text "Este ora două"
    python tools/mosu_g2p.py --plan      # every clip, text and phonemes
    python tools/mosu_g2p.py --selftest
"""

from __future__ import annotations

import argparse
import sys

# One entry per distinct word in the 78 clips.  Order of the phonemes is the
# order they are spoken; the stress marks belong to the word.
LEXICON = {
    # Hours: "Este ora ..."
    "este": "jˌeste",
    "ora": "ˈoɾa",
    "unu": "ˈunu",
    "două": "dˈouˌə",
    "trei": "tɾˈeɪ",
    "patru": "pˈatɾu",
    "cinci": "tʃˈintʃʲ",
    "șase": "ʃˈase",
    "șapte": "ʃˈapte",
    "opt": "ˈopt",
    "nouă": "nˈouˌə",
    "zece": "zˈetʃe",
    "unsprezece": "unspɾezˌetʃe",
    "douăsprezece": "dˈouˌəspɾezˌetʃe",
    # Minutes: "și ... minute."
    "și": "ʃi",
    "un": "un",
    "una": "ˌuna",
    "de": "de",
    "minut": "minˈut",
    "minute": "minˈute",
    "treisprezece": "tɾˈeɪspɾezˌetʃe",
    "paisprezece": "pˈaɪspɾezˌetʃe",
    "cincisprezece": "tʃintʃˈispɾezˌetʃe",
    "șaisprezece": "ʃˈaɪspɾezˌetʃe",
    "șaptesprezece": "ʃaptˈespɾezˌetʃe",
    "optsprezece": "ˌoptspɾezˈetʃe",
    "nouăsprezece": "nˈouˌəspɾezˌetʃe",
    "douăzeci": "dˌoʊəzˈetʃʲ",
    "treizeci": "tɾˌeizˈetʃʲ",
    "patruzeci": "pˌatɾuzˈetʃʲ",
    "cincizeci": "tʃˌintʃizˈetʃʲ",
    # Weekdays: "Luni."
    "luni": "lˈunʲʲ",
    "marți": "mˈartsʲʲ",
    "miercuri": "mjerkˈurʲ",
    "joi": "ʒˈoj",
    "vineri": "vˈineɾʲˌʲ",
    "sâmbătă": "sˈɨmbətə",
    "duminică": "dˌuminˈikə",
}

# Every symbol this module can emit.  gen_voice.py checks these against the
# voice's phoneme_id_map before it renders anything.
SYMBOLS = frozenset("".join(LEXICON.values()))

VOWELS = frozenset("aeiouəɨɪʊ")

# Words that palatalise a preceding "și" when the next word starts with a vowel.
PALATALISING = ("și",)


def _starts_with_vowel(phonemes: str) -> bool:
    """True when the first spoken sound is a vowel, ignoring stress marks."""
    for symbol in phonemes:
        if symbol in "ˈˌ":
            continue
        return symbol in VOWELS
    return False


def _split_punctuation(token: str) -> tuple[str, str]:
    """Split "minute." into ("minute", ".") so the stop keeps no space."""
    end = len(token)
    while end > 0 and not token[end - 1].isalnum():
        end -= 1
    return token[:end], token[end:]


def phonemize(text: str) -> str:
    """Return the phoneme string for one Romanian phrase.

    Raises KeyError on a word the lexicon does not cover: a silent wrong
    pronunciation is worse than a failed build.
    """
    tokens = text.split()
    if not tokens:
        return ""

    words = []
    for token in tokens:
        word, punctuation = _split_punctuation(token)
        key = word.lower()
        if key not in LEXICON:
            raise KeyError(f"{word!r} is not in the voice lexicon")
        words.append((key, LEXICON[key], punctuation))

    parts = []
    for index, (key, phonemes, punctuation) in enumerate(words):
        if key in PALATALISING and index + 1 < len(words):
            if _starts_with_vowel(words[index + 1][1]):
                phonemes += "ʲ"
        parts.append(phonemes + punctuation)
    return " ".join(parts)


def selftest() -> int:
    failures = 0

    def expect(text: str, wanted: str) -> None:
        nonlocal failures
        try:
            got = phonemize(text)
        except KeyError as error:
            print(f"FAIL {text!r}: {error}")
            failures += 1
            return
        if got != wanted:
            print(f"FAIL {text!r}\n  want {wanted}\n  got  {got}")
            failures += 1

    # Sandhi: "și" is palatalised only before a vowel.
    expect("și un minut.", "ʃiʲ un minˈut.")
    expect("și opt minute.", "ʃiʲ ˈopt minˈute.")
    expect("și două minute.", "ʃi dˈouˌə minˈute.")
    expect("și treizeci de minute.", "ʃi tɾˌeizˈetʃʲ de minˈute.")
    # Punctuation keeps its place, stress marks survive.
    expect("Luni.", "lˈunʲʲ.")
    expect("Este ora unu", "jˌeste ˈoɾa ˈunu")
    expect("Este ora douăsprezece", "jˌeste ˈoɾa dˈouˌəspɾezˌetʃe")

    # Unknown words must fail loudly.
    try:
        phonemize("Este ora banana")
    except KeyError:
        pass
    else:
        print("FAIL an unknown word was accepted")
        failures += 1

    # The lexicon must stay inside the symbol set the voice is checked against.
    for word, phonemes in LEXICON.items():
        stray = set(phonemes) - SYMBOLS
        if stray:
            print(f"FAIL {word!r} uses symbols outside SYMBOLS: {sorted(stray)}")
            failures += 1
        if " " in phonemes:
            print(f"FAIL {word!r} contains a space; join words in phonemize()")
            failures += 1

    if failures:
        print(f"selftest: {failures} failure(s)")
        return 1
    print(f"selftest: OK, {len(LEXICON)} words, {len(SYMBOLS)} symbols")
    return 0


def clip_texts():
    """Every phrase the watch speaks, in clip order.

    gen_voice owns the phrase tables; importing it here keeps one source of
    truth.  The import is lazy because gen_voice imports this module.
    """
    import gen_voice

    for _name, text in gen_voice.clip_plan():
        yield text


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--text", action="append", default=[],
                        help="phrase to phonemise (repeatable)")
    parser.add_argument("--plan", action="store_true",
                        help="print every clip with its phonemes")
    parser.add_argument("--selftest", action="store_true",
                        help="run the built-in checks")
    args = parser.parse_args()

    if args.selftest:
        return selftest()

    if args.plan:
        for text in clip_texts():
            print(f"{text:44s} {phonemize(text)}")
        return 0

    if not args.text:
        parser.print_help()
        return 2
    for text in args.text:
        print(phonemize(text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
