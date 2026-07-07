/* PAC-MAN CE DX+ PC PAC-MAN.dat audio reader.
 *
 * The PC archive is page-based: every 0x1000-byte physical page is XORed
 * with the same generated key, and one map page precedes each 0x400 data
 * pages. Directory records are fixed 0x80-byte entries.
 */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) {
    module.exports = factory();
  } else {
    root.pmcedxPcDatAudio = factory();
  }
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const PAGE_SIZE = 0x1000;
  const PAGES_PER_GROUP = 0x400;
  const GROUP_SPAN = (PAGES_PER_GROUP + 1) * PAGE_SIZE;
  const DIR_ENTRY_SIZE = 0x80;
  const ENTRY_FILE = 1;
  const ENTRY_DIR = 2;

  const makeKey = () => {
    let x = 0x42574954 >>> 0;
    const key = new Uint8Array(PAGE_SIZE);
    for (let i = 0; i < PAGE_SIZE; i++) {
      x = Math.imul(x, 0x4357) >>> 0;
      key[i] = ((x >>> 24) ^ (x >>> 16) ^ (x >>> 8) ^ x) & 0xff;
    }
    return key;
  };

  const KEY = makeKey();

  const asByteSource = (input) => {
    if (input && typeof input.read === 'function' && Number.isFinite(input.size)) {
      return input;
    }
    if (!(input instanceof Uint8Array)) {
      throw new Error('PAC-MAN.dat reader needs a Uint8Array or { size, read(offset, length) } source');
    }
    return {
      size: input.length,
      read(offset, length) {
        if (offset < 0 || length < 0 || offset + length > input.length) {
          throw new Error(`read outside PAC-MAN.dat at 0x${offset.toString(16)}`);
        }
        return input.subarray(offset, offset + length);
      },
    };
  };

  const u32le = (bytes, offset) =>
    (bytes[offset] |
     (bytes[offset + 1] << 8) |
     (bytes[offset + 2] << 16) |
     (bytes[offset + 3] << 24)) >>> 0;

  const hasPrefix = (bytes, offset, text) => {
    if (offset + text.length > bytes.length) return false;
    for (let i = 0; i < text.length; i++) {
      if (bytes[offset + i] !== text.charCodeAt(i)) return false;
    }
    return true;
  };

  const readName = (bytes, offset, length) => {
    let end = offset;
    const limit = offset + length;
    while (end < limit && bytes[end] !== 0) end += 1;
    let s = '';
    for (let i = offset; i < end; i++) s += String.fromCharCode(bytes[i]);
    return s;
  };

  const posixJoin = (parts) => parts.filter(Boolean).join('/');

  const BGM_ALIASES = new Map([
    ['bgm1', 'bgm1_rainbow'],
    ['bgm2', 'bgm2_dimensions'],
    ['bgm3', 'bgm3_avenue'],
    ['bgm4', 'bgm4_logic'],
    ['bgm5', 'bgm5_ost-pac-man-ce'],
  ]);

  const unique = (items) => {
    const seen = new Set();
    return items.filter((item) => {
      if (seen.has(item)) return false;
      seen.add(item);
      return true;
    });
  };

  const toOutputPaths = (entry) => {
    const paths = [];
    if (entry.kind === 'bgm') {
      const oggName = entry.name.replace(/\.bgm$/i, '.ogg');
      const aliased = oggName.replace(/^(bgm[1-5])_(1min|3min|5min|10min)/i, (match, prefix, rest) => {
        const alias = BGM_ALIASES.get(prefix.toLowerCase());
        return alias ? `${alias}_${rest}` : match;
      });
      paths.push(`assets/sound/${aliased}`);
      paths.push(`assets/sound/${oggName}`);
      return unique(paths);
    }

    const oggName = entry.name.replace(/\.wav$/i, '.ogg');
    paths.push(`assets/sound/${oggName}`);
    paths.push(`assets/sound/${oggName.toLowerCase()}`);
    paths.push(`assets/sound/${entry.name}`);
    return unique(paths);
  };

  class PcDatReader {
    constructor(input) {
      this.source = asByteSource(input);
      this._maps = new Map();
      this._dirCache = new Map();
    }

    _readPhysicalPage(offset) {
      const encrypted = this.source.read(offset, PAGE_SIZE);
      if (encrypted.length !== PAGE_SIZE) {
        throw new Error(`short page at 0x${offset.toString(16)}`);
      }
      const out = new Uint8Array(PAGE_SIZE);
      for (let i = 0; i < PAGE_SIZE; i++) out[i] = encrypted[i] ^ KEY[i];
      return out;
    }

    _mapForGroup(group) {
      let map = this._maps.get(group);
      if (map) return map;
      const page = this._readPhysicalPage(group * GROUP_SPAN);
      map = new Uint32Array(PAGES_PER_GROUP);
      for (let i = 0; i < PAGES_PER_GROUP; i++) {
        map[i] = u32le(page, i * 4);
      }
      this._maps.set(group, map);
      return map;
    }

    _pageOffset(pageNo) {
      return ((pageNo >>> 10) + pageNo + 1) * PAGE_SIZE;
    }

    readPage(pageNo) {
      if (!Number.isInteger(pageNo) || pageNo < 0) {
        throw new Error(`bad virtual page ${pageNo}`);
      }
      const offset = this._pageOffset(pageNo);
      if (offset + PAGE_SIZE > this.source.size) {
        throw new Error(`virtual page ${pageNo} points outside PAC-MAN.dat`);
      }
      return this._readPhysicalPage(offset);
    }

    nextPage(pageNo) {
      const group = pageNo >>> 10;
      const index = pageNo & (PAGES_PER_GROUP - 1);
      const entry = this._mapForGroup(group)[index] >>> 0;
      return (entry & 0x80000000) ? (entry & 0x7fffffff) : null;
    }

    readChain(startPage, byteLength, options = {}) {
      const strict = options.strict !== false;
      const chunks = [];
      let page = startPage;
      let remaining = byteLength;
      let pagesRead = 0;
      const maxPages = Math.ceil(this.source.size / PAGE_SIZE);

      while (page !== null && page !== 0xffffffff && remaining > 0) {
        if (pagesRead++ > maxPages) throw new Error('page chain loop detected');
        const data = this.readPage(page);
        const take = Math.min(PAGE_SIZE, remaining);
        chunks.push(data.subarray(0, take));
        remaining -= take;
        if (remaining <= 0) break;
        page = this.nextPage(page);
      }

      if (strict && remaining > 0) {
        throw new Error(`page chain ended ${remaining} byte(s) early`);
      }

      const total = byteLength - remaining;
      const out = new Uint8Array(total);
      let pos = 0;
      for (const chunk of chunks) {
        out.set(chunk, pos);
        pos += chunk.length;
      }
      return out;
    }

    parseDir(startPage, byteLength, path = '') {
      const cacheKey = `${startPage}:${byteLength}:${path}`;
      const cached = this._dirCache.get(cacheKey);
      if (cached) return cached;

      const bytes = this.readChain(startPage, byteLength, { strict: false });
      const entries = [];
      for (let off = 0; off + DIR_ENTRY_SIZE <= bytes.length; off += DIR_ENTRY_SIZE) {
        const type = bytes[off];
        if (type !== ENTRY_FILE && type !== ENTRY_DIR) continue;
        const name = readName(bytes, off + 1, 0x77);
        if (!name || name === '.' || name === '..') continue;
        const entry = {
          type,
          kind: type === ENTRY_DIR ? 'dir' : 'file',
          name,
          path: posixJoin([path, name]),
          startPage: u32le(bytes, off + 0x78),
          length: u32le(bytes, off + 0x7c),
        };
        entries.push(entry);
      }
      this._dirCache.set(cacheKey, entries);
      return entries;
    }

    rootEntries() {
      return this.parseDir(0, PAGE_SIZE, '');
    }

    findPath(path) {
      const parts = Array.isArray(path) ? path : String(path).split('/').filter(Boolean);
      let entries = this.rootEntries();
      let current = null;
      for (const part of parts) {
        current = entries.find((entry) => entry.name.toLowerCase() === part.toLowerCase());
        if (!current) return null;
        if (part !== parts[parts.length - 1]) {
          if (current.type !== ENTRY_DIR) return null;
          entries = this.parseDir(current.startPage, current.length, current.path);
        }
      }
      return current;
    }

    readFile(entry) {
      if (!entry || entry.type !== ENTRY_FILE) {
        throw new Error('readFile expects a file entry');
      }
      return this.readChain(entry.startPage, entry.length, { strict: true });
    }

    listAudioFiles() {
      const soundDir = this.findPath('data/sound') || {
        type: ENTRY_DIR,
        name: 'sound',
        path: 'data/sound',
        startPage: 42878,
        length: 8192,
      };
      const soundEntries = this.parseDir(soundDir.startPage, soundDir.length, soundDir.path);
      const dirs = new Map(soundEntries
        .filter((entry) => entry.type === ENTRY_DIR)
        .map((entry) => [entry.name.toLowerCase(), entry]));
      const out = [];

      for (const kind of ['bgm', 'wav']) {
        const dir = dirs.get(kind);
        if (!dir) continue;
        const files = this.parseDir(dir.startPage, dir.length, dir.path)
          .filter((entry) => entry.type === ENTRY_FILE)
          .sort((a, b) => a.name.localeCompare(b.name, 'en'));
        for (const file of files) {
          const audio = {
            ...file,
            kind,
            sourcePath: file.path,
          };
          audio.outputPaths = toOutputPaths(audio);
          audio.outputPath = audio.outputPaths[0];
          audio.note = kind === 'bgm'
            ? 'PC .bgm payload is Ogg Vorbis; beta maps it to the Android .ogg name when one exists.'
            : 'PC WAV payload maps onto the Android sound-effect name when one exists.';
          out.push(audio);
        }
      }

      return out;
    }

    validateAudio(entry, data) {
      if (entry.kind === 'bgm') {
        return hasPrefix(data, 0, 'OggS')
          ? { ok: true, format: 'Ogg Vorbis' }
          : { ok: false, format: 'unknown', message: 'missing OggS magic' };
      }
      if (entry.kind === 'wav') {
        return hasPrefix(data, 0, 'RIFF') && hasPrefix(data, 8, 'WAVE')
          ? { ok: true, format: 'WAV' }
          : { ok: false, format: 'unknown', message: 'missing RIFF/WAVE magic' };
      }
      return { ok: true, format: 'unknown' };
    }
  }

  const open = (input) => new PcDatReader(input);

  return {
    PAGE_SIZE,
    PAGES_PER_GROUP,
    GROUP_SPAN,
    open,
    makeKey,
  };
});
