"use strict";

const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];

let station = null;
let latestConfiguration = null;
let latestLogSequence = 0;
let polling = false;
let toastTimer = null;
let activeManeuver = null;
let maneuverQueue = Promise.resolve();
let manualThrusterEditing = false;

function tankTemplate(index) {
  return `<article class="tank-card" data-tank="${index}">
    <div class="tank-title"><div><span>BALAST 0${index}</span><strong>Tank ${index}</strong></div><strong data-role="percent">--<small>%</small></strong></div>
    <div class="tank-visual"><div class="tank-fill" data-role="fill"></div><div class="tank-target" data-role="target"></div></div>
    <div class="tank-meta"><span data-role="position">konum ---</span><span data-role="stroke">strok ayarsız</span><span data-role="fresh">veri yok</span></div>
    <div class="preset-row"><button data-goto="0">%0</button><button data-goto="10">%10</button><button data-goto="20">%20</button><button data-goto="30">%30</button><button data-goto="50">%50</button><button data-goto="75">%75</button><button data-goto="100">%100</button></div>
    <form class="numeric-percent tank-percent-form" data-percent-form>
      <label>Özel hedef <input type="number" min="0" max="100" step="1" value="0" data-percent-input required aria-label="Tank ${index} yüzde hedefi"><span>%</span></label>
      <button class="mini-button" type="submit">HEDEFE GİT</button>
    </form>
    <div class="tank-tools">
      <button class="mini-button" data-jog="-100">−100</button><button class="mini-button" data-jog="100">+100</button>
      <button class="mini-button" data-zero>Sıfırla</button><button class="mini-button" data-calibrate>Ayarları aç</button>
      <div class="speed-control"><span>Hız</span><input type="range" min="50" max="2000" step="50" value="800" data-speed><output>800/s</output></div>
    </div>
  </article>`;
}

function thrusterTemplate(index) {
  return `<article class="thruster-card" data-thruster="${index}">
    <div class="thruster-title"><span>İTİCİ 0${index}</span><strong>${index === 1 ? "İskele" : "Sancak"}</strong></div>
    <div class="power-readout" data-role="power">0<small>%</small></div>
    <input class="power-slider" type="range" min="-100" max="100" step="5" value="0" aria-label="İtici ${index} gücü">
    <div class="slider-labels"><span>GERİ</span><span>BOŞTA</span><span>İLERİ</span></div>
  </article>`;
}

$("#tankGrid").innerHTML = tankTemplate(1) + tankTemplate(2);
$("#thrusterGrid").innerHTML = thrusterTemplate(1) + thrusterTemplate(2);

async function api(path, body) {
  const options = body === undefined ? {} : {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify(body),
  };
  const response = await fetch(path, options);
  const data = await response.json().catch(() => ({message: "Geçersiz sunucu yanıtı"}));
  if (!response.ok || data.ok === false) throw new Error(data.message || `HTTP ${response.status}`);
  return data;
}

function notify(message, error = false) {
  const element = $("#toast");
  element.textContent = message;
  element.className = `toast show${error ? " error" : ""}`;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => element.className = "toast", 3000);
}

async function action(task, successMessage) {
  try {
    await task();
    if (successMessage) notify(successMessage);
    await pollStatus();
  } catch (error) {
    notify(error.message, true);
  }
}

function selectTab(name) {
  $$(`[data-tab-target]`).forEach(button => button.classList.toggle("active", button.dataset.tabTarget === name));
  $$(`[data-tab-panel]`).forEach(panel => panel.classList.toggle("active", panel.dataset.tabPanel === name));
  try { sessionStorage.setItem("seastars-tab", name); } catch (_) {}
}

$$(`[data-tab-target]`).forEach(button => button.addEventListener("click", () => selectTab(button.dataset.tabTarget)));
try { selectTab(sessionStorage.getItem("seastars-tab") || "manual"); } catch (_) { selectTab("manual"); }

async function refreshPorts() {
  try {
    const data = await api("/api/ports");
    const select = $("#portSelect");
    const previous = select.value;
    select.innerHTML = "";
    if (!data.ports.length) select.add(new Option("Port bulunamadı", ""));
    data.ports.forEach(port => select.add(new Option(port, port, false, port === data.connected || port === previous)));
    $("#modeBadge").textContent = data.ports.includes("SIMULATOR") ? "SİMÜLATÖR" : "DONANIM";
    if (data.error) $("#connectionNote").textContent = data.error;
  } catch (error) {
    notify(`Portlar okunamadı: ${error.message}`, true);
  }
}

