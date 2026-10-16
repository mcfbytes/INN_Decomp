// "Buoy Meets Gull": G Mixolydian, 6/8, a seaside jig for squeezebox-organ with a buoy-bell motif.
// Original; GPL-3.0-or-later with the project.
{
  const chords = ['G', 'F', 'G', 'F', 'C', 'G', 'F', 'G', 'G', 'F', 'G', 'F', 'C', 'D', 'F', 'G',
    'C', 'C', 'G', 'G', 'Am', 'Am7', 'F', 'D', 'G', 'F', 'G', 'F', 'C', 'G', 'F.D', 'D'];
  // the buoy: two bell strokes a bar, on the chord's own notes
  const buoy = { G: 'g6 d6', F: 'f6 c6', C: 'g6 e6', D: 'a6 d6', Am: 'e6 a6' };
  const rest = Array(8).fill('z12');
  const A = ['D2 G2 B2 d4 B2', 'c6 A2 F2 A2', 'G2 B2 d2 g4 d2', 'f4 e2 c6', 'E2 G2 c2 e4 c2', 'd6 B2 G2 B2', 'A2 c2 f2 a4 f2', 'g4 d2 B4 G2'];
  InnMusic.song({
    id: 'harbor', title: 'Buoy Meets Gull', bpm: 100, bar: 12, bars: 32, chords,
    parts: [
      { i: 'organ', d: 'flute', v: .9, pan: -.1, n: [...A,
        'D2 G2 B2 d4 B2', 'c6 A2 F2 A2', 'G2 B2 d2 g4 b2', 'a4 g2 f6', 'e2 g2 c\'2 g4 e2', 'A2 d2 ^f2 a4 ^f2', 'c6 A2 F4', 'G6 z6',
        'g6 e6', 'c6 e2 d2 c2', 'B6 d6', 'g6 d2 B2 G2', 'A2 c2 e2 a4 e2', 'a6 g2 e2 c2', 'f6 a6', '^f4 a2 d\'6',
        ...A.slice(0, 3), 'f4 e2 c4 A2', 'E2 G2 c2 e4 c2', 'd6 B2 G2 B2', 'A2 c2 f2 ^f2 a2 d\'2', 'd6 A4 ^F2'].join('|') },
      { i: 'flute', v: .55, pan: .2, n: [...rest, ...rest, 'c12', 'G12', 'D12', 'B12', 'c12', 'G12', 'A12', '^F12', ...rest].join('|') },
      { i: 'bell', v: .55, pan: .3, n: chords.map((c) => buoy[c.split('.')[0].replace('7', '')]).join('|') },
      { i: 'pluck', v: .4, pan: .35, k: 'comp', lo: 53, gate: .5, p: ['--y-z---y-z-'] },
      { i: 'bass', v: .8, p: [...Array(7).fill('1.....5.....'), '1.....5...n.'] },
      { i: 'drums', v: .4, p: ['(kh).h.h.(rh).h.h.'] },
    ],
  });
}
