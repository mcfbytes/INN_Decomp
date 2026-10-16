// "The Pie Is Not a Metaphor": A blues in a desert-Americana shuffle, 4/4, three twelve-bar choruses.
// Original; GPL-3.0-or-later with the project.
{
  const head = ['E2 A2 ^c2 e2 ^c2 A2 E4', 'G4 A2 =c2 ^c4 A4', 'e4 e2 ^c2 e4 g4', '^c4 A4 G4 ^F4',
    'D2 ^F2 A2 d2 A2 ^F2 D4', 'c4 A2 ^F2 A4 z4', 'E2 A2 ^c2 e2 g4 e4', '^c4 A4 E4 z4',
    'B4 ^G4 B4 d4', '^f4 d4 A4 ^F4', 'e2 ^c2 A2 G2 E4 A4', 'A4 ^c4 B4 ^G4'];
  const reply = ['e8 ^c4 A4', 'g4 e4 =c4 ^c4', 'A8 E4 G4', 'A12 ^F4', 'd8 ^f4 d4', 'c8 A4 ^F4',
    'E4 A4 ^c4 e4', 'g8 e4 ^c4', '^g8 b4 ^g4', 'a8 ^f4 d4', '^c4 A4 E4 A4', '^c4 e4 ^g4 b4'];
  const rest = Array(12).fill('z16');
  const walk = ['1...3...5...6...', '1...3...5...6...', '1...3...5...3...', '1...3...5...n...',
    '1...3...5...6...', '1...3...5...n...', '1...3...5...6...', '1...3...5...n...',
    '1...3...5...6...', '1...3...5...n...', '1...3...5...6...', '1...5...1...n...'];
  InnMusic.song({
    id: 'diner', title: 'The Pie Is Not a Metaphor', bpm: 100, bar: 16, bars: 36, swing: .33,
    chords: [...Array(3)].flatMap(() => ['A7', 'A7', 'A7', 'A7', 'D7', 'D7', 'A7', 'A7', 'E7', 'D7', 'A7', 'A7.E7']),
    parts: [
      { i: 'pluck', d: 'harpsichord', v: .85, pan: -.1, n: [...head, ...rest, ...head].join('|') },
      { i: 'strings', d: 'organ', v: .55, pan: -.1, n: [...rest, ...reply, ...rest].join('|') },
      { i: 'flute', v: .5, pan: .25, n: [...rest, ...rest, '^c16', '^c16', 'e16', 'e16', '^f16', '^f16', 'e16', '^c16', '^g16', 'a16', 'e16', 'e8 ^g8'].join('|') },
      { i: 'organ', v: .3, pan: .3, k: 'comp', lo: 57, gate: .4, p: ['----y-------z---'] },
      { i: 'bass', v: .8, p: walk },
      { i: 'drums', v: .45, p: ['(kh).h.(sh).h.(kh).h.(sh).h.'] },
    ],
  });
}