async function toggleConnection() {
  if (station?.connected) {
    await action(() => api("/api/disconnect", {}), "Bağlantı güvenli biçimde kesildi");
  } else {
    const port = $("#portSelect").value;
    if (!port) return notify("Önce geçerli bir port seçin", true);
    await action(() => api("/api/connect", {port}), `${port} bağlantısı başlatıldı`);
  }
  await refreshPorts();
}

function renderTank(tank) {
  const card = $(`[data-tank="${tank.id}"]`);
  const percent = tank.percent;
  $(`[data-role="percent"]`, card).innerHTML = percent == null ? `--<small>%</small>` : `${percent.toFixed(1)}<small>%</small>`;
  $(`[data-role="fill"]`, card).style.width = `${percent ?? 0}%`;
  const target = $(`[data-role="target"]`, card);
  if (tank.target_percent == null) target.style.display = "none";
  else {
    target.style.display = "block";
    target.style.left = `${tank.target_percent}%`;
  }
  $(`[data-role="position"]`, card).textContent = tank.fresh ? `konum ${tank.live_position} adım` : "konum ---";
  $(`[data-role="stroke"]`, card).textContent = tank.full_stroke > 0 ? `strok ${tank.full_stroke}` : "strok ayarsız";
  $(`[data-role="fresh"]`, card).textContent = tank.fresh ? (tank.zeroed ? "sıfır geçerli" : "sıfır gerekli") : "veri yok";
  $(`[data-speed]`, card).value = tank.speed;
  $("output", card).value = `${tank.speed}/s`;
}

function setAngleBar(selector, value) {
  const bar = $(selector);
  const width = Math.min(50, Math.abs(Number(value)) / 90 * 50);
  bar.style.width = `${width}%`;
  bar.style.left = Number(value) < 0 ? `${50 - width}%` : "50%";
}

function renderImu(imu) {
  $("#imuLive").className = `live-dot${imu.fresh ? " online" : ""}`;
  $("#imuLive").textContent = imu.fresh ? (imu.model || "CANLI") : "VERİ YOK";
  if (imu.fresh) {
    $("#headingValue").textContent = `${Number(imu.heading).toFixed(1)}°`;
    $("#rollValue").textContent = `${Number(imu.roll).toFixed(1)}°`;
    $("#pitchValue").textContent = `${Number(imu.pitch).toFixed(1)}°`;
    $("#compassNeedle").style.transform = `rotate(${Number(imu.heading)}deg)`;
    setAngleBar("#rollBar", imu.roll);
    setAngleBar("#pitchBar", imu.pitch);
  } else {
    $("#headingValue").textContent = "---°";
    $("#rollValue").textContent = "--°";
    $("#pitchValue").textContent = "--°";
  }
  const labels = [["SYS", "system"], ["GYR", "gyro"], ["ACC", "accelerometer"], ["MAG", "magnetometer"]];
  $("#calibrationBlocks").innerHTML = labels.map(([label, key]) => {
    const value = imu.calibration_parts?.[key] ?? 0;
    return `<div class="cal-block${value >= 3 ? " good" : ""}"><b>${value}/3</b><small>${label}</small></div>`;
  }).join("");
}

function renderPower(element, value) {
  const number = Number(value) || 0;
  element.innerHTML = `${number > 0 ? "+" : ""}${Math.round(number)}<small>%</small>`;
  element.className = `power-readout${number > 0 ? " positive" : number < 0 ? " negative" : ""}`;
}

const routeDefinitions = {
  test: [
    ["COUNTDOWN", "Sayaç"], ["DIVE", "Dalış balastı"], ["HOVER_SETTLE", "Askıda dengele"],
    ["TEST_FORWARD", "Kısa ileri test"], ["SURFACE", "Balastı boşalt"],
  ],
  competition: [
    ["COUNTDOWN", "Sayaç"], ["DIVE", "Dalış balastı"], ["HOVER_SETTLE", "Askıda dengele"],
    ["STRAIGHT_1", "≥15 sn düz"], ["TURN_1", "Sağ 90°"], ["STRAIGHT_2", "≥15 sn düz"],
    ["CIRCLE", "≥1 tur daire"], ["STRAIGHT_3", "≥15 sn düz"], ["TURN_2", "Sağ 90°"],
    ["STRAIGHT_4", "≥15 sn düz"], ["SURFACE", "Balastı boşalt"],
  ],
};

