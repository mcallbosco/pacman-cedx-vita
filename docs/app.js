(() => {
  const apkDrop = document.getElementById('apk-drop');
  const obbDrop = document.getElementById('obb-drop');
  const apkInput = document.getElementById('apk-input');
  const obbInput = document.getElementById('obb-input');
  const apkName = document.getElementById('apk-name');
  const obbName = document.getElementById('obb-name');
  const buildBtn = document.getElementById('build-btn');
  const progressBox = document.getElementById('progress');
  const bar = document.getElementById('bar');
  const status = document.getElementById('status');
  const logBox = document.getElementById('log');
  const hashWarning = document.getElementById('hash-warning');

  /** SHA-256 of known-good inputs (version 120); mismatch shows a warning only. */
  const EXPECTED_APK_SHA256 =
    '206be87112980605a5367f7432b69c38e95a5811fae9862afb66ad8bc08bf2a6';
  const EXPECTED_OBB_SHA256 =
    'f49083211370109f883965404b4a335b3125443379fc98197007e41e31337397';

  const state = {
    apk: null,
    obb: null,
    apkHashOk: null,
    obbHashOk: null,
  };

  const fmtBytes = (n) => {
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
    if (n < 1024 * 1024 * 1024) return `${(n / (1024 * 1024)).toFixed(1)} MB`;
    return `${(n / (1024 * 1024 * 1024)).toFixed(2)} GB`;
  };

  const setProgress = (pct, msg) => {
    progressBox.hidden = false;
    bar.style.setProperty('--pct', `${Math.max(0, Math.min(100, pct))}%`);
    if (msg) status.textContent = msg;
  };

  const log = (msg, cls) => {
    logBox.hidden = false;
    const line = document.createElement('div');
    if (cls) line.className = cls;
    line.textContent = msg;
    logBox.appendChild(line);
    logBox.scrollTop = logBox.scrollHeight;
  };

  const refreshButton = () => {
    buildBtn.disabled = !(state.apk && state.obb);
  };

  const bytesToHex = (buf) =>
    Array.from(new Uint8Array(buf))
      .map((b) => b.toString(16).padStart(2, '0'))
      .join('');

  const sha256HexOfFile = async (file) => {
    const data = await readFile(file);
    const digest = await crypto.subtle.digest('SHA-256', data);
    return bytesToHex(digest);
  };

  const updateHashWarning = () => {
    const apkBad = state.apk && state.apkHashOk === false;
    const obbBad = state.obb && state.obbHashOk === false;
    hashWarning.hidden = !(apkBad || obbBad);
  };

  const bindDrop = (zone, input, slot) => {
    zone.addEventListener('click', () => input.click());
    zone.addEventListener('dragover', (e) => {
      e.preventDefault();
      zone.classList.add('drag');
    });
    zone.addEventListener('dragleave', () => zone.classList.remove('drag'));
    zone.addEventListener('drop', (e) => {
      e.preventDefault();
      zone.classList.remove('drag');
      const f = e.dataTransfer.files?.[0];
      if (f) handleFile(slot, f);
    });
    input.addEventListener('change', () => {
      const f = input.files?.[0];
      if (f) handleFile(slot, f);
    });
  };

  const handleFile = (slot, file) => {
    state[slot] = file;
    if (slot === 'apk') state.apkHashOk = null;
    else state.obbHashOk = null;
    updateHashWarning();

    const zone = slot === 'apk' ? apkDrop : obbDrop;
    const label = slot === 'apk' ? apkName : obbName;
    label.textContent = `${file.name} — ${fmtBytes(file.size)}`;
    zone.classList.add('ready');
    refreshButton();

    const expected = slot === 'apk' ? EXPECTED_APK_SHA256 : EXPECTED_OBB_SHA256;
    const canDigest =
      typeof crypto !== 'undefined' && crypto.subtle && typeof crypto.subtle.digest === 'function';

    if (!canDigest) {
      /* No Web Crypto (e.g. non-secure origin): skip check, do not show mismatch banner. */
      return;
    }

    sha256HexOfFile(file)
      .then((hex) => {
        if (state[slot] !== file) return;
        const ok = hex === expected;
        if (slot === 'apk') state.apkHashOk = ok;
        else state.obbHashOk = ok;
        updateHashWarning();
      })
      .catch(() => {
        if (state[slot] !== file) return;
        if (slot === 'apk') state.apkHashOk = false;
        else state.obbHashOk = false;
        updateHashWarning();
      });
  };

  bindDrop(apkDrop, apkInput, 'apk');
  bindDrop(obbDrop, obbInput, 'obb');

  // Prevent the window from eating drops that miss the zones.
  ['dragover', 'drop'].forEach((ev) =>
    window.addEventListener(ev, (e) => e.preventDefault())
  );

  const readFile = (file, onProgress) =>
    new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onprogress = (e) => {
        if (e.lengthComputable && onProgress) onProgress(e.loaded / e.total);
      };
      reader.onload = () => resolve(new Uint8Array(reader.result));
      reader.onerror = () => reject(reader.error);
      reader.readAsArrayBuffer(file);
    });

  const unzipAsync = (data) =>
    new Promise((resolve, reject) => {
      fflate.unzip(data, (err, files) => (err ? reject(err) : resolve(files)));
    });

  const zipAsync = (files, opts) =>
    new Promise((resolve, reject) => {
      fflate.zip(files, opts || {}, (err, data) =>
        err ? reject(err) : resolve(data)
      );
    });

  // VPAT v1 binary patch applier. Format:
  //   "VPAT" u8(ver=1) u8(0) u32(srcLen) u32(tgtLen) u32(editCount)
  //   edits: u32(srcStart) u32(srcEnd) u32(replLen) bytes[replLen]
  // Walk edits in order; copy source[cursor..srcStart], emit replacement,
  // advance cursor to srcEnd; copy tail at end.
  const applyVpat = (source, patch) => {
    if (
      patch.length < 18 ||
      patch[0] !== 0x56 || patch[1] !== 0x50 ||
      patch[2] !== 0x41 || patch[3] !== 0x54
    ) {
      throw new Error('bad VPAT magic');
    }
    const version = patch[4];
    if (version !== 1) throw new Error(`unsupported VPAT version ${version}`);
    const dv = new DataView(patch.buffer, patch.byteOffset, patch.byteLength);
    const srcLen = dv.getUint32(6, true);
    const tgtLen = dv.getUint32(10, true);
    const editCount = dv.getUint32(14, true);
    if (srcLen !== source.length) {
      throw new Error(`VPAT source length mismatch: expected ${srcLen}, got ${source.length}`);
    }
    const out = new Uint8Array(tgtLen);
    let pos = 18;
    let srcCursor = 0;
    let dstCursor = 0;
    for (let i = 0; i < editCount; i++) {
      const srcStart = dv.getUint32(pos, true); pos += 4;
      const srcEnd = dv.getUint32(pos, true); pos += 4;
      const replLen = dv.getUint32(pos, true); pos += 4;
      const prefix = source.subarray(srcCursor, srcStart);
      out.set(prefix, dstCursor);
      dstCursor += prefix.length;
      const repl = patch.subarray(pos, pos + replLen);
      out.set(repl, dstCursor);
      dstCursor += replLen;
      pos += replLen;
      srcCursor = srcEnd;
    }
    const tail = source.subarray(srcCursor, srcLen);
    out.set(tail, dstCursor);
    dstCursor += tail.length;
    if (dstCursor !== tgtLen) {
      throw new Error(`VPAT target length mismatch: expected ${tgtLen}, got ${dstCursor}`);
    }
    return out;
  };

  const fetchBytes = async (url) => {
    const r = await fetch(url, { cache: 'reload' });
    if (!r.ok) throw new Error(`fetch ${url} failed (${r.status})`);
    return new Uint8Array(await r.arrayBuffer());
  };

  // Crop + resize a PNG in the browser via ImageBitmap + canvas, then
  // re-encode with UPNG.js so the output is an 8-bit palettised PNG
  // (<= 256 colours). Vita SceShell rejects truecolour LiveArea assets
  // with 0x8010113D, so we *must* quantise. For icon0.png we also strip
  // alpha (LiveArea icon requires no alpha channel).
  const derivePng = async (sourceBytes, crop, resize, opts = {}) => {
    const blob = new Blob([sourceBytes], { type: 'image/png' });
    const bitmap = await createImageBitmap(blob);
    const [dw, dh] = resize || [bitmap.width, bitmap.height];
    const canvas = document.createElement('canvas');
    canvas.width = dw;
    canvas.height = dh;
    const ctx = canvas.getContext('2d', { willReadFrequently: true });
    if (crop) {
      const [sx, sy, sw, sh] = crop;
      ctx.drawImage(bitmap, sx, sy, sw, sh, 0, 0, dw, dh);
    } else {
      ctx.drawImage(bitmap, 0, 0, dw, dh);
    }
    bitmap.close?.();

    const imgData = ctx.getImageData(0, 0, dw, dh);
    const rgba = imgData.data; // Uint8ClampedArray, length = dw*dh*4
    if (opts.noAlpha) {
      for (let i = 3; i < rgba.length; i += 4) rgba[i] = 255;
    }
    if (typeof UPNG === 'undefined') {
      throw new Error('UPNG.js not loaded — cannot emit indexed PNG');
    }
    // UPNG.encode(frames, w, h, cnum). cnum<=256 forces palette output.
    // Copy into a plain ArrayBuffer so UPNG doesn't choke on a typed-array view.
    const buf = new Uint8Array(rgba.length);
    buf.set(rgba);
    const png = UPNG.encode([buf.buffer], dw, dh, 256);
    return new Uint8Array(png);
  };

  // Clear one or more axis-aligned rectangles in a PNG to fully transparent,
  // preserving the rest of the image. Rects are [x1, y1, x2, y2] in pixels
  // (x2/y2 exclusive). Output is truecolour RGBA PNG — these are game
  // textures, not LiveArea assets, so no palette requirement.
  const cutoutPng = async (sourceBytes, rects) => {
    const blob = new Blob([sourceBytes], { type: 'image/png' });
    const bitmap = await createImageBitmap(blob);
    const w = bitmap.width;
    const h = bitmap.height;
    const canvas = document.createElement('canvas');
    canvas.width = w;
    canvas.height = h;
    const ctx = canvas.getContext('2d', { willReadFrequently: true });
    ctx.drawImage(bitmap, 0, 0);
    bitmap.close?.();
    for (const [x1, y1, x2, y2] of rects) {
      ctx.clearRect(x1, y1, x2 - x1, y2 - y1);
    }
    const imgData = ctx.getImageData(0, 0, w, h);
    const rgba = imgData.data;
    if (typeof UPNG === 'undefined') {
      throw new Error('UPNG.js not loaded — cannot emit PNG');
    }
    const buf = new Uint8Array(rgba.length);
    buf.set(rgba);
    const png = UPNG.encode([buf.buffer], w, h, 0);
    return new Uint8Array(png);
  };

  // -----------------------------------------------------------------------
  // Main build pipeline
  //
  // Produces a single self-contained VPK:
  //   - base VPK (loader + LiveArea assets), fetched from docs/pacmancedx.vpk
  //   - game data from APK + OBB, placed inside the VPK under data/…
  //
  // The loader reads DATA_PATH=app0:data/ at runtime, so once VitaShell
  // installs the VPK, everything at data/… inside the VPK is visible at
  // app0:data/… and no manual file copying is needed.
  // -----------------------------------------------------------------------
  buildBtn.addEventListener('click', async () => {
    buildBtn.disabled = true;
    logBox.innerHTML = '';
    logBox.hidden = true;
    progressBox.hidden = false;

    try {
      const { apk, obb } = state;

      setProgress(2, `Reading APK (${fmtBytes(apk.size)})…`);
      const apkBytes = await readFile(apk, (p) =>
        setProgress(2 + p * 10, `Reading APK… ${(p * 100).toFixed(0)}%`)
      );
      log(`Read APK: ${fmtBytes(apkBytes.length)}`, 'ok');

      setProgress(12, `Reading OBB (${fmtBytes(obb.size)})…`);
      const obbBytes = await readFile(obb, (p) =>
        setProgress(12 + p * 18, `Reading OBB… ${(p * 100).toFixed(0)}%`)
      );
      log(`Read OBB: ${fmtBytes(obbBytes.length)}`, 'ok');

      setProgress(30, 'Fetching base VPK…');
      const vpkResp = await fetch('pacmancedx.vpk', { cache: 'reload' });
      if (!vpkResp.ok) throw new Error(`Failed to fetch pacmancedx.vpk (${vpkResp.status})`);
      const baseVpk = new Uint8Array(await vpkResp.arrayBuffer());
      log(`Fetched base VPK: ${fmtBytes(baseVpk.length)}`, 'ok');

      setProgress(34, 'Decompressing base VPK…');
      const vpkFiles = await unzipAsync(baseVpk);
      log(`Base VPK entries: ${Object.keys(vpkFiles).length}`);

      setProgress(38, 'Decompressing APK…');
      const apkFiles = await unzipAsync(apkBytes);
      log(`APK entries: ${Object.keys(apkFiles).length}`);

      setProgress(56, 'Decompressing OBB…');
      const obbFiles = await unzipAsync(obbBytes);
      log(`OBB entries: ${Object.keys(obbFiles).length}`);

      // -------- Build the combined VPK entry set --------
      // Start with everything from the base VPK (eboot, sce_sys, etc.).
      const out = Object.create(null);
      for (const [name, data] of Object.entries(vpkFiles)) {
        if (!data || name.endsWith('/')) continue;
        out[name] = data;
      }

      // Pick game files out of the APK. The loader expects data/… inside
      // the VPK so it resolves to app0:data/… at runtime. We keep only the
      // ARMv7 libraries (Vita is 32-bit) and everything under assets/.
      // Also stash a few APK resources under __stage/apk/ so the LiveArea
      // derive_png overrides can reach them without plumbing apkFiles
      // through the override pipeline. __stage/* entries are filtered out
      // before packing.
      setProgress(60, 'Selecting files from APK…');
      const DATA_PREFIX = 'data/';
      const STAGE_PREFIX = '__stage/';
      const wantedLibs = new Set([
        'lib/armeabi-v7a/libPacmanCE.so',
        'lib/armeabi-v7a/libfmod.so',
      ]);
      const wantedApkResources = new Set([
        'res/drawable-xxxhdpi-v4/icon.png',
      ]);
      let libsFound = 0;
      let apkAssets = 0;
      for (const [name, data] of Object.entries(apkFiles)) {
        if (!data || name.endsWith('/')) continue;
        if (wantedLibs.has(name)) {
          out[DATA_PREFIX + name] = data;
          libsFound += 1;
          log(`  + ${name} (${fmtBytes(data.length)})`);
        } else if (name.startsWith('assets/')) {
          out[DATA_PREFIX + name] = data;
          apkAssets += 1;
        } else if (wantedApkResources.has(name)) {
          out[STAGE_PREFIX + 'apk/' + name] = data;
        }
      }
      if (libsFound < 2) {
        throw new Error(
          `APK is missing ARMv7 libraries (found ${libsFound}/2). This build ` +
          `does not appear to ship lib/armeabi-v7a/libPacmanCE.so + libfmod.so. ` +
          `Are you sure this is the Android PAC-MAN CE DX APK and not a 64-bit-only repack?`
        );
      }
      log(`Selected ${libsFound} libs + ${apkAssets} APK assets`, 'ok');

      // Merge OBB assets on top. The OBB is the canonical asset superset;
      // any collisions are resolved in the OBB's favor.
      setProgress(72, 'Merging OBB assets…');
      let obbAdded = 0;
      let obbOverrides = 0;
      for (const [name, data] of Object.entries(obbFiles)) {
        if (!data || name.endsWith('/')) continue;
        const dest = DATA_PREFIX + name;
        if (out[dest]) obbOverrides += 1;
        out[dest] = data;
        obbAdded += 1;
      }
      log(`OBB: ${obbAdded} entries merged (${obbOverrides} overrode APK)`, 'ok');

      // -------- Apply Vita-specific overrides on top of OBB data --------
      // docs/overrides/manifest.json lists small per-file edits that the
      // port needs (shader tweaks + a new texture). Shader edits are
      // shipped as VPAT binary patches so only the diff travels over the
      // wire; the originals must already be present from the OBB.
      setProgress(74, 'Fetching overrides…');
      let manifest;
      try {
        const mResp = await fetch('overrides/manifest.json', { cache: 'reload' });
        if (!mResp.ok) throw new Error(`HTTP ${mResp.status}`);
        manifest = await mResp.json();
      } catch (e) {
        log(`No overrides applied: ${e.message || e}`, 'warn');
        manifest = { overrides: [] };
      }
      let overridesApplied = 0;
      for (const o of manifest.overrides || []) {
        if (o.type === 'patch') {
          if (o.format !== 'vpat1') {
            throw new Error(`unknown patch format: ${o.format}`);
          }
          const source = out[o.target];
          if (!source) {
            throw new Error(
              `override target missing (OBB did not provide it): ${o.target}`
            );
          }
          const patch = await fetchBytes(o.patch);
          out[o.target] = applyVpat(source, patch);
          log(`  ~ patched ${o.target} (${fmtBytes(patch.length)} patch)`);
          overridesApplied += 1;
        } else if (o.type === 'add' || o.type === 'replace') {
          const data = await fetchBytes(o.source);
          out[o.target] = data;
          log(`  + ${o.target} (${fmtBytes(data.length)})`);
          overridesApplied += 1;
        } else if (o.type === 'png_cutout') {
          const source = out[o.target];
          if (!source) {
            throw new Error(`png_cutout target missing: ${o.target}`);
          }
          const modified = await cutoutPng(source, o.rects || []);
          out[o.target] = modified;
          log(`  ~ cut ${(o.rects || []).length} region(s) in ${o.target} (${fmtBytes(modified.length)})`);
          overridesApplied += 1;
        } else if (o.type === 'derive_png') {
          const source = out[o.from];
          if (!source) {
            throw new Error(`derive_png source missing: ${o.from}`);
          }
          const derived = await derivePng(source, o.crop, o.resize, {
            noAlpha: !!o.no_alpha,
          });
          out[o.target] = derived;
          const tag = o.crop
            ? `crop ${o.crop.join('x')} → ${o.resize.join('x')}`
            : `→ ${o.resize.join('x')}`;
          log(`  ~ derived ${o.target} (${tag}, ${fmtBytes(derived.length)})`);
          overridesApplied += 1;
        } else {
          throw new Error(`unknown override type: ${o.type}`);
        }
      }
      log(`Applied ${overridesApplied} override(s)`, 'ok');

      // Drop staging entries so they don't leak into the final VPK.
      for (const k of Object.keys(out)) {
        if (k.startsWith(STAGE_PREFIX)) delete out[k];
      }

      // -------- GXT sidecars for PNG textures the runtime decodes -------
      // PAC-MAN's startup sequence spends ~10–15 s doing libpng CPU decode +
      // vitaGL GPU upload on a handful of large UI PNGs. Emit a raw-RGBA8
      // sidecar (.png.gxt, magic "PGX1") next to each hot texture; the port's
      // png_read_image hook copies rows directly from the sidecar at runtime
      // and skips libpng entirely. See ASSET_OPTIMIZATIONS.md § GXT Conversion.
      setProgress(75, 'Converting hot PNGs to GXT…');
      const DATA_PREFIX_ASSET = DATA_PREFIX + 'assets/data/';
      // Startup hot set (17 files) measured on hardware 2026-04-19 via the
      // pgxt.c LoadFromMem hook. Every listed file is fopen'd → LoadFromMem'd
      // → libpng-decoded during boot-to-menu. Anything outside this list is
      // either not decoded at startup (e.g. skin[A-Z]/pac_ce_maze*.png —
      // those pages are fopen'd but never hit LoadFromMem in the hot path)
      // or <20 KB PNG where decode cost is dwarfed by sidecar disk overhead.
      const gxtTargets = [
        // Menu scaffolding textures. Kept at native 2048² — an earlier
        // experiment downscaled to 1024² to halve sidecar cost, but menu
        // backgrounds rendered wrong on hardware (UVs are baked for 2048²;
        // sampling a 1024² texture picks up the wrong region). Icons use
        // separate tex/ atlases at native size, which is why they stayed
        // correct during the broken run.
        { key: 'runny/textures/splash.png',      resize: null },
        { key: 'runny/textures/menu.png',        resize: null },
        { key: 'runny/textures/how_to_play.PNG', resize: null },
        { key: 'runny/textures/menu2.png',       resize: null },
        { key: 'runny/textures/controller.png',  resize: null },
        { key: 'runny/textures/Fruits.png',      resize: null },
        // Maze preview renders drawn behind the main menu. Note the
        // uppercase .PNG extension — earlier packager globs missed these.
        { key: 'runny/textures/maze_Championship1.PNG', resize: null },
        { key: 'runny/textures/maze_Championship2.PNG', resize: null },
        { key: 'runny/textures/maze_Darkness.PNG',      resize: null },
        { key: 'runny/textures/maze_Dungeon.PNG',       resize: null },
        { key: 'runny/textures/maze_Free.PNG',          resize: null },
        { key: 'runny/textures/maze_Half.PNG',          resize: null },
        { key: 'runny/textures/maze_Highway.PNG',       resize: null },
        { key: 'runny/textures/maze_Junction.PNG',      resize: null },
        { key: 'runny/textures/maze_Manhattan.PNG',     resize: null },
        { key: 'runny/textures/maze_Spiral.PNG',        resize: null },
        { key: 'runny/textures/maze_Tutorial.PNG',      resize: null },
        // Character + effect atlases decoded as the menu builds.
        { key: 'tex/pac_ce_chara01.png',         resize: null },
        { key: 'tex/pac_ce_chara10g_en.png',     resize: null },
        { key: 'tex/pac_ce_eff00.png',           resize: null },
        { key: 'tex/pac_ce_eff02.png',           resize: null },
        { key: 'tex/speed_en.png',               resize: null },
        { key: 'tex/finger1.png',                resize: null },
      ];

      const decodeToRgba = async (pngBytes) => {
        const blob = new Blob([pngBytes], { type: 'image/png' });
        const bitmap = await createImageBitmap(blob);
        const w = bitmap.width;
        const h = bitmap.height;
        const canvas = document.createElement('canvas');
        canvas.width = w;
        canvas.height = h;
        const ctx = canvas.getContext('2d', { willReadFrequently: true });
        ctx.drawImage(bitmap, 0, 0);
        bitmap.close?.();
        const imgData = ctx.getImageData(0, 0, w, h);
        return { rgba: new Uint8Array(imgData.data.buffer), w, h };
      };

      const resizeRgba = async (pngBytes, targetW, targetH) => {
        const blob = new Blob([pngBytes], { type: 'image/png' });
        const bitmap = await createImageBitmap(blob);
        const canvas = document.createElement('canvas');
        canvas.width = targetW;
        canvas.height = targetH;
        const ctx = canvas.getContext('2d', { willReadFrequently: true });
        ctx.drawImage(bitmap, 0, 0, targetW, targetH);
        bitmap.close?.();
        const imgData = ctx.getImageData(0, 0, targetW, targetH);
        const rgba = new Uint8Array(imgData.data.buffer);
        const pngOut = UPNG.encode([rgba.buffer], targetW, targetH, 0);
        return { rgba, png: new Uint8Array(pngOut), w: targetW, h: targetH };
      };

      // CRC-32/ISO-HDLC for PNG chunk validation (poly 0xEDB88320).
      const CRC32_TABLE = (() => {
        const t = new Uint32Array(256);
        for (let n = 0; n < 256; n++) {
          let c = n;
          for (let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
          t[n] = c >>> 0;
        }
        return t;
      })();
      const crc32 = (bytes) => {
        let c = 0xFFFFFFFF;
        for (let i = 0; i < bytes.length; i++) {
          c = CRC32_TABLE[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
        }
        return (c ^ 0xFFFFFFFF) >>> 0;
      };

      // Replace a PNG's IDAT payload with a stub: keep the 8-byte signature
      // plus every pre-IDAT chunk verbatim (IHDR + PLTE/tRNS/sRGB/gAMA/…),
      // emit a zero-length IDAT, then IEND. libpng's png_read_info stops at
      // IDAT without reading its data or validating its CRC, so info_ptr
      // still populates width/height/color_type/rowbytes correctly. Since
      // our png_read_image hook serves pixels from the sidecar and
      // png_read_end is a no-op on served streams, the empty IDAT is never
      // actually decoded. Saves ~30 MB of redundant PNG bytes across the
      // 23-file hot set (sidecars already carry the pixel data).
      const buildPngStub = (pngBytes) => {
        const sig = [137, 80, 78, 71, 13, 10, 26, 10];
        for (let i = 0; i < 8; i++) {
          if (pngBytes[i] !== sig[i]) throw new Error('not a PNG');
        }
        let p = 8;
        while (p + 12 <= pngBytes.length) {
          const len = ((pngBytes[p] << 24) | (pngBytes[p + 1] << 16) |
                       (pngBytes[p + 2] << 8) | pngBytes[p + 3]) >>> 0;
          const type = String.fromCharCode(pngBytes[p + 4], pngBytes[p + 5],
                                           pngBytes[p + 6], pngBytes[p + 7]);
          if (type === 'IDAT') break;
          p += 12 + len;
        }
        const preLen = p;  // bytes [0, preLen) = signature + all pre-IDAT chunks
        const mk = (typeStr) => {
          const chunk = new Uint8Array(12);
          chunk[4] = typeStr.charCodeAt(0);
          chunk[5] = typeStr.charCodeAt(1);
          chunk[6] = typeStr.charCodeAt(2);
          chunk[7] = typeStr.charCodeAt(3);
          const c = crc32(chunk.subarray(4, 8));
          chunk[8]  = (c >>> 24) & 0xFF;
          chunk[9]  = (c >>> 16) & 0xFF;
          chunk[10] = (c >>> 8)  & 0xFF;
          chunk[11] =  c         & 0xFF;
          return chunk;
        };
        const idat = mk('IDAT');
        const iend = mk('IEND');
        const out = new Uint8Array(preLen + idat.length + iend.length);
        out.set(pngBytes.subarray(0, preLen), 0);
        out.set(idat, preLen);
        out.set(iend, preLen + idat.length);
        return out;
      };

      let gxtEmitted = 0;
      let gxtSkipped = 0;
      let gxtBytes = 0;
      let gxtPngSaved = 0;
      for (const t of gxtTargets) {
        const assetPath = DATA_PREFIX_ASSET + t.key;
        const src = out[assetPath];
        if (!src) {
          gxtSkipped += 1;
          log(`  gxt: ${t.key} not present — skipped`, 'warn');
          continue;
        }
        try {
          let rgba, w, h;
          if (t.resize) {
            const r = await resizeRgba(src, t.resize[0], t.resize[1]);
            out[assetPath] = r.png; // shrink PNG too so libpng's IHDR matches
            rgba = r.rgba; w = r.w; h = r.h;
          } else {
            const d = await decodeToRgba(src);
            rgba = d.rgba; w = d.w; h = d.h;
          }
          const sidecar = pgxt.encodeRGBA8(rgba, w, h);
          out[assetPath + '.gxt'] = sidecar;
          // Sidecar committed — now safely strip IDAT from the shipped PNG.
          // If this throws, catch leaves out[assetPath] un-stubbed and drops
          // the sidecar assignment via gxtSkipped semantics at the call-site.
          const before = out[assetPath].length;
          const stub = buildPngStub(out[assetPath]);
          out[assetPath] = stub;
          const saved = before - stub.length;
          gxtPngSaved += saved;
          gxtEmitted += 1;
          gxtBytes += sidecar.length;
          log(`  gxt: ${t.key} → ${w}x${h} sidecar=${fmtBytes(sidecar.length)} ` +
              `png=${fmtBytes(before)}→${fmtBytes(stub.length)} (−${fmtBytes(saved)})`);
        } catch (e) {
          gxtSkipped += 1;
          log(`  gxt: ${t.key} failed (${e.message || e})`, 'warn');
        }
      }
      log(`GXT: ${gxtEmitted} sidecars, ${gxtSkipped} skipped, ` +
          `${fmtBytes(gxtBytes)} sidecar bytes, ${fmtBytes(gxtPngSaved)} PNG bytes stripped`, 'ok');

      // -------- Split the output set into two archives -------------------
      // The loader reads DATA_PATH=ux0:data/pacmancedx/ at runtime, so
      // bulk game data lives on the memory card, not in the VPK. We
      // produce:
      //   1. pacmancedx.vpk  — ~2 MB: loader, LiveArea, sce_sys only.
      //      Installs via VitaShell in seconds.
      //   2. pacmancedx-data.zip — ~130 MB: every data/* entry, re-rooted
      //      under pacmancedx/ so copying that folder to ux0:data/ (e.g. via FTP)
      //      lands everything at ux0:data/pacmancedx/…
      // VitaShell's VPK installer is slow per-MB (signature rewrite, pfs
      // setup); plain zip extract is ~10× faster for the same bytes.
      const vpkEntries = Object.create(null);
      const dataEntries = Object.create(null);
      const DATA_ROOT_IN = 'data/';
      const DATA_ROOT_OUT = 'pacmancedx/';
      for (const k of Object.keys(out)) {
        if (k.startsWith(DATA_ROOT_IN)) {
          dataEntries[DATA_ROOT_OUT + k.substring(DATA_ROOT_IN.length)] = out[k];
        } else {
          vpkEntries[k] = out[k];
        }
      }

      for (const required of ['eboot.bin', 'sce_sys/param.sfo']) {
        if (!vpkEntries[required]) {
          throw new Error(`missing entry in output VPK: ${required}`);
        }
      }
      for (const required of [
        'pacmancedx/lib/armeabi-v7a/libPacmanCE.so',
        'pacmancedx/lib/armeabi-v7a/libfmod.so',
      ]) {
        if (!dataEntries[required]) {
          throw new Error(`missing entry in data zip: ${required}`);
        }
      }

      // -------- Pack. Stored (level 0) for both archives. Every input
      //          byte is already compressed (ogg/png/deflated), so deflate
      //          gains ~0% for a lot of CPU on the Vita side.  -----------
      const packStored = (set) => {
        const input = {};
        for (const k of Object.keys(set).sort()) {
          input[k] = [set[k], { level: 0 }];
        }
        return zipAsync(input);
      };

      setProgress(78, `Packing VPK (${Object.keys(vpkEntries).length} entries)…`);
      log(`Packing VPK: ${Object.keys(vpkEntries).length} entries`);
      let fakePct = 78;
      const ticker = setInterval(() => {
        fakePct = Math.min(95, fakePct + 0.5);
        setProgress(fakePct, 'Packing…');
      }, 150);

      let vpkBytes;
      try {
        vpkBytes = await packStored(vpkEntries);
        log(`Built VPK: ${fmtBytes(vpkBytes.length)}`, 'ok');
      } finally {
        clearInterval(ticker);
      }

      setProgress(95, 'Bundling VPK + data folder into a single zip…');
      fetch('https://counters.mcallbos.co/v1/hit/pacmancedx-vpk', {
        method: 'POST',
        keepalive: true,
      }).catch(() => {});
      // Single download: VPK at the root + data laid out as the
      // pacmancedx/ folder users will FTP into ux0:data/.
      const bundleBytes = await packStored({
        'pacmancedx.vpk': vpkBytes,
        ...dataEntries,
      });
      const blob = new Blob([bundleBytes], { type: 'application/zip' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = 'pacmancedx.zip';
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      setTimeout(() => URL.revokeObjectURL(url), 60_000);

      setProgress(
        100,
        `Done — bundle ${fmtBytes(bundleBytes.length)} (VPK ${fmtBytes(vpkBytes.length)}).`
      );
      log(
        'All done. Unzip pacmancedx.zip — inside you\'ll find pacmancedx.vpk (install with VitaShell) and the pacmancedx/ folder (FTP it to ux0:data/).',
        'ok'
      );
    } catch (err) {
      console.error(err);
      log(`ERROR: ${err.message || err}`, 'err');
      status.textContent = 'Build failed — see log.';
    } finally {
      buildBtn.disabled = !(state.apk && state.obb);
    }
  });
})();
