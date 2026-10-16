// "The Stairs Have Opinions": D Dorian, 4/4, a slow processional with harpsichord, organ and a twinkling bell.
// Original; GPL-3.0-or-later with the project.
{
  const chords = ['Dm', 'Dm7', 'C', 'C', 'Dm', 'G', 'Am', 'Dm',
    'Dm', 'Dm7', 'C', 'C', 'Dm', 'G', 'C', 'Am',
    'F', 'F', 'C', 'C', 'G', 'G7', 'Am', 'A',
    'Dm', 'Dm7', 'C', 'C', 'Dm', 'G', 'Em', 'A'];
  const bell = { Dm: 'z6 a2 z4 d\'4', C: 'z6 g2 z4 e4', G: 'z6 d\'2 z4 g4', Am: 'z6 e2 z4 a4', F: 'z6 c\'2 z4 a4', A: 'z6 e2 z4 ^c\'4', Em: 'z6 b2 z4 g4' };
  const z = Array(8).fill('z16');
  InnMusic.song({
    id: 'tower', title: 'The Stairs Have Opinions', bpm: 72, bar: 16, bars: 32, chords,
    parts: [
      { i: 'harpsichord', d: 'bell', v: .8, pan: -.1, n: [
        'D4 F4 A4 d4', 'c6 B2 A4 F4', 'E4 G4 c4 G4', 'e6 d2 c4 G4', 'F4 A4 d4 f4', 'g6 f2 d4 B4', 'A4 c4 e4 c4', 'd12 z4',
        'D4 F4 A4 d4', 'c6 B2 A4 F4', 'E4 G4 c4 e4', 'g6 e2 c4 G4', 'F4 A4 d4 a4', 'g4 f4 d4 B4', 'c4 e4 g4 c\'4', 'a8 e4 c4',
        ...z,
        'D4 F4 A4 d4', 'c6 B2 A4 F4', 'E4 G4 c4 G4', 'e6 d2 c4 e4', 'F4 A4 d4 f4', 'g6 f2 d4 B4', 'B4 e4 g4 e4', '^c4 e4 a4 e4'].join('|') },
      { i: 'strings', v: .5, pan: .1, n: [...Array(16).fill('z16'),
        'c8 A4 F4', 'A4 c4 f4 c4', 'e8 g4 e4', 'c8 G4 E4', 'B8 d4 g4', 'f8 d4 B4', 'e4 c4 A4 c4', '^c8 e4 ^c4', ...z].join('|') },
      { i: 'bell', v: .5, pan: .3, n: chords.map((c) => bell[c.replace('7', '')]).join('|') },
      { i: 'organ', v: .2, pan: -.3, k: 'comp', lo: 50, gate: .95, p: ['y...............'] },
      { i: 'organ', v: .2, pan: .3, k: 'comp', lo: 50, gate: .95, p: ['z...............'] },
      { i: 'bass', v: .7, p: ['1.......5.......'] },
      { i: 'drums', v: .35, p: ['K.......r.......'] },
    ],
  });
}