const missionLabels = {
  IDLE: "HAZIR", STARTING: "BAŞLIYOR", COUNTDOWN: "GERİ SAYIM", DIVE: "DALIŞ",
  HOVER_SETTLE: "ASKIDA DENGE", TEST_FORWARD: "İLERİ TEST", STRAIGHT_1: "DÜZ 1",
  TURN_1: "SAĞ 90°", STRAIGHT_2: "DÜZ 2", CIRCLE: "DAİRE", STRAIGHT_3: "DÜZ 3",
  TURN_2: "SAĞ 90°", STRAIGHT_4: "DÜZ 4", SURFACE: "YÜZEYE ÇIKIŞ",
  COMPLETE: "TAMAMLANDI", ABORTING: "İPTAL", ABORTED: "İPTAL EDİLDİ",
  FAULT_SURFACE: "HATA / BALAST BOŞALT", ESTOP: "ACİL STOP",
};

function buildRoute(route) {
  const definition = routeDefinitions[route] || routeDefinitions.test;
  $("#missionRoute").className = `route route-${route}`;
  $("#missionRoute").innerHTML = definition.map(([state, label], index) =>
    `<div data-route-state="${state}"><i>${index + 1}</i><span>${label}</span></div>`).join("");
}

function renderMission(mission) {
  const route = mission.route || latestConfiguration?.autonomy?.route || "test";
  if ($("#missionRoute").dataset.route !== route) {
    $("#missionRoute").dataset.route = route;
    buildRoute(route);
  }
  $("#missionTitle").textContent = route === "competition" ? "Görev rotası" : "Test rotası";
  $("#routeBadge").textContent = route === "competition" ? "GÖREV" : "TEST";
  $("#missionState").textContent = missionLabels[mission.state] || mission.state;
  $("#missionElapsed").textContent = `${(Number(mission.elapsed_ms) / 1000).toFixed(1)} sn`;
  $("#missionFault").textContent = mission.fault === "NONE" ? "YOK" : mission.fault;
  const definition = routeDefinitions[route] || routeDefinitions.test;
  let activeState = mission.state;
  if (mission.state === "STARTING") activeState = "DIVE";
  if (mission.state === "FAULT_SURFACE") activeState = "SURFACE";
  const activeIndex = definition.findIndex(([state]) => state === activeState);
  $$("#missionRoute [data-route-state]").forEach((step, index) => {
    const completed = mission.state === "COMPLETE" || (activeIndex >= 0 && index < activeIndex);
    step.classList.toggle("done", completed);
    step.classList.toggle("active", index === activeIndex && mission.state !== "COMPLETE");
    step.classList.toggle("fault", mission.state === "FAULT_SURFACE" && index === activeIndex);
  });
  const autoEnabled = Boolean(latestConfiguration?.autonomy?.auto_start_enabled);
  const delay = Number(latestConfiguration?.autonomy?.auto_start_delay_seconds || 60);
  if (mission.state === "COUNTDOWN") {
    const seconds = Math.ceil(Number(mission.countdown_remaining_ms || 0) / 1000);
    $("#countdownValue").textContent = `${Math.floor(seconds / 60).toString().padStart(2, "0")}:${(seconds % 60).toString().padStart(2, "0")}`;
    $("#countdownDisplay").classList.add("running");
  } else {
    $("#countdownValue").textContent = autoEnabled ? `${delay} SN AYARLI` : "KAPALI";
    $("#countdownDisplay").classList.remove("running");
  }
}

