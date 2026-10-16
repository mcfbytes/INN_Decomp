// "Hay Fever Waltz": G major / E minor, 3/4, a calm and slightly wistful autumn afternoon. Original; GPL-3.0-or-later with the project.
InnMusic.song({
  id: 'valley', title: 'Hay Fever Waltz', bpm: 92, bar: 12, bars: 40, sig: '^F',
  chords: ['Em', 'C', 'G', 'D', 'Em', 'C', 'Am', 'D',
    'C', 'Cm', 'G', 'E7', 'Am', 'D7', 'G', 'D',
    'Em', 'C', 'G', 'D', 'Em', 'Am', 'D7', 'G',
    'G', 'D', 'Em', 'Bm', 'C', 'G', 'Am', 'D7',
    'Em', 'C', 'G', 'D', 'Em', 'C', 'Am', 'B7'],
  parts: [
    { i: 'flute', d: 'strings', v: .85, pan: -.15, n: [
      'B4 e4 g4', 'e6 d2 c4', 'B6 c2 d4', 'A6 B2 A4', 'g6 f2 e4', 'g4 e4 c4', 'A4 c4 e4', 'f6 e2 d4',
      'c8 d4', '_e6 d2 c4', 'B6 d2 g4', '^g4 f4 e4', 'c6 B2 A4', 'c4 A4 F4', 'G8 B4', 'A8 z4',
      'B4 e4 g4', 'e6 d2 c4', 'B6 c2 d4', 'A6 B2 A4', 'g4 f4 e4', 'c4 e4 a4', 'f6 e2 d4', 'g8 z4',
      'G12', 'A12', 'B12', 'F12', 'G12', 'd12', 'e12', 'c12',
      'B4 e4 g4', 'e6 d2 c4', 'B6 c2 d4', 'A6 B2 A4', 'g6 f2 e4', 'g4 e4 c4', 'A4 c4 e4', '^d4 f4 ^d4'].join('|') },
    { i: 'strings', v: .6, pan: .15, n: [...Array(24).fill('z12'),
      'B2 d2 g2 f2 d2 B2', 'A2 d2 f2 e2 d2 A2', 'G2 B2 e2 d2 B2 G2', 'B2 d2 f2 e2 d2 B2',
      'c2 e2 g2 f2 e2 c2', 'B2 d2 g2 a2 g2 d2', 'c2 e2 a2 g2 e2 c2', 'd4 c4 A4', ...Array(8).fill('z12')].join('|') },
    { i: 'harpsichord', v: .5, pan: .3, k: 'comp', lo: 48, gate: .9, p: ['x...y...z...', 'x...y.z.y...'] },
    { i: 'strings', v: .22, pan: -.3, k: 'bass', lo: 55, p: ['x...........'] },
    { i: 'bass', v: .8, p: ['1.......5...'] },
  ],
});
