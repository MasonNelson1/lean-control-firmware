'use strict';

// Browser-only .lean codec. Keep format/version and limits in sync with app.py.
// The archive reader deliberately accepts one ordinary DEFLATE ZIP entry only.
const LeanProfiles = (() => {
  const limit = 1024 * 1024;
  const encoder = new TextEncoder();
  const decoder = new TextDecoder('utf-8', {fatal: true});
  const invalid = () => new Error('Invalid .lean file. Expected a ZIP containing compressed profile.json.');
  const crcTable = Uint32Array.from({length: 256}, (_, value) => {
    for (let bit = 0; bit < 8; bit++) value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
    return value >>> 0;
  });
  function crc32(bytes) {
    let crc = 0xffffffff;
    for (const byte of bytes) crc = crcTable[(crc ^ byte) & 255] ^ (crc >>> 8);
    return (crc ^ 0xffffffff) >>> 0;
  }
  function validate(profile) {
    if (!profile || profile.format !== 'lean-config' || profile.version !== 1 || !Array.isArray(profile.fields))
      throw new Error('Invalid or unsupported Lean configuration profile.');
    return profile;
  }
  async function encode(profile) {
    const bytes = encoder.encode(JSON.stringify(validate(profile), null, 2));
    if (bytes.length > limit) throw new Error('Expanded profile JSON must be at most 1 MiB.');
    const archive = fflate.zipSync({'profile.json': [bytes, {level: 6}]});
    if (archive.length > limit) throw new Error('Profile files must be at most 1 MiB.');
    return archive;
  }
  async function decode(input) {
    const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
    if (bytes.length > limit) throw new Error('Profile files must be at most 1 MiB.');
    try {
      const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      const u16 = offset => view.getUint16(offset, true), u32 = offset => view.getUint32(offset, true);
      const earliestEnd = Math.max(0, bytes.length - 65557);
      let end = bytes.length - 22;
      while (end >= earliestEnd &&
          (u32(end) !== 0x06054b50 || end + 22 + u16(end + 20) !== bytes.length)) end--;
      if (end < earliestEnd || u32(end) !== 0x06054b50 || u16(end + 4) || u16(end + 6) ||
          u16(end + 8) !== 1 || u16(end + 10) !== 1) throw invalid();
      const central = u32(end + 16), centralSize = u32(end + 12);
      if (central + centralSize !== end || u32(central) !== 0x02014b50) throw invalid();
      const flags = u16(central + 8), method = u16(central + 10);
      const crc = u32(central + 16), compressed = u32(central + 20), expanded = u32(central + 24);
      const nameLength = u16(central + 28), extraLength = u16(central + 30), commentLength = u16(central + 32);
      if (flags & ~0x080e || method !== 8 || expanded > limit || u16(central + 34) || u32(central + 42) !== 0 ||
          centralSize !== 46 + nameLength + extraLength + commentLength ||
          decoder.decode(bytes.subarray(central + 46, central + 46 + nameLength)) !== 'profile.json') throw invalid();
      if (u32(0) !== 0x04034b50 || u16(6) !== flags || u16(8) !== method || u16(26) !== nameLength ||
          decoder.decode(bytes.subarray(30, 30 + nameLength)) !== 'profile.json') throw invalid();
      const start = 30 + nameLength + u16(28), finish = start + compressed;
      if (start > central || finish > central || !compressed) throw invalid();
      if (flags & 8) {
        const descriptor = finish + (u32(finish) === 0x08074b50 ? 4 : 0);
        if (descriptor + 12 !== central || u32(descriptor) !== crc || u32(descriptor + 4) !== compressed ||
            u32(descriptor + 8) !== expanded) throw invalid();
      } else if (finish !== central || u32(14) !== crc || u32(18) !== compressed || u32(22) !== expanded) throw invalid();
      let size = 0;
      const chunks = [];
      const inflater = new fflate.Inflate(chunk => {
        size += chunk.length;
        if (size > limit || size > expanded) throw invalid();
        chunks.push(chunk);
      });
      // Small compressed chunks bound each inflate allocation, including when
      // a hostile archive lies about its uncompressed size in both headers.
      for (let offset = start; offset < finish; offset += 256)
        inflater.push(bytes.subarray(offset, Math.min(offset + 256, finish)), offset + 256 >= finish);
      if (size !== expanded) throw invalid();
      const content = new Uint8Array(size);
      let offset = 0;
      for (const chunk of chunks) { content.set(chunk, offset); offset += chunk.length; }
      if (crc32(content) !== crc) throw invalid();
      return validate(JSON.parse(decoder.decode(content)));
    } catch (error) {
      if (error.message === 'Invalid or unsupported Lean configuration profile.') throw error;
      throw invalid();
    }
  }
  return {encode, decode, limit};
})();