function applyControlLocks(data) {
  const manualReady = data.connected && !data.safety.estop_latched && !data.mission.active;
  $$(`[data-tank] button, [data-tank] input, [data-both-tanks] button, #bothTankPercentForm input, #bothTankPercentForm button`).forEach(control => control.disabled = !manualReady);
  $$(".power-slider, .manual-thruster-control, .hold-maneuver").forEach(control => control.disabled = !manualReady);
  $$(`[data-turn]`).forEach(control => control.disabled = !manualReady || !data.imu.fresh);
  $("#openCalibrationButton").disabled = data.mission.active;
  $("#headingZeroButton").disabled = !data.connected || !data.imu.fresh || data.mission.active;
  $("#disarmButton").disabled = !data.connected || !data.safety.armed || data.mission.active;
  $$("#autonomyForm input, #autonomyForm select, #autonomyForm button").forEach(control => control.disabled = data.mission.active);
  autonomyField("autoStartDelay").disabled = data.mission.active || autonomyField("autoStartEnabled").value !== "true";
  const captureReady = manualReady && data.tanks.every(tank => tank.fresh && tank.percent != null);
  $("#captureDiveButton").disabled = !captureReady;
  $("#captureHoverButton").disabled = !captureReady;
  const autoEnabled = Boolean(latestConfiguration?.autonomy?.auto_start_enabled);
  $("#missionPrepareButton").disabled = !data.connected || data.mission.active || data.safety.estop_latched || !autoEnabled;
  $("#missionAbortButton").disabled = !data.connected || !data.mission.active;
  if (!manualReady && activeManeuver) endManeuver(activeManeuver);
  if (!data.safety.armed && !manualThrusterEditing) {
    $("#manualLeftPercent").value = 0;
    $("#manualRightPercent").value = 0;
  }
}

function renderStatus(data) {
  station = data;
  const connected = data.connected;
  const pill = $("#connectionPill");
  pill.className = `status-pill ${connected ? "online" : "offline"}`;
  $("span", pill).textContent = connected ? `Bağlı · ${data.port}` : "Bağlantı yok";
  $("#connectButton").textContent = connected ? "Güvenli kes" : "Bağlan";
  $("#portSelect").disabled = connected;
  $("#refreshButton").disabled = connected;
  $("#connectionNote").textContent = data.connection_error || (connected ? "STM32 telemetrisi alınıyor." : "Bir port seçip bağlantıyı başlatın.");
  $("#safetyText").textContent = data.safety.estop_latched ? "ACİL STOP KİLİTLİ" : (data.safety.armed ? "İTİCİLER ETKİN" : "GÜVENLİ / DISARMED");
  $("#safetyText").className = data.safety.estop_latched ? "danger-text" : (data.safety.armed ? "armed-text" : "");
  $("#clearEstopButton").classList.toggle("hidden", !data.safety.estop_latched);
  $("#powerLimitText").textContent = `%${data.safety.power_limit}`;
  $("#armIndicator").className = `arm-indicator${data.safety.armed ? " armed" : ""}`;
  $("#armIndicator").innerHTML = `<i></i> ${data.safety.armed ? "ESC ETKİN" : "DISARMED"}`;
  const telemetryOk = connected && data.imu.fresh && data.tanks.every(tank => tank.fresh);
  $("#telemetryText").textContent = telemetryOk ? "CANLI" : (connected ? "EKSİK VERİ" : "BEKLENİYOR");
  const autoEnabled = Boolean(latestConfiguration?.autonomy?.auto_start_enabled);
  const delay = Number(latestConfiguration?.autonomy?.auto_start_delay_seconds || 60);
  $("#autoStartSummary").textContent = data.mission.state === "COUNTDOWN" ? "GERİ SAYIM" : (autoEnabled ? `${delay} SN` : "KAPALI");
  data.tanks.forEach(renderTank);
  renderImu(data.imu);
  data.thrusters.forEach(thruster => renderPower($(`[data-thruster="${thruster.id}"] [data-role="power"]`), thruster.percent));
  renderPower($("#bothPower"), data.thrusters.reduce((sum, item) => sum + Number(item.percent), 0) / 2);
  renderMission(data.mission);
  applyControlLocks(data);
  $("#developerCard").classList.toggle("hidden", !data.capabilities.developer_mode);
}

async function pollStatus() {
  if (polling) return;
  polling = true;
  try {
    renderStatus(await api("/api/status"));
  } catch (_) {
    const pill = $("#connectionPill");
    pill.className = "status-pill offline";
    $("span", pill).textContent = "İstasyon yanıt vermiyor";
  } finally {
    polling = false;
  }
}

async function pollLogs() {
  try {
    const data = await api(`/api/log?since=${latestLogSequence}`);
    if (!data.items.length) return;
    const log = $("#eventLog");
    $(".empty-log", log)?.remove();
    data.items.forEach(item => {
      const line = document.createElement("div");
      line.className = `event-line ${item.category}`;
      line.innerHTML = `<span class="time"></span><span class="kind"></span><span class="message"></span>`;
      $(".time", line).textContent = item.time;
      $(".kind", line).textContent = item.category;
      $(".message", line).textContent = item.message;
      log.append(line);
    });
    while (log.children.length > 200) log.firstChild.remove();
    latestLogSequence = data.latest_sequence;
    log.scrollTop = log.scrollHeight;
  } catch (_) {}
}

