// Browser-test-only device. Never include this file in the deployment artifact.
module.exports = async function mockSerial(page) {
  await page.addInitScript(() => {
    let controller, dirty = false;
    const fields = [
      {id: 10, group: 1, type: 1, name: 'Front gain', min: .01, max: 100, default: 2, value: 2},
      {id: 30, group: 1, type: 13, editor: 1, name: 'Response curve', default: [.21, .09], value: [.21, .09]},
      {id: 60, group: 2, type: 4, name: 'Left VESC ID', min: 0, max: 253, default: 1, value: 1}
    ];
    let saved = fields.map(f => structuredClone(f.value));
    window.serialTestCommands = [];
    const text = value => { const bytes = new TextEncoder().encode(value); return [bytes.length, ...bytes]; };
    const u16 = value => [value & 255, value >> 8];
    const f32 = value => { const b = new Uint8Array(4); new DataView(b.buffer).setFloat32(0, value, true); return [...b]; };
    const valueBytes = (field, value = field.value) => field.type === 13
      ? [field.editor, ...value.flatMap(f32)] : field.type === 1 ? f32(value) : [value];
    const port = {
      async open() {
        this.readable = new ReadableStream({start(next) { controller = next; }});
        this.writable = new WritableStream({write(frame) {
          const raw = ConfigWire.decodeFrame(frame.subarray(0, -1));
          const command = raw[3], body = raw.subarray(4, -2), id = body.length >= 2 ? body[0] | body[1] << 8 : 0;
          window.serialTestCommands.push(command);
          let content = [], status = 0;
          const field = fields.find(f => f.id === id);
          if (command === 1) content = [...u16(2), ...u16(fields.length)];
          else if (command === 2) content = [...u16(id + 1), ...u16(0), id === 1 ? 1 : 0,
            ...text(id === 0 ? 'Sensor gains' : 'Motor configuration')];
          else if (command === 3) {
            const f = fields[id];
            content = [...u16(f.id), ...u16(f.group), f.type, 0, ...text(f.name), ...text('Test device setting'), ...text('')];
            if (f.type === 13) content.push(f.editor, 2,
              ...text('Knee input'), ...f32(f.default[0]), ...f32(.001), ...f32(.999),
              ...text('Knee output'), ...f32(f.default[1]), ...f32(.001), ...f32(.999));
            else content.push(...valueBytes(f, f.default), ...valueBytes(f, f.min), ...valueBytes(f, f.max));
            content.push(...valueBytes(f));
          } else if (command === 4) content = valueBytes(field);
          else if (command === 5) {
            const reader = new ConfigWire.Reader(body.subarray(2));
            if (field.type === 13) { reader.u8(); field.value = [reader.get(1), reader.get(1)]; }
            else field.value = reader.get(field.type);
            dirty = true;
            content = valueBytes(field);
          } else if (command === 6) { saved = fields.map(f => structuredClone(f.value)); dirty = false; }
          else if (command === 7) { fields.forEach((f, i) => { f.value = structuredClone(saved[i]); }); dirty = false; }
          else if (command === 8) { fields.filter(f => !id || f.group === id).forEach(f => { f.value = structuredClone(f.default); }); dirty = true; }
          else status = 3;
          const reply = Uint8Array.from([0xc2, 1, raw[2], command, status, dirty ? 2 : 0, ...content, 0, 0]);
          new DataView(reply.buffer).setUint16(reply.length - 2, ConfigWire.crc16(reply.subarray(0, -2)), true);
          controller.enqueue(Uint8Array.from([0, ...ConfigWire.encodeFrame(reply)]));
        }});
        controller.enqueue(new TextEncoder().encode('1000,100,200,300,400,1000,370,0,0,0,0,0,0,1,DISARM\n'));
      },
      async close() {}
    };
    Object.defineProperty(navigator, 'serial', {configurable: true, value: {requestPort: async () => port}});
  });
};
