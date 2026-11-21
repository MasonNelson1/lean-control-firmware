// HC-06 is Bluetooth Classic SPP (serial), not a BLE GATT device.
const telemetryFields = 'time_ms F B L R total userWeight speed turn leftTargetRPM rightTargetRPM leftSmoothedRPM rightSmoothedRPM stopped leftActualRPM rightActualRPM leftBoostCurrent rightBoostCurrent'.split(' ');

function parseDynamicGains(line) {
  const match = line.trim().match(/^DYNAMIC_GAINS F=(\d+(?:\.\d+)?) B=(\d+(?:\.\d+)?) L=(\d+(?:\.\d+)?) R=(\d+(?:\.\d+)?)$/);
  if (!match) return null;
  const values = match.slice(1).map(Number);
  if (!values.every(value => Number.isFinite(value) && value > 0)) return null;
  return Object.fromEntries(['F', 'B', 'L', 'R'].map((key, index) => [key, values[index]]));
}

// Gain status belongs to the connection, independently of CSV history.
function setDynamicGains(gains) {
  dynamicGains = gains;
  sensors(lastRow);
}

function parseBluetoothLine(line) {
  const parts = line.trim().split(',').map(value => value.trim());
  if (![12, 14, 15, 19].includes(parts.length)) return null;
  const hasMode = parts.length === 15 || parts.length === 19;
  const numeric = hasMode ? parts.slice(0, -1) : parts;
  const fields = numeric.length === 12 ? [...telemetryFields.slice(0, 11), 'stopped'] : telemetryFields;
  const row = {};
  for (const [index, value] of numeric.entries()) {
    // Nonfinite float samples are unavailable, never zero. Keep the other
    // fields (especially seat sensors and mode) usable when one calculation fails.
    const optional = parts.length === 19 && index >= 7 && index <= 17 && index !== 13;
    if (optional && /^[+-]?(?:nan|inf(?:inity)?)$/i.test(value)) continue;
    if (!value || !Number.isFinite(Number(value))) return null;
    row[fields[index]] = Number(value);
  }
  const mode = hasMode ? parts.at(-1) : null;
  row.mode = ['LEAN', 'REMOTE', 'DISARM', 'ASSIST'].includes(mode) ? mode : 'UNKNOWN';
  return row;
}

// Recover only fully validated current rows with a recognizable timestamp
// boundary. Never guess missing bytes or splice numeric fields across samples.
function recoverTelemetryRows(line) {
  const number = '[+-]?(?:\\d+(?:\\.\\d*)?(?:[eE][+-]?\\d+)?|nan|inf(?:inity)?)';
  const pattern = new RegExp(`(?:^|(?<=[A-Za-z]))\\d+,(?:${number},){17}(?:LEAN|REMOTE|DISARM|ASSIST)(?=$|\\d)`, 'gi');
  return [...line.matchAll(pattern)].map(match => match[0]).filter(candidate => {
    const row = parseBluetoothLine(candidate);
    return row && row.time_ms <= 0xffffffff;
  });
}

// Legacy telemetry is newline-delimited. Configuration replies are framed
// with a zero on each side of their COBS payload.
class BluetoothReceiver {
  constructor(onLine, onFrame, validFrame = () => true) {
    this.onLine = onLine;
    this.onFrame = onFrame;
    this.validFrame = validFrame;
    this.decoder = new TextDecoder();
    this.line = [];
    this.frame = null;
  }

  push(bytes) {
    for (const byte of bytes) {
      if (byte === 0) {
        if (this.frame === null) {
          this.line = [];
          this.frame = [];
        } else {
          const candidate = Uint8Array.from(this.frame);
          if (candidate.length && candidate.length <= 402 && this.validFrame(candidate)) {
            this.onFrame(candidate);
            this.frame = null;
          } else {
            // A delimiter may be the opening boundary of the next packet if
            // the previous boundary was lost. Keep searching at every zero
            // until a complete checksum-validated response restores alignment.
            this.frame = [];
          }
        }
      } else if (this.frame !== null) {
        if (this.frame.length <= 402) this.frame.push(byte);
      } else if (byte === 10) {
        if (this.line.length <= 4096) this.onLine(this.decoder.decode(Uint8Array.from(this.line)).replace(/\r$/, ''));
        this.line = [];
      } else if (this.line.length <= 4096) {
        this.line.push(byte);
      }
    }
  }
}