$$(`[data-tank]`).forEach(card => {
  const index = Number(card.dataset.tank);
  $$(`[data-goto]`, card).forEach(button => button.addEventListener("click", () =>
    action(() => api(`/api/tank/${index}/goto`, {percent: Number(button.dataset.goto)}), `Tank ${index} hedefi gönderildi`)));
  $("[data-percent-form]", card).addEventListener("submit", event => {
    event.preventDefault();
    if (!event.currentTarget.reportValidity()) return;
    const percent = Number($("[data-percent-input]", card).value);
    action(() => api(`/api/tank/${index}/goto`, {percent}), `Tank ${index} için %${percent} hedefi gönderildi`);
  });
  $$(`[data-jog]`, card).forEach(button => button.addEventListener("click", () =>
    action(() => api(`/api/tank/${index}/jog`, {steps: Number(button.dataset.jog)}))));
  $("[data-zero]", card).addEventListener("click", () => {
    if (confirm(`Tank ${index} şu anda fiziksel olarak TAMAMEN BOŞ mu? Mevcut konum 0 kabul edilecek.`))
      action(() => api(`/api/tank/${index}/zero`, {}), `Tank ${index} sıfırlandı`);
  });
  $("[data-calibrate]", card).addEventListener("click", () => openSettings(`tank${index}Full`));
  const speed = $("[data-speed]", card);
  speed.addEventListener("input", () => $("output", speed.parentElement).value = `${speed.value}/s`);
  speed.addEventListener("change", () => action(() => api(`/api/tank/${index}/speed`, {sps: Number(speed.value)})));
});

$$(`[data-both-tanks] button`).forEach(button => button.addEventListener("click", () =>
  action(() => api("/api/tanks/goto", {percent: Number(button.dataset.percent)}), "İki tankın hedefi gönderildi")));

$("#bothTankPercentForm").addEventListener("submit", event => {
  event.preventDefault();
  if (!event.currentTarget.reportValidity()) return;
  const percent = Number($("#bothTankPercent").value);
  action(() => api("/api/tanks/goto", {percent}), `İki tank için %${percent} hedefi gönderildi`);
});

function installSpringSlider(slider, endpoint, readout) {
  let lastSentAt = 0;
  let sending = false;
  let queued = null;
  async function send(value) {
    queued = value;
    if (sending) return;
    const delay = Math.max(0, 90 - (performance.now() - lastSentAt));
    if (delay) await new Promise(resolve => setTimeout(resolve, delay));
    const next = queued;
    queued = null;
    sending = true;
    lastSentAt = performance.now();
    try { await api(endpoint, {percent: Number(next)}); }
    catch (error) { notify(error.message, true); slider.value = 0; renderPower(readout, 0); }
    finally { sending = false; if (queued !== null) send(queued); }
  }
  slider.addEventListener("input", () => { renderPower(readout, Number(slider.value)); send(slider.value); });
  const release = () => {
    if (Number(slider.value) !== 0) {
      slider.value = 0;
      renderPower(readout, 0);
      send(0);
    }
  };
  slider.addEventListener("pointerup", release);
  slider.addEventListener("pointercancel", release);
  slider.addEventListener("keyup", release);
  slider.addEventListener("blur", release);
}

$$(`[data-thruster]`).forEach(card => {
  const index = card.dataset.thruster;
  installSpringSlider($(".power-slider", card), `/api/esc/${index}/set`, $("[data-role=power]", card));
});
installSpringSlider($("#bothThrusterSlider"), "/api/esc/both", $("#bothPower"));

const manualThrusterForm = $("#manualThrusterForm");
manualThrusterForm.addEventListener("focusin", () => { manualThrusterEditing = true; });
manualThrusterForm.addEventListener("focusout", () => setTimeout(() => {
  manualThrusterEditing = manualThrusterForm.contains(document.activeElement);
}, 0));
manualThrusterForm.addEventListener("submit", event => {
  event.preventDefault();
  if (!manualThrusterForm.reportValidity()) return;
  const left = Number($("#manualLeftPercent").value);
  const right = Number($("#manualRightPercent").value);
  action(async () => {
    await api("/api/esc/pair", {left, right});
    manualThrusterEditing = false;
    document.activeElement?.blur();
  }, `İskele %${left}, sancak %${right} uygulandı`);
});
$("#manualThrusterStop").addEventListener("click", () => action(async () => {
  await api("/api/esc/pair", {left: 0, right: 0});
  $("#manualLeftPercent").value = 0;
  $("#manualRightPercent").value = 0;
  manualThrusterEditing = false;
}, "İki itici durduruldu"));

