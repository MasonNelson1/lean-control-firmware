'use strict';

/**
 * Builds the existing dashboard's configuration pages from the device schema.
 * groups/fields describe the wheelchair, drafts hold unapplied browser edits,
 * and link.dirty reports applied RAM changes awaiting an EEPROM save.
 */
(() => {
  const {commands, types, editors} = ConfigWire;
  const link = new ConfigWire.Link();
  window.configLink = link; // BluetoothReceiver delivers decoded configuration frames here.
  let groups = new Map(), fields = new Map(), drafts = new Map();
  const pidPlots = new Map();
  let page = 'telemetry', loaded = false, busy = false;

  const byId = id => document.getElementById(id);
  const element = (tag, className, text) => {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  };
  const button = (label, action) => {
    const node = element('button', '', label);
    node.type = 'button';
    node.onclick = action;
    return node;
  };

  function status(message, error = false) {
    const node = byId('config-status');
    node.textContent = message;
    node.dataset.error = error;
  }

  function updateToolbar() {
    const editable = loaded && !busy;
    const hasDrafts = drafts.size > 0;
    byId('config-reload').disabled = !link.port || busy;
    byId('config-apply').disabled = !editable || !hasDrafts;
    byId('config-save').disabled = !editable || !(link.dirty || hasDrafts);
    byId('config-revert').disabled = busy || !(hasDrafts || (editable && link.dirty));
    byId('config-defaults').disabled = !editable;
    byId('config-summary').textContent = loaded
      ? [
          hasDrafts ? `${drafts.size} unapplied ${drafts.size === 1 ? 'edit' : 'edits'}` : '',
          link.dirty ? 'Wheelchair has unsaved changes' : 'Wheelchair settings match saved values'
        ].filter(Boolean).join('  |  ')
      : '';
  }

  // Keep operations sequential and prevent an old connection's completion from
  // replacing the status or controls belonging to a newly connected device.
  async function run(action) {
    if (busy || !link.port) return;
    const generation = link.generation;
    busy = true;
    render();
    try {
      await action();
    } catch (error) {
      if (generation === link.generation) status(error.message, true);
    } finally {
      if (generation === link.generation) {
        busy = false;
        render();
      }
    }
  }

  /**
   * Reads counts, parent-first groups, then field descriptions and current values.
   * Replaces the visible schema after discovery completes. Compatible local
   * drafts survive a reload so refreshing metadata does not lose those edits.
   */
  async function discover() {
    await run(async () => {
      status('Reading configuration from the wheelchair…');
      const info = await link.request(commands.hello);
      const groupCount = info.u16(), fieldCount = info.u16();
      info.done();
      const nextGroups = new Map(), nextFields = new Map();
      for (let index = 0; index < groupCount; index++) {
        const group = ConfigWire.readGroup(await link.request(commands.group, ConfigWire.idBody(index)));
        if (nextGroups.has(group.id) || (group.parent && !nextGroups.has(group.parent)))
          throw new Error('The wheelchair returned an invalid group hierarchy.');
        nextGroups.set(group.id, group);
      }
      for (let index = 0; index < fieldCount; index++) {
        const field = ConfigWire.readField(await link.request(commands.describe, ConfigWire.idBody(index)));
        if (nextFields.has(field.id) || !nextGroups.has(field.group))
          throw new Error('The wheelchair returned an invalid field list.');
        nextFields.set(field.id, field);
      }
      // Optional graph metadata keeps PID bindings independent of field names
      // and IDs. Older firmware replies Unknown and still supplies its forms.
      for (const group of nextGroups.values()) {
        try {
          group.preview = ConfigWire.readPreview(await link.request(commands.preview, ConfigWire.idBody(group.id)));
        } catch (error) {
          if (error.status !== 3) throw error;
        }
      }
      const compatibleDrafts = new Map();
      for (const [id, raw] of drafts) {
        const old = fields.get(id), next = nextFields.get(id);
        if (!next || next.unsupported || old.type !== next.type) continue;
        if (old.editor !== next.editor || old.parameters?.length !== next.parameters?.length) continue;
        compatibleDrafts.set(id, raw);
      }
      groups = nextGroups;
      fields = nextFields;
      drafts = compatibleDrafts;
      loaded = true;
      status('Stop the chair before changing settings. Apply updates RAM, and Save keeps the settings after power-off.');
    });
  }

  // Defaults and Revert can affect several fields, including hidden sections.
  async function refreshValues() {
    for (const field of fields.values()) {
      if (field.unsupported) continue;
      const reply = await link.request(commands.read, ConfigWire.idBody(field.id));
      field.value = ConfigWire.readValue(field, reply);
      reply.done();
    }
  }

  // Display a short decimal that round-trips to the original float32. This
  // avoids creating a draft merely by displaying an existing firmware value.
  function formatNumber(value, type) {
    if (type === types.f32 && Number.isFinite(value)) {
      for (let digits = 1; digits <= 9; digits++) {
        const candidate = Number(value.toPrecision(digits));
        if (Math.fround(candidate) === value) return String(candidate);
      }
    }
    return String(value);
  }

  function numberInput(raw, type, min, max) {
    raw = raw.trim();
    if (raw === '') throw new Error('Enter a value.');
    let value;
    if (type === types.i64 || type === types.u64) {
      if (!/^-?\d+$/.test(raw)) throw new Error('Enter a whole number.');
      value = BigInt(raw);
    } else {
      value = Number(raw);
      if (type === types.f32) value = Math.fround(value);
      if (!Number.isFinite(value)) throw new Error('Enter a finite number.');
      if (type !== types.f32 && type !== types.f64 && !Number.isInteger(value))
        throw new Error('Enter a whole number.');
    }
    if (value < min || value > max)
      throw new Error(`Enter a value from ${formatNumber(min, type)} to ${formatNumber(max, type)}.`);
    return value;
  }

  // Browser validation gives prompt feedback. Firmware validation is authoritative
  // and also enforces relationships between separate configuration fields.
  function parseDraft(field, raw) {
    if (field.type === types.boolean) return raw;
    if (field.type === types.enum) {
      const value = Number(raw);
      if (!field.choices.some(choice => choice.value === value)) throw new Error('Select an available choice.');
      return value;
    }
    if (field.type !== types.profile) return numberInput(raw, field.type, field.min, field.max);

    const values = raw.map((part, index) => {
      const parameter = field.parameters[index];
      return numberInput(part, types.f32, parameter.min, parameter.max);
    });
    if (field.editor === editors.piecewiseLinear) {
      for (let i = 2; i < values.length; i += 2) {
        if (values[i] <= values[i - 2]) throw new Error('Profile inputs must increase from left to right.');
      }
    }
    return values;
  }

  function equalValue(first, second) {
    return Array.isArray(first) ? first.every((part, index) => part === second[index]) : first === second;
  }

  // Keep raw input so incomplete or invalid edits survive a rerender. Remove
  // edits that resolve to the current device value from the unapplied set.
  function setDraft(field, raw, inputs, message) {
    let value;
    let error = '';
    try {
      value = parseDraft(field, raw);
      if (equalValue(value, field.value)) drafts.delete(field.id);
      else drafts.set(field.id, raw);
    } catch (failure) {
      error = failure.message;
      drafts.set(field.id, raw);
    }
    for (const input of inputs) input.setAttribute('aria-invalid', String(Boolean(error)));
    message.textContent = error;
    updateToolbar();
    updatePidPlots();
    return value;
  }

  async function applyDrafts() {
    // Validate the entire draft before sending the first write.
    const updates = [...drafts].map(([id, raw]) => {
      const field = fields.get(id);
      try {
        return [field, parseDraft(field, raw)];
      } catch (error) {
        throw new Error(`${field.name}: ${error.message}`);
      }
    });
    // Each accepted field is a separate RAM update. If a later request fails,
    // retain the remaining drafts and keep earlier acknowledgments visible.
    for (const [field, value] of updates) {
      try {
        status(`Applying ${field.name}…`);
        const reply = await link.request(commands.set, ConfigWire.setBody(field, value));
        field.value = ConfigWire.readValue(field, reply);
        reply.done();
        drafts.delete(field.id);
      } catch (error) {
        throw new Error(`${field.name}: ${error.message} Earlier acknowledged changes remain in RAM.`);
      }
    }
  }

  function groupWithin(id, ancestor) {
    if (!ancestor) return true;
    for (let group = groups.get(id); group; group = groups.get(group.parent)) {
      if (group.id === ancestor) return true;
    }
    return false;
  }

  async function restoreDefaults(id = 0) {
    const name = id ? groups.get(id).name : 'all settings';
    if (!window.confirm('Restore firmware defaults for ' + name + '?')) return;
    await run(async () => {
      status('Restoring defaults…');
      const reply = await link.request(commands.defaults, ConfigWire.idBody(id));
      reply.done();
      for (const field of fields.values()) if (groupWithin(field.group, id)) drafts.delete(field.id);
      await refreshValues();
      status('Defaults applied to RAM. Save to keep them.');
    });
  }

  function numericControl(type, min, max, value) {
    const input = element('input');
    const wide = type === types.i64 || type === types.u64;
    input.type = wide ? 'text' : 'number';
    if (wide) input.inputMode = 'numeric';
    else {
      input.min = formatNumber(min, type);
      input.max = formatNumber(max, type);
      input.step = type === types.f32 || type === types.f64 ? 'any' : '1';
    }
    setInputValue(input, value, type);
    return input;
  }

  // Keep the exact value behind the rounded display until the user edits it.
  function setInputValue(input, value, type) {
    input.dataset.precise = String(value);
    input.value = type === types.f32 || type === types.f64
      ? String(Number(Number(value).toFixed(2))) : String(value);
    input.dataset.display = input.value;
  }

  function inputValue(input) {
    return input.value === input.dataset.display ? input.dataset.precise : input.value;
  }

  function resetActions(name, current, defaults, disabled) {
    const actions = element('div', 'value-actions');
    const reset = button('↶', current);
    reset.title = 'Reset to the current wheelchair value';
    reset.setAttribute('aria-label', 'Reset ' + name + ' to current value');
    const factory = button('Default', () => {
      if (window.confirm('Reset ' + name + ' to its firmware default?')) defaults();
    });
    factory.title = 'Reset to the firmware default';
    reset.disabled = factory.disabled = disabled;
    actions.append(reset, factory);
    return actions;
  }

  function numberControls(type, min, max, value, disabled, onChange, options = {}) {
    const container = element('div', 'number-controls');
    if (options.slider === false) container.classList.add('no-slider');
    const input = numericControl(type, min, max, value);
    const slider = element('input', 'config-slider');
    slider.type = 'range';
    const wide = type === types.i64 || type === types.u64;
    slider.min = wide ? '0' : formatNumber(min, type);
    const sliderMax = options.sliderMax ?? max;
    slider.max = wide ? '1000' : formatNumber(sliderMax, type);
    slider.step = wide || (type !== types.f32 && type !== types.f64) ? '1' : 'any';
    slider.disabled = disabled || min === max;
    input.disabled = disabled;
    const limits = element('div', 'input-limits');
    limits.append(element('span', '', 'Min ' + formatNumber(min, type)),
                  element('span', '', 'Max ' + formatNumber(sliderMax, type)));
    if (sliderMax < max) limits.append(element('span', '', 'Box max ' + formatNumber(max, type)));
    const sync = () => {
      try {
        const value = numberInput(inputValue(input), type, min, max);
        slider.value = wide
          ? String(max === min ? 0 : Number((value - min) * 1000n / (max - min)))
          : String(value);
      } catch {
        // Keep the slider at its last valid value during an incomplete edit.
      }
    };
    input.addEventListener('input', () => {
      input.dataset.precise = input.dataset.display = input.value;
      sync();
      onChange();
    });
    input.addEventListener('blur', () => {
      try { setInputValue(input, numberInput(inputValue(input), type, min, max), type); } catch {}
    });
    slider.addEventListener('input', () => {
      const value = wide
        ? min + (max - min) * BigInt(slider.value) / 1000n
        : type === types.f32 ? Math.fround(Number(slider.value)) : Number(slider.value);
      setInputValue(input, value, type);
      onChange();
    });
    const setValue = value => { setInputValue(input, value, type); sync(); };
    const restore = value => { setValue(value); onChange(); };
    const actions = resetActions(options.name || 'value',
      () => restore(options.current), () => restore(options.default), disabled);
    sync();
    if (options.slider !== false) container.append(slider);
    container.append(input, actions, limits);
    return {container, input, slider, sync, setValue};
  }

  function renderScalar(field, card, label, error, disabled) {
    const raw = drafts.get(field.id);
    let input;
    let slider;
    let container;
    const change = () => setDraft(field, field.type === types.boolean ? input.checked
                                    : field.type === types.enum ? input.value : inputValue(input),
                                  slider ? [input, slider] : [input], error);
    if (field.type === types.boolean) {
      input = element('input');
      input.type = 'checkbox';
      input.checked = raw ?? field.value;
    } else if (field.type === types.enum) {
      input = element('select');
      for (const choice of field.choices) {
        const option = element('option', '', choice.name);
        option.value = choice.value;
        input.append(option);
      }
      input.value = raw ?? field.value;
    } else {
      const controls = numberControls(field.type, field.min, field.max,
        raw ?? formatNumber(field.value, field.type), disabled, change, {
          name: field.name, current: field.value, default: field.default,
          slider: !/VESC.*ID|wheel circumference/i.test(field.name),
          sliderMax: /sensor gains/i.test(groups.get(field.group)?.name || '') ? Math.min(5, field.max) : field.max
        });
      ({input, slider, container} = controls);
      slider.setAttribute('aria-label', field.name);
    }
    input.id = 'field-' + field.id;
    label.htmlFor = input.id;
    input.disabled = disabled;
    if (!slider) {
      input.addEventListener('input', change);
      container = element('div', 'scalar-value');
      const restore = value => {
        if (field.type === types.boolean) input.checked = value;
        else input.value = value;
        change();
      };
      container.append(input, resetActions(field.name, () => restore(field.value), () => restore(field.default), disabled));
    }
    if (raw !== undefined) change();
    card.append(container || input);
  }

  // Signed power uses the full normalized -1..+1 domain. Piecewise profiles
  // derive their displayed axes from the firmware's coordinate constraints.
  function graphScale(canvas, field) {
    const pad = 30;
    const power = field.editor === editors.signedPower;
    const xs = field.parameters.filter((_, i) => i % 2 === 0);
    const ys = field.parameters.filter((_, i) => i % 2 === 1);
    const xMin = power ? 0 : Math.min(...xs.map(parameter => parameter.min));
    const yMin = power ? 0 : Math.min(...ys.map(parameter => parameter.min));
    const xMax = power ? 1 : Math.max(...xs.map(parameter => parameter.max), xMin + 0.00001);
    const yMax = power ? 1 : Math.max(...ys.map(parameter => parameter.max), yMin + 0.00001);
    const width = canvas.width - 2 * pad, height = canvas.height - 2 * pad;
    return {
      pad, xMin, xMax, yMin, yMax,
      x: value => pad + (value - xMin) * width / (xMax - xMin),
      y: value => pad + (yMax - value) * height / (yMax - yMin),
      xValue: px => xMin + (px - pad) * (xMax - xMin) / width,
      yValue: py => yMax - (py - pad) * (yMax - yMin) / height
    };
  }

  function drawProfile(canvas, field, values, scale) {
    const {pad, x, y, xMin, xMax, yMin, yMax} = scale;
    const g = canvas.getContext('2d');
    g.clearRect(0, 0, canvas.width, canvas.height);
    g.fillStyle = '#f5f9fc';
    g.fillRect(x(xMin), y(yMax), x(xMax) - x(xMin), y(yMin) - y(yMax));
    g.strokeStyle = '#b8c9d8';
    g.lineWidth = 1;
    g.strokeRect(x(xMin), y(yMax), x(xMax) - x(xMin), y(yMin) - y(yMax));
    g.beginPath();
    if (yMin <= 0 && yMax >= 0) { g.moveTo(x(xMin), y(0)); g.lineTo(x(xMax), y(0)); }
    if (xMin <= 0 && xMax >= 0) { g.moveTo(x(0), y(yMin)); g.lineTo(x(0), y(yMax)); }
    g.stroke();
    g.fillStyle = '#536378';
    g.font = '12px system-ui';
    g.textAlign = 'center';
    g.fillText(formatNumber(xMin, types.f32), x(xMin), canvas.height - 12);
    g.fillText(formatNumber(xMax, types.f32), x(xMax), canvas.height - 12);
    g.fillText('Input', canvas.width / 2, canvas.height - 12);
    g.save();
    g.translate(12, canvas.height / 2);
    g.rotate(-Math.PI / 2);
    g.fillText('Output', 0, 0);
    g.restore();
    g.textAlign = 'right';
    g.fillText(formatNumber(yMin, types.f32), pad - 5, y(yMin));
    g.fillText(formatNumber(yMax, types.f32), pad - 5, y(yMax) + 4);

    g.strokeStyle = '#287d9c';
    g.lineWidth = 2.5;
    g.beginPath();
    if (field.editor === editors.signedPower) {
      const exponent = Math.log(values[1]) / Math.log(values[0]);
      g.moveTo(x(0), y(0));
      for (let step = 1; step <= 120; step++) {
        const input = step / 120;
        const output = Math.sign(input) * Math.pow(Math.abs(input), exponent);
        g.lineTo(x(input), y(output));
      }
    } else {
      g.moveTo(x(values[0]), y(values[1]));
      for (let i = 2; i < values.length; i += 2) g.lineTo(x(values[i]), y(values[i + 1]));
    }
    g.stroke();
    g.fillStyle = '#146db0';
    for (let i = 0; i < values.length; i += 2) {
      g.beginPath();
      g.arc(x(values[i]), y(values[i + 1]), 6, 0, 2 * Math.PI);
      g.fill();
    }
  }

  /**
   * Reusable editor for every field declaring a supported profile model.
   * Dragging and numerical edits update the local draft and preview immediately.
   * Apply or Save sends that draft to the wheelchair through the normal protocol.
   */
  function renderProfile(field, card, error, disabled) {
    const canvas = element('canvas', 'profile-plot');
    canvas.width = 440;
    canvas.height = 250;
    canvas.setAttribute('aria-label', field.name + ' input/output graph');
    const scale = graphScale(canvas, field);
    const controls = element('div', 'profile-inputs');
    const raw = drafts.get(field.id) ?? field.value.map(value => formatNumber(value, types.f32));
    const numeric = field.parameters.map((parameter, index) => {
      const label = element('label', '', parameter.name);
      const number = numberControls(types.f32, parameter.min, parameter.max, raw[index], disabled, () => change(), {
        name: field.name + ' ' + parameter.name, current: field.value[index], default: field.default[index]
      });
      number.input.id = 'field-' + field.id + '-' + index;
      label.htmlFor = number.input.id;
      number.slider.setAttribute('aria-label', field.name + ' ' + parameter.name);
      label.append(number.container);
      controls.append(label);
      return number;
    });
    const inputs = numeric.map(number => number.input);
    let plotted = field.value;
    const change = () => {
      const values = setDraft(field, inputs.map(inputValue), inputs, error);
      if (values) plotted = values;
      numeric.forEach(number => number.sync());
      drawProfile(canvas, field, plotted, scale);
    };
    if (!disabled) {
      let drag = null;
      const position = event => {
        const rect = canvas.getBoundingClientRect();
        return [(event.clientX - rect.left) * canvas.width / rect.width,
                (event.clientY - rect.top) * canvas.height / rect.height];
      };
      canvas.addEventListener('pointerdown', event => {
        if (drag || event.button !== 0) return;
        const [px, py] = position(event);
        for (let index = 0; index < plotted.length; index += 2) {
          if (Math.hypot(px - scale.x(plotted[index]), py - scale.y(plotted[index + 1])) > 18) continue;
          drag = {id: event.pointerId, index};
          canvas.setPointerCapture(event.pointerId);
          break;
        }
      });
      canvas.addEventListener('pointermove', event => {
        if (drag?.id !== event.pointerId) return;
        const [px, py] = position(event);
        const i = drag.index;
        const clamp = (value, lo, hi) => Math.max(lo, Math.min(hi, value));
        const input = field.parameters[i], output = field.parameters[i + 1];
        let x = clamp(scale.xValue(px), input.min, input.max);
        let y = clamp(scale.yValue(py), output.min, output.max);
        if (field.editor === editors.piecewiseLinear) {
          const lo = i > 0 ? Math.max(input.min, plotted[i - 2] + 0.00001) : input.min;
          const hi = i + 2 < plotted.length ? Math.min(input.max, plotted[i + 2] - 0.00001) : input.max;
          x = lo <= hi ? clamp(x, lo, hi) : plotted[i];
        }
        numeric[i].setValue(Math.fround(x));
        numeric[i + 1].setValue(Math.fround(y));
        change();
      });
      const endDrag = event => { if (drag?.id === event.pointerId) drag = null; };
      canvas.addEventListener('pointerup', endDrag);
      canvas.addEventListener('pointercancel', endDrag);
      canvas.addEventListener('lostpointercapture', endDrag);
    }
    card.append(canvas, controls);
    if (drafts.has(field.id)) change();
    else drawProfile(canvas, field, plotted, scale);
  }

  function renderField(field) {
    const card = element('article', field.type === types.profile ? 'config-field profile-field' : 'config-field');
    const title = element('div', 'config-field-title');
    const caption = field.name + (field.units ? ' (' + field.units + ')' : '');
    const name = element(field.type === types.profile || field.unsupported ? 'span' : 'label', '', caption);
    const info = element('div', 'field-help');
    const help = button('?', () => {});
    help.className = 'help-button';
    help.setAttribute('aria-label', 'About ' + field.name);
    const tooltip = element('div', 'field-tooltip', field.description);
    tooltip.id = 'field-help-' + field.id;
    tooltip.setAttribute('role', 'tooltip');
    help.setAttribute('aria-describedby', tooltip.id);
    help.addEventListener('keydown', event => { if (event.key === 'Escape') help.blur(); });
    if (field.restart) tooltip.append(element('p', '', 'Takes effect after saving and restarting.'));
    info.append(help, tooltip);
    title.append(name, info);
    card.append(title);
    if (field.unsupported) {
      card.append(element('p', 'config-field-note', 'This field type requires a newer dashboard.'));
      return card;
    }

    const disabled = busy;
    const error = element('p', 'config-field-error');
    if (field.type === types.profile) renderProfile(field, card, error, disabled);
    else renderScalar(field, card, name, error, disabled);
    card.append(error);
    return card;
  }

  function updatePidPlots() {
    for (const {descriptor, canvas, caption} of pidPlots.values()) {
      try {
        const values = descriptor.fieldIds.map(id => {
          const field = fields.get(id);
          if (!field || field.unsupported) throw new Error('This preview references an unavailable setting.');
          return drafts.has(id) ? parseDraft(field, drafts.get(id)) : field.value;
        });
        const response = PidPreview.simulate(values, descriptor.constants);
        PidPreview.draw(canvas, response);
        caption.textContent = `Simulated command smoothing · 0 → ${response.target.toFixed(1)} RPM · ${formatNumber(values[6], types.f32)} ms updates`;
      } catch (error) {
        canvas.getContext('2d').clearRect(0, 0, canvas.width, canvas.height);
        caption.textContent = error.message;
      }
    }
  }

  function renderPidPreview(group, section) {
    if (group.preview?.type !== 1 || group.preview.fieldIds.length !== 8 || group.preview.constants.length !== 2) return;
    const preview = element('div', 'pid-preview');
    const header = element('div', 'pid-preview-heading');
    header.append(element('h4', '', 'Step response'));
    const legend = element('div', 'pid-legend');
    legend.append(element('span', 'target-legend', 'Target'), element('span', 'response-legend', 'Smoothed RPM'));
    header.append(legend);
    const canvas = element('canvas', 'pid-step-plot');
    canvas.width = 720;
    canvas.height = 280;
    canvas.setAttribute('aria-label', group.name + ' simulated PID step response');
    const caption = element('p', 'config-field-note');
    preview.append(header, canvas, caption);
    section.append(preview);
    pidPlots.set(group.id, {descriptor: group.preview, canvas, caption});
  }

  // Follow parent IDs recursively so new nested groups need no frontend entries.
  function renderGroup(group) {
    const section = element('section', 'config-group');
    const header = element('div', 'config-group-head');
    const restore = button('Restore defaults', () => restoreDefaults(group.id));
    restore.title = 'Apply the firmware defaults to this group and its nested groups in RAM. Use Save to keep them after power-off.';
    restore.disabled = busy;
    restore.setAttribute('aria-label', 'Restore defaults for ' + group.name);
    header.append(element('h3', '', group.name), restore);
    section.append(header);

    const cards = element('div', 'config-fields');
    for (const field of fields.values()) {
      if (field.group === group.id) cards.append(renderField(field));
    }
    if (cards.childElementCount) section.append(cards);
    renderPidPreview(group, section);
    for (const child of groups.values()) {
      if (child.parent === group.id) section.append(renderGroup(child));
    }
    return section;
  }

  function render() {
    pidPlots.clear();
    const panel = byId('config-groups');
    panel.replaceChildren();
    if (loaded && page !== 'telemetry') {
      for (const group of groups.values()) {
        if (!group.parent && group.advanced === (page === 'advanced')) panel.append(renderGroup(group));
      }
      if (!panel.childElementCount) panel.append(element('p', 'config-field-note', 'No settings in this section.'));
    }
    updatePidPlots();
    updateToolbar();
  }

  function selectPage(next) {
    page = next;
    byId('telemetry-page').hidden = page !== 'telemetry';
    byId('config-page').hidden = page === 'telemetry';
    for (const item of document.querySelectorAll('[data-page]')) {
      if (item.dataset.page === page) item.setAttribute('aria-current', 'page');
      else item.removeAttribute('aria-current');
    }
    if (page === 'telemetry') {
      graphs();
      sensors(lastRow);
    } else {
      byId('config-heading').textContent = page === 'advanced' ? 'Advanced Configuration' : 'Configuration';
      if (link.port && !loaded && !busy) discover();
      render();
    }
  }

  // Called by bluetooth.js when its selected serial port connects or closes.
  // Discard device-specific schema and drafts before discovering the new source.
  window.configurationConnectionChanged = port => {
    const closing = link.attach(port);
    groups.clear();
    fields.clear();
    drafts.clear();
    loaded = false;
    busy = false;
    status(port ? 'Ready to discover settings.' : 'Connect to the wheelchair over Bluetooth to view settings.');
    if (port && page !== 'telemetry') discover();
    render();
    return closing;
  };

  for (const item of document.querySelectorAll('[data-page]')) item.onclick = () => selectPage(item.dataset.page);
  byId('config-reload').onclick = discover;
  byId('config-apply').onclick = () => run(async () => {
    await applyDrafts();
    status('Changes applied to RAM. Save to keep them.');
  });
  byId('config-save').onclick = () => run(async () => {
    await applyDrafts();
    status('Saving configuration…');
    const reply = await link.request(commands.save);
    reply.done();
    status('Configuration saved. Settings marked for restart take effect at the next power-on.');
  });
  byId('config-revert').onclick = () => run(async () => {
    if (link.dirty) {
      status('Reverting configuration…');
      const reply = await link.request(commands.revert);
      reply.done();
      await refreshValues();
    }
    drafts.clear();
    status('Reverted to saved settings.');
  });
  byId('config-defaults').onclick = () => restoreDefaults();
  window.addEventListener('resize', updatePidPlots);
  window.configurationConnectionChanged(null);
})();
