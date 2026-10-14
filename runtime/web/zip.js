// Reads the user's zip of an INN install: stored and deflated entries, no ZIP64, no encryption.
// Inflate is the browser's own DecompressionStream, so nothing third-party is loaded.
'use strict';

const InnZip = (() => {
  const SIG = { endOfDirectory: 0x06054b50, directoryEntry: 0x02014b50, localHeader: 0x04034b50 };
  const METHOD = { stored: 0, deflate: 8 };
  const FLAG_ENCRYPTED = 0x0001;
  const END_RECORD_SIZE = 22, MAX_COMMENT = 0xffff, ENTRY_SIZE = 46, LOCAL_SIZE = 30;

  function findEndRecord(view) {
    const lowest = Math.max(0, view.byteLength - END_RECORD_SIZE - MAX_COMMENT);
    for (let at = view.byteLength - END_RECORD_SIZE; at >= lowest; at--)
      if (view.getUint32(at, true) === SIG.endOfDirectory) return at;
    throw new Error('not a zip file');
  }

  async function inflateRaw(bytes) {
    const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }

  function parseEntry(view, at) {
    if (view.getUint32(at, true) !== SIG.directoryEntry) throw new Error('corrupt zip directory');
    const nameLength = view.getUint16(at + 28, true);
    const nameBytes = new Uint8Array(view.buffer, at + 46, nameLength);
    return {
      flags: view.getUint16(at + 8, true),
      method: view.getUint16(at + 10, true),
      packedSize: view.getUint32(at + 20, true),
      size: view.getUint32(at + 24, true),
      localHeader: view.getUint32(at + 42, true),
      path: new TextDecoder().decode(nameBytes).replace(/\\/g, '/'),
      next: at + ENTRY_SIZE + nameLength + view.getUint16(at + 30, true) + view.getUint16(at + 32, true),
    };
  }

  async function extract(view, entry) {
    if (entry.flags & FLAG_ENCRYPTED) throw new Error(`${entry.path} is encrypted`);
    const at = entry.localHeader;
    if (view.getUint32(at, true) !== SIG.localHeader) throw new Error(`corrupt zip entry ${entry.path}`);
    const start = at + LOCAL_SIZE + view.getUint16(at + 26, true) + view.getUint16(at + 28, true);
    const packed = new Uint8Array(view.buffer, start, entry.packedSize);
    let data;
    if (entry.method === METHOD.stored) data = packed.slice();
    else if (entry.method === METHOD.deflate) data = await inflateRaw(packed);
    else throw new Error(`${entry.path}: compression method ${entry.method} is not supported; re-zip with deflate`);
    if (data.length !== entry.size) throw new Error(`${entry.path}: size mismatch after inflate`);
    return data;
  }

  // Resolves to [{ path, data: Uint8Array }] for every file (directories are implied by paths).
  async function read(buffer) {
    const view = new DataView(buffer);
    const end = findEndRecord(view);
    const count = view.getUint16(end + 10, true);
    let at = view.getUint32(end + 16, true);
    const files = [];
    for (let i = 0; i < count; i++) {
      const entry = parseEntry(view, at);
      at = entry.next;
      if (!entry.path.endsWith('/')) files.push({ path: entry.path, data: await extract(view, entry) });
    }
    return files;
  }

  return { read };
})();