function queueThrusterPair(left, right) {
  maneuverQueue = maneuverQueue.catch(() => {}).then(() => api("/api/esc/pair", {left, right}));
  maneuverQueue.catch(error => notify(error.message, true));
  return maneuverQueue;
}

function beginManeuver(event) {
  if (event.button !== undefined && event.button !== 0) return;
  const button = event.currentTarget;
  if (button.disabled) return;
  event.preventDefault();
  if (activeManeuver && activeManeuver !== button) endManeuver(activeManeuver);
  activeManeuver = button;
  button.classList.add("active");
  queueThrusterPair(Number(button.dataset.left), Number(button.dataset.right));
}

function endManeuver(button) {
  if (activeManeuver !== button) return;
  button.classList.remove("active");
  activeManeuver = null;
  queueThrusterPair(0, 0);
}

$$(`.hold-maneuver`).forEach(button => {
  button.addEventListener("pointerdown", beginManeuver);
  button.addEventListener("pointerup", () => endManeuver(button));
  button.addEventListener("pointerleave", () => endManeuver(button));
  button.addEventListener("pointercancel", () => endManeuver(button));
});
$$(`[data-turn]`).forEach(button => button.addEventListener("click", () =>
  action(() => api("/api/maneuver/turn", {degrees: Number(button.dataset.turn)}), `${button.dataset.turn}° dönüş başlatıldı`)));

const settingsDialog = $("#settingsDialog");
const settingsForm = $("#settingsForm");
const settingField = name => settingsForm.elements.namedItem(name);
const autonomyForm = $("#autonomyForm");
const autonomyField = name => autonomyForm.elements.namedItem(name);

function updateRouteFieldVisibility() {
  const route = autonomyField("route").value;
  $("#testRouteFields").hidden = route !== "test";
  $("#competitionRouteFields").hidden = route !== "competition";
  $("#routeBadge").textContent = route === "competition" ? "GÖREV" : "TEST";
  if (!station?.mission?.active) {
    $("#missionRoute").dataset.route = route;
    buildRoute(route);
    $("#missionTitle").textContent = route === "competition" ? "Görev rotası" : "Test rotası";
  }
}

function fillConfiguration(config) {
  latestConfiguration = config;
  settingField("tank1Full").value = config.tanks["1"].full_stroke;
  settingField("tank2Full").value = config.tanks["2"].full_stroke;
  settingField("headingOffset").value = config.imu.heading_offset_deg;
  settingField("headingSign").value = String(config.imu.heading_sign);
  settingField("balanceAxis").value = config.imu.balance_axis;
  settingField("balanceSign").value = String(config.imu.balance_sign);
  settingField("levelKp").value = config.imu.level_kp_pct_per_degree;
  settingField("maxBalance").value = config.imu.maximum_balance_pct;
  autonomyField("route").value = config.autonomy.route;
  autonomyField("autoStartEnabled").value = String(config.autonomy.auto_start_enabled);
  autonomyField("autoStartDelay").value = String(config.autonomy.auto_start_delay_seconds);
  autonomyField("autoZeroTanks").checked = config.autonomy.auto_zero_tanks;
  autonomyField("diveBallast").value = config.autonomy.dive_ballast_pct;
  autonomyField("diveSeconds").value = config.autonomy.dive_seconds;
  autonomyField("hoverBallast").value = config.autonomy.hover_ballast_pct;
  autonomyField("hoverSettle").value = config.autonomy.hover_settle_seconds;
  autonomyField("testForwardSeconds").value = config.autonomy.test_forward_seconds;
  autonomyField("testForwardPower").value = config.autonomy.test_forward_power_pct;
  autonomyField("straightSeconds").value = config.mission.straight_seconds;
  autonomyField("forwardPower").value = config.mission.forward_power_pct;
  autonomyField("circleForward").value = config.mission.circle_forward_power_pct;
  autonomyField("circleTurn").value = config.mission.circle_turn_power_pct;
  autonomyField("headingKp").value = config.mission.heading_kp;
  updateRouteFieldVisibility();
}

