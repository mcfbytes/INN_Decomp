// "Main Street, Mostly Paved": F major, 4/4, a light ragtime lilt. Original; GPL-3.0-or-later with the project.
InnMusic.song({
  id: 'town', title: 'Main Street, Mostly Paved', bpm: 104, bar: 16, bars: 32, swing: .2, sig: '_B',
  chords: ['F', 'D7', 'Gm', 'C7', 'F', 'F7', 'Bb', 'C7', 'F', 'D7', 'Gm', 'C7', 'F', 'Bb', 'F', 'F',
    'Bb', 'Bb', 'F', 'D7', 'G7', 'G7', 'C7', 'C7', 'F', 'D7', 'Gm', 'C7', 'F', 'Bb', 'F', 'C7'],
  parts: [
    { i: 'brass', d: 'flute', v: .8, pan: -.1, n: [
      'F2 A2 c4 c2 d2 c4', 'd2 ^f2 a4 c4 a2 ^f2', 'g4 d4 B4 d2 c2', 'E2 G2 B4 A2 G2 E4',
      'A2 c2 f4 c4 d2 c2', 'c4 A2 F2 _E4 D2 C2', 'D2 F2 B4 d4 c2 B2', 'G2 A2 B2 A2 G4 E4',
      'F2 A2 c4 c2 d2 c2 A2', 'd2 ^f2 a4 c4 a2 ^f2', 'g4 d4 B4 d2 c2', 'E2 G2 B4 A2 G2 E4',
      'A2 c2 f4 a4 g2 f2', 'B4 d4 f4 d2 B2', 'c4 A4 c4 F4', 'F2 A2 c2 f2 c4 z4',
      ...Array(8).fill('z16'),
      'F2 A2 c4 c2 d2 c4', 'd2 ^f2 a4 c4 a2 ^f2', 'g4 d4 B4 d2 c2', 'E2 G2 B4 A2 G2 E2 C2',
      'A2 c2 f4 c4 d2 c2', 'B2 d2 f4 d2 c2 B4', 'A2 c2 f4 c4 A4', 'E2 G2 B2 A2 G2 E2 C4'].join('|') },
    { i: 'flute', v: .8, pan: .1, n: [...Array(16).fill('z16'),
      'd4 d2 c2 B4 F4', 'B2 d2 f4 d4 B4', 'c4 c2 A2 c4 A4', '^f4 a4 ^f2 d2 A4',
      '=B2 d2 g4 f4 d4', 'g4 f2 e2 d4 =B4', 'c4 e4 g4 b4', 'a4 g2 f2 e4 z4', ...Array(8).fill('z16')].join('|') },
    { i: 'strings', v: .32, pan: .25, n: [...Array(8).fill('z16'),
      'A4 G4 F4 G4', 'A4 ^F4 D4 E4', 'D4 B4 A4 G4', 'C4 G4 B4 G4', 'c4 A4 F4 A4', 'F4 D4 F4 D4', 'A4 F4 A4 c4', 'A4 F4 C4 z4',
      ...Array(16).fill('z16')].join('|') },
    { i: 'pluck', v: .5, pan: .3, k: 'comp', lo: 53, gate: .5, p: ['----c-------c---', '----c-------c-c-'] },
    { i: 'bass', v: .8, p: ['1.....-.5.....-.', '1.....-.5.....-.', '1.....-.5.....-.', '1.....-.5.....-.', '1.....-.5.....-.', '1.....-.5.....-.', '1.....-.5.....-.', '1.....-.5.....n.'] },
    { i: 'drums', v: .5, p: ['(kh).h.(sh).h.(kh).h.(sh).h.'] },
  ],
});
