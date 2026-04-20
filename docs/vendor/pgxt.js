// PGX1: pre-decoded RGBA8 texture sidecar.
//
// Layout (little-endian):
//   off  0  u8[4]  magic   "PGX1"
//   off  4  u32    width
//   off  8  u32    height
//   off 12  u32    format  (0 = RGBA8 row-major, origin top-left — matches PNG)
//   off 16  u8[]   pixels  (width * height * 4 bytes)
//
// Runtime hooks libpng's png_read_image; when a foo.png.gxt sidecar is present
// alongside foo.png, the rows are copied straight from this buffer and libpng's
// per-pixel CPU decode is skipped.

(function (global) {
  const MAGIC = 0x31584750; // 'P','G','X','1' little-endian
  const FMT_RGBA8 = 0;
  const HEADER_SIZE = 16;

  function pgxtEncodeRGBA8(rgba, width, height) {
    const expected = width * height * 4;
    if (rgba.length !== expected) {
      throw new Error(`pgxt: rgba length ${rgba.length} != w*h*4 ${expected}`);
    }
    const out = new Uint8Array(HEADER_SIZE + expected);
    const dv = new DataView(out.buffer);
    dv.setUint32(0, MAGIC, true);
    dv.setUint32(4, width, true);
    dv.setUint32(8, height, true);
    dv.setUint32(12, FMT_RGBA8, true);
    out.set(rgba, HEADER_SIZE);
    return out;
  }

  global.pgxt = { encodeRGBA8: pgxtEncodeRGBA8, HEADER_SIZE, MAGIC, FMT_RGBA8 };
})(typeof window !== 'undefined' ? window : globalThis);