let bluetoothPort = null, bluetoothReader = null, bluetoothReadTask = null;
let bluetoothRows = [], bluetoothLastRowAt = 0, bluetoothTimer = null;
let connectionBusy = false;

function syncUsbMonitoring() {
  if (!dashboardBackend) return;
  if (ws?.readyState === 1) ws.send(connectionMode === 'usb' && usbConnected ? 'connect' : 'disconnect');
}

function updateConnectionButtons() {
  const connected = connectionMode === 'bluetooth' ? bluetoothActive : usbConnected;
  $('#connection-mode').disabled = connectionBusy;
  $('#connection-connect').disabled = connectionBusy || connected;
  $('#connection-disconnect').disabled = connectionBusy || !connected;
}

function showDisconnected(message) {
  showMode(lastRow, true);
  $('#status').textContent = message;
  $('#status').classList.add('off');
}

function renderBluetooth() {
  const stale = !bluetoothRows.length || performance.now() - bluetoothLastRowAt > 2000;
  window.configurationModeChanged?.(stale ? null : bluetoothRows.at(-1).mode);
  if (connectionMode === 'bluetooth') applyTelemetry(bluetoothRows, stale, 'Bluetooth');
}

async function readBluetooth(port) {
  let lastTelemetryWarning = -Infinity, suppressedTelemetryRows = 0;
  const warnTelemetry = detail => {
    suppressedTelemetryRows++;
    const now = performance.now();
    if (now - lastTelemetryWarning < 5000) return;
    appendConsole(`Telemetry warning (${suppressedTelemetryRows} affected rows since last warning): ${detail}`);
    lastTelemetryWarning = now;
    suppressedTelemetryRows = 0;
  };
  const receiver = new BluetoothReceiver(line => {
    const gains = parseDynamicGains(line);
    if (gains) {
      if (connectionMode === 'bluetooth') setDynamicGains(gains);
      return;
    }
    const row = parseBluetoothLine(line);
    if (row) {
      const missing = ['speed', 'turn'].filter(field => !Number.isFinite(row[field]));
      if (missing.length) warnTelemetry(`unavailable ${missing.join(', ')}; showing remaining fields`);
      if (bluetoothRows.length && row.time_ms < bluetoothRows.at(-1).time_ms) bluetoothRows = [];
      bluetoothRows.push(row);
      bluetoothLastRowAt = performance.now();
      while (bluetoothRows.length > 13000 || row.time_ms - bluetoothRows[0].time_ms > 60000) bluetoothRows.shift();
    } else if (line.trim() && !line.trimStart().startsWith('time_ms')) {
      // CSV rejected by the parser is telemetry, not a human status message.
      // Retain one example for diagnosis without printing 20 rows per second.
      if (/^\s*\d+\s*,/.test(line)) {
        const recovered = recoverTelemetryRows(line);
        warnTelemetry(`${recovered.length ? `recovered ${recovered.length} complete rows from damaged CSV` : 'rejected CSV'} (${line.split(',').length} columns): ${line.slice(0, 256)}`);
        for (const candidate of recovered) receiver.onLine(candidate);
      }
      else appendConsole(line);
    }
  }, frame => window.configLink?.receive(frame),
    frame => typeof ConfigWire === 'undefined' || ConfigWire.responseFrame(frame) !== null);
  try {
    while (bluetoothActive && port.readable) {
      const stream = port.readable;
      const reader = stream.getReader();
      bluetoothReader = reader;
      try {
        while (bluetoothActive) {
          const {value, done} = await reader.read();
          if (done) return;
          receiver.push(value);
        }
      } catch (error) {
        if (bluetoothActive) appendConsole(`Bluetooth read interrupted: ${error.message}`);
      } finally {
        reader.releaseLock();
        bluetoothReader = null;
      }
      if (bluetoothActive && port.readable === stream) {
        await new Promise(resolve => setTimeout(resolve, 100));
      }
    }
  } finally {
    clearInterval(bluetoothTimer);
    await window.configurationConnectionChanged?.(null);
    try { await port.close(); } catch (error) { appendConsole(`Bluetooth close: ${error.message}`); }
    bluetoothPort = null;
    bluetoothActive = false;
    if (connectionMode === 'bluetooth') setDynamicGains(null);
    updateConnectionButtons();
    if (connectionMode === 'bluetooth') showDisconnected('Bluetooth disconnected');
    appendConsole('Bluetooth disconnected. Choose a connection mode and click Connect to reconnect.');
  }
}

