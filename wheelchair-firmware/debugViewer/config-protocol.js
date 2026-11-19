'use strict';

/**
 * Browser counterpart of src/config/Protocol and Registry.
 * The firmware defines field IDs, groups, constraints, and values. This module
 * knows the shared binary types and profile editors used to describe them.
 * See docs/CONFIGURATION.md before extending any wire identifier or layout.
 */
const ConfigWire = (() => {
  const version = 1;
  const commands = Object.freeze({hello: 1, group: 2, describe: 3, read: 4, set: 5, save: 6, revert: 7, defaults: 8, preview: 9});
  const types = Object.freeze({f32: 1, f64: 2, i8: 3, u8: 4, i16: 5, u16: 6, i32: 7, u32: 8, i64: 9, u64: 10, boolean: 11, enum: 12, profile: 13});
  const editors = Object.freeze({signedPower: 1, piecewiseLinear: 2});
  // Each scalar type maps to its byte width and DataView accessor suffix.
  // BigInt accessors preserve the full range of 64-bit integer settings.
  const scalar = {
    1: [4, 'Float32'], 2: [8, 'Float64'], 3: [1, 'Int8'], 4: [1, 'Uint8'],
    5: [2, 'Int16'], 6: [2, 'Uint16'], 7: [4, 'Int32'], 8: [4, 'Uint32'],
    9: [8, 'BigInt64'], 10: [8, 'BigUint64'], 11: [1, 'Uint8'], 12: [1, 'Uint8']
  };
  const errors = [
    '', 'The wheelchair uses a different configuration protocol version.',
    'The wheelchair rejected the request format.', 'This setting or group is unavailable.',
    'The wheelchair rejected the value or a combination of settings.',
    'Configuration rejected: a wheel is commanded above 1 RPM. Stop the chair and try again.',
    'The wheelchair could not save or restore the configuration.',
    'This schema entry exceeds the protocol limit.'
  ];
  const utf8 = new TextDecoder('utf-8', {fatal: true});

  /** Reads one decoded reply body. done() verifies complete consumption. */
  class Reader {
    constructor(bytes) {
      this.bytes = bytes;
      this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      this.offset = 0;
    }

    get(type) {
      const [width, name] = scalar[type];
      const value = this.view['get' + name](this.offset, true);
      this.offset += width;
      if (type === types.boolean && value > 1) throw new Error('Invalid Boolean value in reply.');
      return type === types.boolean ? Boolean(value) : value;
    }

    u8() { return this.get(types.u8); }
    u16() { return this.get(types.u16); }

    text() {
      const size = this.u8();
      const end = this.offset + size;
      if (end > this.bytes.length) throw new Error('Incomplete configuration reply.');
      const value = utf8.decode(this.bytes.subarray(this.offset, end));
      this.offset = end;
      return value;
    }

    done() {
      if (this.offset !== this.bytes.length) throw new Error('Unexpected configuration data.');
    }
  }

  function crc16(data) {
    let crc = 0xffff;
    for (const byte of data) {
      crc ^= byte << 8;
      for (let bit = 0; bit < 8; bit++) {
        crc = ((crc << 1) ^ (crc & 0x8000 ? 0x1021 : 0)) & 0xffff;
      }
    }
    return crc;
  }

  /** Produces a COBS frame including its trailing zero for PacketSerial. */
  function encodeFrame(data) {
    const frame = new Uint8Array(data.length + Math.floor(data.length / 254) + 2);
    let codeIndex = 0, end = 1;
    for (const byte of data) {
      if (byte === 0) {
        frame[codeIndex] = end - codeIndex;
        codeIndex = end++;
      } else {
        frame[end++] = byte;
        if (end - codeIndex === 255) {
          frame[codeIndex] = 255;
          codeIndex = end++;
        }
      }
    }
    frame[codeIndex] = end - codeIndex;
    return frame.subarray(0, end + 1);
  }

  // BluetoothReceiver strips the surrounding zeros before handing us a frame.
  function decodeFrame(data) {
    const result = new Uint8Array(data.length);
    let index = 0, length = 0;
    while (index < data.length) {
      const code = data[index++];
      const end = index + code - 1;
      if (code === 0 || end > data.length) return null;
      while (index < end) {
        const byte = data[index++];
        if (byte === 0) return null;
        result[length++] = byte;
      }
      if (code < 255 && index < data.length) result[length++] = 0;
    }
    return result.subarray(0, length);
  }

  // CRC covers the decoded header and body, before COBS encoding.
  function requestFrame(seq, command, body) {
    const data = new Uint8Array(6 + body.length);
    if (data.length > 48) throw new Error('Configuration request exceeds the protocol limit.');
    data.set([0xc1, version, seq, command]);
    data.set(body, 4);
    new DataView(data.buffer).setUint16(data.length - 2, crc16(data.subarray(0, -2)), true);
    return encodeFrame(data);
  }

  // Reject damaged packets before interpreting status, metadata, or field values.
  function responseFrame(frame) {
    const data = decodeFrame(frame);
    if (!data || data.length < 8 || data.length > 400 || data[0] !== 0xc2) return null;
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    if (crc16(data.subarray(0, -2)) !== view.getUint16(data.length - 2, true)) return null;
    return {
      version: data[1], seq: data[2], command: data[3], status: data[4],
      dirty: Boolean(data[5] & 2),
      body: new Reader(data.subarray(6, -2))
    };
  }

  function idBody(id) {
    const data = new Uint8Array(2);
    new DataView(data.buffer).setUint16(0, id, true);
    return data;
  }

  function readGroup(reader) {
    const group = {id: reader.u16(), parent: reader.u16(), advanced: Boolean(reader.u8()), name: reader.text()};
    reader.done();
    return group;
  }

  function readPreview(reader) {
    const type = reader.u8();
    const fieldCount = reader.u8();
    const fieldIds = Array.from({length: fieldCount}, () => reader.u16());
    const constantCount = reader.u8();
    const constants = Array.from({length: constantCount}, () => reader.get(types.f32));
    reader.done();
    return {type, fieldIds, constants};
  }

  function readValue(field, reader) {
    if (field.type !== types.profile) return reader.get(field.type);
    if (reader.u8() !== field.editor) throw new Error('The profile type changed.');
    return field.parameters.map(() => reader.get(types.f32));
  }

  /**
   * Decodes the common field header followed by metadata for its declared type.
   * Unsupported types retain their name and description so the UI can explain
   * that a newer editor is needed while other fields remain available.
   */
  function readField(reader) {
    const field = {
      id: reader.u16(), group: reader.u16(), type: reader.u8(), restart: Boolean(reader.u8() & 1),
      name: reader.text(), description: reader.text(), units: reader.text()
    };
    if (field.type === types.profile) {
      field.editor = reader.u8();
      if (!Object.values(editors).includes(field.editor)) return {...field, unsupported: true};
      const count = reader.u8();
      if (count < 2 || count > 8 || count % 2 || (field.editor === editors.signedPower && count !== 2))
        throw new Error('Invalid profile dimensions.');
      field.parameters = Array.from({length: count}, () => ({
        name: reader.text(), default: reader.get(types.f32), min: reader.get(types.f32), max: reader.get(types.f32)
      }));
      field.default = field.parameters.map(parameter => parameter.default);
    } else if (Object.hasOwn(scalar, field.type)) {
      field.default = reader.get(field.type);
      if (field.type === types.enum) {
        const count = reader.u8();
        field.choices = Array.from({length: count}, () => ({value: reader.u8(), name: reader.text()}));
      } else if (field.type !== types.boolean) {
        field.min = reader.get(field.type);
        field.max = reader.get(field.type);
      }
    } else {
      return {...field, unsupported: true};
    }
    field.value = readValue(field, reader);
    reader.done();
    return field;
  }

  // Values use the same typed encoding for updates, replies, and persistence.
  function setBody(field, value) {
    const size = field.type === types.profile ? 1 + 4 * value.length : scalar[field.type][0];
    const data = new Uint8Array(2 + size);
    const view = new DataView(data.buffer);
    view.setUint16(0, field.id, true);
    if (field.type === types.profile) {
      view.setUint8(2, field.editor);
      value.forEach((part, index) => view.setFloat32(3 + 4 * index, part, true));
    } else {
      view['set' + scalar[field.type][1]](2, value, true);
    }
    return data;
  }

  /**
   * Owns the configuration side of an existing Web Serial connection.
   * Only one request may await a reply, matching the firmware's single slot.
   * bluetooth.js owns the reader and passes configuration frames to receive().
   */
  class Link {
    constructor() {
      this.port = null;
      this.writer = null;
      this.writeTask = Promise.resolve();
      this.pending = null;
      this.sequence = 0;
      this.generation = 0;
      this.dirty = false;
    }

    // A new generation invalidates UI work from the previous connection.
    // Passing null disconnects and rejects any request still awaiting a reply.
    attach(port) {
      this.generation++;
      this.port = port;
      this.dirty = false;
      if (this.pending) this.finish(new Error('Bluetooth connection closed.'));
      if (this.writer) return this.writer.abort().catch(() => {});
    }

    finish(error, body) {
      const pending = this.pending;
      this.pending = null;
      clearTimeout(pending.timer);
      if (error) pending.reject(error);
      else pending.resolve(body);
    }

    // Sequence and operation matching prevents unrelated replies from completing
    // the current request. Device errors become rejected promises for the UI.
    receive(frame) {
      if (!this.pending) return;
      const reply = responseFrame(frame);
      if (!reply || reply.seq !== this.pending.seq || reply.command !== this.pending.command) return;
      this.dirty = reply.dirty;
      const status = reply.version === version ? reply.status : 1;
      const error = status ? new Error(errors[status] || 'The wheelchair rejected the request.') : null;
      if (error) error.status = status;
      this.finish(error, reply.body);
    }

    async request(command, body = new Uint8Array()) {
      if (!this.port?.writable) throw new Error('Connect the wheelchair over Bluetooth first.');
      if (this.pending) throw new Error('Another configuration request is in progress.');
      const seq = this.sequence++ & 0xff;
      const frame = requestFrame(seq, command, body);
      const port = this.port;
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
          this.finish(new Error('No configuration reply. Check the connection and wheelchair firmware.'));
        }, command === commands.save ? 8000 : 2500);
        const pending = {seq, command, resolve, reject, timer};
        this.pending = pending;
        // Serialize writer ownership even if a reply arrives before the previous
        // stream write promise has released its Web Serial writer lock.
        this.writeTask = this.writeTask.then(() => {
          if (this.pending === pending && this.port === port) return this.write(port, frame);
        }).catch(error => {
          if (this.pending === pending) this.finish(error);
        });
      });
    }

    async write(port, frame) {
      const writer = port.writable.getWriter();
      this.writer = writer;
      try {
        await writer.write(frame);
      } finally {
        writer.releaseLock();
        if (this.writer === writer) this.writer = null;
      }
    }
  }

  return {commands, types, editors, Reader, Link, crc16, encodeFrame, decodeFrame, requestFrame,
          responseFrame, idBody, readGroup, readPreview, readField, readValue, setBody};
})();
