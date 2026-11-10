// HC-06 is Bluetooth Classic SPP (serial), not a BLE GATT device.
const telemetryFields = 'time_ms F B L R total userWeight speed turn leftTargetRPM rightTargetRPM leftSmoothedRPM rightSmoothedRPM stopped'.split(' ');

function parseBluetoothLine(line) {
  const parts = line.trim().split(',').map(value => value.trim());
  if (![12, 14, 15].includes(parts.length)) return null;
  const numeric = parts.length === 15 ? parts.slice(0, -1) : parts;
  if (numeric.some(value => value === '' || !Number.isFinite(Number(value)))) return null;
  const fields = numeric.length === 14 ? telemetryFields : [...telemetryFields.slice(0, 11), 'stopped'];
  const row = Object.fromEntries(fields.map((field, i) => [field, Number(numeric[i])]));
  row.mode = parts.length === 15 && ['LEAN', 'REMOTE'].includes(parts[14]) ? parts[14] : 'UNKNOWN';
  return row;
}

let bluetoothPort = null, bluetoothReader = null, bluetoothReadTask = null;
let bluetoothRows = [], bluetoothLastRowAt = 0, bluetoothTimer = null;
let connectionBusy = false;

function syncUsbMonitoring() {
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
  applyTelemetry(bluetoothRows, !bluetoothRows.length || performance.now() - bluetoothLastRowAt > 2000, 'Bluetooth');
}

async function readBluetooth(port) {
  const decoder = new TextDecoder();
  let buffer = '';
  try {
    bluetoothReader = port.readable.getReader();
    while (bluetoothActive) {
      const {value, done} = await bluetoothReader.read();
      if (done) break;
      buffer += decoder.decode(value, {stream: true});
      let newline;
      while ((newline = buffer.indexOf('\n')) !== -1) {
        const line = buffer.slice(0, newline).replace(/\r$/, '');
        buffer = buffer.slice(newline + 1);
        const row = parseBluetoothLine(line);
        if (row) {
          if (bluetoothRows.length && row.time_ms < bluetoothRows.at(-1).time_ms) bluetoothRows = [];
          bluetoothRows.push(row);
          bluetoothLastRowAt = performance.now();
          while (bluetoothRows.length > 13000 || row.time_ms - bluetoothRows[0].time_ms > 60000) bluetoothRows.shift();
        } else if (line.trim() && !line.startsWith('time_ms')) appendConsole(line);
      }
      // A damaged/mid-stream line must not grow the buffer indefinitely.
      if (buffer.length > 4096) buffer = '';
    }
  } catch (error) {
    if (bluetoothActive) appendConsole(`Bluetooth read failed: ${error.message}`);
  } finally {
    bluetoothReader?.releaseLock();
    bluetoothReader = null;
    clearInterval(bluetoothTimer);
    try { await port.close(); } catch (error) { appendConsole(`Bluetooth close: ${error.message}`); }
    bluetoothPort = null;
    bluetoothActive = false;
    updateConnectionButtons();
    showDisconnected('Bluetooth disconnected');
    appendConsole('Bluetooth disconnected. Choose a connection mode and click Connect to reconnect.');
  }
}

async function connectBluetooth() {
  if (!window.isSecureContext || !navigator.serial) {
    appendConsole('Bluetooth serial requires Chrome on Linux with Web Serial enabled. Open this dashboard at http://localhost:8000 or HTTPS, and pair the HC-06 in Linux Bluetooth settings first.');
    return;
  }
  connectionBusy = true;
  updateConnectionButtons();
  let port;
  try {
    // Standard Serial Port Profile services are allowed by default.
    port = await navigator.serial.requestPort({filters: [{bluetoothServiceClassId: 0x1101}]});
    await port.open({baudRate: 115200, bufferSize: 4096});
    bluetoothPort = port;
    bluetoothActive = true;
    bluetoothRows = [];
    bluetoothLastRowAt = 0;
    // Do not overlay old USB data on the newly selected wireless source.
    displayed = [];
    graphs();
    renderBluetooth();
    appendConsole('Bluetooth connected. Select LEAN on the wheelchair to receive telemetry.');
    bluetoothTimer = setInterval(renderBluetooth, 100);
    bluetoothReadTask = readBluetooth(port);
  } catch (error) {
    if (error.name !== 'NotFoundError') appendConsole(`Bluetooth connection failed: ${error.message}`);
    if (port && !bluetoothActive) {
      appendConsole('Selecting a receiver only grants browser access. In Linux Bluetooth settings, finish pairing the receiver first; then close other serial apps and disconnect the handheld controller before retrying Connect.');
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
    await disconnectBluetooth();
    connectionMode = $('#connection-mode').value;
    // A new source starts disconnected and never shows the old source's data.
    latest = []; displayed = []; lastRow = null;
    graphs(); sensors(null);
    $('pre').textContent = '';
    showDisconnected(`${connectionMode === 'bluetooth' ? 'Bluetooth' : 'USB / replay'} disconnected · click Connect`);
  } finally {
    connectionBusy = false;
    updateConnectionButtons();
  }
}

async function connectSelected() {
  if (connectionMode === 'bluetooth') return connectBluetooth();
  usbConnected = true;
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
updateConnectionButtons();
syncUsbMonitoring();