async function connectBluetooth() {
  if (!window.isSecureContext || !navigator.serial) {
    appendConsole('Bluetooth serial requires desktop Chrome with Web Serial enabled. Open this dashboard at http://localhost:8000 or HTTPS, and pair the HC-06 in your system Bluetooth settings first.');
    return;
  }
  connectionBusy = true;
  updateConnectionButtons();
  let port;
  try {
    // Standard Serial Port Profile services are allowed by default.
    port = await navigator.serial.requestPort({filters: [{bluetoothServiceClassId: 0x1101}]});
    // Leave room for short rendering pauses on phones without overrunning reads.
    await port.open({baudRate: 115200, bufferSize: 65536});
    bluetoothPort = port;
    bluetoothActive = true;
    bluetoothRows = [];
    bluetoothLastRowAt = 0;
    setDynamicGains(null);
    // Do not overlay old USB data on the newly selected wireless source.
    displayed = [];
    graphs();
    renderBluetooth();
    appendConsole('Bluetooth connected. Live sensor telemetry is available in every mode.');
    bluetoothTimer = setInterval(renderBluetooth, 100);
    bluetoothReadTask = readBluetooth(port);
    window.configurationConnectionChanged?.(port);
  } catch (error) {
    if (error.name !== 'NotFoundError') appendConsole(`Bluetooth connection failed: ${error.message}`);
    if (port && !bluetoothActive) {
      appendConsole('In your system Bluetooth settings, finish pairing the receiver. Close other serial apps and disconnect the handheld controller before retrying Connect.');
    }
    showDisconnected(error.name === 'NotFoundError' ? 'Bluetooth disconnected' : `Bluetooth connection failed: ${error.message}`);
  } finally {
    connectionBusy = false;
    updateConnectionButtons();
  }
}

async function disconnectBluetooth() {
  if (!bluetoothPort) return;
  bluetoothActive = false;
  await bluetoothReader?.cancel();
  await bluetoothReadTask;
}

async function changeConnectionMode() {
  connectionBusy = true;
  updateConnectionButtons();
  usbConnected = false;
  syncUsbMonitoring();
  try {
    connectionMode = dashboardBackend ? $('#connection-mode').value : 'bluetooth';
    // A new source starts disconnected and never shows the old source's data.
    latest = []; displayed = []; lastRow = null;
    setDynamicGains(null);
    graphs(); sensors(null);
    $('pre').textContent = '';
    if (connectionMode === 'bluetooth' && bluetoothActive) renderBluetooth();
    else showDisconnected(`${connectionMode === 'bluetooth' ? 'Bluetooth' : 'USB / replay'} disconnected · click Connect`);
  } finally {
    connectionBusy = false;
    updateConnectionButtons();
  }
}

async function connectSelected() {
  if (!dashboardBackend || connectionMode === 'bluetooth') return connectBluetooth();
  usbConnected = true;
  setDynamicGains(null);
  syncUsbMonitoring();
  showDisconnected('USB / replay · waiting for telemetry');
  updateConnectionButtons();
}

async function disconnectSelected() {
  connectionBusy = true;
  updateConnectionButtons();
  try {
    if (connectionMode === 'bluetooth') await disconnectBluetooth();
    else {
      usbConnected = false;
      setDynamicGains(null);
      syncUsbMonitoring();
      showDisconnected('USB / replay disconnected');
    }
  } finally {
    connectionBusy = false;
    updateConnectionButtons();
  }
}

$('#connection-mode').onchange = changeConnectionMode;
$('#connection-connect').onclick = connectSelected;
$('#connection-disconnect').onclick = disconnectSelected;
if (!dashboardBackend) {
  $('#connection-mode').querySelector('option[value="usb"]').remove();
  $('#connection-mode').value = 'bluetooth';
  showDisconnected('Bluetooth disconnected');
}
updateConnectionButtons();
syncUsbMonitoring();