async function loadConfiguration() {
  const config = await api("/api/configuration");
  fillConfiguration(config);
  return config;
}

async function openSettings(focusName = null) {
  try {
    $("#settingsStatus").textContent = "Ayarlar yükleniyor…";
    await loadConfiguration();
    $("#settingsStatus").textContent = "Ayarlar bilgisayara ve bağlıysa STM32’ye kaydedilir.";
    if (!settingsDialog.open) settingsDialog.showModal();
    if (focusName) requestAnimationFrame(() => {
      settingField(focusName).focus();
      settingField(focusName).select();
    });
  } catch (error) {
    notify(`Ayarlar açılamadı: ${error.message}`, true);
  }
}

function hardwareConfigurationFromForm() {
  const number = name => Number(settingField(name).value);
  return {
    tanks: {
      "1": {full_stroke: number("tank1Full")},
      "2": {full_stroke: number("tank2Full")},
    },
    imu: {
      heading_offset_deg: number("headingOffset"),
      heading_sign: number("headingSign"),
      balance_axis: settingField("balanceAxis").value,
      balance_sign: number("balanceSign"),
      level_kp_pct_per_degree: number("levelKp"),
      maximum_balance_pct: number("maxBalance"),
    },
  };
}

function autonomyConfigurationFromForm() {
  const number = name => Number(autonomyField(name).value);
  return {
    autonomy: {
      route: autonomyField("route").value,
      auto_start_enabled: autonomyField("autoStartEnabled").value === "true",
      auto_start_delay_seconds: number("autoStartDelay"),
      auto_zero_tanks: autonomyField("autoZeroTanks").checked,
      dive_ballast_pct: number("diveBallast"),
      dive_seconds: number("diveSeconds"),
      hover_ballast_pct: number("hoverBallast"),
      hover_settle_seconds: number("hoverSettle"),
      test_forward_seconds: number("testForwardSeconds"),
      test_forward_power_pct: number("testForwardPower"),
    },
    mission: {
      straight_seconds: number("straightSeconds"),
      forward_power_pct: number("forwardPower"),
      circle_forward_power_pct: number("circleForward"),
      circle_turn_power_pct: number("circleTurn"),
      heading_kp: number("headingKp"),
    },
  };
}

settingsForm.addEventListener("submit", async event => {
  event.preventDefault();
  if (!settingsForm.reportValidity()) return;
  $("#settingsStatus").textContent = "Doğrulanıyor ve kaydediliyor…";
  try {
    const result = await api("/api/configuration", hardwareConfigurationFromForm());
    fillConfiguration(result.configuration);
    settingsDialog.close();
    notify("Tank ve IMU ayarları kaydedildi");
    await pollStatus();
  } catch (error) {
    $("#settingsStatus").textContent = error.message;
    notify(error.message, true);
  }
});

autonomyForm.addEventListener("submit", async event => {
  event.preventDefault();
  if (!autonomyForm.reportValidity()) return;
  const changes = autonomyConfigurationFromForm();
  if (changes.autonomy.auto_zero_tanks && !confirm("DİKKAT: Güç her verildiğinde iki tankın fiziksel olarak tamamen boş olacağını garanti ediyor musunuz? Limit anahtarı yoktur.")) return;
  $("#autonomyStatus").textContent = "Doğrulanıyor ve STM32’ye aktarılıyor…";
  try {
    const result = await api("/api/configuration", changes);
    fillConfiguration(result.configuration);
    $("#autonomyStatus").textContent = changes.autonomy.auto_start_enabled
      ? "Kaydedildi; koşullar hazırsa otomatik sayaç başlatıldı."
      : "Kaydedildi; otomatik başlatma kapalı.";
    notify("Otonomi profili kaydedildi");
    await pollStatus();
  } catch (error) {
    $("#autonomyStatus").textContent = error.message;
    notify(error.message, true);
  }
});

