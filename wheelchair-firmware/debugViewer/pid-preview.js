'use strict';

// Mirrors RPMSetpointPID::smoothRPM for a fresh controller and a half-scale RPM
// step. The device schema supplies tuning field IDs and the fixed linear limits.
const PidPreview = (() => {
  function simulate(values, limits) {
    const [lowTau, highTau, ki, kd, filter, jerk, periodMS, circumference] = values;
    const [maxSpeed, maxAcceleration] = limits;
    if (values.length !== 8 || limits.length !== 2 ||
        ![...values, ...limits].every(Number.isFinite) ||
        lowTau <= 0 || highTau <= 0 || ki < 0 || kd < 0 || filter < 0 ||
        jerk <= 0 || periodMS <= 0 || circumference <= 0 || maxSpeed <= 0 || maxAcceleration <= 0)
      throw new Error('Enter valid PID settings to preview the response.');

    const dt = periodMS / 1000;
    const duration = 5;
    const steps = Math.ceil(duration / dt);
    if (steps > 20000) throw new Error('The control period is too small for this preview.');
    const conversion = 60 / circumference;
    const maxRPM = maxSpeed * conversion;
    const accelerationLimit = maxAcceleration * conversion;
    const target = maxRPM / 2;
    const clip = (value, lo, hi) => Math.min(hi, Math.max(lo, value));
    let rpm = 0, acceleration = 0, integral = 0, filteredRate = 0;
    let previousRPM = 0, havePrevious = false;
    const samples = [{time: 0, rpm: 0, target: 0}];

    for (let step = 1; step <= steps; step++) {
      const time = step * dt;
      const requestedRPM = time >= 0.25 ? target : 0;
      if (havePrevious) {
        const rate = (rpm - previousRPM) / dt;
        filteredRate += dt / (filter + dt) * (rate - filteredRate);
      }
      previousRPM = rpm;
      havePrevious = true;
      let fraction = clip(Math.abs(rpm) / maxRPM, 0, 1);
      fraction = fraction * fraction * (3 - 2 * fraction);
      const tau = lowTau + fraction * (highTau - lowTau);
      const error = requestedRPM - rpm;
      const requestedAcceleration = error / tau + ki * integral - kd * filteredRate;
      const boundedAcceleration = clip(requestedAcceleration, -accelerationLimit, accelerationLimit);
      acceleration += clip(boundedAcceleration - acceleration, -jerk * dt, jerk * dt);
      acceleration = clip(acceleration, -accelerationLimit, accelerationLimit);
      if (ki > 0 && (Math.abs(requestedAcceleration - acceleration) < 1e-5 ||
                    error * (requestedAcceleration - acceleration) <= 0)) {
        integral = clip(integral + error * dt, -accelerationLimit / ki, accelerationLimit / ki);
      }
      const nextRPM = rpm + acceleration * dt;
      if (![nextRPM, filteredRate, integral].every(Number.isFinite))
        throw new Error('These settings produce an invalid simulated response.');
      rpm = clip(nextRPM, -maxRPM, maxRPM);
      if (rpm !== nextRPM) {
        acceleration = integral = filteredRate = 0;
        havePrevious = false;
      }
      samples.push({time, rpm, target: requestedRPM});
    }
    return {samples, target, periodMS, duration};
  }

  function draw(canvas, response) {
    const bounds = canvas.getBoundingClientRect();
    const w = Math.max(1, bounds.width), h = Math.max(1, bounds.height);
    const ratio = window.devicePixelRatio || 1;
    canvas.width = Math.round(w * ratio);
    canvas.height = Math.round(h * ratio);
    const g = canvas.getContext('2d');
    g.scale(ratio, ratio);
    const margin = {left: 56, right: 18, top: 18, bottom: 38};
    const width = w - margin.left - margin.right, height = h - margin.top - margin.bottom;
    const lo = 0, hi = 50;
    const x = time => margin.left + time / response.duration * width;
    const y = rpm => margin.top + (hi - rpm) / (hi - lo) * height;
    g.clearRect(0, 0, w, h);
    g.fillStyle = '#f8fafb';
    g.fillRect(margin.left, margin.top, width, height);
    g.font = '12px system-ui';
    g.lineWidth = 1;
    for (let tick = 0; tick <= 5; tick++) {
      const rpm = tick * 10;
      g.strokeStyle = '#e0e6eb';
      g.beginPath(); g.moveTo(margin.left, y(rpm)); g.lineTo(w - margin.right, y(rpm)); g.stroke();
      g.fillStyle = '#667586'; g.textAlign = 'right';
      g.fillText(String(rpm), margin.left - 8, y(rpm) + 4);
    }
    for (let second = 0; second <= response.duration; second++) {
      g.strokeStyle = '#e0e6eb';
      g.beginPath(); g.moveTo(x(second), margin.top); g.lineTo(x(second), h - margin.bottom); g.stroke();
      g.fillStyle = '#667586'; g.textAlign = 'center';
      g.fillText(String(second), x(second), h - margin.bottom + 18);
    }
    g.save();
    g.beginPath(); g.rect(margin.left, margin.top, width, height); g.clip();
    for (const [key, color, dash] of [['target', '#8996a4', [5, 4]], ['rpm', '#146db0', []]]) {
      g.strokeStyle = color;
      g.lineWidth = 2;
      g.setLineDash(dash);
      g.beginPath();
      response.samples.forEach((sample, index) => {
        if (index === 0) g.moveTo(x(sample.time), y(sample[key]));
        else g.lineTo(x(sample.time), y(sample[key]));
      });
      g.stroke();
    }
    g.restore();
    g.fillStyle = '#43566a'; g.textAlign = 'center';
    g.fillText('Time (s)', margin.left + width / 2, h - 4);
    g.save(); g.translate(13, margin.top + height / 2); g.rotate(-Math.PI / 2);
    g.fillText('Wheel RPM', 0, 0); g.restore();
  }

  return {simulate, draw};
})();