autonomyField("route").addEventListener("change", updateRouteFieldVisibility);
autonomyField("autoStartEnabled").addEventListener("change", () => {
  autonomyField("autoStartDelay").disabled = autonomyField("autoStartEnabled").value !== "true";
});
function captureCurrentBallast(fieldName, label) {
  const values = station?.tanks?.map(tank => tank.percent).filter(value => value != null) || [];
  if (values.length !== 2) return notify("İki tankın da canlı ve kalibre edilmiş yüzde verisi gerekli", true);
  const average = Math.round(values.reduce((sum, value) => sum + Number(value), 0) / values.length * 10) / 10;
  autonomyField(fieldName).value = average;
  notify(`${label} %${average} olarak forma alındı; kaydetmeyi unutmayın`);
}
$("#captureDiveButton").addEventListener("click", () => captureCurrentBallast("diveBallast", "Dalış balastı"));
$("#captureHoverButton").addEventListener("click", () => captureCurrentBallast("hoverBallast", "Askıda balast"));
$("#openCalibrationButton").addEventListener("click", () => openSettings());
$("#closeSettingsButton").addEventListener("click", () => settingsDialog.close());
$("#cancelSettingsButton").addEventListener("click", () => settingsDialog.close());
$("#headingZeroButton").addEventListener("click", async () => {
  try {
    const result = await api("/api/calibration/heading-zero", {});
    settingField("headingOffset").value = result.heading_offset_deg;
    await loadConfiguration();
    notify("Mevcut IMU yönü 0° olarak kaydedildi");
  } catch (error) {
    notify(error.message, true);
  }
});

$("#missionPrepareButton").addEventListener("click", () => {
  const autoZero = latestConfiguration?.autonomy?.auto_zero_tanks;
  const warning = autoZero
    ? "Tankların şu anda TAMAMEN BOŞ olduğunu doğruluyor musunuz? Sayaç yeniden başlayacak ve konumlar 0 kabul edilecek."
    : "Tank sıfırları ve tam strok ayarları doğrulandı mı? Otomatik sayaç yeniden başlayacak.";
  if (confirm(warning)) action(() => api("/api/mission/schedule", {}), "Otomatik başlatma sayacı yeniden hazırlandı");
});
$("#missionAbortButton").addEventListener("click", () =>
  action(() => api("/api/mission/abort", {}), "Görev iptal edildi; balastlar boşaltılıyor"));

$("#disarmButton").addEventListener("click", () => action(() => api("/api/esc/disarm", {}), "İticiler durduruldu ve DISARM edildi"));
$("#estopButton").addEventListener("click", () => action(() => api("/api/estop", {}), "ACİL STOP uygulandı"));
$("#clearEstopButton").addEventListener("click", () => {
  if (confirm("Fiziksel ortam güvenli mi ve tüm hareket kontrolleri boşta mı?"))
    action(() => api("/api/estop/reset", {}), "Acil stop kilidi kaldırıldı; sistem DISARMED");
});
$("#connectButton").addEventListener("click", toggleConnection);
$("#refreshButton").addEventListener("click", refreshPorts);
$("#clearLogButton").addEventListener("click", () => $("#eventLog").innerHTML = '<div class="empty-log">Görünüm temizlendi.</div>');
$("#commandForm").addEventListener("submit", event => {
  event.preventDefault();
  const input = $("#commandInput");
  if (input.value.trim()) action(() => api("/api/cmd", {cmd: input.value.trim()}), "Komut gönderildi");
  input.value = "";
});

document.addEventListener("keydown", event => {
  if (event.key === "Escape" && !settingsDialog.open) action(() => api("/api/estop", {}), "ACİL STOP uygulandı");
});
document.addEventListener("visibilitychange", () => {
  if (document.hidden && activeManeuver) endManeuver(activeManeuver);
  if (document.hidden && station?.safety.armed && !station?.mission.active) api("/api/esc/disarm", {}).catch(() => {});
});
window.addEventListener("blur", () => {
  if (activeManeuver) endManeuver(activeManeuver);
  if (station?.safety.armed && !station?.mission.active) api("/api/esc/disarm", {}).catch(() => {});
});

setInterval(() => $("#clock").textContent = new Date().toLocaleTimeString("tr-TR", {hour12: false}), 500);
setInterval(() => {
  if (station?.safety.armed && !station?.mission.active && !document.hidden && document.hasFocus())
    api("/api/heartbeat", {}).catch(() => {});
}, 300);
setInterval(pollStatus, 250);
setInterval(pollLogs, 400);
setInterval(() => { if (!station?.connected) refreshPorts(); }, 4000);

refreshPorts();
loadConfiguration().then(pollStatus).catch(() => pollStatus());
pollLogs();
